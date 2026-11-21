#!/usr/bin/env python3
"""Build and verify the pinned Windows Zenz inference runtime.

The historical checked-in llama.cpp binaries do not carry enough source or
toolchain provenance to be release inputs.  Windows release jobs therefore
build a static llama-server from a checksum-pinned upstream archive and stage
it under an architecture-specific generated directory.  The generated
manifest travels in the MSI and records the exact binary digest and effective
CMake/toolchain contract.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from typing import Any, Mapping, Sequence


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))
MOZC_SOURCE_ROOT = REPOSITORY_ROOT / "src"
if str(MOZC_SOURCE_ROOT) not in sys.path:
    sys.path.insert(0, str(MOZC_SOURCE_ROOT))

from build_tools import vs_util  # noqa: E402
from tools.release import normalize_zenz_gguf  # noqa: E402


SCHEMA_VERSION = "mozkey.windows_zenz_runtime.v2"
LLAMA_CPP_TAG = "v1.0.0"
LLAMA_CPP_COMMIT = "b1b770d90d94cad9e9ea5b993bdbc383a522fb28"
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
NINJA_VERSION = "1.13.2"
NINJA_WIN_ARCHIVE_SHA256 = (
    "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"
)
LLVM_WIN_VERSION = "22.1.8"
LLVM_WIN_ARCHIVE_SHA256 = (
    "d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234"
)
NINJA_RELATIVE_PATH = Path("src/third_party/ninja/ninja.exe")
LLVM_BIN_RELATIVE_PATH = Path("src/third_party/llvm/bin")
ARCHITECTURES = {
    "x64": {
        "cmake_generator": None,
        "cmake_platform": "x64",
        "compiler_id": "MSVC",
        "compiler_target": None,
        "compiler_version": None,
        "pe_machine": 0x8664,
        "system_processors": ("AMD64", "x64", "x86_64"),
        "toolchain_file": None,
        "vcvars_arch": None,
    },
    "arm64": {
        "cmake_generator": "Ninja Multi-Config",
        "cmake_platform": None,
        "compiler_id": "Clang",
        "compiler_target": "arm64-pc-windows-msvc",
        "compiler_version": LLVM_WIN_VERSION,
        "pe_machine": 0xAA64,
        "system_processors": ("arm64",),
        "toolchain_file": "cmake/arm64-windows-llvm.cmake",
        "vcvars_arch": "amd64_arm64",
    },
}
MAX_ARCHIVE_BYTES = 128 * 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_BUILD_LOG_CHARS = 64 * 1024
MAX_COMPILE_COMMANDS_BYTES = 32 * 1024 * 1024
REQUIRED_CLI_FLAGS = (
    b"--api-key",
    b"--host",
    b"--port",
    b"--model",
    b"--ctx-size",
    b"--threads",
    # src/zenz_scorer always launches with `--parallel 1 --cache-ram 0`.
    b"--parallel",
    b"--cache-ram",
)
REQUIRED_CMAKE_BOOL_OPTIONS = {
    "BUILD_SHARED_LIBS": "OFF",
    "GGML_ACCELERATE": "OFF",
    "GGML_BACKEND_DL": "OFF",
    "GGML_BLAS": "OFF",
    "GGML_CPU": "ON",
    "GGML_CPU_ALL_VARIANTS": "OFF",
    "GGML_CPU_KLEIDIAI": "OFF",
    "GGML_CUDA": "OFF",
    "GGML_HIP": "OFF",
    "GGML_LLAMAFILE": "OFF",
    "GGML_METAL": "OFF",
    "GGML_MUSA": "OFF",
    "GGML_NATIVE": "OFF",
    "GGML_OPENCL": "OFF",
    "GGML_OPENMP": "OFF",
    "GGML_OPENVINO": "OFF",
    "GGML_RPC": "OFF",
    "GGML_STATIC": "ON",
    "GGML_SYCL": "OFF",
    "GGML_VULKAN": "OFF",
    "GGML_WEBGPU": "OFF",
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
LEGACY_CMAKE_OFF_OPTIONS = ("LLAMA_CURL",)
REQUIRED_CMAKE_STRING_OPTIONS = {
    "GGML_BUILD_COMMIT": LLAMA_CPP_COMMIT,
    "LLAMA_BUILD_COMMIT": LLAMA_CPP_COMMIT,
    "LLAMA_BUILD_NUMBER": LLAMA_CPP_BUILD_NUMBER,
}
REQUIRED_CMAKE_PATH_OPTIONS = {"GIT_EXE": "FALSE"}


class PreparationError(RuntimeError):
    """A redaction-safe Windows runtime preparation failure."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _require_regular(path: Path, label: str) -> os.stat_result:
    try:
        info = path.lstat()
    except OSError as error:
        raise PreparationError(f"{label}_missing") from error
    if not stat.S_ISREG(info.st_mode) or info.st_size <= 0:
        raise PreparationError(f"{label}_invalid")
    return info


