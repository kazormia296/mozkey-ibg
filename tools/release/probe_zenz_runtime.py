#!/usr/bin/env python3
"""End-to-end gate for the staged Linux Zenz release runtime.

The probe deliberately uses the release layout instead of debug-only path
overrides.  It stages the Bazel outputs with the product installer, starts the
staged scorer in an isolated HOME, validates the spawned llama-server through
/proc, and exercises both the ZNZ1 wire and authenticated /completion paths.

Sensitive runtime material is never printed.  In particular, failures are
reported as stable reason codes rather than exception strings, paths, prompts,
API keys, response values, or captured child output.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import hashlib
import http.client
import ipaddress
import json
import os
import signal
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator


WIRE_MAGIC = 0x315A4E5A
WIRE_VERSION = 2
WIRE_REQUEST = 1
WIRE_RESPONSE = 2
WIRE_STATUS_OK = 0
WIRE_REQUEST_HEADER = struct.Struct("<IHHIIIII")
WIRE_RESPONSE_HEADER = struct.Struct("<IHHIIIII")

DEFAULT_TIMEOUT_SECONDS = 120.0
MAX_CHILD_OUTPUT_BYTES = 1 << 20
MAX_PROC_FILE_BYTES = 1 << 16
MAX_WIRE_PAYLOAD_BYTES = 1 << 16
MAX_HTTP_RESPONSE_BYTES = 1 << 16
DEFAULT_LLAMA_SERVER = Path("/usr/bin/llama-server")
MODEL_NAME = "zenz-v3.2-small-Q5_K_M.gguf"
SHORT_RUNTIME_PARENT = Path("/tmp")
MAX_UNIX_SOCKET_PATH_BYTES = 107

# This is the same short readiness prompt used by the scorer itself.  Neither
# this prompt nor either model response is included in diagnostics.
_PROBE_PROMPT = "\uee02\uee00テスト\uee01"
_MISSING_BACKEND_DEVICE = "MozkeyProbeMissingDevice0"


class ProbeFailure(RuntimeError):
    """A redaction-safe probe failure identified only by a stable code."""

    def __init__(self, code: str):
        super().__init__(code)
        self.code = code


@dataclasses.dataclass(frozen=True)
class LlamaRuntime:
    pid: int
    start_time: str
    port: int
    api_key: str


@dataclasses.dataclass(frozen=True)
class TcpListener:
    inode: str
    address: str
    port: int


class _BoundedOutput:
    """Drains a child pipe while remembering only whether its cap was crossed."""

    def __init__(self, stream: BinaryIO, limit: int = MAX_CHILD_OUTPUT_BYTES):
        self._stream = stream
        self._limit = limit
        self._bytes = 0
        self.exceeded = threading.Event()
        self._thread = threading.Thread(target=self._drain, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def join(self, timeout: float = 2.0) -> None:
        self._thread.join(timeout)

    def _drain(self) -> None:
        try:
            while True:
                chunk = self._stream.read(8192)
                if not chunk:
                    return
                self._bytes += len(chunk)
                if self._bytes > self._limit:
                    self.exceeded.set()
        except (OSError, ValueError):
            # Process termination may close the pipe while this thread drains.
            return


def _terminate_command(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def _run_bounded_command(
    argv: list[str],
    *,
    cwd: Path,
    env: dict[str, str],
    timeout: float,
    failure_code: str,
) -> None:
    try:
        process = subprocess.Popen(
            argv,
            cwd=cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except OSError as error:
        raise ProbeFailure(failure_code) from error

    assert process.stdout is not None
    output = _BoundedOutput(process.stdout)
    output.start()
    deadline = time.monotonic() + timeout
    try:
        while process.poll() is None:
            if output.exceeded.is_set() or time.monotonic() >= deadline:
                _terminate_command(process)
                raise ProbeFailure(failure_code)
            time.sleep(0.05)
        if output.exceeded.is_set() or process.returncode != 0:
            raise ProbeFailure(failure_code)
    finally:
        _terminate_command(process)
        output.join()
        if process.stdout is not None:
            process.stdout.close()


def _read_bounded(path: Path, limit: int = MAX_PROC_FILE_BYTES) -> bytes:
    try:
        with path.open("rb") as stream:
            data = stream.read(limit + 1)
    except OSError as error:
        raise ProbeFailure("proc_read_failed") from error
    if len(data) > limit:
        raise ProbeFailure("proc_value_too_large")
    return data


def _require_private_directory(path: Path) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise ProbeFailure("private_home_invalid") from error
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise ProbeFailure("private_home_invalid")


def _require_short_socket_path(path: Path) -> None:
    try:
        encoded = os.fsencode(path)
    except (TypeError, UnicodeEncodeError) as error:
        raise ProbeFailure("private_home_invalid") from error
    if len(encoded) > MAX_UNIX_SOCKET_PATH_BYTES:
        raise ProbeFailure("private_home_invalid")


@contextlib.contextmanager
def _probe_directories() -> Iterator[tuple[Path, Path]]:
    """Yield a large staging root and a short, private runtime HOME.

    The staged model follows TMPDIR so release jobs can place its 74 MB payload
    on a filesystem with sufficient quota.  The scorer HOME is intentionally
    allocated directly below /tmp because its Unix-domain socket must remain
    below Linux's sockaddr_un path limit.  Keeping these roots separate also
    preserves canonical staged paths for the exact child-argv attestation.
    """

    try:
        with tempfile.TemporaryDirectory(
            prefix="mozkey-zenz-stage-"
        ) as stage_temporary, tempfile.TemporaryDirectory(
            prefix="mz-z-", dir=SHORT_RUNTIME_PARENT
        ) as runtime_temporary:
            probe_root = Path(stage_temporary)
            home = Path(runtime_temporary)
            os.chmod(probe_root, 0o700)
            os.chmod(home, 0o700)
            stage = probe_root / "stage"
            stage.mkdir(mode=0o700)
            _require_private_directory(probe_root)
            _require_private_directory(stage)
            _require_private_directory(home)
            _require_short_socket_path(home / ".mozkey-ibg_zenz_scorer_pipe")
            yield stage, home
    except ProbeFailure:
        raise
    except OSError as error:
        raise ProbeFailure("private_home_invalid") from error


def _require_staged_regular(
    path: Path, *, mode: int, executable: bool, nonempty: bool
) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise ProbeFailure("staged_runtime_invalid") from error
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
        raise ProbeFailure("staged_runtime_invalid")
    if stat.S_IMODE(info.st_mode) != mode:
        raise ProbeFailure("staged_runtime_invalid")
    if executable != bool(info.st_mode & stat.S_IXUSR):
        raise ProbeFailure("staged_runtime_invalid")
    if nonempty and info.st_size <= 0:
        raise ProbeFailure("staged_runtime_invalid")


def _validate_staged_runtime(
    stage: Path, *, bundled_server: bool = False
) -> tuple[Path, Path, Path]:
    scorer = stage / "usr/lib/mozkey-ibg/mozc_zenz_scorer"
    model = stage / "usr/lib/mozkey-ibg/models" / MODEL_NAME
    llama_link = stage / "usr/lib/mozkey-ibg/llama-server"

    _require_staged_regular(scorer, mode=0o755, executable=True, nonempty=True)
    _require_staged_regular(model, mode=0o644, executable=False, nonempty=True)
    if bundled_server:
        _require_staged_regular(
            llama_link, mode=0o755, executable=True, nonempty=True
        )
    else:
        try:
            link_info = llama_link.lstat()
            target = os.readlink(llama_link)
        except OSError as error:
            raise ProbeFailure("staged_runtime_invalid") from error
        if (
            not stat.S_ISLNK(link_info.st_mode)
            or link_info.st_uid != os.geteuid()
            or target != str(DEFAULT_LLAMA_SERVER)
        ):
            raise ProbeFailure("staged_runtime_invalid")
    return scorer, model, llama_link


def _proc_start_time(pid: int) -> str:
    # /proc/<pid>/stat field 22 is the process start time.  The executable name
    # can contain spaces, so count from the final ')' rather than relying only
    # on split positions.
    raw = _read_bounded(Path("/proc") / str(pid) / "stat").decode(
        "ascii", errors="strict"
    )
    end = raw.rfind(")")
    if end < 0:
        raise ProbeFailure("proc_identity_invalid")
    tail = raw[end + 2 :].split()
    if len(tail) < 20:
        raise ProbeFailure("proc_identity_invalid")
    return tail[19]


def _proc_uid(pid: int) -> int:
    text = _read_bounded(Path("/proc") / str(pid) / "status").decode(
        "ascii", errors="strict"
    )
    for line in text.splitlines():
        if line.startswith("Uid:"):
            fields = line.split()
            if len(fields) >= 2 and fields[1].isdigit():
                return int(fields[1])
    raise ProbeFailure("proc_identity_invalid")


def _direct_children(pid: int) -> list[int]:
    data = _read_bounded(
        Path("/proc") / str(pid) / "task" / str(pid) / "children", 4096
    )
    try:
        values = data.decode("ascii", errors="strict").split()
    except UnicodeError as error:
        raise ProbeFailure("proc_children_invalid") from error
    if any(not value.isdigit() for value in values):
        raise ProbeFailure("proc_children_invalid")
    return [int(value) for value in values]


def _process_argv(pid: int) -> list[str]:
    data = _read_bounded(Path("/proc") / str(pid) / "cmdline")
    if not data or not data.endswith(b"\0"):
        raise ProbeFailure("llama_arguments_invalid")
    try:
        return [item.decode("utf-8", errors="strict") for item in data[:-1].split(b"\0")]
    except UnicodeError as error:
        raise ProbeFailure("llama_arguments_invalid") from error


def _inspect_llama_argv(
    argv: list[str],
    *,
    expected_link: Path,
    expected_model: Path,
    expected_backend_device: str | None = None,
) -> tuple[int, str]:
    if len(argv) not in {13, 15}:
        raise ProbeFailure("llama_arguments_invalid")
    if argv[0] != str(expected_link):
        raise ProbeFailure("llama_arguments_invalid")
    if argv[1:4:2] != ["-m", "-c"]:
        raise ProbeFailure("llama_arguments_invalid")
    if argv[2] != str(expected_model) or argv[5] != "-t":
        raise ProbeFailure("llama_arguments_invalid")
    if argv[7:12:2] != ["--host", "--port", "--api-key"]:
        raise ProbeFailure("llama_arguments_invalid")
    backend_device = ""
    if len(argv) == 15:
        backend_device = argv[14]
        if (
            argv[13] != "--device"
            or not backend_device
            or len(backend_device) > 128
            or any(
                not (
                    character.isascii()
                    and (character.isalnum() or character in "_.-")
                )
                for character in backend_device
            )
        ):
            raise ProbeFailure("llama_arguments_invalid")
    if (
        expected_backend_device is not None
        and backend_device != expected_backend_device
    ):
        raise ProbeFailure("llama_device_invalid")
    if argv[8] != "127.0.0.1":
        raise ProbeFailure("llama_host_invalid")

    try:
        context = int(argv[4], 10)
        threads = int(argv[6], 10)
        port = int(argv[10], 10)
    except ValueError as error:
        raise ProbeFailure("llama_arguments_invalid") from error
    if not 64 <= context <= 1024 or not 1 <= threads <= 16:
        raise ProbeFailure("llama_arguments_invalid")
    if not 49152 <= port <= 65535:
        raise ProbeFailure("llama_port_invalid")

    api_key = argv[12]
    if len(api_key) != 64 or any(character not in "0123456789abcdef" for character in api_key):
        raise ProbeFailure("llama_api_key_invalid")
    return port, api_key


def _wait_for_llama_child(
    scorer: subprocess.Popen[bytes],
    *,
    expected_link: Path,
    expected_model: Path,
    expected_executable: Path,
    expected_backend_device: str | None,
    deadline: float,
) -> LlamaRuntime:
    last_failure = "llama_child_missing"
    while time.monotonic() < deadline:
        if scorer.poll() is not None:
            raise ProbeFailure("scorer_exited_early")
        try:
            children = _direct_children(scorer.pid)
            if len(children) != 1:
                last_failure = "llama_child_count_invalid"
                time.sleep(0.05)
                continue
            child = children[0]
            if _proc_uid(child) != os.geteuid():
                raise ProbeFailure("llama_identity_invalid")
            port, api_key = _inspect_llama_argv(
                _process_argv(child),
                expected_link=expected_link,
                expected_model=expected_model,
                expected_backend_device=expected_backend_device,
            )
            try:
                executable = (Path("/proc") / str(child) / "exe").resolve(strict=True)
                expected = expected_executable.resolve(strict=True)
            except OSError as error:
                raise ProbeFailure("llama_identity_invalid") from error
            if executable != expected:
                raise ProbeFailure("llama_identity_invalid")
            return LlamaRuntime(
                pid=child,
                start_time=_proc_start_time(child),
                port=port,
                api_key=api_key,
            )
        except ProbeFailure as error:
            if error.code not in {
                "proc_read_failed",
                "proc_identity_invalid",
                "llama_arguments_invalid",
            }:
                raise
            last_failure = error.code
        time.sleep(0.05)
    raise ProbeFailure(last_failure)


def _decode_proc_address(value: str, family: int) -> str:
    try:
        raw = bytes.fromhex(value)
        if family == socket.AF_INET and len(raw) == 4:
            return socket.inet_ntop(family, raw[::-1])
        if family == socket.AF_INET6 and len(raw) == 16:
            # Linux prints each 32-bit word in host byte order.
            network = b"".join(raw[index : index + 4][::-1] for index in range(0, 16, 4))
            return socket.inet_ntop(family, network)
    except (OSError, ValueError):
        pass
    raise ProbeFailure("listener_table_invalid")


def _parse_tcp_listeners(text: str, family: int) -> list[TcpListener]:
    listeners: list[TcpListener] = []
    for line in text.splitlines()[1:]:
        fields = line.split()
        if len(fields) < 10 or fields[3] != "0A":
            continue
        try:
            address_hex, port_hex = fields[1].rsplit(":", 1)
            address = _decode_proc_address(address_hex, family)
            port = int(port_hex, 16)
        except (ValueError, ProbeFailure) as error:
            raise ProbeFailure("listener_table_invalid") from error
        listeners.append(TcpListener(fields[9], address, port))
    return listeners


def _socket_inodes(pid: int) -> set[str]:
    directory = Path("/proc") / str(pid) / "fd"
    try:
        entries = list(directory.iterdir())
    except OSError as error:
        raise ProbeFailure("listener_inspection_failed") from error
    if len(entries) > 4096:
        raise ProbeFailure("listener_inspection_failed")
    inodes: set[str] = set()
    for entry in entries:
        try:
            target = os.readlink(entry)
        except OSError:
            continue
        if target.startswith("socket:[") and target.endswith("]"):
            inode = target[8:-1]
            if inode.isdigit():
                inodes.add(inode)
    return inodes


def _validate_loopback_listener(runtime: LlamaRuntime, deadline: float) -> None:
    proc = Path("/proc") / str(runtime.pid)
    while time.monotonic() < deadline:
        if not proc.exists():
            raise ProbeFailure("llama_exited_early")
        try:
            inodes = _socket_inodes(runtime.pid)
            listeners: list[TcpListener] = []
            for name, family in (("tcp", socket.AF_INET), ("tcp6", socket.AF_INET6)):
                table = _read_bounded(proc / "net" / name).decode(
                    "ascii", errors="strict"
                )
                listeners.extend(
                    listener
                    for listener in _parse_tcp_listeners(table, family)
                    if listener.inode in inodes
                )
            if not listeners:
                time.sleep(0.05)
                continue
            if any(
                not ipaddress.ip_address(listener.address).is_loopback
                for listener in listeners
            ):
                raise ProbeFailure("non_loopback_listener")
            exact = [
                listener
                for listener in listeners
                if listener.port == runtime.port
                and listener.address == "127.0.0.1"
            ]
            if len(exact) != 1:
                raise ProbeFailure("loopback_listener_invalid")
            return
        except UnicodeError as error:
            raise ProbeFailure("listener_table_invalid") from error
        except ProbeFailure as error:
            if error.code not in {
                "proc_read_failed",
                "listener_inspection_failed",
            }:
                raise
        time.sleep(0.05)
    raise ProbeFailure("loopback_listener_missing")


def _recv_exact(connection: socket.socket, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = connection.recv(remaining)
        if not chunk:
            raise ProbeFailure("wire_response_truncated")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _send_wire_request(
    socket_path: Path,
    *,
    generation: int,
    timeout: float,
    prompt: str = _PROBE_PROMPT,
    backend_device: str = "",
) -> bool:
    prompt_bytes = prompt.encode("utf-8")
    backend_device_bytes = backend_device.encode("ascii", errors="strict")
    request = WIRE_REQUEST_HEADER.pack(
        WIRE_MAGIC,
        WIRE_VERSION,
        WIRE_REQUEST,
        generation,
        5000,
        16,
        len(prompt_bytes),
        len(backend_device_bytes),
    )
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(max(0.1, min(timeout, 7.0)))
    try:
        connection.connect(str(socket_path))
        connection.sendall(request)
        connection.sendall(prompt_bytes)
        connection.sendall(backend_device_bytes)
        raw_header = _recv_exact(connection, WIRE_RESPONSE_HEADER.size)
        (
            magic,
            version,
            kind,
            response_generation,
            status_code,
            _latency_msec,
            value_size,
            debug_size,
        ) = WIRE_RESPONSE_HEADER.unpack(raw_header)
        if (
            magic != WIRE_MAGIC
            or version != WIRE_VERSION
            or kind != WIRE_RESPONSE
            or response_generation != generation
        ):
            raise ProbeFailure("wire_response_invalid")
        if (
            value_size > MAX_WIRE_PAYLOAD_BYTES
            or debug_size > MAX_WIRE_PAYLOAD_BYTES
            or value_size + debug_size > MAX_WIRE_PAYLOAD_BYTES
        ):
            raise ProbeFailure("wire_response_too_large")
        value = _recv_exact(connection, value_size)
        _recv_exact(connection, debug_size)
        if status_code != WIRE_STATUS_OK:
            return False
        try:
            return bool(value.decode("utf-8", errors="strict").strip())
        except UnicodeError as error:
            raise ProbeFailure("wire_value_invalid") from error
    except (OSError, TimeoutError) as error:
        raise ProbeFailure("wire_io_failed") from error
    finally:
        connection.close()


def _wait_for_wire_success(
    scorer: subprocess.Popen[bytes],
    socket_path: Path,
    output: _BoundedOutput,
    deadline: float,
    *,
    backend_device: str = "",
) -> None:
    generation = 1
    while time.monotonic() < deadline:
        if scorer.poll() is not None:
            raise ProbeFailure("scorer_exited_early")
        if output.exceeded.is_set():
            raise ProbeFailure("scorer_output_too_large")
        remaining = deadline - time.monotonic()
        try:
            if _send_wire_request(
                socket_path,
                generation=generation,
                timeout=remaining,
                backend_device=backend_device,
            ):
                return
        except ProbeFailure as error:
            if error.code not in {"wire_io_failed"}:
                raise
        generation += 1
        time.sleep(0.1)
    raise ProbeFailure("wire_completion_timeout")


def _authenticated_completion(
    runtime: LlamaRuntime,
    *,
    timeout: float,
    prompt: str = _PROBE_PROMPT,
) -> None:
    body = json.dumps(
        {
            "prompt": prompt,
            "n_predict": 4,
            "temperature": 0.0,
            "stream": False,
        },
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    connection = http.client.HTTPConnection(
        "127.0.0.1", runtime.port, timeout=max(0.1, min(timeout, 15.0))
    )
    try:
        connection.request(
            "POST",
            "/completion",
            body=body,
            headers={
                "Authorization": "Bearer " + runtime.api_key,
                "Content-Type": "application/json; charset=utf-8",
                "Connection": "close",
            },
        )
        response = connection.getresponse()
        payload = response.read(MAX_HTTP_RESPONSE_BYTES + 1)
        if response.status != 200 or len(payload) > MAX_HTTP_RESPONSE_BYTES:
            raise ProbeFailure("authenticated_completion_failed")
        try:
            document = json.loads(payload.decode("utf-8", errors="strict"))
        except (UnicodeError, json.JSONDecodeError) as error:
            raise ProbeFailure("authenticated_completion_invalid") from error
        if not isinstance(document, dict):
            raise ProbeFailure("authenticated_completion_invalid")
        value = document.get("content")
        if not isinstance(value, str) or not value.strip():
            raise ProbeFailure("authenticated_completion_empty")
    except (OSError, TimeoutError, http.client.HTTPException) as error:
        raise ProbeFailure("authenticated_completion_failed") from error
    finally:
        connection.close()


def _validate_socket(path: Path) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise ProbeFailure("wire_socket_invalid") from error
    if (
        not stat.S_ISSOCK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & 0o077
    ):
        raise ProbeFailure("wire_socket_invalid")


def _same_process(runtime: LlamaRuntime) -> bool:
    proc = Path("/proc") / str(runtime.pid)
    if not proc.exists():
        return False
    try:
        return _proc_start_time(runtime.pid) == runtime.start_time
    except (ProbeFailure, OSError):
        # Failure to inspect a still-present PID is not proof of termination.
        return True


def _stop_runtime(
    scorer: subprocess.Popen[bytes],
    runtime: LlamaRuntime | None,
    output: _BoundedOutput,
) -> None:
    if scorer.poll() is None:
        try:
            scorer.send_signal(signal.SIGTERM)
            scorer.wait(timeout=8)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(scorer.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                scorer.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(scorer.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                scorer.wait(timeout=3)
    output.join()
    if scorer.stdout is not None:
        scorer.stdout.close()

    if runtime is not None:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline and _same_process(runtime):
            time.sleep(0.05)
        if _same_process(runtime):
            try:
                os.killpg(scorer.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline and _same_process(runtime):
                time.sleep(0.05)
        if _same_process(runtime):
            raise ProbeFailure("runtime_cleanup_failed")


def _stage_release_runtime(
    repo_root: Path,
    stage: Path,
    *,
    llama_server: Path = DEFAULT_LLAMA_SERVER,
    llama_server_sha256: str | None = None,
    layout: str = "archlinux-x86_64",
) -> None:
    source_root = repo_root / "src"
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith("LD_"):
            environment.pop(name)
    environment.pop("MOZKEY_LLAMA_ALLOW_UNTRUSTED_FOR_TESTS", None)
    environment.pop("MOZKEY_LLAMA_PROBE_TIMEOUT_SECONDS", None)
    environment.update(
        {
            "PREFIX": "/usr",
            "DESTDIR": str(stage),
            "MOZKEY_LINUX_BUILD_LAYOUT": layout,
            "MOZKEY_ZENZ_LLAMA_SERVER_TARGET": str(llama_server),
            "PATH": "/usr/bin:/bin",
            "TMPDIR": str(stage.parent),
        }
    )
    if llama_server != DEFAULT_LLAMA_SERVER:
        environment["MOZKEY_ZENZ_LLAMA_SERVER_SOURCE"] = str(llama_server)
        if llama_server_sha256 is None:
            raise ProbeFailure("llama_server_digest_invalid")
        environment["MOZKEY_LLAMA_ATTESTED_SHA256"] = llama_server_sha256
    _run_bounded_command(
        [
            str(repo_root / "scripts/verify_llama_server_compatibility"),
            str(llama_server),
        ],
        cwd=source_root,
        env=environment,
        timeout=20,
        failure_code="llama_preflight_failed",
    )
    _run_bounded_command(
        [str(repo_root / "scripts/install_server_bazel")],
        cwd=source_root,
        env=environment,
        timeout=120,
        failure_code="runtime_staging_failed",
    )


def run_probe(
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    *,
    llama_server: Path = DEFAULT_LLAMA_SERVER,
    llama_server_sha256: str | None = None,
    layout: str = "archlinux-x86_64",
) -> None:
    if not sys.platform.startswith("linux") or not Path("/proc/self").is_dir():
        raise ProbeFailure("linux_proc_required")
    if not 30.0 <= timeout_seconds <= 300.0:
        raise ProbeFailure("timeout_invalid")
    if layout not in {"ubuntu-layout", "fedora-x86_64", "archlinux-x86_64"}:
        raise ProbeFailure("layout_invalid")
    bundled_server = llama_server != DEFAULT_LLAMA_SERVER
    if bundled_server:
        try:
            server_info = llama_server.lstat()
        except OSError as error:
            raise ProbeFailure("llama_server_invalid") from error
        if (
            not stat.S_ISREG(server_info.st_mode)
            or server_info.st_size <= 0
            or not os.access(llama_server, os.X_OK)
        ):
            raise ProbeFailure("llama_server_invalid")
        llama_server = llama_server.resolve(strict=True)
        if (
            llama_server_sha256 is None
            or len(llama_server_sha256) != 64
            or any(character not in "0123456789abcdef" for character in llama_server_sha256)
        ):
            raise ProbeFailure("llama_server_digest_invalid")
        digest = hashlib.sha256(llama_server.read_bytes()).hexdigest()
        if digest != llama_server_sha256:
            raise ProbeFailure("llama_server_digest_invalid")
    elif llama_server_sha256 is not None:
        raise ProbeFailure("llama_server_digest_invalid")

    repo_root = Path(__file__).resolve().parents[2]
    with _probe_directories() as (stage, home):
        _stage_release_runtime(
            repo_root,
            stage,
            llama_server=llama_server,
            llama_server_sha256=llama_server_sha256,
            layout=layout,
        )
        scorer_path, model_path, llama_link = _validate_staged_runtime(
            stage, bundled_server=bundled_server
        )
        expected_executable = llama_link if bundled_server else DEFAULT_LLAMA_SERVER

        environment = os.environ.copy()
        environment["HOME"] = str(home)
        environment["TMPDIR"] = str(home)
        environment["PATH"] = "/usr/bin:/bin"
        for name in list(environment):
            if name.startswith("LD_"):
                environment.pop(name)
        # Release builds ignore these, but removing them makes this invariant
        # explicit and prevents a debug build from weakening the gate.
        for name in (
            "MOZC_ZENZ_LLAMA_SERVER",
            "MOZC_ZENZ_MODEL",
            "MOZC_ZENZ_CTX",
            "MOZC_ZENZ_THREADS",
            "MOZC_ZENZ_N_PREDICT",
            "MOZC_ZENZ_DIAGNOSTIC_JSONL",
            "MOZC_ZENZ_RUNTIME_SHA256",
            "MOZC_ZENZ_MODEL_SHA256",
            "MOZC_ZENZ_FLASH_ATTENTION",
        ):
            environment.pop(name, None)

        old_umask = os.umask(0o077)
        try:
            scorer = subprocess.Popen(
                [str(scorer_path)],
                cwd=home,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        except OSError as error:
            raise ProbeFailure("scorer_launch_failed") from error
        finally:
            os.umask(old_umask)

        assert scorer.stdout is not None
        output = _BoundedOutput(scorer.stdout)
        output.start()
        runtime: LlamaRuntime | None = None
        socket_path = home / ".mozkey-ibg_zenz_scorer_pipe"
        lock_path = home / ".mozkey-ibg_zenz_scorer.lock"
        deadline = time.monotonic() + timeout_seconds
        failure: BaseException | None = None
        try:
            while not socket_path.exists() and time.monotonic() < deadline:
                if scorer.poll() is not None:
                    raise ProbeFailure("scorer_exited_early")
                time.sleep(0.05)
            _validate_socket(socket_path)
            _require_staged_regular(
                lock_path, mode=0o600, executable=False, nonempty=False
            )
            _wait_for_wire_success(
                scorer,
                socket_path,
                output,
                deadline,
                backend_device=_MISSING_BACKEND_DEVICE,
            )
            if _send_wire_request(
                socket_path,
                generation=0x7FFFFFFF,
                timeout=max(0.1, deadline - time.monotonic()),
                backend_device="CUDA0 invalid",
            ):
                raise ProbeFailure("wire_device_validation_failed")
            runtime = _wait_for_llama_child(
                scorer,
                expected_link=llama_link,
                expected_model=model_path,
                expected_executable=expected_executable,
                expected_backend_device="none",
                deadline=deadline,
            )
            if not _send_wire_request(
                socket_path,
                generation=0x7FFFFFFE,
                timeout=max(0.1, deadline - time.monotonic()),
                backend_device=_MISSING_BACKEND_DEVICE,
            ):
                raise ProbeFailure("llama_device_fallback_not_reused")
            reused_runtime = _wait_for_llama_child(
                scorer,
                expected_link=llama_link,
                expected_model=model_path,
                expected_executable=expected_executable,
                expected_backend_device="none",
                deadline=deadline,
            )
            if reused_runtime != runtime:
                raise ProbeFailure("llama_device_fallback_restarted")
            _validate_loopback_listener(runtime, deadline)
            _authenticated_completion(
                runtime, timeout=max(0.1, deadline - time.monotonic())
            )
            if output.exceeded.is_set():
                raise ProbeFailure("scorer_output_too_large")
        except BaseException as error:
            failure = error
        try:
            _stop_runtime(scorer, runtime, output)
        except BaseException as error:
            if failure is None:
                failure = error
        for private_path in (socket_path, lock_path):
            try:
                private_path.unlink(missing_ok=True)
            except OSError as error:
                if failure is None:
                    failure = ProbeFailure("runtime_cleanup_failed")
                    failure.__cause__ = error
        if failure is not None:
            raise failure


def _parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Probe the staged Mozkey Linux Zenz release runtime"
    )
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="runtime readiness deadline (30-300 seconds; default: 120)",
    )
    parser.add_argument(
        "--llama-server",
        type=Path,
        default=DEFAULT_LLAMA_SERVER,
        help="verified llama-server to bundle into the staged runtime",
    )
    parser.add_argument(
        "--llama-server-sha256",
        help="attested lowercase SHA-256 for a bundled llama-server",
    )
    parser.add_argument(
        "--layout",
        choices=("ubuntu-layout", "fedora-x86_64", "archlinux-x86_64"),
        default="archlinux-x86_64",
        help="attested Linux build layout used for staging",
    )
    return parser.parse_args(list(argv))


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        run_probe(
            arguments.timeout_seconds,
            llama_server=arguments.llama_server,
            llama_server_sha256=arguments.llama_server_sha256,
            layout=arguments.layout,
        )
    except ProbeFailure as error:
        print(f"Zenz release runtime probe failed: {error.code}", file=sys.stderr)
        return 1
    except Exception:
        # Never let an unexpected exception render paths, model material, or
        # child process state into a release log.
        print("Zenz release runtime probe failed: unexpected_runtime_error", file=sys.stderr)
        return 1
    print(
        "PASS: staged Zenz release runtime loaded its model and completed "
        "private authenticated requests"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
