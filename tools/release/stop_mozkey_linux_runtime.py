#!/usr/bin/env python3
"""Stop only the installed Mozkey Linux runtime processes.

Run this as the desktop user after Fcitx has exited and before removing the
installed files.  The implementation deliberately has no basename, argv, or
``pkill`` fallback: every signalled process must retain the same UID, procfs
start time, and exact executable path through a pidfd revalidation.

The detached Zenz scorer is a product runtime root in its own right.  A
``llama-server`` is eligible only when a stable procfs snapshot proves that it
is a descendant of that exact scorer.  Unrelated llama.cpp processes are never
signalled.
"""

from __future__ import annotations

import dataclasses
import errno
import math
import os
import signal
import sys
import time
from pathlib import Path
from typing import Callable, Iterable, Protocol, Sequence


MOZKEY_SERVER = "/usr/lib/mozkey-ibg/mozc_server"
MOZKEY_SCORER = "/usr/lib/mozkey-ibg/mozc_zenz_scorer"
LLAMA_SERVER = "/usr/bin/llama-server"
BUNDLED_LLAMA_SERVER = "/usr/lib/mozkey-ibg/llama-server"
LLAMA_SERVERS = frozenset({LLAMA_SERVER, BUNDLED_LLAMA_SERVER})
RUNTIME_ROOTS = frozenset({MOZKEY_SERVER, MOZKEY_SCORER})
# Linux appends this to /proc/<pid>/exe once the executable file has been
# unlinked, which is what a package upgrade does to a runtime that is still
# running.
_REPLACED_EXECUTABLE_SUFFIX = " (deleted)"
# Linux TASK_COMM_LEN includes the trailing NUL, so an executable basename is
# truncated to at most 15 bytes.  These names are never sufficient authority
# to signal a process; they only prevent an unreadable runtime candidate from
# being mistaken for proof that no Mozkey runtime exists.
RUNTIME_ROOT_COMMS = frozenset(
    Path(executable).name.encode("ascii")[:15] for executable in RUNTIME_ROOTS
)

DEFAULT_TERM_TIMEOUT_MSEC = 5000
DEFAULT_KILL_TIMEOUT_MSEC = 2000
MAX_TERM_TIMEOUT_MSEC = 30000
MAX_KILL_TIMEOUT_MSEC = 10000
POLL_INTERVAL_SECONDS = 0.05
MAX_PROC_VALUE_BYTES = 1 << 16


class StopFailure(RuntimeError):
    """A failure whose code is safe and stable enough for release logs."""

    def __init__(self, code: str, *, exit_code: int = 1):
        super().__init__(code)
        self.code = code
        self.exit_code = exit_code


@dataclasses.dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    uids: tuple[int, int, int, int]
    ppid: int
    start_time: str
    exe: str


@dataclasses.dataclass(frozen=True)
class ForeignProcess:
    """A live PID whose UID no longer belongs to the requested user."""

    pid: int
    uids: tuple[int, int, int, int]
    ppid: int


ProcessObservation = ProcessIdentity | ForeignProcess


@dataclasses.dataclass(frozen=True)
class StopResult:
    no_runtime: bool
    forced: bool


@dataclasses.dataclass(frozen=True)
class CommandOptions:
    target_uid: int | None
    term_timeout_msec: int
    kill_timeout_msec: int
    help_requested: bool = False


class SignalBackend(Protocol):
    def open(self, pid: int) -> int: ...

    def send(self, handle: int, sig: int) -> None: ...

    def close(self, handle: int) -> None: ...


class PidfdSignalBackend:
    """Race-free signalling through Linux pidfds."""

    def __init__(self) -> None:
        if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
            raise StopFailure("pidfd_unavailable")

    def open(self, pid: int) -> int:
        return os.pidfd_open(pid, 0)

    def send(self, handle: int, sig: int) -> None:
        signal.pidfd_send_signal(handle, sig, None, 0)

    def close(self, handle: int) -> None:
        os.close(handle)


class _ProcessGone(Exception):
    pass


def _canonical_executable(executable: str) -> str:
    """Map a product executable replaced by an upgrade to its product path.

    Only the exact product paths qualify.  They live in root-owned directories,
    so a same-UID process can only show one of them if it executed the file
    root installed there; every other path, deleted or not, stays as reported
    and is never treated as a Mozkey runtime.
    """
    if executable.endswith(_REPLACED_EXECUTABLE_SUFFIX):
        original = executable[: -len(_REPLACED_EXECUTABLE_SUFFIX)]
        if original in RUNTIME_ROOTS or original in LLAMA_SERVERS:
            return original
    return executable