def _verify_archive(path: Path) -> None:
    info = _require_regular(path, "llama_archive")
    if info.st_size > MAX_ARCHIVE_BYTES:
        raise PreparationError("llama_archive_too_large")
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
            headers={"User-Agent": "Mozkey-Windows-runtime-builder/1"},
        )
        total = 0
        with os.fdopen(descriptor, "wb") as output:
            descriptor = -1
            with urllib.request.urlopen(request, timeout=60) as response:
                if not response.geturl().startswith("https://"):
                    raise PreparationError("llama_archive_redirect_invalid")
                while chunk := response.read(1024 * 1024):
                    total += len(chunk)
                    if total > MAX_ARCHIVE_BYTES:
                        raise PreparationError("llama_archive_too_large")
                    output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        _verify_archive(temporary)
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
    path = path.resolve()
    if path.exists() or path.is_symlink():
        _verify_archive(path)
    else:
        _download_archive(path)
    return path


def _extract_archive(archive: Path, destination: Path) -> None:
    """Extracts regular files/directories while rejecting archive links."""
    destination.mkdir(parents=True, exist_ok=True)
    try:
        with tarfile.open(archive, "r:gz") as source:
            members = source.getmembers()
            roots = {
                Path(member.name).parts[0]
                for member in members
                if Path(member.name).parts
            }
            if len(roots) != 1:
                raise PreparationError("llama_archive_layout_invalid")
            root = next(iter(roots))
            for member in members:
                parts = Path(member.name).parts
                if not parts or parts[0] != root:
                    raise PreparationError("llama_archive_path_invalid")
                relative = Path(*parts[1:])
                if not relative.parts:
                    continue
                if relative.is_absolute() or ".." in relative.parts:
                    raise PreparationError("llama_archive_path_invalid")
                target = destination / relative
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                if not member.isfile():
                    raise PreparationError("llama_archive_entry_invalid")
                target.parent.mkdir(parents=True, exist_ok=True)
                input_stream = source.extractfile(member)
                if input_stream is None:
                    raise PreparationError("llama_archive_entry_invalid")
                with input_stream, target.open("wb") as output_stream:
                    shutil.copyfileobj(input_stream, output_stream, 1024 * 1024)
    except PreparationError:
        raise
    except (OSError, tarfile.TarError) as error:
        raise PreparationError("llama_archive_extract_failed") from error
    if not (destination / "CMakeLists.txt").is_file():
        raise PreparationError("llama_archive_layout_invalid")


