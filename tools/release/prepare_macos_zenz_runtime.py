#!/usr/bin/env python3
"""Shared llama.cpp pins and helpers for the Mozkey IbG Zenz runtime tooling.

prepare_linux_zenz_runtime.py and preflight_release_inputs.py import the pinned
llama.cpp source identity and the build helpers from this module.

Its own entry point builds the former fork-specific arm64 macOS runtime
(unpatched llama.cpp plus the normalized GGUF) into
src/mac/zenz_runtime/generated.  Since the sync to upstream Mozkey v1.0.0 the
macOS package no longer consumes that tree: //mac:package ships upstream's
Universal runtime, built and staged by
src/mac/installer/zenz_runtime/build_universal_runtime.zsh and
stage_runtime_assets.zsh (see docs/build_mozc_in_osx.md).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path
from typing import Iterable, Sequence


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.release import normalize_zenz_gguf  # noqa: E402


LLAMA_CPP_TAG = "v1.0.0"
# What llama.cpp derives from `git rev-list --count` for the tagged commit;
# the source archive carries no git history.
LLAMA_CPP_BUILD_NUMBER = "12359"
LLAMA_CPP_ARCHIVE_URL = (
    "https://github.com/ggml-org/llama.cpp/archive/refs/tags/v1.0.0.tar.gz"
)
LLAMA_CPP_ARCHIVE_SHA256 = (
    "3f5f750e36326aea4d65c7623e7d597a33e395d65f4d76e5e0532171f252532b"
)
NORMALIZED_MODEL_SHA256 = (
    "601572033a0c231857864ab0a2ccf40fbd1abe6ee4ccecd5399bf82e3e559772"
)
MODEL_NAME = "zenz-v3.2-small-Q5_K_M.gguf"
SCORER_NAME = "mozc_zenz_scorer"
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024
MAX_PROBE_OUTPUT_BYTES = 1024 * 1024
ARCHITECTURES = (("arm64", "12.0"),)
REQUIRED_CMAKE_BOOL_OPTIONS = {
    "BUILD_SHARED_LIBS": "OFF",
    "GGML_ACCELERATE": "ON",
    "GGML_BACKEND_DL": "OFF",
    "GGML_BLAS": "ON",
    "GGML_CPU": "ON",
    "GGML_LLAMAFILE": "OFF",
    "GGML_METAL": "OFF",
    "GGML_NATIVE": "OFF",
    "GGML_OPENMP": "OFF",
    "GGML_RPC": "OFF",
    "GGML_STATIC": "ON",
    "LLAMA_BUILD_APP": "OFF",
    "LLAMA_BUILD_EXAMPLES": "OFF",
    "LLAMA_BUILD_SERVER": "ON",
    "LLAMA_BUILD_TESTS": "OFF",
    # llama.cpp defines llama-server under the tools subtree.  Configure that
    # subtree, but build and stage only the explicit llama-server target.
    "LLAMA_BUILD_TOOLS": "ON",
    "LLAMA_BUILD_UI": "OFF",
    "LLAMA_OPENSSL": "OFF",
    "LLAMA_USE_PREBUILT_UI": "OFF",
    "MTMD_VIDEO": "OFF",
}
LEGACY_CMAKE_OFF_OPTIONS = ("LLAMA_CURL",)


class PreparationError(RuntimeError):
    """Fail-closed runtime preparation error."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _require_regular(path: Path, label: str) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise PreparationError(f"{label}_missing") from error
    if not stat.S_ISREG(info.st_mode) or info.st_size <= 0:
        raise PreparationError(f"{label}_invalid")


def _verify_archive(path: Path) -> None:
    _require_regular(path, "llama_archive")
    if sha256_file(path) != LLAMA_CPP_ARCHIVE_SHA256:
        raise PreparationError("llama_archive_checksum_mismatch")


def _download_archive(destination: Path) -> None:
    if not LLAMA_CPP_ARCHIVE_URL.startswith("https://"):
        raise PreparationError("llama_archive_url_invalid")
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        request = urllib.request.Request(
            LLAMA_CPP_ARCHIVE_URL,
            headers={"User-Agent": "Mozkey-macOS-runtime-builder/1"},
        )
        total = 0
        with os.fdopen(descriptor, "wb") as output:
            descriptor = -1
            with urllib.request.urlopen(request, timeout=60) as response:
                final_url = response.geturl()
                if not final_url.startswith("https://"):
                    raise PreparationError("llama_archive_redirect_invalid")
                while chunk := response.read(1024 * 1024):
                    total += len(chunk)
                    if total > MAX_ARCHIVE_BYTES:
                        raise PreparationError("llama_archive_too_large")
                    output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        _verify_archive(temporary)
        os.chmod(temporary, 0o644)
        os.replace(temporary, destination)
    except PreparationError:
        raise
    except (OSError, urllib.error.URLError) as error:
        raise PreparationError("llama_archive_download_failed") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        temporary.unlink(missing_ok=True)


