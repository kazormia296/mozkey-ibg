import os
import hashlib
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("verify_llama_server_compatibility")
FLAGS = (
    "--api-key --host --port --model --ctx-size --threads "
    "--device --list-devices --flash-attn"
)


class VerifyLlamaServerCompatibilityTest(unittest.TestCase):
    def make_server(self, directory: Path, body: str) -> Path:
        server = directory / "llama-server"
        server.write_text("#!/bin/sh\n" + textwrap.dedent(body), encoding="utf-8")
        server.chmod(0o755)
        return server

    def run_verifier(self, server: Path, **environment: str) -> subprocess.CompletedProcess:
        env = os.environ.copy()
        env.update(environment)
        return subprocess.run(
            [str(SCRIPT), str(server)],
            text=True,
            capture_output=True,
            env=env,
            timeout=10,
            check=False,
        )

    def value_form_server_body(self, version: str) -> str:
        return f'''\
        case "$1" in
          --version) echo "version: {version}" ;;
          --help) echo "{FLAGS}" ;;
          --flash-attn)
            case "$2" in
              auto|on|off) [ "$3" = --version ] || exit 2 ;;
              *) exit 2 ;;
            esac
            echo "version: {version}"
            ;;
          *) exit 2 ;;
        esac
        '''

    def test_accepts_bounded_compatible_cli_under_test_override(self):
        with tempfile.TemporaryDirectory() as temporary:
            server = self.make_server(
                Path(temporary),
                self.value_form_server_body("test"),
            )
            result = self.run_verifier(
                server, MOZKEY_LLAMA_ALLOW_UNTRUSTED_FOR_TESTS="1"
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("compatible llama-server CLI: version: test", result.stdout)

    def test_rejects_user_owned_executable_without_override(self):
        with tempfile.TemporaryDirectory() as temporary:
            server = self.make_server(
                Path(temporary),
                f'echo "{FLAGS}"',
            )
            result = self.run_verifier(server)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("root-owned", result.stderr)

    def test_accepts_user_owned_build_output_only_with_matching_sha256(self):
        with tempfile.TemporaryDirectory() as temporary:
            server = self.make_server(
                Path(temporary),
                self.value_form_server_body("attested"),
            )
            digest = hashlib.sha256(server.read_bytes()).hexdigest()
            result = self.run_verifier(
                server, MOZKEY_LLAMA_ATTESTED_SHA256=digest
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            rejected = self.run_verifier(
                server, MOZKEY_LLAMA_ATTESTED_SHA256="0" * 64
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("SHA-256 mismatch", rejected.stderr)

    def test_times_out_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            server = self.make_server(
                Path(temporary),
                f'''\
                if [ "$1" = --version ]; then sleep 3; fi
                echo "{FLAGS}"
                ''',
            )
            result = self.run_verifier(
                server,
                MOZKEY_LLAMA_ALLOW_UNTRUSTED_FOR_TESTS="1",
                MOZKEY_LLAMA_PROBE_TIMEOUT_SECONDS="1",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("timed out", result.stderr)

    def test_rejects_probe_that_exceeds_output_file_limit(self):
        with tempfile.TemporaryDirectory() as temporary:
            server = self.make_server(
                Path(temporary),
                f'''\
                if [ "$1" = --version ]; then
                  head -c 1048576 /dev/zero
                  exit 0
                fi
                echo "{FLAGS}"
                ''',
            )
            result = self.run_verifier(
                server, MOZKEY_LLAMA_ALLOW_UNTRUSTED_FOR_TESTS="1"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("\x00" * 128, result.stderr)
            self.assertLess(len(result.stderr), 4096)

    def test_rejects_missing_required_flag(self):
        with tempfile.TemporaryDirectory() as temporary:
            server = self.make_server(
                Path(temporary),
                '''\
                if [ "$1" = --version ]; then echo version; else echo --host; fi
                ''',
            )
            result = self.run_verifier(
                server, MOZKEY_LLAMA_ALLOW_UNTRUSTED_FOR_TESTS="1"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing required CLI flag", result.stderr)

    def test_rejects_boolean_only_flash_attention(self):
        with tempfile.TemporaryDirectory() as temporary:
            server = self.make_server(
                Path(temporary),
                f'''\
                case "$1" in
                  --version) echo "version: boolean-only" ;;
                  --help) echo "{FLAGS}" ;;
                  --flash-attn)
                    [ "$2" = --version ] || exit 2
                    echo "version: boolean-only"
                    ;;
                  *) exit 2 ;;
                esac
                ''',
            )
            result = self.run_verifier(
                server, MOZKEY_LLAMA_ALLOW_UNTRUSTED_FOR_TESTS="1"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--flash-attn auto --version failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