def _run(
    command: Sequence[str],
    *,
    cwd: Path | None = None,
    environment: Mapping[str, str] | None = None,
    timeout: int = 1800,
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            list(command),
            cwd=cwd,
            check=True,
            env=environment,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        output = getattr(error, "stdout", None) or getattr(error, "output", None)
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        if output:
            print(
                "Windows Zenz runtime build output (tail):",
                file=sys.stderr,
            )
            print(str(output)[-MAX_BUILD_LOG_CHARS:], file=sys.stderr)
        raise PreparationError("runtime_build_command_failed") from error
    except OSError as error:
        raise PreparationError("runtime_build_command_failed") from error


def _pe_machine(path: Path) -> int:
    _require_regular(path, "llama_server")
    try:
        with path.open("rb") as stream:
            if stream.read(2) != b"MZ":
                raise PreparationError("llama_server_pe_invalid")
            stream.seek(0x3C)
            offset_data = stream.read(4)
            if len(offset_data) != 4:
                raise PreparationError("llama_server_pe_invalid")
            pe_offset = struct.unpack("<I", offset_data)[0]
            if pe_offset < 0x40 or pe_offset > 16 * 1024 * 1024:
                raise PreparationError("llama_server_pe_invalid")
            stream.seek(pe_offset)
            if stream.read(4) != b"PE\0\0":
                raise PreparationError("llama_server_pe_invalid")
            machine_data = stream.read(2)
            if len(machine_data) != 2:
                raise PreparationError("llama_server_pe_invalid")
            return struct.unpack("<H", machine_data)[0]
    except PreparationError:
        raise
    except OSError as error:
        raise PreparationError("llama_server_read_failed") from error


def _verify_cli_contract(path: Path) -> None:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise PreparationError("llama_server_read_failed") from error
    if any(flag not in data for flag in REQUIRED_CLI_FLAGS):
        raise PreparationError("llama_server_cli_incompatible")


def _load_manifest(path: Path) -> Mapping[str, Any]:
    info = _require_regular(path, "runtime_manifest")
    if info.st_size > MAX_MANIFEST_BYTES:
        raise PreparationError("runtime_manifest_too_large")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PreparationError("runtime_manifest_invalid") from error
    if not isinstance(value, dict):
        raise PreparationError("runtime_manifest_invalid")
    return value


def verify_runtime(output: Path, architecture: str) -> None:
    if architecture not in ARCHITECTURES:
        raise PreparationError("architecture_invalid")
    output = output.resolve()
    server = output / "llama-server.exe"
    model = output / "models" / MODEL_NAME
    manifest_path = output / "runtime-manifest.json"
    server_info = _require_regular(server, "llama_server")
    model_info = _require_regular(model, "zenz_model")
    if _pe_machine(server) != ARCHITECTURES[architecture]["pe_machine"]:
        raise PreparationError("llama_server_architecture_mismatch")
    _verify_cli_contract(server)
    if sha256_file(model) != NORMALIZED_MODEL_SHA256:
        raise PreparationError("zenz_model_checksum_mismatch")

    manifest = _load_manifest(manifest_path)
    if (
        set(manifest)
        != {
            "architecture",
            "cmake_options",
            "cmake_path_options",
            "cmake_string_options",
            "legacy_cmake_options_disabled",
            "llama_cpp",
            "llama_server",
            "model",
            "schema_version",
            "toolchain",
        }
        or manifest.get("schema_version") != SCHEMA_VERSION
        or manifest.get("architecture") != architecture
        or manifest.get("cmake_options") != REQUIRED_CMAKE_BOOL_OPTIONS
        or manifest.get("cmake_path_options") != REQUIRED_CMAKE_PATH_OPTIONS
        or manifest.get("legacy_cmake_options_disabled")
        != list(LEGACY_CMAKE_OFF_OPTIONS)
        or manifest.get("cmake_string_options")
        != REQUIRED_CMAKE_STRING_OPTIONS
        or manifest.get("llama_cpp")
        != {
            "archive_sha256": LLAMA_CPP_ARCHIVE_SHA256,
            "archive_url": LLAMA_CPP_ARCHIVE_URL,
            "built_targets": ["llama-server"],
            "commit": LLAMA_CPP_COMMIT,
            "tag": LLAMA_CPP_TAG,
        }
        or manifest.get("llama_server")
        != {
            "filename": "llama-server.exe",
            "pe_machine": f"0x{ARCHITECTURES[architecture]['pe_machine']:04x}",
            "sha256": sha256_file(server),
            "size_bytes": server_info.st_size,
            "static_runtime": True,
        }
        or manifest.get("model")
        != {
            "filename": f"models/{MODEL_NAME}",
            "sha256": NORMALIZED_MODEL_SHA256,
            "size_bytes": model_info.st_size,
        }
    ):
        raise PreparationError("runtime_manifest_contract_mismatch")
    toolchain = manifest.get("toolchain")
    expected_toolchain_keys = {
        "cmake",
        "compiler_id",
        "compiler_version",
        "generator",
        "system_processor",
        "windows_sdk_version",
    }
    if architecture == "x64":
        expected_toolchain_keys.add("generator_platform")
    else:
        expected_toolchain_keys.update(
            (
                "compiler_archive_sha256",
                "compiler_target",
                "ninja_archive_sha256",
                "ninja_version",
                "toolchain_file",
            )
        )
    if (
        not isinstance(toolchain, dict)
        or set(toolchain) != expected_toolchain_keys
        or not all(
            isinstance(toolchain.get(key), str) and toolchain[key]
            for key in expected_toolchain_keys
        )
    ):
        raise PreparationError("runtime_manifest_toolchain_invalid")
    contract = ARCHITECTURES[architecture]
    if (
        toolchain.get("compiler_id") != contract["compiler_id"]
        or toolchain.get("system_processor")
        not in contract["system_processors"]
        or re.fullmatch(r"\d+\.\d+(?:\.\d+){1,2}", toolchain["windows_sdk_version"])
        is None
    ):
        raise PreparationError("runtime_manifest_toolchain_invalid")
    if architecture == "x64":
        if (
            not toolchain["generator"].startswith("Visual Studio ")
            or toolchain.get("generator_platform") != contract["cmake_platform"]
        ):
            raise PreparationError("runtime_manifest_toolchain_invalid")
    elif (
        toolchain.get("generator") != contract["cmake_generator"]
        or toolchain.get("compiler_version") != contract["compiler_version"]
        or toolchain.get("compiler_target") != contract["compiler_target"]
        or toolchain.get("compiler_archive_sha256")
        != LLVM_WIN_ARCHIVE_SHA256
        or toolchain.get("ninja_archive_sha256") != NINJA_WIN_ARCHIVE_SHA256
        or toolchain.get("ninja_version") != NINJA_VERSION
        or toolchain.get("toolchain_file") != contract["toolchain_file"]
    ):
        raise PreparationError("runtime_manifest_toolchain_invalid")


def _cache_value(cache: str, name: str) -> str:
    prefix = f"{name}:"
    for line in cache.splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1].strip()
    raise PreparationError("runtime_toolchain_metadata_missing")


