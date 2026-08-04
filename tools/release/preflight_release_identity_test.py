from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.release.preflight_release_identity import validate_preflight_identity
from tools.release.validate_mozkey_release import ReleaseValidationError


def _git(repository: Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout.strip()


def _write_version(
    path: Path,
    version: tuple[int, int, int],
    *,
    build_expression: str = "BUILD_OSS",
    engine_version: int = 24,
    data_version: int = 11,
    suffix: str = "",
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "\n".join(
            (
                "# Representative src/version.bzl content used by CI preflight.",
                "MAJOR = 3",
                "MINOR = 33",
                "BUILD_OSS = 6154",
                f"BUILD = {build_expression}",
                "REVISION = 100",
                "",
                "# User-facing release identity.",
                f"MOZKEY_RELEASE_VERSION_MAJOR = {version[0]}",
                f"MOZKEY_RELEASE_VERSION_MINOR = {version[1]}",
                f"MOZKEY_RELEASE_VERSION_PATCH = {version[2]}",
                "",
                'DEFAULT_BUILD_LABEL_MACOS = "%d.%d.%d.%d" % (',
                "    MAJOR, MINOR, BUILD, REVISION + 1",
                ")",
                f"ENGINE_VERSION = {engine_version}",
                f"DATA_VERSION = {data_version}",
                "",
            )
        )
        + suffix,
        encoding="utf-8",
    )


class PreflightReleaseIdentityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.repository = Path(self.temporary_directory.name)
        _git(self.repository, "init", "--initial-branch=main")
        _git(self.repository, "config", "user.name", "Mozkey Preflight Test")
        _git(
            self.repository,
            "config",
            "user.email",
            "preflight-test@example.invalid",
        )
        self.version_file = self.repository / "src" / "version.bzl"
        _write_version(self.version_file, (0, 9, 4))
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "main release identity")
        _git(self.repository, "tag", "v0.9.4")

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def _validate(
        self,
        phase: str,
        candidate_tag: str | None = None,
        base_ref: str | None = None,
    ):
        return validate_preflight_identity(
            phase=phase,
            candidate_tag=candidate_tag,
            version_file=self.version_file,
            repository=self.repository,
            main_ref="main",
            base_ref=base_ref,
        )

    def test_pull_request_accepts_ordinary_change_at_current_version(self) -> None:
        _git(self.repository, "switch", "-c", "feature")
        (self.repository / "feature.txt").write_text("feature\n", encoding="utf-8")
        _git(self.repository, "add", "feature.txt")
        _git(self.repository, "commit", "-m", "feature")

        identity = self._validate("pull-request")

        self.assertEqual(identity.candidate_tag, "v0.9.4")

    def test_pull_request_accepts_new_version_only_when_tag_is_new(self) -> None:
        _git(self.repository, "switch", "-c", "release")
        _write_version(self.version_file, (0, 9, 5))
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "version 0.9.5")

        self.assertEqual(
            self._validate("pull-request").candidate_tag,
            "v0.9.5",
        )
        _git(self.repository, "tag", "v0.9.5")
        with self.assertRaisesRegex(ReleaseValidationError, "reuses existing"):
            self._validate("pull-request")

    def test_pull_request_rejects_build_change_in_version_only_diff(self) -> None:
        _git(self.repository, "switch", "-c", "broken-build")
        _write_version(
            self.version_file,
            (0, 9, 4),
            build_expression='"broken"',
        )
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "break build identity")

        with self.assertRaisesRegex(ReleaseValidationError, "permits changes only"):
            self._validate("pull-request")

    def test_pull_request_rejects_compatibility_version_changes(self) -> None:
        _git(self.repository, "switch", "-c", "compatibility")
        _write_version(
            self.version_file,
            (0, 9, 4),
            engine_version=25,
            data_version=0,
        )
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "change compatibility identity")

        with self.assertRaisesRegex(ReleaseValidationError, "permits changes only"):
            self._validate("pull-request")

    def test_pull_request_rejects_comment_in_version_only_diff(self) -> None:
        _git(self.repository, "switch", "-c", "comment")
        _write_version(
            self.version_file,
            (0, 9, 4),
            suffix="# unexpected release comment\n",
        )
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "add version comment")

        with self.assertRaisesRegex(ReleaseValidationError, "permits changes only"):
            self._validate("pull-request")

    def test_pull_request_rejects_statement_in_version_only_diff(self) -> None:
        _git(self.repository, "switch", "-c", "statement")
        _write_version(
            self.version_file,
            (0, 9, 4),
            suffix="UNEXPECTED_VERSION_STATE = True\n",
        )
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "add version statement")

        with self.assertRaisesRegex(ReleaseValidationError, "permits changes only"):
            self._validate("pull-request")

    def test_pull_request_defers_mixed_version_change_to_full_ci(self) -> None:
        _git(self.repository, "switch", "-c", "mixed")
        _write_version(
            self.version_file,
            (0, 9, 4),
            build_expression='"broken"',
        )
        (self.repository / "feature.txt").write_text("feature\n", encoding="utf-8")
        _git(self.repository, "add", "src/version.bzl", "feature.txt")
        _git(self.repository, "commit", "-m", "mixed change")

        self.assertEqual(
            self._validate("pull-request").candidate_tag,
            "v0.9.4",
        )

    def test_pull_request_rejects_version_older_than_main(self) -> None:
        _git(self.repository, "switch", "-c", "old-release")
        _write_version(self.version_file, (0, 9, 3))
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "old version")

        with self.assertRaisesRegex(ReleaseValidationError, "not newer than"):
            self._validate("pull-request")

    def test_branch_accepts_version_only_bump_from_push_base(self) -> None:
        base_ref = _git(self.repository, "rev-parse", "HEAD")
        _write_version(self.version_file, (0, 9, 5))
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "version 0.9.5")

        identity = self._validate("branch", base_ref=base_ref)

        self.assertEqual(identity.candidate_tag, "v0.9.5")
        self.assertEqual(identity.commit, identity.main_commit)

    def test_branch_rejects_non_release_version_change(self) -> None:
        base_ref = _git(self.repository, "rev-parse", "HEAD")
        _write_version(
            self.version_file,
            (0, 9, 4),
            build_expression='"broken"',
        )
        _git(self.repository, "add", "src/version.bzl")
        _git(self.repository, "commit", "-m", "break branch identity")

        with self.assertRaisesRegex(ReleaseValidationError, "permits changes only"):
            self._validate("branch", base_ref=base_ref)

    def test_pre_tag_requires_clean_main_and_absent_matching_tag(self) -> None:
        _git(self.repository, "tag", "-d", "v0.9.4")
        identity = self._validate("pre-tag", "v0.9.4")
        self.assertEqual(identity.commit, identity.main_commit)

        (self.repository / "dirty.txt").write_text("dirty\n", encoding="utf-8")
        with self.assertRaisesRegex(ReleaseValidationError, "must be clean"):
            self._validate("pre-tag", "v0.9.4")

    def test_pre_tag_rejects_existing_or_mismatched_tag(self) -> None:
        with self.assertRaisesRegex(ReleaseValidationError, "already exists"):
            self._validate("pre-tag", "v0.9.4")
        with self.assertRaisesRegex(ReleaseValidationError, "does not match"):
            self._validate("pre-tag", "v0.9.5")

    def test_tag_phase_reuses_release_boundary_validation(self) -> None:
        identity = self._validate("tag", "v0.9.4")
        self.assertEqual(identity.commit, _git(self.repository, "rev-parse", "HEAD"))


if __name__ == "__main__":
    unittest.main()
