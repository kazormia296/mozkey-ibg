import http.server
import io
import json
import os
import socket
import tempfile
import threading
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

from tools.release import probe_zenz_runtime as probe


class ProbeZenzRuntimeTest(unittest.TestCase):
    def test_accepts_exact_release_llama_arguments(self):
        link = Path("/private/stage/usr/lib/mozkey-ibg/llama-server")
        model = Path("/private/stage/usr/lib/mozkey-ibg/models") / probe.MODEL_NAME
        arguments = [
            str(link),
            "-m",
            str(model),
            "-c",
            "256",
            "-t",
            "4",
            "--host",
            "127.0.0.1",
            "--port",
            "57321",
            "--api-key",
            "a" * 64,
            "--flash-attn",
            "auto",
        ]
        for flash_attention in ("auto", "on", "off"):
            with self.subTest(flash_attention=flash_attention):
                candidate = list(arguments)
                candidate[14] = flash_attention
                port, key = probe._inspect_llama_argv(
                    candidate,
                    expected_link=link,
                    expected_model=model,
                )
                self.assertEqual(port, 57321)
                self.assertEqual(key, "a" * 64)
        device_port, device_key = probe._inspect_llama_argv(
            arguments + ["--device", "Vulkan0"],
            expected_link=link,
            expected_model=model,
            expected_backend_device="Vulkan0",
        )
        self.assertEqual(device_port, 57321)
        self.assertEqual(device_key, "a" * 64)

    def test_rejects_non_loopback_or_malformed_llama_arguments(self):
        link = Path("/private/stage/usr/lib/mozkey-ibg/llama-server")
        model = Path("/private/stage/usr/lib/mozkey-ibg/models") / probe.MODEL_NAME
        base = [
            str(link),
            "-m",
            str(model),
            "-c",
            "256",
            "-t",
            "4",
            "--host",
            "127.0.0.1",
            "--port",
            "57321",
            "--api-key",
            "b" * 64,
            "--flash-attn",
            "auto",
        ]
        exposed = list(base)
        exposed[8] = "0.0.0.0"
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_host_invalid"):
            probe._inspect_llama_argv(
                exposed, expected_link=link, expected_model=model
            )
        duplicate = list(base) + ["--host", "127.0.0.1"]
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_arguments_invalid"):
            probe._inspect_llama_argv(
                duplicate, expected_link=link, expected_model=model
            )
        duplicate_flash_attention = list(base) + ["--flash-attn", "auto"]
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_arguments_invalid"):
            probe._inspect_llama_argv(
                duplicate_flash_attention,
                expected_link=link,
                expected_model=model,
            )
        extra = list(base) + ["--device", "Vulkan0", "extra"]
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_arguments_invalid"):
            probe._inspect_llama_argv(
                extra, expected_link=link, expected_model=model
            )
        missing_flash_attention = base[:-2]
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_arguments_invalid"):
            probe._inspect_llama_argv(
                missing_flash_attention,
                expected_link=link,
                expected_model=model,
            )
        invalid_flash_attention = list(base)
        invalid_flash_attention[14] = "enabled"
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_arguments_invalid"):
            probe._inspect_llama_argv(
                invalid_flash_attention,
                expected_link=link,
                expected_model=model,
            )
        invalid_flash_attention_flag = list(base)
        invalid_flash_attention_flag[13] = "--flash-attention"
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_arguments_invalid"):
            probe._inspect_llama_argv(
                invalid_flash_attention_flag,
                expected_link=link,
                expected_model=model,
            )
        weak_key = list(base)
        weak_key[12] = "not-a-release-key"
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_api_key_invalid"):
            probe._inspect_llama_argv(
                weak_key, expected_link=link, expected_model=model
            )
        unsafe_device = list(base) + ["--device", "CUDA0 --host 0.0.0.0"]
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_arguments_invalid"):
            probe._inspect_llama_argv(
                unsafe_device, expected_link=link, expected_model=model
            )
        with self.assertRaisesRegex(probe.ProbeFailure, "llama_arguments_invalid"):
            probe._inspect_llama_argv(
                base,
                expected_link=Path("/private/stage/usr/lib/mozkey-ibg/other-server"),
                expected_model=model,
            )

    def test_parses_only_listening_proc_tcp_entries(self):
        table = """\
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:C001 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 12345
   1: 00000000:C002 00000000:0000 01 00000000:00000000 00:00000000 00000000  1000        0 99999
"""
        listeners = probe._parse_tcp_listeners(table, socket.AF_INET)
        self.assertEqual(
            listeners,
            [probe.TcpListener(inode="12345", address="127.0.0.1", port=49153)],
        )

    def test_wire_request_requires_status_zero_and_nonempty_value(self):
        with tempfile.TemporaryDirectory(
            prefix="mz-z-test-", dir="/tmp"
        ) as temporary:
            socket_path = Path(temporary) / "wire.sock"
            ready = threading.Event()
            captured: list[tuple[bytes, bytes]] = []

            def receive_exact(connection: socket.socket, size: int) -> bytes:
                output = b""
                while len(output) < size:
                    output += connection.recv(size - len(output))
                return output

            def serve() -> None:
                listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    listener.bind(str(socket_path))
                    listener.listen(1)
                    ready.set()
                    connection, _ = listener.accept()
                    with connection:
                        header = receive_exact(
                            connection, probe.WIRE_REQUEST_HEADER.size
                        )
                        fields = probe.WIRE_REQUEST_HEADER.unpack(header)
                        prompt_size = fields[-2]
                        backend_device_size = fields[-1]
                        captured.append(
                            (
                                receive_exact(connection, prompt_size),
                                receive_exact(connection, backend_device_size),
                            )
                        )
                        value = "成功".encode("utf-8")
                        response = probe.WIRE_RESPONSE_HEADER.pack(
                            probe.WIRE_MAGIC,
                            probe.WIRE_VERSION,
                            probe.WIRE_RESPONSE,
                            fields[3],
                            probe.WIRE_STATUS_OK,
                            7,
                            len(value),
                            2,
                        )
                        connection.sendall(response + value + b"ok")
                finally:
                    listener.close()

            server = threading.Thread(target=serve)
            server.start()
            self.assertTrue(ready.wait(2))
            self.assertTrue(
                probe._send_wire_request(
                    socket_path,
                    generation=41,
                    timeout=2,
                    prompt="private probe",
                    backend_device="Vulkan0",
                )
            )
            server.join(2)
            self.assertFalse(server.is_alive())
            self.assertEqual(captured, [(b"private probe", b"Vulkan0")])

    def test_authenticated_completion_uses_bearer_and_bounded_json(self):
        key = "c" * 64
        seen: dict[str, object] = {}

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                seen["path"] = self.path
                seen["authorization"] = self.headers.get("Authorization")
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                seen["prompt"] = json.loads(body)["prompt"]
                payload = json.dumps({"content": "完了"}).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, _format, *args):
                del args

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            runtime = probe.LlamaRuntime(
                pid=os.getpid(),
                start_time="test",
                port=server.server_port,
                api_key=key,
            )
            probe._authenticated_completion(
                runtime, timeout=2, prompt="private http probe"
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(2)
        self.assertEqual(seen["path"], "/completion")
        self.assertEqual(seen["authorization"], "Bearer " + key)
        self.assertEqual(seen["prompt"], "private http probe")

    def test_staged_runtime_rejects_wrong_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            root = stage / "usr/lib/mozkey-ibg"
            model = root / "models" / probe.MODEL_NAME
            model.parent.mkdir(parents=True)
            scorer = root / "mozc_zenz_scorer"
            scorer.write_bytes(b"binary")
            scorer.chmod(0o755)
            model.write_bytes(b"model")
            model.chmod(0o664)
            (root / "llama-server").symlink_to(probe.DEFAULT_LLAMA_SERVER)
            with self.assertRaisesRegex(probe.ProbeFailure, "staged_runtime_invalid"):
                probe._validate_staged_runtime(stage)

    def test_staged_runtime_accepts_verified_bundled_server(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            root = stage / "usr/lib/mozkey-ibg"
            model = root / "models" / probe.MODEL_NAME
            model.parent.mkdir(parents=True)
            scorer = root / "mozc_zenz_scorer"
            scorer.write_bytes(b"scorer")
            scorer.chmod(0o755)
            model.write_bytes(b"model")
            model.chmod(0o644)
            server = root / "llama-server"
            server.write_bytes(b"server")
            server.chmod(0o755)
            self.assertEqual(
                probe._validate_staged_runtime(stage, bundled_server=True),
                (scorer, model, server),
            )

    def test_bundled_probe_requires_matching_attested_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            server = Path(temporary) / "llama-server"
            server.write_bytes(b"#!/bin/sh\nexit 0\n")
            server.chmod(0o755)
            with self.assertRaisesRegex(
                probe.ProbeFailure, "llama_server_digest_invalid"
            ):
                probe.run_probe(
                    llama_server=server,
                    llama_server_sha256="0" * 64,
                    layout="ubuntu-layout",
                )

    def test_probe_directories_split_large_stage_from_short_runtime_home(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as temporary:
            long_tmp = Path(temporary) / ("long-stage-parent-" * 5)
            long_tmp.mkdir()
            with mock.patch.dict(
                os.environ, {"TMPDIR": str(long_tmp)}
            ), mock.patch.object(tempfile, "tempdir", None):
                with probe._probe_directories() as (stage, home):
                    self.assertEqual(stage.parent.parent, long_tmp)
                    self.assertEqual(home.parent, probe.SHORT_RUNTIME_PARENT)
                    self.assertLessEqual(
                        len(os.fsencode(home / ".mozkey-ibg_zenz_scorer_pipe")),
                        probe.MAX_UNIX_SOCKET_PATH_BYTES,
                    )
                    self.assertEqual(stage.stat().st_mode & 0o777, 0o700)
                    self.assertEqual(home.stat().st_mode & 0o777, 0o700)

    def test_rejects_overlong_runtime_socket_path(self):
        path = Path("/tmp") / ("x" * probe.MAX_UNIX_SOCKET_PATH_BYTES)
        with self.assertRaisesRegex(probe.ProbeFailure, "private_home_invalid"):
            probe._require_short_socket_path(path)

    def test_main_never_emits_a_sensitive_exception_cause(self):
        failure = probe.ProbeFailure("wire_io_failed")
        failure.__cause__ = RuntimeError(
            "secret-key private-prompt private-value /raw/private/path"
        )
        stderr = io.StringIO()
        with mock.patch.object(probe, "run_probe", side_effect=failure):
            with redirect_stderr(stderr):
                self.assertEqual(probe.main([]), 1)
        self.assertEqual(
            stderr.getvalue(),
            "Zenz release runtime probe failed: wire_io_failed\n",
        )

        stderr = io.StringIO()
        with mock.patch.object(
            probe,
            "run_probe",
            side_effect=RuntimeError(
                "secret-key private-prompt private-value /raw/private/path"
            ),
        ):
            with redirect_stderr(stderr):
                self.assertEqual(probe.main([]), 1)
        self.assertEqual(
            stderr.getvalue(),
            "Zenz release runtime probe failed: unexpected_runtime_error\n",
        )


if __name__ == "__main__":
    unittest.main()