def _cmake_cache_values(cache: str) -> dict[str, tuple[str, str]]:
    values: dict[str, tuple[str, str]] = {}
    for line in cache.splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        if ":" not in key_and_type:
            continue
        key, value_type = key_and_type.split(":", 1)
        values[key] = (value_type, value)
    return values


def _same_file(
    left: str | os.PathLike[str], right: str | os.PathLike[str]
) -> bool:
    try:
        return os.path.samefile(left, right)
    except OSError:
        return os.path.normcase(os.path.abspath(left)) == os.path.normcase(
            os.path.abspath(right)
        )


def _verify_cmake_cache(
    cache: str,
    architecture: str,
    source: Path | None = None,
    ninja: Path | None = None,
) -> None:
    values = _cmake_cache_values(cache)
    for key, expected in REQUIRED_CMAKE_BOOL_OPTIONS.items():
        if values.get(key) != ("BOOL", expected):
            raise PreparationError("cmake_backend_contract_invalid")
    for key in LEGACY_CMAKE_OFF_OPTIONS:
        if values.get(key) not in {("BOOL", "OFF"), ("UNINITIALIZED", "OFF")}:
            raise PreparationError("cmake_backend_contract_invalid")
    for key, expected in REQUIRED_CMAKE_STRING_OPTIONS.items():
        if values.get(key) not in {
            ("STRING", expected),
            ("UNINITIALIZED", expected),
        }:
            raise PreparationError("cmake_backend_contract_invalid")
    for key, expected in REQUIRED_CMAKE_PATH_OPTIONS.items():
        if values.get(key) != ("FILEPATH", expected):
            raise PreparationError("cmake_backend_contract_invalid")
    contract = ARCHITECTURES[architecture]
    generator = _cache_value(cache, "CMAKE_GENERATOR")
    if architecture == "x64":
        if (
            not generator.startswith("Visual Studio ")
            or _cache_value(cache, "CMAKE_GENERATOR_PLATFORM")
            != contract["cmake_platform"]
        ):
            raise PreparationError("cmake_architecture_contract_invalid")
        return
    if (
        generator != contract["cmake_generator"]
        or source is None
        or ninja is None
    ):
        raise PreparationError("cmake_generator_contract_invalid")
    toolchain = values.get("CMAKE_TOOLCHAIN_FILE")
    expected_toolchain = source / str(contract["toolchain_file"])
    if (
        toolchain is None
        or not toolchain[1]
        or not _same_file(toolchain[1], expected_toolchain)
    ):
        raise PreparationError("cmake_toolchain_file_contract_invalid")
    make_program = values.get("CMAKE_MAKE_PROGRAM")
    if (
        make_program is None
        or not make_program[1]
        or not _same_file(make_program[1], ninja)
    ):
        raise PreparationError("cmake_make_program_contract_invalid")


