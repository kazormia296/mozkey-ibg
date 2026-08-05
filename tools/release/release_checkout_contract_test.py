from __future__ import annotations

from pathlib import Path
import re
import unittest


class ReleaseCheckoutContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        repository = Path(__file__).resolve().parents[2]
        workflows = repository / ".github" / "workflows"
        cls.release = (workflows / "release.yaml").read_text(encoding="utf-8")
        cls.preflight = (workflows / "release-preflight.yaml").read_text(
            encoding="utf-8"
        )

    @staticmethod
    def _job(workflow: str, name: str) -> str:
        jobs = workflow.split("\njobs:\n", maxsplit=1)[1]
        match = re.search(
            rf"(?ms)^  {re.escape(name)}:\n(.*?)(?=^  [a-z][a-z0-9_-]*:\n|\Z)",
            jobs,
        )
        if match is None:
            raise AssertionError(f"workflow job is missing: {name}")
        return match.group(0)

    def test_release_gate_preserves_history_without_historical_blobs(self) -> None:
        gate = self._job(self.release, "release-gate")
        self.assertIn("timeout-minutes: 10", gate)
        self.assertNotIn("timeout-minutes: 5", gate)
        self.assertIn("fetch-depth: 0", gate)
        self.assertIn("filter: blob:none", gate)
        self.assertIn("--main-ref origin/main", gate)

    def test_portable_preflight_uses_the_same_blobless_history_contract(self) -> None:
        portable = self._job(self.preflight, "portable")
        self.assertIn("timeout-minutes: 10", portable)
        self.assertIn("fetch-depth: 0", portable)
        self.assertIn("filter: blob:none", portable)
        self.assertIn("--phase pull-request", portable)
        self.assertIn("--phase pre-tag", portable)
        self.assertIn("--phase tag", portable)


if __name__ == "__main__":
    unittest.main()