class Procfs:
    def __init__(self, root: Path = Path("/proc")) -> None:
        self.root = root

    def _read_bounded(self, path: Path) -> bytes:
        try:
            with path.open("rb") as stream:
                data = stream.read(MAX_PROC_VALUE_BYTES + 1)
        except (FileNotFoundError, ProcessLookupError, NotADirectoryError) as error:
            raise _ProcessGone from error
        except OSError as error:
            raise StopFailure("proc_identity_unreadable") from error
        if len(data) > MAX_PROC_VALUE_BYTES:
            raise StopFailure("proc_identity_invalid")
        return data

    @staticmethod
    def _parse_status(data: bytes) -> tuple[tuple[int, int, int, int], int]:
        try:
            text = data.decode("ascii", errors="strict")
        except UnicodeError as error:
            raise StopFailure("proc_identity_invalid") from error
        uid_values: tuple[int, int, int, int] | None = None
        ppid: int | None = None
        for line in text.splitlines():
            if line.startswith("Uid:"):
                fields = line.split()[1:]
                if len(fields) != 4 or any(not field.isdigit() for field in fields):
                    raise StopFailure("proc_identity_invalid")
                uid_values = (
                    int(fields[0]),
                    int(fields[1]),
                    int(fields[2]),
                    int(fields[3]),
                )
            elif line.startswith("PPid:"):
                fields = line.split()[1:]
                if len(fields) != 1 or not fields[0].isdigit():
                    raise StopFailure("proc_identity_invalid")
                ppid = int(fields[0])
        if uid_values is None or ppid is None:
            raise StopFailure("proc_identity_invalid")
        return uid_values, ppid

    @staticmethod
    def _parse_start_time(data: bytes) -> str:
        try:
            text = data.decode("ascii", errors="strict")
        except UnicodeError as error:
            raise StopFailure("proc_identity_invalid") from error
        closing_parenthesis = text.rfind(")")
        if closing_parenthesis < 0:
            raise StopFailure("proc_identity_invalid")
        tail = text[closing_parenthesis + 2 :].split()
        # tail[0] is field 3 (state), so field 22 (starttime) is tail[19].
        if len(tail) < 20 or not tail[19].isdigit():
            raise StopFailure("proc_identity_invalid")
        return tail[19]

    @staticmethod
    def _parse_comm(data: bytes) -> bytes:
        comm = data.removesuffix(b"\n")
        if (
            not comm
            or len(comm) > 15
            or b"\n" in comm
            or b"\x00" in comm
            or any(byte < 0x20 or byte > 0x7E for byte in comm)
        ):
            raise StopFailure("proc_identity_invalid")
        return comm

    def observe(
        self, pid: int, target_uid: int, *, strict: bool
    ) -> ProcessObservation | None:
        process_dir = self.root / str(pid)
        try:
            first_status = self._read_bounded(process_dir / "status")
            first_uids, first_ppid = self._parse_status(first_status)
            if any(uid != target_uid for uid in first_uids):
                return ForeignProcess(pid=pid, uids=first_uids, ppid=first_ppid)

            first_start = self._parse_start_time(
                self._read_bounded(process_dir / "stat")
            )
            try:
                executable = _canonical_executable(
                    os.readlink(process_dir / "exe")
                )
            except (FileNotFoundError, ProcessLookupError, NotADirectoryError) as error:
                raise _ProcessGone from error
            except PermissionError as error:
                # A normal desktop session contains same-UID non-dumpable
                # processes such as systemd --user, ssh-agent, and privileged
                # GVfs helpers.  They are not eligible for signalling because
                # their executable identity cannot be proven.  During broad
                # discovery, an unrelated protected process may be ignored,
                # but a process whose kernel comm matches a Mozkey runtime is
                # an unreadable candidate and must prevent a false
                # ``no_runtime`` result.  comm is deliberately never returned
                # as a ProcessIdentity and therefore can never authorize a
                # signal.
                if not strict:
                    try:
                        comm_data = self._read_bounded(process_dir / "comm")
                    except _ProcessGone:
                        # A vanished process is harmless.  If status remains
                        # readable, however, the identity is only partially
                        # hidden and discovery must fail closed.
                        self._read_bounded(process_dir / "status")
                        raise StopFailure("proc_identity_unreadable") from error
                    comm = self._parse_comm(comm_data)
                    if comm not in RUNTIME_ROOT_COMMS:
                        return None
                raise StopFailure("proc_identity_unreadable") from error
            except OSError as error:
                raise StopFailure("proc_identity_unreadable") from error

            second_status = self._read_bounded(process_dir / "status")
            second_uids, second_ppid = self._parse_status(second_status)
            second_start = self._parse_start_time(
                self._read_bounded(process_dir / "stat")
            )
        except _ProcessGone:
            return None

        if (
            first_uids != second_uids
            or first_ppid != second_ppid
            or first_start != second_start
        ):
            if strict:
                raise StopFailure("proc_identity_changed")
            return None
        if any(uid != target_uid for uid in second_uids):
            return ForeignProcess(pid=pid, uids=second_uids, ppid=second_ppid)
        return ProcessIdentity(
            pid=pid,
            uids=second_uids,
            ppid=second_ppid,
            start_time=second_start,
            exe=executable,
        )

    def scan(self, target_uid: int) -> list[ProcessIdentity]:
        try:
            with os.scandir(self.root) as entries:
                pids = sorted(
                    int(entry.name)
                    for entry in entries
                    if entry.name.isdigit() and int(entry.name) > 1
                )
        except OSError as error:
            raise StopFailure("proc_scan_failed") from error

        identities: list[ProcessIdentity] = []
        for pid in pids:
            observation = self.observe(pid, target_uid, strict=False)
            if isinstance(observation, ProcessIdentity):
                identities.append(observation)
        return identities