def _find_server(build: Path) -> Path:
    candidates = [
        build / "bin" / "Release" / "llama-server.exe",
        build / "bin" / "llama-server.exe",
    ]
    existing = [path for path in candidates if path.is_file()]
    if len(existing) != 1:
        raise PreparationError("llama_server_build_output_invalid")
    return existing[0]


def _cmake_set_value(content: str, name: str, error_code: str) -> str:
    match = re.search(
        rf'^set\({re.escape(name)}\s+(?:"([^"]+)"|([^\s\)]+))\s*\)$',
        content,
        flags=re.MULTILINE,
    )
    if match is None:
        raise PreparationError(error_code)
    return match.group(1) or match.group(2)


def _compiler_metadata(build: Path, architecture: str) -> dict[str, str]:
    candidates = list(build.glob("CMakeFiles/*/CMakeCXXCompiler.cmake"))
    if len(candidates) != 1:
        raise PreparationError("runtime_compiler_metadata_missing")
    try:
        content = candidates[0].read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise PreparationError("runtime_compiler_metadata_missing") from error
    values = {}
    for output_name, cmake_name in (
        ("compiler_id", "CMAKE_CXX_COMPILER_ID"),
        ("compiler_version", "CMAKE_CXX_COMPILER_VERSION"),
    ):
        values[output_name] = _cmake_set_value(
            content, cmake_name, "runtime_compiler_metadata_missing"
        )
    contract = ARCHITECTURES[architecture]
    if values["compiler_id"] != contract["compiler_id"]:
        raise PreparationError("runtime_toolchain_invalid")
    if (
        contract["compiler_version"] is not None
        and values["compiler_version"] != contract["compiler_version"]
    ):
        raise PreparationError("runtime_toolchain_invalid")
    if contract["compiler_target"] is not None:
        compile_commands = build / "compile_commands.json"
        info = _require_regular(compile_commands, "compile_commands")
        if info.st_size > MAX_COMPILE_COMMANDS_BYTES:
            raise PreparationError("compile_commands_too_large")
        try:
            commands = compile_commands.read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError as error:
            raise PreparationError("compile_commands_read_failed") from error
        expected_flag = f"--target={contract['compiler_target']}"
        if expected_flag not in commands:
            raise PreparationError("runtime_compiler_target_invalid")
        values["compiler_target"] = str(contract["compiler_target"])
    return values


def _system_metadata(build: Path, architecture: str) -> dict[str, str]:
    candidates = list(build.glob("CMakeFiles/*/CMakeSystem.cmake"))
    if len(candidates) != 1:
        raise PreparationError("runtime_system_metadata_missing")
    try:
        content = candidates[0].read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise PreparationError("runtime_system_metadata_missing") from error
    processor = _cmake_set_value(
        content, "CMAKE_SYSTEM_PROCESSOR", "runtime_system_metadata_missing"
    )
    if processor not in ARCHITECTURES[architecture]["system_processors"]:
        raise PreparationError("runtime_system_metadata_invalid")
    return {"processor": processor}


def _windows_sdk_version(
    build: Path,
    architecture: str,
    environment: Mapping[str, str] | None,
) -> str:
    if architecture == "x64":
        candidates = list(build.rglob("llama-server.vcxproj"))
        if len(candidates) != 1:
            raise PreparationError("windows_sdk_metadata_missing")
        try:
            project = candidates[0].read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError as error:
            raise PreparationError("windows_sdk_metadata_missing") from error
        match = re.search(
            r"<WindowsTargetPlatformVersion>([^<]+)"
            r"</WindowsTargetPlatformVersion>",
            project,
        )
        version = match.group(1).strip() if match else ""
    else:
        normalized = {
            key.upper(): value for key, value in (environment or {}).items()
        }
        version = normalized.get("WINDOWSSDKVERSION", "").strip("\\/")
    if re.fullmatch(r"\d+\.\d+(?:\.\d+){1,2}", version) is None:
        raise PreparationError("windows_sdk_metadata_invalid")
    return version


