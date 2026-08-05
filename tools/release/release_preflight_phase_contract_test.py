from __future__ import annotations

from pathlib import Path
import unittest


class ReleasePreflightPhaseContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        repository = Path(__file__).resolve().parents[2]
        workflow_directory = repository / ".github" / "workflows"
        cls.preflight = (
            workflow_directory / "release-preflight.yaml"
        ).read_text(encoding="utf-8")
        cls.release = (workflow_directory / "release.yaml").read_text(
            encoding="utf-8"
        )

    def test_direct_dispatch_is_fixed_to_pre_tag_phase(self) -> None:
        dispatch = self.preflight.split(
            "  workflow_dispatch:\n", maxsplit=1
        )[1].split("  workflow_call:\n", maxsplit=1)[0]
        self.assertIn("      identity_phase:\n", dispatch)
        self.assertIn("        default: pre-tag\n", dispatch)
        self.assertIn("        type: choice\n", dispatch)
        self.assertIn("        options:\n          - pre-tag\n", dispatch)
        self.assertNotIn("          - tag\n", dispatch)

    def test_reusable_call_defaults_to_tag_phase(self) -> None:
        workflow_call = self.preflight.split(
            "  workflow_call:\n", maxsplit=1
        )[1].split("\npermissions:\n", maxsplit=1)[0]
        self.assertIn("      identity_phase:\n", workflow_call)
        self.assertIn("        default: tag\n", workflow_call)
        self.assertIn("        type: string\n", workflow_call)

        release_preflight = self.release.split(
            "\n  preflight:\n", maxsplit=1
        )[1].split("\n  linux:\n", maxsplit=1)[0]
        self.assertIn(
            "uses: ./.github/workflows/release-preflight.yaml",
            release_preflight,
        )

    def test_identity_selection_is_phase_driven_and_fail_closed(self) -> None:
        self.assertIn(
            "if: ${{ inputs.identity_phase == 'pre-tag' }}",
            self.preflight,
        )
        self.assertIn(
            "if: ${{ inputs.identity_phase == 'tag' }}",
            self.preflight,
        )
        self.assertIn("Validate identity phase contract", self.preflight)
        self.assertIn("pre-tag|tag) ;;", self.preflight)
        self.assertIn(
            "Unsupported release identity phase",
            self.preflight,
        )
        self.assertNotIn(
            "github.event_name != 'workflow_dispatch'",
            self.preflight,
        )


if __name__ == "__main__":
    unittest.main()
