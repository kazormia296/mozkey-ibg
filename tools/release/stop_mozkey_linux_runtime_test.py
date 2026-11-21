import io
import os
import signal
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

from tools.release import stop_mozkey_linux_runtime as stopper


def _stat_line(pid: int, ppid: int, start_time: int) -> str:
    tail = ["S", str(ppid), *(["0"] * 17), str(start_time), "0"]
    return f"{pid} (fake process) " + " ".join(tail) + "\n"


class FakeProcTree:
    def __init__(self, root: Path) -> None:
        self.root = root

    def add(
        self,
        pid: int,
        executable: str,
        *,
        uid: int = 1000,
        ppid: int = 1,
        start_time: int = 10,
    ) -> None:
        process = self.root / str(pid)
        process.mkdir(parents=True, exist_ok=True)
        (process / "status").write_text(
            f"Name:\tfake\nPPid:\t{ppid}\nUid:\t{uid}\t{uid}\t{uid}\t{uid}\n",
            encoding="ascii",
        )
        (process / "stat").write_text(
            _stat_line(pid, ppid, start_time), encoding="ascii"
        )
        (process / "comm").write_bytes(
            Path(executable).name.encode("ascii")[:15] + b"\n"
        )
        (process / "exe").symlink_to(executable)

    def remove(self, pid: int) -> None:
        process = self.root / str(pid)
        if not process.exists():
            return
        for child in process.iterdir():
            child.unlink()
        process.rmdir()

    def replace(
        self,
        pid: int,
        executable: str,
        *,
        uid: int = 1000,
        ppid: int = 1,
        start_time: int = 99,
    ) -> None:
        self.remove(pid)
        self.add(
            pid,
            executable,
            uid=uid,
            ppid=ppid,
            start_time=start_time,
        )


class FakePidfdBackend:
    def __init__(self, on_open=None, on_send=None) -> None:
        self.on_open = on_open
        self.on_send = on_send
        self.signals: list[tuple[int, int]] = []
        self.closed: list[int] = []

    def open(self, pid: int) -> int:
        if self.on_open is not None:
            self.on_open(pid)
        return pid + 100000

    def send(self, handle: int, sig: int) -> None:
        pid = handle - 100000
        self.signals.append((pid, sig))
        if self.on_send is not None:
            self.on_send(pid, sig)

    def close(self, handle: int) -> None:
        self.closed.append(handle)


