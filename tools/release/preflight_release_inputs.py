#!/usr/bin/env python3
"""Quickly verify checksum-pinned external inputs used by release builds."""

from __future__ import annotations

import argparse
import ast
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys
import time
from typing import Callable, Mapping, Sequence
import urllib.error
import urllib.parse
import urllib.request


_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_USER_AGENT = "Mozkey-release-preflight/1"
_MAX_FULL_DOWNLOAD_BYTES = 128 * 1024 * 1024


class InputPreflightError(RuntimeError):
    """A pinned release input is unavailable or violates its lock."""


@dataclass(frozen=True)
class PinnedInput:
    name: str
    url: str
    sha256: str
    size: int | None
    download_and_hash: bool


def _literal_assignments(path: Path, names: set[str]) -> dict[str, object]:
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), str(path))
    except (OSError, UnicodeError, SyntaxError) as error:
        raise InputPreflightError(f"could not parse {path}: {error}") from error
    values: dict[str, object] = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name) or target.id not in names:
            continue
        if target.id in values:
            raise InputPreflightError(f"duplicate assignment for {target.id} in {path}")
        try:
            values[target.id] = ast.literal_eval(node.value)
        except (TypeError, ValueError) as error:
            raise InputPreflightError(
                f"{target.id} must be a literal in {path}"
            ) from error
    missing = names.difference(values)
    if missing:
        raise InputPreflightError(f"missing assignments in {path}: {sorted(missing)}")
    return values


def _archive_assignments(
    path: Path, names: Sequence[str]
) -> dict[str, Mapping[str, object]]:
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), str(path))
    except (OSError, UnicodeError, SyntaxError) as error:
        raise InputPreflightError(f"could not parse {path}: {error}") from error
    requested = set(names)
    values: dict[str, Mapping[str, object]] = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name) or target.id not in requested:
            continue
        call = node.value
        if not isinstance(call, ast.Call) or not isinstance(call.func, ast.Name):
            raise InputPreflightError(f"{target.id} is not an ArchiveInfo call")
        if call.func.id != "ArchiveInfo" or call.args:
            raise InputPreflightError(f"{target.id} is not a canonical ArchiveInfo")
        try:
            record = {
                keyword.arg: ast.literal_eval(keyword.value)
                for keyword in call.keywords
                if keyword.arg is not None
            }
        except (TypeError, ValueError) as error:
            raise InputPreflightError(
                f"{target.id} ArchiveInfo fields must be literals"
            ) from error
        if set(record) != {"url", "size", "sha256"}:
            raise InputPreflightError(f"{target.id} ArchiveInfo fields are incomplete")
        values[target.id] = record
    missing = requested.difference(values)
    if missing:
        raise InputPreflightError(
            f"missing ArchiveInfo assignments in {path}: {sorted(missing)}"
        )
    return values


def _require_https(url: object, label: str) -> str:
    if not isinstance(url, str) or not url.startswith("https://"):
        raise InputPreflightError(f"{label} must use an HTTPS URL")
    return url


def _require_digest(value: object, label: str) -> str:
    if not isinstance(value, str) or _HEX64.fullmatch(value) is None:
        raise InputPreflightError(f"{label} must be a lowercase SHA-256 digest")
    return value


def _require_size(value: object, label: str) -> int:
    if type(value) is not int or value <= 0:
        raise InputPreflightError(f"{label} must be a positive byte size")
    return value