def _same_lifetime(expected: ProcessIdentity, observed: ProcessObservation) -> bool:
    return (
        isinstance(observed, ProcessIdentity)
        and observed.pid == expected.pid
        and observed.uids == expected.uids
        and observed.start_time == expected.start_time
        and observed.exe == expected.exe
    )


def _require_same_lifetime(
    expected: ProcessIdentity, observed: ProcessObservation
) -> ProcessIdentity:
    if not _same_lifetime(expected, observed):
        raise StopFailure("proc_identity_changed")
    assert isinstance(observed, ProcessIdentity)
    return observed


def _descendant_llamas(
    identities: Sequence[ProcessIdentity], scorer_pids: set[int]
) -> list[ProcessIdentity]:
    by_pid = {identity.pid: identity for identity in identities}
    output: list[ProcessIdentity] = []
    for identity in identities:
        if identity.exe not in LLAMA_SERVERS:
            continue
        seen: set[int] = set()
        ancestor_pid = identity.ppid
        while ancestor_pid > 1 and ancestor_pid not in seen:
            if ancestor_pid in scorer_pids:
                output.append(identity)
                break
            seen.add(ancestor_pid)
            parent = by_pid.get(ancestor_pid)
            if parent is None:
                break
            ancestor_pid = parent.ppid
    return output


def _deduplicate(identities: Iterable[ProcessIdentity]) -> list[ProcessIdentity]:
    unique: dict[tuple[int, str], ProcessIdentity] = {}
    for identity in identities:
        unique[(identity.pid, identity.start_time)] = identity
    return sorted(unique.values(), key=lambda identity: identity.pid)


def _signal_identity(
    procfs: Procfs,
    backend: SignalBackend,
    identity: ProcessIdentity,
    target_uid: int,
    sig: int,
) -> bool:
    try:
        handle = backend.open(identity.pid)
    except OSError as error:
        if error.errno == errno.ESRCH:
            return False
        if error.errno in {errno.EPERM, errno.EACCES}:
            raise StopFailure("signal_denied") from error
        raise StopFailure("pidfd_open_failed") from error

    try:
        observation = procfs.observe(identity.pid, target_uid, strict=True)
        if observation is None:
            return False
        _require_same_lifetime(identity, observation)
        try:
            backend.send(handle, sig)
        except OSError as error:
            if error.errno == errno.ESRCH:
                return False
            if error.errno in {errno.EPERM, errno.EACCES}:
                raise StopFailure("signal_denied") from error
            raise StopFailure("signal_failed") from error
        return True
    finally:
        try:
            backend.close(handle)
        except OSError as error:
            raise StopFailure("pidfd_close_failed") from error


