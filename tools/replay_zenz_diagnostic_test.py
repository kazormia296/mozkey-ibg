import argparse
import base64
import importlib.util
import json
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("replay_zenz_diagnostic.py")
SPEC = importlib.util.spec_from_file_location("replay_zenz_diagnostic", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ReplayZenzDiagnosticTest(unittest.TestCase):
    def test_clean_generated_text_stops_at_private_use_marker(self) -> None:
        self.assertEqual(
            MODULE._clean_generated_text("璋†堯賄賂\uee00ignored"), "璋†堯賄賂"
        )

    def test_exact_body_is_preserved_by_default(self) -> None:
        body = b'{"prompt":"\\uee02\xe7\x9a\x84","cache_prompt":true}'
        replay_body, modified = MODULE._body_for_replay(body, "preserve")
        self.assertEqual(replay_body, body)
        self.assertEqual(modified, [])

    def test_n_probs_override_is_explicitly_recorded(self) -> None:
        body = b'{"prompt":"test","cache_prompt":true}'
        replay_body, modified = MODULE._body_for_replay(body, "preserve", 5)
        self.assertEqual(json.loads(replay_body)["n_probs"], 5)
        self.assertEqual(modified, ["n_probs"])

    def test_cache_prompt_override_is_json_and_utf8(self) -> None:
        body = json.dumps(
            {"prompt": "的", "cache_prompt": True},
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        replay_body, modified = MODULE._body_for_replay(body, "false")
        self.assertEqual(json.loads(replay_body.decode("utf-8"))["cache_prompt"], False)
        self.assertEqual(modified, ["cache_prompt"])

    def test_selects_scorer_requests_in_file_order(self) -> None:
        prompt = base64.b64encode(b"prompt").decode("ascii")
        records = [
            {"event": "session_request", "generation": 1},
            {
                "event": "scorer_request",
                "generation": 1,
                "request_kind": "client_request",
                "http_json_base64": prompt,
                "_line_number": 2,
            },
        ]
        selected = MODULE._select_requests(
            records,
            generation=None,
            request_kind=set(),
            exclude_kind=set(),
            skip_sequences=set(),
            skip_lines=set(),
            limit=None,
        )
        self.assertEqual([item.line_number for item in selected], [2])

    def test_server_command_restores_and_overrides_flash_attention(self) -> None:
        request = MODULE.CapturedRequest(
            1,
            {
                "model_path": "captured-model.gguf",
                "runtime_args": {
                    "ctx": 128,
                    "threads": 3,
                    "flash_attention": "off",
                },
            },
            b"{}",
        )
        args = argparse.Namespace(
            api_key="",
            ctx=None,
            device=None,
            flash_attention=None,
            llama_server=Path("llama-server"),
            model=None,
            threads=None,
            url="http://127.0.0.1:18080/completion",
        )
        command = MODULE._server_command(args, [request])
        self.assertEqual(command[-2:], ["--flash-attn", "off"])

        args.flash_attention = "on"
        command = MODULE._server_command(args, [request])
        self.assertEqual(command[-2:], ["--flash-attn", "on"])


if __name__ == "__main__":
    unittest.main()