class StopMozkeyLinuxRuntimeTest(unittest.TestCase):
    def test_discovers_only_exact_roots_and_scorer_descendant_llama(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(10, stopper.MOZKEY_SERVER, start_time=101)
            tree.add(20, stopper.MOZKEY_SCORER, start_time=102)
            tree.add(21, stopper.LLAMA_SERVER, ppid=20, start_time=103)
            tree.add(22, stopper.LLAMA_SERVER, ppid=1, start_time=104)
            tree.add(23, stopper.MOZKEY_SERVER + ".backup", start_time=105)
            identities = stopper.Procfs(tree.root).scan(1000)
            roots = [item for item in identities if item.exe in stopper.RUNTIME_ROOTS]
            llamas = stopper._descendant_llamas(identities, {20})
            self.assertEqual([item.pid for item in roots], [10, 20])
            self.assertEqual([item.pid for item in llamas], [21])

    def test_discovers_runtime_whose_executable_an_upgrade_replaced(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(10, stopper.MOZKEY_SERVER + " (deleted)", start_time=101)
            tree.add(20, stopper.MOZKEY_SCORER + " (deleted)", start_time=102)
            tree.add(21, stopper.LLAMA_SERVER + " (deleted)", ppid=20,
                     start_time=103)
            tree.add(22, "/tmp/mozc_server (deleted)", start_time=104)
            tree.add(23, stopper.MOZKEY_SERVER + ".backup (deleted)",
                     start_time=105)
            identities = stopper.Procfs(tree.root).scan(1000)
            roots = [item for item in identities if item.exe in stopper.RUNTIME_ROOTS]
            llamas = stopper._descendant_llamas(identities, {20})
            self.assertEqual([item.pid for item in roots], [10, 20])
            self.assertEqual([item.pid for item in llamas], [21])

    def test_discovers_bundled_llama_only_as_scorer_descendant(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(20, stopper.MOZKEY_SCORER, start_time=102)
            tree.add(21, stopper.BUNDLED_LLAMA_SERVER, ppid=20, start_time=103)
            tree.add(22, stopper.BUNDLED_LLAMA_SERVER, ppid=1, start_time=104)
            identities = stopper.Procfs(tree.root).scan(1000)
            llamas = stopper._descendant_llamas(identities, {20})
            self.assertEqual([item.pid for item in llamas], [21])

    def test_scan_skips_unreadable_same_uid_but_strict_check_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(10, stopper.MOZKEY_SERVER, start_time=101)
            tree.add(20, "/usr/lib/systemd/systemd", start_time=102)
            real_readlink = os.readlink

            def readlink(path):
                if Path(path).parent.name == "20":
                    raise PermissionError("protected same-UID process")
                return real_readlink(path)

            procfs = stopper.Procfs(tree.root)
            with mock.patch.object(stopper.os, "readlink", side_effect=readlink):
                identities = procfs.scan(1000)
                self.assertEqual([item.pid for item in identities], [10])
                with self.assertRaisesRegex(
                    stopper.StopFailure, "proc_identity_unreadable"
                ):
                    procfs.observe(20, 1000, strict=True)

    def test_scan_fails_closed_for_unreadable_runtime_candidate(self):
        for executable in [stopper.MOZKEY_SERVER, stopper.MOZKEY_SCORER]:
            with self.subTest(executable=executable):
                with tempfile.TemporaryDirectory() as temporary:
                    tree = FakeProcTree(Path(temporary))
                    tree.add(20, executable, start_time=102)

                    with mock.patch.object(
                        stopper.os,
                        "readlink",
                        side_effect=PermissionError("protected runtime"),
                    ):
                        with self.assertRaisesRegex(
                            stopper.StopFailure, "proc_identity_unreadable"
                        ):
                            stopper.Procfs(tree.root).scan(1000)

    def test_unreadable_comm_fails_closed_when_executable_is_unreadable(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(20, "/usr/lib/systemd/systemd", start_time=102)
            (tree.root / "20" / "comm").unlink()

            with mock.patch.object(
                stopper.os,
                "readlink",
                side_effect=PermissionError("protected process"),
            ):
                with self.assertRaisesRegex(
                    stopper.StopFailure, "proc_identity_unreadable"
                ):
                    stopper.Procfs(tree.root).scan(1000)

    def test_graceful_stop_signals_roots_but_not_unrelated_llama(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(10, stopper.MOZKEY_SERVER)
            tree.add(20, stopper.MOZKEY_SCORER)
            tree.add(21, stopper.LLAMA_SERVER, ppid=20)
            tree.add(22, stopper.LLAMA_SERVER, ppid=1)

            def on_send(pid: int, sig: int) -> None:
                self.assertEqual(sig, signal.SIGTERM)
                tree.remove(pid)
                if pid == 20:
                    tree.remove(21)

            backend = FakePidfdBackend(on_send=on_send)
            result = stopper.stop_runtime(
                target_uid=1000,
                procfs=stopper.Procfs(tree.root),
                backend=backend,
                term_timeout_seconds=0,
                kill_timeout_seconds=0,
            )
            self.assertEqual(
                backend.signals,
                [(10, signal.SIGTERM), (20, signal.SIGTERM)],
            )
            self.assertTrue((tree.root / "22").exists())
            self.assertEqual(result, stopper.StopResult(False, False))

    def test_forced_stop_kills_only_captured_scorer_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(20, stopper.MOZKEY_SCORER)
            tree.add(21, stopper.LLAMA_SERVER, ppid=20)
            tree.add(22, stopper.LLAMA_SERVER, ppid=1)

            def on_send(pid: int, sig: int) -> None:
                if sig == signal.SIGKILL:
                    tree.remove(pid)

            backend = FakePidfdBackend(on_send=on_send)
            result = stopper.stop_runtime(
                target_uid=1000,
                procfs=stopper.Procfs(tree.root),
                backend=backend,
                term_timeout_seconds=0,
                kill_timeout_seconds=0,
            )
            self.assertEqual(
                backend.signals,
                [
                    (20, signal.SIGTERM),
                    (20, signal.SIGKILL),
                    (21, signal.SIGKILL),
                ],
            )
            self.assertTrue((tree.root / "22").exists())
            self.assertEqual(result, stopper.StopResult(False, True))

    def test_revalidates_start_time_before_first_signal(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(10, stopper.MOZKEY_SERVER, start_time=10)

            def on_open(pid: int) -> None:
                tree.replace(pid, stopper.MOZKEY_SERVER, start_time=11)

            backend = FakePidfdBackend(on_open=on_open)
            with self.assertRaisesRegex(stopper.StopFailure, "proc_identity_changed"):
                stopper.stop_runtime(
                    target_uid=1000,
                    procfs=stopper.Procfs(tree.root),
                    backend=backend,
                    term_timeout_seconds=0,
                    kill_timeout_seconds=0,
                )
            self.assertEqual(backend.signals, [])

    def test_revalidates_uid_and_exact_executable_before_first_signal(self):
        replacements = [
            (stopper.MOZKEY_SERVER, 1001),
            (stopper.MOZKEY_SERVER + ".replacement", 1000),
        ]
        for executable, uid in replacements:
            with self.subTest(executable=executable, uid=uid):
                with tempfile.TemporaryDirectory() as temporary:
                    tree = FakeProcTree(Path(temporary))
                    tree.add(10, stopper.MOZKEY_SERVER, start_time=10)

                    def on_open(pid: int) -> None:
                        tree.replace(
                            pid,
                            executable,
                            uid=uid,
                            start_time=10,
                        )

                    backend = FakePidfdBackend(on_open=on_open)
                    with self.assertRaisesRegex(
                        stopper.StopFailure, "proc_identity_changed"
                    ):
                        stopper.stop_runtime(
                            target_uid=1000,
                            procfs=stopper.Procfs(tree.root),
                            backend=backend,
                            term_timeout_seconds=0,
                            kill_timeout_seconds=0,
                        )
                    self.assertEqual(backend.signals, [])

    def test_never_kills_a_reused_pid_after_term(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(10, stopper.MOZKEY_SERVER, start_time=10)

            def on_send(pid: int, sig: int) -> None:
                if sig == signal.SIGTERM:
                    tree.replace(pid, stopper.MOZKEY_SERVER, start_time=11)

            backend = FakePidfdBackend(on_send=on_send)
            with self.assertRaisesRegex(stopper.StopFailure, "proc_identity_changed"):
                stopper.stop_runtime(
                    target_uid=1000,
                    procfs=stopper.Procfs(tree.root),
                    backend=backend,
                    term_timeout_seconds=0,
                    kill_timeout_seconds=0,
                )
            self.assertEqual(backend.signals, [(10, signal.SIGTERM)])

    def test_no_runtime_is_a_successful_noop(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = FakeProcTree(Path(temporary))
            tree.add(10, stopper.LLAMA_SERVER)
            backend = FakePidfdBackend()
            result = stopper.stop_runtime(
                target_uid=1000,
                procfs=stopper.Procfs(tree.root),
                backend=backend,
                term_timeout_seconds=0,
                kill_timeout_seconds=0,
            )
            self.assertEqual(result, stopper.StopResult(True, False))
            self.assertEqual(backend.signals, [])

    def test_staged_root_is_refused_before_runtime_scan(self):
        stderr = io.StringIO()
        with mock.patch.dict(os.environ, {"DESTDIR": "/tmp/stage"}, clear=False):
            with mock.patch.object(stopper, "stop_runtime") as stop:
                with mock.patch.object(stopper.os, "geteuid", return_value=1000):
                    with redirect_stderr(stderr):
                        self.assertEqual(stopper.main([]), 2)
                stop.assert_not_called()
        self.assertEqual(
            stderr.getvalue(),
            "Mozkey runtime stop failed: staged_install_root\n",
        )

    def test_nonstandard_prefix_is_refused_before_runtime_scan(self):
        stderr = io.StringIO()
        with mock.patch.dict(
            os.environ, {"DESTDIR": "", "PREFIX": "/opt/mozkey"}
        ):
            with mock.patch.object(stopper, "stop_runtime") as stop:
                with mock.patch.object(stopper.os, "geteuid", return_value=1000):
                    with redirect_stderr(stderr):
                        self.assertEqual(stopper.main([]), 2)
                stop.assert_not_called()
        self.assertEqual(
            stderr.getvalue(),
            "Mozkey runtime stop failed: unsupported_prefix\n",
        )

    def test_root_caller_must_name_a_nonroot_target_uid(self):
        stderr = io.StringIO()
        with mock.patch.dict(os.environ, {"DESTDIR": "", "PREFIX": "/usr"}):
            with mock.patch.object(stopper.os, "geteuid", return_value=0):
                with redirect_stderr(stderr):
                    self.assertEqual(stopper.main([]), 2)
        self.assertEqual(
            stderr.getvalue(),
            "Mozkey runtime stop failed: target_uid_required\n",
        )

    def test_main_emits_only_fixed_diagnostics(self):
        stderr = io.StringIO()
        with mock.patch.dict(os.environ, {"DESTDIR": "", "PREFIX": "/usr"}):
            with mock.patch.object(stopper.os, "geteuid", return_value=1000):
                with mock.patch.object(
                    stopper,
                    "stop_runtime",
                    side_effect=RuntimeError("pid=123 private path secret"),
                ):
                    with redirect_stderr(stderr):
                        self.assertEqual(stopper.main([]), 1)
        self.assertEqual(
            stderr.getvalue(),
            "Mozkey runtime stop failed: unexpected_runtime_error\n",
        )

    def test_main_success_messages_are_fixed(self):
        for result, expected in [
            (stopper.StopResult(True, False), "no_runtime"),
            (stopper.StopResult(False, False), "stopped"),
            (stopper.StopResult(False, True), "forced"),
        ]:
            stdout = io.StringIO()
            with mock.patch.dict(
                os.environ, {"DESTDIR": "", "PREFIX": "/usr"}
            ):
                with mock.patch.object(stopper.os, "geteuid", return_value=1000):
                    with mock.patch.object(stopper, "stop_runtime", return_value=result):
                        with redirect_stdout(stdout):
                            self.assertEqual(stopper.main([]), 0)
            self.assertEqual(
                stdout.getvalue(), f"Mozkey runtime stop complete: {expected}\n"
            )


if __name__ == "__main__":
    unittest.main()
