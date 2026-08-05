from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ZenzScorerSecurityContractTest(unittest.TestCase):
    def test_scorer_reserves_loopback_port_and_verifies_child_image(self) -> None:
        source = (ROOT / "src/zenz_scorer/main.cc").read_text(encoding="utf-8")
        for marker in (
            "class LoopbackPortReservation",
            "SO_EXCLUSIVEADDRUSE",
            "FD_CLOEXEC",
            "IsExpectedLlamaProcessImage",
            "WaitForExpectedLlamaProcessImage",
            "llama_process_identity_invalid",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, source)

    def test_same_user_api_key_residual_is_documented(self) -> None:
        policy = (ROOT / "docs/security/offline_guarantee.md").read_text(
            encoding="utf-8"
        )
        self.assertIn("same-user residual risk is explicit", policy)
        self.assertIn("--api-key", policy)

    def test_diagnostic_runtime_and_http_request_are_reproducible(self) -> None:
        source = (ROOT / "src/zenz_scorer/main.cc").read_text(encoding="utf-8")
        for marker in (
            "MOZC_ZENZ_FLASH_ATTENTION",
            "--flash-attn",
            "runtime_args.AddString(\"flash_attention\", flash_attention)",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, source)
        self.assertNotIn('body += "\\\"n_probs\\\":5,"', source)


if __name__ == "__main__":
    unittest.main()
