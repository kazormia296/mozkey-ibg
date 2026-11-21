#!/usr/bin/env python3
"""Build a pinned, CPU-only x86_64 llama-server for Linux packages."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import stat
import sys
import tempfile
from typing import Iterable, Sequence

try:
    from tools.release import prepare_macos_zenz_runtime as common
except ModuleNotFoundError:  # Direct execution sets sys.path to tools/release.
    import prepare_macos_zenz_runtime as common


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIR = REPOSITORY_ROOT / "dist/zenz/linux/runtime"
SERVER_NAME = "llama-server"
MANIFEST_NAME = "llama-server-manifest.json"
SCHEMA = "mozkey.linux_llama_server.v1"
ARCHITECTURE = "x86_64"
REQUIRED_BOOL_OPTIONS = {
    "BUILD_SHARED_LIBS": "OFF",
    "GGML_ACCELERATE": "OFF",
    "GGML_AVX": "OFF",
    "GGML_AVX2": "OFF",
    "GGML_AVX512": "OFF",
    "GGML_AVX_VNNI": "OFF",
    "GGML_BACKEND_DL": "OFF",
    "GGML_BLAS": "OFF",
    "GGML_BMI2": "OFF",
    "GGML_CPU": "ON",
    "GGML_F16C": "OFF",
    "GGML_FMA": "OFF",
    "GGML_LLAMAFILE": "OFF",
    "GGML_NATIVE": "OFF",
    "GGML_OPENMP": "OFF",
    "GGML_RPC": "OFF",
    "GGML_SSE42": "OFF",
    "GGML_STATIC": "ON",
    "LLAMA_BUILD_APP": "OFF",
    "LLAMA_BUILD_EXAMPLES": "OFF",
    "LLAMA_BUILD_SERVER": "ON",
    "LLAMA_BUILD_TESTS": "OFF",
    "LLAMA_BUILD_TOOLS": "ON",
    "LLAMA_BUILD_UI": "OFF",
    "LLAMA_OPENSSL": "OFF",
    "LLAMA_USE_PREBUILT_UI": "OFF",
    "MTMD_VIDEO": "OFF",
}


class PreparationError(RuntimeError):
    """The pinned Linux runtime could not be built or verified."""


def cmake_configure_command(
    source: Path, build: Path, ninja: Path
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
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DGGML_STATIC=ON",
        "-DGGML_BACKEND_DL=OFF",
        "-DGGML_NATIVE=OFF",
        "-DGGML_SSE42=OFF",
        "-DGGML_AVX=OFF",
        "-DGGML_AVX_VNNI=OFF",
        "-DGGML_AVX2=OFF",
        "-DGGML_AVX512=OFF",
        "-DGGML_F16C=OFF",
        "-DGGML_FMA=OFF",
        "-DGGML_BMI2=OFF",
        "-DGGML_OPENMP=OFF",
        "-DGGML_LLAMAFILE=OFF",
        "-DGGML_CPU=ON",
        "-DGGML_ACCELERATE=OFF",
        "-DGGML_BLAS=OFF",
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
        f"-DLLAMA_BUILD_NUMBER={common.LLAMA_CPP_BUILD_NUMBER}",
        f"-DLLAMA_BUILD_COMMIT={common.LLAMA_CPP_TAG}",
        f"-DGGML_BUILD_COMMIT={common.LLAMA_CPP_TAG}",
    ]


def _cache_values(cache: Path) -> dict[str, tuple[str, str]]:
    try:
        lines = cache.read_text(encoding="utf-8").splitlines()
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
    return values


def _verify_cmake_cache(build: Path) -> None:
    values = _cache_values(build / "CMakeCache.txt")
    for key, expected in REQUIRED_BOOL_OPTIONS.items():
        if values.get(key) != ("BOOL", expected):
            raise PreparationError("cmake_backend_contract_invalid")
    if values.get("LLAMA_CURL") not in {
        ("BOOL", "OFF"),
        ("UNINITIALIZED", "OFF"),
    }:
        raise PreparationError("cmake_backend_contract_invalid")


def _require_executable(path: Path) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise PreparationError("llama_server_missing") from error
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_size <= 0
        or not os.access(path, os.X_OK)
    ):
        raise PreparationError("llama_server_invalid")


def _run(
    command: Sequence[str],
    *,
    cwd: Path | None = None,
    capture: bool = False,
    timeout: int | None = None,
):
    try:
        return common._run(
            command, cwd=cwd, capture=capture, timeout=timeout
        )
    except common.PreparationError as error:
        raise PreparationError("runtime_build_command_failed") from error


def verify_server(path: Path) -> None:
    _require_executable(path)
    elf = _run(["readelf", "-h", str(path)], capture=True, timeout=20)
    if "Machine:" not in elf.stdout or "X86-64" not in elf.stdout:
        raise PreparationError("llama_server_architecture_invalid")
    dependencies = _run(["ldd", str(path)], capture=True, timeout=20)
    dependency_text = dependencies.stdout or ""
    if any(
        forbidden in dependency_text
        for forbidden in ("not found", "libcurl", "libggml", "libllama")
    ):
        raise PreparationError("llama_server_dynamic_dependency_invalid")
    help_output = _run([str(path), "--help"], capture=True, timeout=20)
    for required_flag in (
        "--api-key",
        "--host",
        "--port",
        "--model",
        "--ctx-size",
        "--threads",
        # src/zenz_scorer always launches with `--parallel 1 --cache-ram 0`.
        "--parallel",
        "--cache-ram",
    ):
        if required_flag not in (help_output.stdout or ""):
            raise PreparationError("llama_server_cli_incompatible")


def _write_manifest(destination: Path, server: Path) -> None:
    document = {
        "architecture": ARCHITECTURE,
        "backend": {
            "blas": False,
            "cpu": True,
            "curl": False,
            "extended_x86_instruction_sets": False,
            "native_cpu_tuning": False,
            "openmp": False,
            "rpc": False,
            "shared_libraries": False,
            "web_ui": False,
        },
        "llama_cpp": {
            "archive_sha256": common.LLAMA_CPP_ARCHIVE_SHA256,
            "archive_url": common.LLAMA_CPP_ARCHIVE_URL,
            "built_targets": [SERVER_NAME],
            "tag": common.LLAMA_CPP_TAG,
        },
        "schema_version": SCHEMA,
        "server": {
            "filename": SERVER_NAME,
            "sha256": common.sha256_file(server),
            "size_bytes": server.stat().st_size,
        },
    }
    payload = (
        json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
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
    if not sys.platform.startswith("linux"):
        raise PreparationError("linux_required")
    if platform.machine() not in {"x86_64", "amd64"}:
        raise PreparationError("x86_64_required")
    root = root.resolve(strict=True)
    try:
        archive = common._ensure_archive(archive)
        ninja = common._find_bundled_ninja(root)
    except common.PreparationError as error:
        raise PreparationError(str(error)) from error

    with tempfile.TemporaryDirectory(prefix="mozkey-linux-llama-") as name:
        work = Path(name)
        source = work / "source"
        source.mkdir()
        _run(
            [
                "tar",
                "-xzf",
                str(archive),
                "-C",
                str(source),
                "--strip-components=1",
            ],
            timeout=60,
        )
        if not (source / "CMakeLists.txt").is_file():
            raise PreparationError("llama_archive_layout_invalid")
        build = work / "build"
        _run(cmake_configure_command(source, build, ninja), timeout=600)
        _verify_cmake_cache(build)
        _run(
            [
                "cmake",
                "--build",
                str(build),
                "--target",
                SERVER_NAME,
                "--parallel",
            ],
            timeout=1800,
        )
        try:
            server = common._find_built_server(build)
        except common.PreparationError as error:
            raise PreparationError(str(error)) from error
        os.chmod(server, 0o755)
        verify_server(server)
        common._atomic_copy(server, output / SERVER_NAME, 0o755)

    verify_server(output / SERVER_NAME)
    _write_manifest(output / MANIFEST_NAME, output / SERVER_NAME)


def _parse_args(argv: Iterable[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the pinned CPU-only x86_64 Linux llama-server"
    )
    parser.add_argument("--root", type=Path, default=REPOSITORY_ROOT)
    parser.add_argument(
        "--archive",
        type=Path,
        default=REPOSITORY_ROOT
        / "src/third_party_cache"
        / f"llama.cpp-{common.LLAMA_CPP_TAG}.tar.gz",
    )
    parser.add_argument("--output", type=Path, default=OUTPUT_DIR)
    return parser.parse_args(list(argv))


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        prepare_runtime(arguments.root, arguments.archive, arguments.output)
    except (PreparationError, common.PreparationError) as error:
        print(f"Linux Zenz runtime preparation failed: {error}", file=sys.stderr)
        return 1
    except Exception:
        print(
            "Linux Zenz runtime preparation failed: unexpected_runtime_error",
            file=sys.stderr,
        )
        return 1
    print(f"Linux Zenz runtime prepared: {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