def load_pinned_inputs(repository: Path) -> tuple[list[PinnedInput], Path, str, int]:
    repository = repository.resolve()
    macos_path = repository / "tools/release/prepare_macos_zenz_runtime.py"
    windows_path = repository / "tools/release/prepare_windows_zenz_runtime.py"
    dependencies_path = repository / "src/build_tools/update_deps.py"
    lock_path = repository / "tools/release/zenz_gguf_normalization.lock.json"

    runtime_names = {
        "LLAMA_CPP_ARCHIVE_URL",
        "LLAMA_CPP_ARCHIVE_SHA256",
        "NORMALIZED_MODEL_SHA256",
    }
    macos = _literal_assignments(macos_path, runtime_names)
    windows = _literal_assignments(
        windows_path,
        runtime_names | {"NINJA_WIN_ARCHIVE_SHA256", "LLVM_WIN_ARCHIVE_SHA256"},
    )
    for name in runtime_names:
        if macos[name] != windows[name]:
            raise InputPreflightError(f"macOS/Windows runtime lock drift: {name}")

    archive_names = (
        "QT6",
        "NDK_LINUX",
        "NDK_MAC",
        "NINJA_MAC",
        "NINJA_WIN",
        "NINJA_WIN_ARM64",
        "LLVM_WIN",
        "LLVM_WIN_ARM64",
        "MSYS2",
    )
    archives = _archive_assignments(dependencies_path, archive_names)
    if archives["NINJA_WIN"]["sha256"] != windows["NINJA_WIN_ARCHIVE_SHA256"]:
        raise InputPreflightError("Windows Ninja checksum drift")
    if archives["LLVM_WIN"]["sha256"] != windows["LLVM_WIN_ARCHIVE_SHA256"]:
        raise InputPreflightError("Windows LLVM checksum drift")

    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise InputPreflightError(f"could not parse {lock_path}: {error}") from error
    try:
        source = lock["source"]
        normalized = lock["normalized"]
    except (KeyError, TypeError) as error:
        raise InputPreflightError("Zenz normalization lock is incomplete") from error
    normalized_digest = _require_digest(
        normalized.get("sha256"), "normalized Zenz model"
    )
    if normalized_digest != macos["NORMALIZED_MODEL_SHA256"]:
        raise InputPreflightError("normalized Zenz model checksum drift")

    model_path_value = source.get("path")
    if not isinstance(model_path_value, str) or not model_path_value:
        raise InputPreflightError("source Zenz model path is invalid")
    model_path = repository / model_path_value
    model_digest = _require_digest(source.get("sha256"), "source Zenz model")
    model_size = _require_size(source.get("size_bytes"), "source Zenz model")
    repository_url = _require_https(
        source.get("repository"), "source Zenz repository"
    ).rstrip("/")
    repository_commit = source.get("repository_commit")
    source_filename = source.get("source_filename")
    if not isinstance(repository_commit, str) or not repository_commit:
        raise InputPreflightError("source Zenz repository commit is invalid")
    if not isinstance(source_filename, str) or not source_filename:
        raise InputPreflightError("source Zenz filename is invalid")
    # macOS packages upstream Mozkey's Universal runtime, which loads the
    # unmodified source model pinned by src/zenz_runtime/llama_runtime.lock.json.
    # Keep that pin identical to the source side of the normalization lock so
    # every platform starts from the same model bytes.
    runtime_lock_path = repository / "src/zenz_runtime/llama_runtime.lock.json"
    try:
        runtime_lock = json.loads(runtime_lock_path.read_text(encoding="utf-8"))
        runtime_model = runtime_lock["model"]
        runtime_model_digest = runtime_model["sha256"]
        runtime_model_revision = runtime_model["revision"]
        runtime_model_file = runtime_model["windows_file"]
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise InputPreflightError(
            f"could not parse {runtime_lock_path}: {error}"
        ) from error
    except (KeyError, TypeError) as error:
        raise InputPreflightError("Zenz runtime lock is incomplete") from error
    if (
        not isinstance(runtime_model_digest, str)
        or runtime_model_digest.lower() != model_digest
        or runtime_model_revision != repository_commit
        or runtime_model_file != model_path_value
    ):
        raise InputPreflightError("Zenz runtime lock/normalization lock model drift")

    model_url = (
        f"{repository_url}/resolve/"
        f"{urllib.parse.quote(repository_commit, safe='')}/"
        f"{urllib.parse.quote(source_filename)}?download=true"
    )

    inputs = [
        PinnedInput(
            name="llama.cpp source",
            url=_require_https(macos["LLAMA_CPP_ARCHIVE_URL"], "llama.cpp source"),
            sha256=_require_digest(
                macos["LLAMA_CPP_ARCHIVE_SHA256"], "llama.cpp source"
            ),
            size=None,
            download_and_hash=True,
        ),
        PinnedInput(
            name="Zenz source model",
            url=model_url,
            sha256=model_digest,
            size=model_size,
            download_and_hash=False,
        ),
    ]
    for name in archive_names:
        record = archives[name]
        inputs.append(
            PinnedInput(
                name=name,
                url=_require_https(record["url"], name),
                sha256=_require_digest(record["sha256"], name),
                size=_require_size(record["size"], name),
                download_and_hash=name.startswith("NINJA_"),
            )
        )
    return inputs, model_path, model_digest, model_size


