#!/usr/bin/env python3
"""Fail-closed identity checks for Mozkey release workflows."""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Sequence


_TAG_PATTERN = re.compile(
    r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)
_VERSION_FIELDS = (
    "MOZKEY_RELEASE_VERSION_MAJOR",
    "MOZKEY_RELEASE_VERSION_MINOR",
    "MOZKEY_RELEASE_VERSION_PATCH",
)
_VERSION_ASSIGNMENT_PATTERN = re.compile(
    rf"^({'|'.join(re.escape(field) for field in _VERSION_FIELDS)}) = "
    r"(?:0|[1-9][0-9]*)$",
    re.MULTILINE,
)
_VERSION_VALUE_SENTINEL = "<RELEASE_VERSION>"


class ReleaseValidationError(RuntimeError):
    """Raised when a release identity or Git boundary is invalid."""


@dataclass(frozen=True)
class ReleaseIdentity:
    tag: str
    version: str
    commit: str


def parse_release_tag(tag: str) -> tuple[int, int, int]:
    match = _TAG_PATTERN.fullmatch(tag)
    if match is None:
        raise ReleaseValidationError(
            f"release tag must be canonical vMAJOR.MINOR.PATCH: {tag!r}"
        )
    return tuple(int(value) for value in match.groups())  # type: ignore[return-value]


def parse_version_source(source: str, label: str) -> tuple[int, int, int]:
    try:
        tree = ast.parse(source, label)
    except SyntaxError as error:
        raise ReleaseValidationError(
            f"failed to parse release version source {label}: {error}"
        ) from error

    assignments: dict[str, list[ast.expr]] = {
        field: [] for field in _VERSION_FIELDS
    }
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id in assignments:
                    if len(node.targets) != 1:
                        raise ReleaseValidationError(
                            f"{target.id} must use one direct assignment"
                        )
                    assignments[target.id].append(node.value)
        elif isinstance(node, ast.AnnAssign):
            if isinstance(node.target, ast.Name) and node.target.id in assignments:
                if node.value is None:
                    raise ReleaseValidationError(
                        f"{node.target.id} must have an integer value"
                    )
                assignments[node.target.id].append(node.value)
        elif isinstance(node, ast.AugAssign):
            if isinstance(node.target, ast.Name) and node.target.id in assignments:
                raise ReleaseValidationError(
                    f"{node.target.id} must not use an augmented assignment"
                )

    values: list[int] = []
    for field in _VERSION_FIELDS:
        field_assignments = assignments[field]
        if len(field_assignments) != 1:
            raise ReleaseValidationError(
                f"{field} must be assigned exactly once; found {len(field_assignments)}"
            )
        try:
            value = ast.literal_eval(field_assignments[0])
        except (ValueError, TypeError) as error:
            raise ReleaseValidationError(
                f"{field} must be a literal non-negative integer"
            ) from error
        if type(value) is not int or value < 0:
            raise ReleaseValidationError(
                f"{field} must be a literal non-negative integer"
            )
        values.append(value)
    return tuple(values)  # type: ignore[return-value]


def validate_version_only_release_change(
    *,
    base_source: str,
    candidate_source: str,
    base_label: str,
    candidate_label: str,
) -> tuple[int, int, int]:
    """Allows only a strict release-version bump between two source files."""
    base_version = parse_version_source(base_source, base_label)
    candidate_version = parse_version_source(candidate_source, candidate_label)

    def normalize(source: str, label: str) -> str:
        normalized, replacements = _VERSION_ASSIGNMENT_PATTERN.subn(
            lambda match: f"{match.group(1)} = {_VERSION_VALUE_SENTINEL}",
            source,
        )
        if replacements != len(_VERSION_FIELDS):
            raise ReleaseValidationError(
                f"{label} must use the three canonical release-version assignments"
            )
        return normalized

    if normalize(base_source, base_label) != normalize(
        candidate_source, candidate_label
    ):
        raise ReleaseValidationError(
            "version-only CI bypass permits changes only to "
            "MOZKEY_RELEASE_VERSION_MAJOR, MOZKEY_RELEASE_VERSION_MINOR, "
            "and MOZKEY_RELEASE_VERSION_PATCH"
        )
    if candidate_version <= base_version:
        base_tag = "v" + ".".join(str(value) for value in base_version)
        candidate_tag = "v" + ".".join(str(value) for value in candidate_version)
        raise ReleaseValidationError(
            f"version-only release change must increase the version: "
            f"{candidate_tag} is not newer than {base_tag}"
        )
    return candidate_version