def _write_manifest(
    destination: Path,
    architecture: str,
    server: Path,
    model: Path,
    cache: str,
    cmake_version: str,
    compiler: Mapping[str, str],
    system: Mapping[str, str],
) -> None:
    contract = ARCHITECTURES[architecture]
    toolchain = {
        "cmake": cmake_version.splitlines()[0].strip(),
        "compiler_id": compiler["compiler_id"],
        "compiler_version": compiler["compiler_version"],
        "generator": _cache_value(cache, "CMAKE_GENERATOR"),
        "system_processor": system["processor"],
        "windows_sdk_version": system["windows_sdk_version"],
    }
    if architecture == "x64":
        toolchain["generator_platform"] = _cache_value(
            cache, "CMAKE_GENERATOR_PLATFORM"
        )
    else:
        toolchain["compiler_archive_sha256"] = LLVM_WIN_ARCHIVE_SHA256
        toolchain["compiler_target"] = compiler["compiler_target"]
        toolchain["ninja_archive_sha256"] = NINJA_WIN_ARCHIVE_SHA256
        toolchain["ninja_version"] = NINJA_VERSION
        toolchain["toolchain_file"] = str(contract["toolchain_file"])
    document = {
        "architecture": architecture,
        "cmake_options": REQUIRED_CMAKE_BOOL_OPTIONS,
        "cmake_path_options": REQUIRED_CMAKE_PATH_OPTIONS,
        "cmake_string_options": REQUIRED_CMAKE_STRING_OPTIONS,
        "legacy_cmake_options_disabled": list(LEGACY_CMAKE_OFF_OPTIONS),
        "llama_cpp": {
            "archive_sha256": LLAMA_CPP_ARCHIVE_SHA256,
            "archive_url": LLAMA_CPP_ARCHIVE_URL,
            "built_targets": ["llama-server"],
            "commit": LLAMA_CPP_COMMIT,
            "tag": LLAMA_CPP_TAG,
        },
        "llama_server": {
            "filename": "llama-server.exe",
            "pe_machine": f"0x{ARCHITECTURES[architecture]['pe_machine']:04x}",
            "sha256": sha256_file(server),
            "size_bytes": server.stat().st_size,
            "static_runtime": True,
        },
        "model": {
            "filename": f"models/{MODEL_NAME}",
            "sha256": sha256_file(model),
            "size_bytes": model.stat().st_size,
        },
        "schema_version": SCHEMA_VERSION,
        "toolchain": toolchain,
    }
    payload = (
        json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True)
        + "\n"
    )
    destination.write_text(payload, encoding="utf-8", newline="\n")


def _build_environment(
    architecture: str, root: Path = REPOSITORY_ROOT
) -> Mapping[str, str] | None:
    vcvars_arch = ARCHITECTURES[architecture]["vcvars_arch"]
    if vcvars_arch is None:
        return None
    try:
        environment = vs_util.get_vs_env_vars(str(vcvars_arch))
    except (ChildProcessError, OSError, ValueError) as error:
        raise PreparationError("visual_studio_environment_invalid") from error
    normalized = {
        key.upper(): value for key, value in environment.items() if value
    }
    if not {
        "PATH",
        "WINDOWSSDKDIR",
        "WINDOWSSDKVERSION",
        "VCTOOLSINSTALLDIR",
    }.issubset(normalized):
        raise PreparationError("visual_studio_environment_invalid")
    llvm_bin = root / LLVM_BIN_RELATIVE_PATH
    _require_regular(llvm_bin / "clang.exe", "pinned_clang")
    _require_regular(llvm_bin / "clang++.exe", "pinned_clangxx")
    path_key = next(
        key for key in environment if key.upper() == "PATH"
    )
    environment[path_key] = f"{llvm_bin}{os.pathsep}{environment[path_key]}"
    return environment


def _pinned_ninja(
    root: Path, environment: Mapping[str, str] | None
) -> Path:
    ninja = (root / NINJA_RELATIVE_PATH).resolve()
    _require_regular(ninja, "pinned_ninja")
    version = _run(
        [str(ninja), "--version"], environment=environment, timeout=30
    ).stdout.strip()
    if version != NINJA_VERSION:
        raise PreparationError("pinned_ninja_version_mismatch")
    return ninja