def _response_size(headers: Mapping[str, str]) -> int | None:
    content_range = headers.get("Content-Range")
    if content_range:
        match = re.search(r"/([0-9]+)$", content_range)
        if match:
            return int(match.group(1))
    content_length = headers.get("Content-Length")
    if content_length and content_length.isdigit():
        return int(content_length)
    return None


def _open_with_retries(
    request: urllib.request.Request,
    opener: Callable[..., object],
):
    last_error: BaseException | None = None
    for attempt in range(3):
        try:
            return opener(request, timeout=30)
        except (OSError, urllib.error.URLError) as error:
            last_error = error
            if attempt < 2:
                time.sleep(attempt + 1)
    raise InputPreflightError(f"request failed for {request.full_url}: {last_error}")


def verify_pinned_input(
    pinned: PinnedInput,
    opener: Callable[..., object] = urllib.request.urlopen,
) -> None:
    method = "GET" if pinned.download_and_hash else "HEAD"
    request = urllib.request.Request(
        pinned.url,
        method=method,
        headers={"User-Agent": _USER_AGENT, "Accept-Encoding": "identity"},
    )
    response = _open_with_retries(request, opener)
    with response:  # type: ignore[attr-defined]
        final_url = response.geturl()  # type: ignore[attr-defined]
        if not final_url.startswith("https://"):
            raise InputPreflightError(f"{pinned.name} redirected outside HTTPS")
        status = getattr(response, "status", 200)
        if status < 200 or status >= 300:
            raise InputPreflightError(f"{pinned.name} returned HTTP {status}")
        headers = response.headers  # type: ignore[attr-defined]
        reported_size = _response_size(headers)
        if (
            pinned.size is not None
            and not pinned.download_and_hash
            and reported_size is None
        ):
            raise InputPreflightError(f"{pinned.name} did not report its size")
        if pinned.size is not None and reported_size not in (None, pinned.size):
            raise InputPreflightError(
                f"{pinned.name} size changed: expected {pinned.size}, "
                f"received {reported_size}"
            )
        if not pinned.download_and_hash:
            return

        digest = hashlib.sha256()
        total = 0
        while chunk := response.read(1024 * 1024):  # type: ignore[attr-defined]
            total += len(chunk)
            if total > _MAX_FULL_DOWNLOAD_BYTES:
                raise InputPreflightError(f"{pinned.name} exceeds preflight limit")
            digest.update(chunk)
        if pinned.size is not None and total != pinned.size:
            raise InputPreflightError(
                f"{pinned.name} size changed: expected {pinned.size}, received {total}"
            )
        if digest.hexdigest() != pinned.sha256:
            raise InputPreflightError(f"{pinned.name} checksum mismatch")


def verify_local_model(path: Path, expected_digest: str, expected_size: int) -> None:
    if path.is_symlink() or not path.is_file():
        raise InputPreflightError(f"source Zenz model is missing: {path}")
    if path.stat().st_size != expected_size:
        raise InputPreflightError("source Zenz model size mismatch")
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    if digest.hexdigest() != expected_digest:
        raise InputPreflightError("source Zenz model checksum mismatch")


def run_input_preflight(repository: Path) -> None:
    inputs, model_path, model_digest, model_size = load_pinned_inputs(repository)
    verify_local_model(model_path, model_digest, model_size)
    errors: list[str] = []
    with ThreadPoolExecutor(max_workers=6) as executor:
        futures = {
            executor.submit(verify_pinned_input, pinned): pinned
            for pinned in inputs
        }
        for future in as_completed(futures):
            pinned = futures[future]
            try:
                future.result()
            except InputPreflightError as error:
                errors.append(f"{pinned.name}: {error}")
            else:
                print(f"verified pinned release input: {pinned.name}")
    if errors:
        raise InputPreflightError("; ".join(sorted(errors)))


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, default=Path("."))
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        run_input_preflight(arguments.repository)
    except InputPreflightError as error:
        print(f"release input preflight failed: {error}", file=sys.stderr)
        return 1
    print("release input preflight passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