def parse_version_file(version_file: Path) -> tuple[int, int, int]:
    if version_file.is_symlink() or not version_file.is_file():
        raise ReleaseValidationError(
            f"version file must be a regular non-symlink file: {version_file}"
        )

    try:
        source = version_file.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ReleaseValidationError(
            f"failed to read release version file {version_file}: {error}"
        ) from error
    return parse_version_source(source, str(version_file))


def _git(
    repository: Path,
    *arguments: str,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown git error"
        raise ReleaseValidationError(
            f"git {' '.join(arguments)} failed in {repository}: {detail}"
        )
    return result


def validate_git_boundary(
    repository: Path,
    tag: str,
    main_ref: str,
) -> str:
    if not repository.is_dir():
        raise ReleaseValidationError(f"repository directory does not exist: {repository}")

    tag_commit = _git(
        repository, "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}"
    ).stdout.strip()
    head_commit = _git(repository, "rev-parse", "--verify", "HEAD^{commit}").stdout.strip()
    if head_commit != tag_commit:
        raise ReleaseValidationError(
            f"checked out HEAD {head_commit} does not match {tag} commit {tag_commit}"
        )

    _git(repository, "rev-parse", "--verify", f"{main_ref}^{{commit}}")
    ancestry = _git(
        repository,
        "merge-base",
        "--is-ancestor",
        tag_commit,
        main_ref,
        check=False,
    )
    if ancestry.returncode == 1:
        raise ReleaseValidationError(
            f"release commit {tag_commit} is not an ancestor of {main_ref}"
        )
    if ancestry.returncode != 0:
        detail = ancestry.stderr.strip() or ancestry.stdout.strip() or "unknown git error"
        raise ReleaseValidationError(
            f"failed to verify {main_ref} ancestry for {tag_commit}: {detail}"
        )
    return tag_commit


def validate_release(
    *,
    tag: str,
    ref_type: str,
    version_file: Path,
    repository: Path,
    main_ref: str,
) -> ReleaseIdentity:
    if ref_type != "tag":
        raise ReleaseValidationError(
            f"release workflow must run from a tag ref; received {ref_type!r}"
        )

    tag_version = parse_release_tag(tag)
    file_version = parse_version_file(version_file)
    if tag_version != file_version:
        expected = ".".join(str(value) for value in file_version)
        raise ReleaseValidationError(
            f"release tag {tag} does not match src/version.bzl version v{expected}"
        )

    commit = validate_git_boundary(repository, tag, main_ref)
    return ReleaseIdentity(
        tag=tag,
        version=".".join(str(value) for value in file_version),
        commit=commit,
    )


def _append_github_output(identity: ReleaseIdentity) -> None:
    output_path = os.environ.get("GITHUB_OUTPUT")
    if not output_path:
        return
    with open(output_path, "a", encoding="utf-8", newline="\n") as output:
        output.write(f"tag={identity.tag}\n")
        output.write(f"version={identity.version}\n")
        output.write(f"commit={identity.commit}\n")


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--ref-type", required=True)
    parser.add_argument("--version-file", type=Path, required=True)
    parser.add_argument("--repository", type=Path, default=Path("."))
    parser.add_argument("--main-ref", default="origin/main")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        identity = validate_release(
            tag=args.tag,
            ref_type=args.ref_type,
            version_file=args.version_file,
            repository=args.repository,
            main_ref=args.main_ref,
        )
    except ReleaseValidationError as error:
        print(f"release validation failed: {error}", file=sys.stderr)
        return 1

    _append_github_output(identity)
    print(
        f"validated Mozkey release {identity.tag} at {identity.commit} "
        f"on {args.main_ref}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