def _ensure_archive(path: Path) -> Path:
    if path.exists() or path.is_symlink():
        _verify_archive(path)
    else:
        _download_archive(path)
    return path.resolve(strict=True)


def _run(
    command: Sequence[str],
    *,
    cwd: Path | None = None,
    capture: bool = False,
    timeout: int | None = None,
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            list(command),
            cwd=cwd,
            check=True,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise PreparationError("runtime_build_command_failed") from error


def cmake_configure_command(
    source: Path,
    build: Path,
    architecture: str,
    deployment_target: str,
    ninja: Path,
) -> list[str]:
    return [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={ninja}",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_OSX_ARCHITECTURES={architecture}",
        f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DGGML_STATIC=ON",
        "-DGGML_BACKEND_DL=OFF",
        "-DGGML_NATIVE=OFF",
        "-DGGML_OPENMP=OFF",
        "-DGGML_LLAMAFILE=OFF",
        "-DGGML_CPU=ON",
        "-DGGML_ACCELERATE=ON",
        "-DGGML_BLAS=ON",
        "-DGGML_BLAS_VENDOR=Apple",
        "-DGGML_METAL=OFF",
        "-DGGML_RPC=OFF",
        "-DMTMD_VIDEO=OFF",
        "-DLLAMA_CURL=OFF",
        "-DLLAMA_OPENSSL=OFF",
        "-DLLAMA_BUILD_TESTS=OFF",
        "-DLLAMA_BUILD_TOOLS=ON",
        "-DLLAMA_BUILD_EXAMPLES=OFF",
        "-DLLAMA_BUILD_SERVER=ON",
        "-DLLAMA_BUILD_APP=OFF",
        "-DLLAMA_BUILD_UI=OFF",
        "-DLLAMA_USE_PREBUILT_UI=OFF",
        "-DLLAMA_FATAL_WARNINGS=OFF",
        "-DGGML_FATAL_WARNINGS=OFF",
        f"-DLLAMA_BUILD_NUMBER={LLAMA_CPP_BUILD_NUMBER}",
        f"-DLLAMA_BUILD_COMMIT={LLAMA_CPP_TAG}",
        f"-DGGML_BUILD_COMMIT={LLAMA_CPP_TAG}",
    ]


def scorer_compile_command(
    root: Path,
    output: Path,
    architecture: str,
    deployment_target: str,
) -> list[str]:
    """Returns the Bazel command that builds the single-architecture scorer.

    The scorer uses the same protobuf-backed JSON parser as the desktop
    targets.  Building it through Bazel keeps that dependency (and the named
    pipe endpoint contract) linked into the macOS runtime instead of silently
    producing a binary that only works when a host protobuf happens to be
    installed.
    """
    return [
        "bazelisk",
        "build",
        "//zenz_scorer:mozc_zenz_scorer",
        "--config",
        "release_build",
        f"--macos_cpus={architecture}",
        f"--macos_minimum_os={deployment_target}",
        f"--copt=-mmacosx-version-min={deployment_target}",
        f"--cxxopt=-mmacosx-version-min={deployment_target}",
        f"--linkopt=-mmacosx-version-min={deployment_target}",
        "--define=mozkey_dictionary_profile=release-approved-only",
    ]


def _find_bundled_ninja(root: Path) -> Path:
    candidates = (
        root / "src/third_party/ninja/ninja",
        Path(shutil.which("ninja") or ""),
    )
    for candidate in candidates:
        if candidate and candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve(strict=True)
    raise PreparationError("ninja_missing")


def _find_built_server(build: Path) -> Path:
    candidates = (build / "bin/llama-server", build / "bin/Release/llama-server")
    matches = [path for path in candidates if path.is_file()]
    if len(matches) != 1:
        raise PreparationError("llama_server_output_invalid")
    return matches[0]


def _verify_cmake_cache(build: Path) -> None:
    cache_path = build / "CMakeCache.txt"
    _require_regular(cache_path, "cmake_cache")
    try:
        lines = cache_path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise PreparationError("cmake_cache_invalid") from error
    values: dict[str, tuple[str, str]] = {}
    for line in lines:
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        if ":" not in key_and_type:
            continue
        key, value_type = key_and_type.split(":", 1)
        values[key] = (value_type, value)
    for key, expected in REQUIRED_CMAKE_BOOL_OPTIONS.items():
        if values.get(key) != ("BOOL", expected):
            raise PreparationError("cmake_backend_contract_invalid")
    # llama.cpp still reads LLAMA_CURL as a deprecated compatibility variable,
    # rather than declaring it with option().  CMake therefore may retain the
    # command-line value as UNINITIALIZED.  Require the exact OFF value here;
    # the Mach-O dependency check below remains the authoritative proof that
    # no CURL library reached the staged binary.
    for key in LEGACY_CMAKE_OFF_OPTIONS:
        if values.get(key) not in {("BOOL", "OFF"), ("UNINITIALIZED", "OFF")}:
            raise PreparationError("cmake_backend_contract_invalid")
    if values.get("GGML_BLAS_VENDOR") != ("STRING", "Apple"):
        raise PreparationError("cmake_backend_contract_invalid")


def _build_architecture(
    source: Path,
    build_root: Path,
    architecture: str,
    deployment_target: str,
    ninja: Path,
) -> Path:
    build = build_root / architecture
    _run(
        cmake_configure_command(
            source, build, architecture, deployment_target, ninja
        )
    )
    _verify_cmake_cache(build)
    _run(
        [
            "cmake",
            "--build",
            str(build),
            "--target",
            "llama-server",
            "--parallel",
        ]
    )
    return _find_built_server(build)


def _build_scorer_architecture(
    root: Path,
    build_root: Path,
    architecture: str,
    deployment_target: str,
) -> Path:
    output = build_root / architecture / SCORER_NAME
    output.parent.mkdir(parents=True, exist_ok=True)
    _run(
        scorer_compile_command(
            root, output, architecture, deployment_target
        ),
        cwd=root / "src",
    )
    built = (root / "src/bazel-bin/zenz_scorer" / SCORER_NAME).resolve(
        strict=True
    )
    _atomic_copy(built, output, 0o755)
    return output


def _verify_arm64_architecture_contract(
    path: Path, error_prefix: str
) -> None:
    _require_regular(path, error_prefix)
    if not os.access(path, os.X_OK):
        raise PreparationError(f"{error_prefix}_not_executable")

    arches = _run(["lipo", "-archs", str(path)], capture=True, timeout=20)
    if set((arches.stdout or "").split()) != {"arm64"}:
        raise PreparationError(f"{error_prefix}_architectures_invalid")

    for architecture, expected_target in ARCHITECTURES:
        load_commands = _run(
            ["otool", "-l", "-arch", architecture, str(path)],
            capture=True,
            timeout=20,
        )
        minos_values = {
            fields[1]
            for line in (load_commands.stdout or "").splitlines()
            if len(fields := line.split()) == 2 and fields[0] == "minos"
        }
        if minos_values != {expected_target}:
            raise PreparationError(
                f"{error_prefix}_deployment_target_invalid"
            )


def _verify_arm64_scorer(path: Path) -> None:
    _verify_arm64_architecture_contract(path, "zenz_scorer")

    dependencies = _run(["otool", "-L", str(path)], capture=True, timeout=20)
    if "@rpath" in (dependencies.stdout or ""):
        raise PreparationError("zenz_scorer_dynamic_dependency_invalid")


def _verify_arm64_server(path: Path) -> None:
    _verify_arm64_architecture_contract(path, "llama_server")

    dependencies = _run(["otool", "-L", str(path)], capture=True, timeout=20)
    dependency_text = dependencies.stdout or ""
    forbidden_dependencies = (
        "@rpath",
        "libcurl",
        "libggml",
        "libllama",
        "/Metal.framework/",
    )
    if any(value in dependency_text for value in forbidden_dependencies):
        raise PreparationError("llama_server_dynamic_dependency_invalid")
    if "/Accelerate.framework/" not in dependency_text:
        raise PreparationError("llama_server_accelerate_missing")

    version = _run([str(path), "--version"], capture=True, timeout=20)
    help_output = _run([str(path), "--help"], capture=True, timeout=20)
    combined = (version.stdout or "") + (help_output.stdout or "")
    if len(combined.encode("utf-8")) > MAX_PROBE_OUTPUT_BYTES:
        raise PreparationError("llama_server_probe_output_too_large")
    for required_flag in (
        "--api-key",
        "--host",
        "--port",
        "--model",
        "--ctx-size",
        "--threads",
    ):
        if required_flag not in (help_output.stdout or ""):
            raise PreparationError("llama_server_cli_incompatible")


def _atomic_copy(source: Path, destination: Path, mode: int) -> None:
    _require_regular(source, "staged_source")
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        with source.open("rb") as input_stream, os.fdopen(
            descriptor, "wb"
        ) as output_stream:
            descriptor = -1
            shutil.copyfileobj(input_stream, output_stream, 1024 * 1024)
            output_stream.flush()
            os.fsync(output_stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, destination)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        temporary.unlink(missing_ok=True)


def _write_manifest(destination: Path) -> None:
    architecture_contract = {
        architecture: {"deployment_target": deployment_target}
        for architecture, deployment_target in ARCHITECTURES
    }
    document = {
        "architectures": architecture_contract,
        "backend": {
            "accelerate": True,
            "cpu": True,
            "curl": False,
            "llama_build_app": False,
            "llama_build_tools": True,
            "metal": False,
            "multimodal_video": False,
            "rpc": False,
            "shared_libraries": False,
            "web_ui": False,
        },
        "llama_cpp": {
            "archive_sha256": LLAMA_CPP_ARCHIVE_SHA256,
            "archive_url": LLAMA_CPP_ARCHIVE_URL,
            "built_targets": ["llama-server"],
            "tag": LLAMA_CPP_TAG,
        },
        "model": {"filename": MODEL_NAME, "sha256": NORMALIZED_MODEL_SHA256},
        "scorer": {
            "architectures": architecture_contract,
            "filename": SCORER_NAME,
        },
        "schema_version": "mozkey.macos_zenz_runtime.v1",
    }
    payload = (
        json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True)
        + "\n"
    ).encode("utf-8")
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = -1
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, destination)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        temporary.unlink(missing_ok=True)


