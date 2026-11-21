#!/usr/bin/env python3
"""CI-only macOS PR3 packaged scorer SIGKILL and noninterference test.

Run on a fresh, dedicated GitHub-hosted macOS runner.  Never invoke against an
installed IME or an existing per-UID Zenz namespace.  All signals use verified
PIDs spawned by this test.  The formal PKG is expanded, never installed.
"""

import argparse
import ctypes
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

MODEL_SHA = "29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673"
MODEL_NAME = "zenz-v3.2-small-Q5_K_M.gguf"
MAX_READY = 240

# macOS process identity must be verified using the kernel's executable path,
# not ps(1)'s argv display, which may vary in spelling or representation.
# Do not weaken the ownership requirement: both inode identity and lstart must
# match before any signal is sent to a PID discovered via process ancestry.
if sys.platform == "darwin":
    _libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
    _proc_pidpath = _libproc.proc_pidpath
    _proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    _proc_pidpath.restype = ctypes.c_int
else:
    _proc_pidpath = None


def executable_path(pid):
    if _proc_pidpath is None:
        return ""
    buf = ctypes.create_string_buffer(4096)
    n = _proc_pidpath(pid, ctypes.cast(buf, ctypes.c_void_p), len(buf))
    return os.fsdecode(buf.value) if n > 0 else ""


def matches_executable(pid, expected):
    actual = executable_path(pid)
    if not actual:
        return False
    try:
        return os.path.samefile(actual, expected)
    except OSError:
        return False