def _configure_command(
    source: Path,
    build: Path,
    architecture: str,
    ninja: Path | None = None,
) -> list[str]:
    contract = ARCHITECTURES[architecture]
    command = ["cmake", "-S", str(source), "-B", str(build)]
    if contract["cmake_generator"] is not None:
        command.extend(("-G", str(contract["cmake_generator"])))
    if contract["cmake_platform"] is not None:
        command.extend(("-A", str(contract["cmake_platform"])))
    if contract["toolchain_file"] is not None:
        toolchain = (source / str(contract["toolchain_file"])).resolve(
            strict=True
        )
        command.append(f"-DCMAKE_TOOLCHAIN_FILE:FILEPATH={toolchain}")
        if ninja is None:
            raise PreparationError("pinned_ninja_missing")
        command.append(f"-DCMAKE_MAKE_PROGRAM:FILEPATH={ninja}")
    command.extend(
        f"-D{name}={value}"
        for name, value in REQUIRED_CMAKE_BOOL_OPTIONS.items()
    )
    command.extend(
        f"-D{name}={value}"
        for name, value in REQUIRED_CMAKE_STRING_OPTIONS.items()
    )
    command.extend(
        f"-D{name}:FILEPATH={value}"
        for name, value in REQUIRED_CMAKE_PATH_OPTIONS.items()
    )
    command.extend(f"-D{name}=OFF" for name in LEGACY_CMAKE_OFF_OPTIONS)
    return command


def prepare_runtime(
    root: Path, archive: Path, output: Path, architecture: str
) -> None:
    if sys.platform != "win32":
        raise PreparationError("windows_required")
    if architecture not in ARCHITECTURES:
        raise PreparationError("architecture_invalid")
    root = root.resolve(strict=True)
    archive = _ensure_archive(archive)
    normalized_model, digest, _ = normalize_zenz_gguf.create(root)
    normalize_zenz_gguf.verify(root)
    if digest != NORMALIZED_MODEL_SHA256:
        raise PreparationError("zenz_model_checksum_mismatch")

    with tempfile.TemporaryDirectory(prefix="mozkey-llama-") as name:
        work = Path(name)
        source = work / "source"
        build = work / "build"
        _extract_archive(archive, source)
        environment = _build_environment(architecture, root)
        ninja = (
            _pinned_ninja(root, environment)
            if architecture == "arm64"
            else None
        )
        _run(
            _configure_command(source, build, architecture, ninja),
            environment=environment,
        )
        _run(
            [
                "cmake",
                "--build",
                str(build),
                "--config",
                "Release",
                "--target",
                "llama-server",
                "--parallel",
            ],
            environment=environment,
        )
        server = _find_server(build)
        if _pe_machine(server) != ARCHITECTURES[architecture]["pe_machine"]:
            raise PreparationError("llama_server_architecture_mismatch")
        _verify_cli_contract(server)

        staged = output.resolve()
        staged.mkdir(parents=True, exist_ok=True)
        (staged / "models").mkdir(parents=True, exist_ok=True)
        shutil.copyfile(server, staged / "llama-server.exe")
        shutil.copyfile(normalized_model, staged / "models" / MODEL_NAME)
        cache = (build / "CMakeCache.txt").read_text(
            encoding="utf-8", errors="replace"
        )
        _verify_cmake_cache(cache, architecture, source, ninja)
        cmake_version = _run(
            ["cmake", "--version"], environment=environment, timeout=30
        ).stdout
        system = _system_metadata(build, architecture)
        system["windows_sdk_version"] = _windows_sdk_version(
            build, architecture, environment
        )
        _write_manifest(
            staged / "runtime-manifest.json",
            architecture,
            staged / "llama-server.exe",
            staged / "models" / MODEL_NAME,
            cache,
            cmake_version,
            _compiler_metadata(build, architecture),
            system,
        )
        verify_runtime(staged, architecture)


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build or verify the pinned Windows Zenz runtime"
    )
    parser.add_argument("--arch", choices=sorted(ARCHITECTURES), required=True)
    parser.add_argument("--root", type=Path, default=REPOSITORY_ROOT)
    parser.add_argument(
        "--archive",
        type=Path,
        default=REPOSITORY_ROOT / "src/third_party_cache/llama.cpp-v1.0.0.tar.gz",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parse_args(sys.argv[1:] if argv is None else argv)
    output = arguments.output or (
        arguments.root
        / "src/win32/installer/zenz_runtime/generated"
        / arguments.arch
    )
    try:
        if arguments.verify_only:
            verify_runtime(output, arguments.arch)
        else:
            prepare_runtime(
                arguments.root, arguments.archive, output, arguments.arch
            )
        print(f"Windows Zenz runtime verified: {output} ({arguments.arch})")
        return 0
    except (PreparationError, normalize_zenz_gguf.NormalizationError) as error:
        print(f"Windows Zenz runtime preparation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