def _remaining(
    procfs: Procfs,
    identities: Iterable[ProcessIdentity],
    target_uid: int,
) -> list[ProcessIdentity]:
    output: list[ProcessIdentity] = []
    for identity in _deduplicate(identities):
        observation = procfs.observe(identity.pid, target_uid, strict=True)
        if observation is None:
            continue
        _require_same_lifetime(identity, observation)
        output.append(identity)
    return output


def _wait_for_exit(
    procfs: Procfs,
    identities: Iterable[ProcessIdentity],
    target_uid: int,
    timeout_seconds: float,
    *,
    monotonic: Callable[[], float],
    sleep: Callable[[float], None],
) -> list[ProcessIdentity]:
    deadline = monotonic() + timeout_seconds
    while True:
        remaining = _remaining(procfs, identities, target_uid)
        if not remaining:
            return []
        now = monotonic()
        if now >= deadline:
            return remaining
        sleep(min(POLL_INTERVAL_SECONDS, deadline - now))


def stop_runtime(
    *,
    target_uid: int,
    term_timeout_seconds: float = DEFAULT_TERM_TIMEOUT_MSEC / 1000,
    kill_timeout_seconds: float = DEFAULT_KILL_TIMEOUT_MSEC / 1000,
    procfs: Procfs | None = None,
    backend: SignalBackend | None = None,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> StopResult:
    if target_uid <= 0:
        raise StopFailure("target_uid_invalid", exit_code=2)
    if (
        not math.isfinite(term_timeout_seconds)
        or not math.isfinite(kill_timeout_seconds)
        or term_timeout_seconds < 0
        or kill_timeout_seconds < 0
    ):
        raise StopFailure("timeout_invalid", exit_code=2)

    procfs = procfs or Procfs()
    backend = backend or PidfdSignalBackend()
    initial = procfs.scan(target_uid)
    roots = _deduplicate(
        identity for identity in initial if identity.exe in RUNTIME_ROOTS
    )
    if not roots:
        return StopResult(no_runtime=True, forced=False)

    scorer_pids = {
        identity.pid for identity in roots if identity.exe == MOZKEY_SCORER
    }
    descendant_llamas = _descendant_llamas(initial, scorer_pids)

    for identity in roots:
        _signal_identity(
            procfs, backend, identity, target_uid, signal.SIGTERM
        )

    tracked = _deduplicate([*roots, *descendant_llamas])
    remaining = _wait_for_exit(
        procfs,
        tracked,
        target_uid,
        term_timeout_seconds,
        monotonic=monotonic,
        sleep=sleep,
    )
    if not remaining:
        final_roots = [
            identity
            for identity in procfs.scan(target_uid)
            if identity.exe in RUNTIME_ROOTS
        ]
        if final_roots:
            raise StopFailure("runtime_respawned")
        return StopResult(no_runtime=False, forced=False)

    # A scorer may have completed its background llama launch while TERM was
    # pending.  Capture exact descendants one final time before forcing the
    # root; they remain eligible by immutable PID/starttime identity even if
    # SIGKILL reparents them to init.
    before_kill = procfs.scan(target_uid)
    live_root_pids = {
        identity.pid
        for identity in remaining
        if identity.exe == MOZKEY_SCORER
    }
    descendant_llamas = _deduplicate(
        [*descendant_llamas, *_descendant_llamas(before_kill, live_root_pids)]
    )

    remaining_roots = [
        identity for identity in remaining if identity.exe in RUNTIME_ROOTS
    ]
    forced = False
    for identity in remaining_roots:
        forced = (
            _signal_identity(
                procfs, backend, identity, target_uid, signal.SIGKILL
            )
            or forced
        )

    after_root_kill = _wait_for_exit(
        procfs,
        [*remaining_roots, *descendant_llamas],
        target_uid,
        kill_timeout_seconds,
        monotonic=monotonic,
        sleep=sleep,
    )
    remaining_llamas = [
        identity for identity in after_root_kill if identity.exe in LLAMA_SERVERS
    ]
    for identity in remaining_llamas:
        forced = (
            _signal_identity(
                procfs, backend, identity, target_uid, signal.SIGKILL
            )
            or forced
        )

    still_running = _wait_for_exit(
        procfs,
        [*remaining_roots, *remaining_llamas],
        target_uid,
        kill_timeout_seconds,
        monotonic=monotonic,
        sleep=sleep,
    )
    if still_running:
        raise StopFailure("runtime_stop_timeout")

    final_roots = [
        identity
        for identity in procfs.scan(target_uid)
        if identity.exe in RUNTIME_ROOTS
    ]
    if final_roots:
        raise StopFailure("runtime_respawned")
    return StopResult(no_runtime=False, forced=forced)


def _parse_nonnegative_integer(value: str, *, maximum: int, code: str) -> int:
    if not value.isdigit() or len(value) > 10:
        raise StopFailure(code, exit_code=2)
    parsed = int(value)
    if parsed > maximum:
        raise StopFailure(code, exit_code=2)
    return parsed


def _parse_arguments(argv: Sequence[str]) -> CommandOptions:
    target_uid: int | None = None
    term_timeout = DEFAULT_TERM_TIMEOUT_MSEC
    kill_timeout = DEFAULT_KILL_TIMEOUT_MSEC
    index = 0
    seen: set[str] = set()
    while index < len(argv):
        argument = argv[index]
        if argument in {"-h", "--help"}:
            if len(argv) != 1:
                raise StopFailure("invalid_arguments", exit_code=2)
            return CommandOptions(None, term_timeout, kill_timeout, True)
        if argument not in {
            "--uid",
            "--term-timeout-msec",
            "--kill-timeout-msec",
        }:
            raise StopFailure("invalid_arguments", exit_code=2)
        if argument in seen:
            raise StopFailure("invalid_arguments", exit_code=2)
        seen.add(argument)
        index += 1
        if index >= len(argv):
            raise StopFailure("invalid_arguments", exit_code=2)
        value = argv[index]
        if argument == "--uid":
            target_uid = _parse_nonnegative_integer(
                value, maximum=(1 << 31) - 1, code="target_uid_invalid"
            )
        elif argument == "--term-timeout-msec":
            term_timeout = _parse_nonnegative_integer(
                value,
                maximum=MAX_TERM_TIMEOUT_MSEC,
                code="timeout_invalid",
            )
        else:
            kill_timeout = _parse_nonnegative_integer(
                value,
                maximum=MAX_KILL_TIMEOUT_MSEC,
                code="timeout_invalid",
            )
        index += 1
    return CommandOptions(target_uid, term_timeout, kill_timeout)


def _resolve_target_uid(requested: int | None) -> int:
    effective_uid = os.geteuid()
    if requested is None:
        if effective_uid == 0:
            raise StopFailure("target_uid_required", exit_code=2)
        return effective_uid
    if requested <= 0:
        raise StopFailure("target_uid_invalid", exit_code=2)
    if effective_uid != 0 and requested != effective_uid:
        raise StopFailure("target_uid_not_permitted", exit_code=2)
    return requested


def _validate_live_layout_environment() -> None:
    if os.environ.get("DESTDIR", ""):
        raise StopFailure("staged_install_root", exit_code=2)
    if os.environ.get("PREFIX", "/usr") != "/usr":
        raise StopFailure("unsupported_prefix", exit_code=2)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    try:
        options = _parse_arguments(arguments)
        if options.help_requested:
            print(
                "usage: stop_mozkey_linux_runtime [--uid UID] "
                "[--term-timeout-msec N] [--kill-timeout-msec N]"
            )
            return 0
        if not sys.platform.startswith("linux"):
            raise StopFailure("unsupported_platform", exit_code=2)
        _validate_live_layout_environment()
        target_uid = _resolve_target_uid(options.target_uid)
        result = stop_runtime(
            target_uid=target_uid,
            term_timeout_seconds=options.term_timeout_msec / 1000,
            kill_timeout_seconds=options.kill_timeout_msec / 1000,
        )
        state = (
            "no_runtime"
            if result.no_runtime
            else "forced"
            if result.forced
            else "stopped"
        )
        print(f"Mozkey runtime stop complete: {state}")
        return 0
    except StopFailure as error:
        print(f"Mozkey runtime stop failed: {error.code}", file=sys.stderr)
        return error.exit_code
    except KeyboardInterrupt:
        print("Mozkey runtime stop failed: interrupted", file=sys.stderr)
        return 1
    except Exception:
        print(
            "Mozkey runtime stop failed: unexpected_runtime_error",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