def prepare_runtime(root: Path, archive: Path, output: Path) -> None:
    if sys.platform != "darwin":
        raise PreparationError("macos_required")
    root = root.resolve(strict=True)
    archive = _ensure_archive(archive)
    ninja = _find_bundled_ninja(root)

    normalized_model, digest, _ = normalize_zenz_gguf.create(root)
    normalize_zenz_gguf.verify(root)
    if digest != NORMALIZED_MODEL_SHA256:
        raise PreparationError("normalized_model_checksum_mismatch")

    with tempfile.TemporaryDirectory(prefix="mozkey-llama-") as temporary:
        work = Path(temporary)
        source = work / "source"
        source.mkdir()
        _run(
            [
                "/usr/bin/tar",
                "-xzf",
                str(archive),
                "-C",
                str(source),
                "--strip-components=1",
            ]
        )
        if not (source / "CMakeLists.txt").is_file():
            raise PreparationError("llama_archive_layout_invalid")

        architecture, deployment = ARCHITECTURES[0]
        server = _build_architecture(
            source, work / "build", architecture, deployment, ninja
        )
        os.chmod(server, 0o755)
        _verify_arm64_server(server)

        scorer = _build_scorer_architecture(
            root, work / "scorer", architecture, deployment
        )
        os.chmod(scorer, 0o755)
        _verify_arm64_scorer(scorer)

        _atomic_copy(server, output / "llama-server", 0o755)
        _atomic_copy(scorer, output / SCORER_NAME, 0o755)
        _atomic_copy(
            normalized_model,
            output / "models" / MODEL_NAME,
            0o644,
        )
        _write_manifest(output / "zenz-runtime-manifest.json")

    _verify_arm64_server(output / "llama-server")
    _verify_arm64_scorer(output / SCORER_NAME)
    if sha256_file(output / "models" / MODEL_NAME) != NORMALIZED_MODEL_SHA256:
        raise PreparationError("staged_model_checksum_mismatch")


def _parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build and stage the pinned arm64 macOS Zenz runtime"
    )
    parser.add_argument("--root", type=Path, default=REPOSITORY_ROOT)
    parser.add_argument(
        "--archive",
        type=Path,
        default=REPOSITORY_ROOT
        / "src/third_party_cache"
        / f"llama.cpp-{LLAMA_CPP_TAG}.tar.gz",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=REPOSITORY_ROOT / "src/mac/zenz_runtime/generated",
    )
    return parser.parse_args(list(argv))


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        prepare_runtime(arguments.root, arguments.archive, arguments.output)
    except (PreparationError, normalize_zenz_gguf.NormalizationError) as error:
        code = (
            str(error)
            if isinstance(error, PreparationError)
            else "model_normalization_failed"
        )
        print(f"macOS Zenz runtime preparation failed: {code}", file=sys.stderr)
        return 1
    except Exception:
        print(
            "macOS Zenz runtime preparation failed: unexpected_runtime_error",
            file=sys.stderr,
        )
        return 1
    print(f"macOS Zenz runtime prepared: {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