def need(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def run(args, *, timeout=90):
    p = subprocess.run([str(x) for x in args], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True, errors="replace", timeout=timeout)
    need(p.returncode == 0, "COMMAND_FAILED " + str(args[0]) + " rc=" + str(p.returncode))
    return p.stdout.strip()


def sha256(file):
    h = hashlib.sha256()
    with open(file, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def ps_value(pid, field):
    p = subprocess.run(["/bin/ps", "-ww", "-p", str(pid), "-o", field + "="],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       text=True, timeout=10)
    return p.stdout.strip() if p.returncode == 0 else ""


def snapshot(pid, executable):
    expected = str(executable)
    actual = executable_path(pid)
    need(bool(actual), "PROCESS_EXECUTABLE_UNAVAILABLE pid=" + str(pid))
    need(matches_executable(pid, expected),
         "UNEXPECTED_PROCESS_EXECUTABLE pid=" + str(pid) +
         " actual=" + actual + " expected=" + expected)
    start = ps_value(pid, "lstart")
    need(bool(start), "PROCESS_START_TIME_MISSING")
    need(ps_value(pid, "state").upper()[:1] != "Z", "PROCESS_ZOMBIE")
    return {"pid": pid, "start": start, "exe": expected}


def alive(ref):
    if not ref:
        return False
    pid = ref["pid"]
    if ps_value(pid, "lstart") != ref["start"]:
        return False
    if not matches_executable(pid, ref["exe"]):
        return False
    state = ps_value(pid, "state")
    return bool(state) and not state.upper().startswith("Z")


def wait_gone(ref, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if not alive(ref):
            return True
        time.sleep(0.15)
    return not alive(ref)


def children(ppid):
    rows = run(["/bin/ps", "-axo", "pid=,ppid="], timeout=15)
    result = []
    for row in rows.splitlines():
        fields = row.split()
        if len(fields) == 2 and all(t.isdigit() for t in fields):
            if int(fields[1]) == ppid:
                result.append(int(fields[0]))
    return result


def wait_child(ppid, exe, seconds=15, parent_proc=None):
    deadline = time.monotonic() + seconds
    mismatches = []
    while time.monotonic() < deadline:
        if parent_proc is not None:
            need(parent_proc.poll() is None,
                 "SCORER_EXITED_WHILE_FINDING_CHILD rc=" + str(parent_proc.returncode))
        found = []
        for pid in children(ppid):
            try:
                ref = snapshot(pid, exe)
                if alive(ref) and ps_value(pid, "ppid") == str(ppid):
                    found.append(ref)
            except RuntimeError as exc:
                if len(mismatches) < 6:
                    mismatches.append(str(exc))
        need(len(found) <= 1, "MULTIPLE_EXPECTED_CHILDREN")
        if found:
            return found[0]
        time.sleep(0.2)
    raise RuntimeError("EXPECTED_CHILD_NOT_FOUND " + exe.name +
                       " parent=" + str(ppid) +
                       " observed_child_count=" + str(len(children(ppid))) +
                       " mismatches=" + repr(mismatches))


def ensure_absent_namespace():
    directory = Path("/tmp/mozkey-ibg_zenz_" + str(os.geteuid()))
    need(not os.path.lexists(directory),
         "EXISTING_ZENZ_NAMESPACE_REFUSE_TO_TOUCH " + str(directory))
    return directory


def tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def health(port, key):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request("GET", "/health", headers={"Authorization": "Bearer " + key})
        answer = connection.getresponse()
        body = answer.read(2048)
        return answer.status == 200 and json.loads(body).get("status") == "ok"
    finally:
        connection.close()


def wait_healthy(proc, port, key, seconds=MAX_READY):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        need(proc.poll() is None, "UNRELATED_LLAMA_SERVER_EXITED_EARLY")
        try:
            if health(port, key):
                return
        except (OSError, ValueError, json.JSONDecodeError):
            pass
        time.sleep(0.5)
    raise RuntimeError("UNRELATED_WORKER_HEALTH_TIMEOUT")


def wait_socket(proc, path, seconds=MAX_READY):
    # Do not connect here. A connect-and-close without a Zenz wire request
    # can race accepted-socket configuration on macOS and terminate the scorer
    # (client_configure_failed_errno_22). Real authenticated wire inference is
    # exercised independently by verify_release_package.zsh on each host.
    # This gate only establishes that our new scorer created its private socket.
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        need(proc.poll() is None, "REAL_SCORER_EXITED_BEFORE_READY")
        try:
            info = path.lstat()
        except FileNotFoundError:
            time.sleep(0.35)
            continue
        need(stat.S_ISSOCK(info.st_mode), "SCORER_SOCKET_IS_NOT_UNIX_SOCKET")
        need(info.st_uid == os.geteuid(), "SCORER_SOCKET_OWNER_MISMATCH")
        need((stat.S_IMODE(info.st_mode) & 0o077) == 0,
             "SCORER_SOCKET_PERMISSIONS_NOT_PRIVATE")
        parent_info = path.parent.lstat()
        need(stat.S_ISDIR(parent_info.st_mode) and
             parent_info.st_uid == os.geteuid(),
             "SCORER_SOCKET_DIRECTORY_OWNER_OR_TYPE_MISMATCH")
        need((stat.S_IMODE(parent_info.st_mode) & 0o077) == 0,
             "SCORER_SOCKET_DIRECTORY_PERMISSIONS_NOT_PRIVATE")
        need(proc.poll() is None, "REAL_SCORER_EXITED_AFTER_SOCKET_CREATION")
        return
    raise RuntimeError("REAL_SCORER_SOCKET_TIMEOUT")


def terminate_owned(ref, signum):
    if alive(ref):
        os.kill(ref["pid"], signum)


def clean_owned(ref):
    if not alive(ref):
        return
    terminate_owned(ref, signal.SIGTERM)
    if not wait_gone(ref, 5):
        terminate_owned(ref, signal.SIGKILL)
    need(wait_gone(ref, 6), "TEST_OWNED_CHILD_CLEANUP_FAILED")


def verify_package(pkg, expected_sha, parent):
    need(pkg.is_file() and not pkg.is_symlink(), "PKG_NOT_REGULAR_FILE")
    need(sha256(pkg) == expected_sha, "PKG_SHA256_MISMATCH")
    expanded = parent / "expanded"
    run(["/usr/sbin/pkgutil", "--expand-full", str(pkg), str(expanded)], timeout=180)
    found = list(expanded.rglob("MozkeyIbG.app"))
    need(len(found) == 1 and found[0].is_dir(), "EXPECTED_ONE_MOZC_APP")
    runtime = found[0] / "Contents/Resources/ZenzRuntime"
    for name in ("mozc_zenz_scorer", "mozc_zenz_guard", "llama-server", "models/" + MODEL_NAME):
        f = runtime / name
        need(f.is_file() and not f.is_symlink(), "PKG_RUNTIME_MISSING " + name)
    need(sha256(runtime / "models" / MODEL_NAME) == MODEL_SHA, "PINNED_MODEL_CHANGED")
    for name in ("mozc_zenz_scorer", "mozc_zenz_guard", "llama-server"):
        run(["/usr/bin/codesign", "--verify", "--strict", str(runtime / name)], timeout=30)
    return runtime


def checked_runtime_copy(runtime, parent):
    folder = parent / "t"  # no spaces: allows exact process identity checks
    folder.mkdir(mode=0o700)
    (folder / "models").mkdir(mode=0o700)
    for name in ("mozc_zenz_scorer", "mozc_zenz_guard", "llama-server"):
        src, dst = runtime / name, folder / name
        shutil.copy2(src, dst)
        os.chmod(dst, 0o700)
        need(sha256(src) == sha256(dst), "RUNTIME_BINARY_COPY_MISMATCH " + name)
        run(["/usr/bin/codesign", "--verify", "--strict", str(dst)], timeout=30)
    # The GGUF stays in the verified extracted package, with no mutable copy.
    (folder / "models" / MODEL_NAME).symlink_to(runtime / "models" / MODEL_NAME)
    return folder


def scrub_diagnostics(text):
    # CI logs are public. Never print token-bearing argv or raw token strings.
    text = re.sub(r"(?i)(--api-key(?:=|\s+))\S+", r"\1[REDACTED]", text)
    text = re.sub(r"(?i)(bearer\s+)\S+", r"\1[REDACTED]", text)
    text = re.sub(r"\b[0-9a-fA-F]{32,}\b", "[HEX_REDACTED]", text)
    return text


def failure_diagnostics(scorer, scorer_ref, guard_ref, worker_ref, root):
    info = {"scorer_exit_code": scorer.poll() if scorer else None}
    if scorer_ref:
        pid = scorer_ref["pid"]
        info["scorer_process"] = {
            "pid": pid, "alive": alive(scorer_ref),
            "kernel_executable": executable_path(pid),
            "start_time": ps_value(pid, "lstart"),
            "state": ps_value(pid, "state")}
        info["scorer_children"] = [
            {"pid": child, "kernel_executable": executable_path(child),
             "start_time": ps_value(child, "lstart"),
             "state": ps_value(child, "state")}
            for child in children(pid)][:12]
    for label, ref in (("guard", guard_ref), ("worker", worker_ref)):
        if ref:
            info[label] = {"pid": ref["pid"], "alive": alive(ref),
                           "kernel_executable": executable_path(ref["pid"]),
                           "ppid": ps_value(ref["pid"], "ppid")}
    log = root / "scorer.log"
    if log.is_file():
        chunk = log.read_bytes()[-16384:].decode("utf-8", "replace")
        lines = [line for line in chunk.splitlines()
                 if any(key in line.lower() for key in
                        ("error", "fail", "exit", "guard", "worker", "start", "kill"))]
        info["scorer_log_filtered"] = scrub_diagnostics("\n".join(lines[-45:]))[-7000:]
    return info


def test(pkg, expected_sha, root):
    namespace = ensure_absent_namespace()
    need(os.geteuid() != 0, "DO_NOT_RUN_AS_ROOT")
    result = {"schema": "pr3-macos-real-scorer-sigkill-ci-v1", "passed": False,
              "pkg_sha256": expected_sha, "architecture": os.uname().machine,
              "checks": {}, "namespace": str(namespace)}
    scorer = foreign = None
    scorer_ref = guard_ref = worker_ref = foreign_ref = None
    failures = []
    try:
        runtime = verify_package(pkg, expected_sha, root)
        testing = checked_runtime_copy(runtime, root)
        model = testing / "models" / MODEL_NAME
        worker = testing / "llama-server"
        guard = testing / "mozc_zenz_guard"
        scorer_path = testing / "mozc_zenz_scorer"
        outsider_port = tcp_port()
        # This non-secret key only protects an isolated CI-local health endpoint.
        outsider_key = "0123456789abcdef" * 4
        with (root / "unrelated-worker.log").open("wb") as log:
            foreign = subprocess.Popen(
                [str(worker), "-m", str(model), "-c", "256", "-t", "4",
                 "--cache-ram", "0", "--host", "127.0.0.1", "--port",
                 str(outsider_port), "--api-key", outsider_key],
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                cwd=testing, start_new_session=True)
        foreign_ref = snapshot(foreign.pid, worker)
        wait_healthy(foreign, outsider_port, outsider_key)
        print("UNRELATED_WORKER_READY=PASS pid=" + str(foreign.pid), flush=True)
        result["checks"]["unrelated_worker_pre_sigkill"] = "PASS"
        ensure_absent_namespace()
        with (root / "scorer.log").open("wb") as log:
            scorer = subprocess.Popen([str(scorer_path)], stdin=subprocess.DEVNULL,
                                      stdout=log, stderr=subprocess.STDOUT,
                                      cwd=testing, start_new_session=True)
        scorer_ref = snapshot(scorer.pid, scorer_path)
        wait_socket(scorer, namespace / "scorer.sock")
        guard_ref = wait_child(scorer.pid, guard, parent_proc=scorer)
        worker_ref = wait_child(guard_ref["pid"], worker, parent_proc=scorer)
        need(alive(guard_ref) and alive(worker_ref), "OWNED_CHAIN_NOT_READY")
        print("SCORER_GUARD_WORKER_CHAIN=PASS scorer_pid={} guard_pid={} worker_pid={}".format(
            scorer.pid, guard_ref["pid"], worker_ref["pid"]), flush=True)
        result["checks"]["real_scorer_guard_worker"] = "PASS"
        # Validate the unrelated server again with both live worker trees.
        need(health(outsider_port, outsider_key), "UNRELATED_WORKER_HEALTH_PRE_KILL_FAILED")
        need(scorer.poll() is None, "SCORER_EXITED_BEFORE_SIGKILL")
        need(alive(guard_ref) and alive(worker_ref), "OWNED_CHAIN_EXITED_BEFORE_SIGKILL")
        now = time.monotonic()
        scorer.kill()  # Only our directly Popen-owned scorer PID.
        need(scorer.wait(timeout=12) == -signal.SIGKILL, "SCORER_DID_NOT_EXIT_BY_SIGKILL")
        need(wait_gone(guard_ref, 15), "GUARD_SURVIVED_SCORER_SIGKILL")
        need(wait_gone(worker_ref, 15), "OWNED_WORKER_SURVIVED_SCORER_SIGKILL")
        elapsed = round(time.monotonic() - now, 3)
        need(alive(foreign_ref) and foreign.poll() is None,
             "UNRELATED_WORKER_DIED_WITH_SCORER")
        need(health(outsider_port, outsider_key),
             "UNRELATED_WORKER_UNHEALTHY_AFTER_SCORER_SIGKILL")
        print("SCORER_SIGKILL=PASS owned_worker_exited=YES seconds=" + str(elapsed), flush=True)
        print("UNRELATED_WORKER_NONINTERFERENCE=PASS alive_and_http_healthy=YES", flush=True)
        result["checks"]["real_scorer_sigkill"] = "PASS"
        result["checks"]["unrelated_worker_after_sigkill"] = "PASS"
        result["scorer_sigkill_elapsed_seconds"] = elapsed
        result["passed"] = True
    except BaseException as exc:
        failures.append(type(exc).__name__ + ":" + str(exc))
        try:
            diag = failure_diagnostics(scorer, scorer_ref, guard_ref, worker_ref, root)
            result["diagnostics"] = diag
            print("STAGE4_DIAGNOSTICS=" + json.dumps(diag, ensure_ascii=False), flush=True)
        except BaseException as dexc:
            failures.append("diagnostics_failed:" + type(dexc).__name__ + ":" + str(dexc))
    finally:
        if scorer is not None and scorer.poll() is None:
            scorer.terminate()
            try:
                scorer.wait(timeout=6)
            except subprocess.TimeoutExpired:
                scorer.kill()
                scorer.wait(timeout=8)
        # Guard/worker identities were captured as direct descendants of the
        # test-owned scorer. They are rechecked before any signal.
        for ref in (guard_ref, worker_ref):
            try:
                clean_owned(ref)
            except BaseException as cleanup_exc:
                failures.append("owned_cleanup:" + str(cleanup_exc))
        if foreign is not None:
            if foreign.poll() is None:
                foreign.terminate()
                try:
                    foreign.wait(timeout=7)
                except subprocess.TimeoutExpired:
                    foreign.kill()
                    foreign.wait(timeout=8)
            if foreign_ref and alive(foreign_ref):
                failures.append("unrelated_worker_still_alive_after_cleanup")
        # In this dedicated runner, keep the namespace untouched for disposal
        # with the VM; never guess ownership of a stale socket or lock file.
        result["namespace_cleanup"] = "DEDICATED_EPHEMERAL_RUNNER_NO_UNLINK"
        if failures:
            result["passed"] = False
            result["failures"] = failures
        (root / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    need(result["passed"], "SIGKILL_NONINTERFERENCE_TEST_FAILED: " + ";".join(failures))
    print("PR3_REAL_SCORER_SIGKILL_NONINTERFERENCE=PASS arch=" + os.uname().machine, flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--package", required=True)
    p.add_argument("--expected-sha256", required=True)
    p.add_argument("--output-root", required=True)
    args = p.parse_args()
    need(sys.platform == "darwin", "MACOS_ONLY")
    need(os.uname().machine in {"arm64", "x86_64"}, "UNKNOWN_MAC_ARCH")
    expected = args.expected_sha256.lower()
    need(len(expected) == 64 and all(x in "0123456789abcdef" for x in expected),
         "EXPECTED_SHA256_INVALID")
    output = Path(args.output_root).absolute()
    need(output.parent.is_dir() and not output.parent.is_symlink(), "OUTPUT_PARENT_INVALID")
    need(not os.path.lexists(output), "OUTPUT_ALREADY_EXISTS")
    output.mkdir(mode=0o700)
    print("PR3_REAL_SCORER_SIGKILL_CI_START arch=" + os.uname().machine, flush=True)
    test(Path(args.package).absolute(), expected, output)


if __name__ == "__main__":
    try:
        main()
    except BaseException as error:
        print("PR3_REAL_SCORER_SIGKILL_CI=FAIL " + str(error), flush=True)
        raise SystemExit(1)
