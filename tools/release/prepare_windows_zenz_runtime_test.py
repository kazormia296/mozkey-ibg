#!/usr/bin/env python3

from __future__ import annotations

import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

from tools.release import prepare_windows_zenz_runtime as target


def _write_fake_server(path: Path, machine: int) -> None:
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, machine)
    data.extend(b"\0".join(target.REQUIRED_CLI_FLAGS))
    path.write_bytes(data)


class PrepareWindowsZenzRuntimeTest(unittest.TestCase):
    def test_pe_machine_accepts_x64_and_arm64(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for architecture, contract in target.ARCHITECTURES.items():
                server = root / f"{architecture}.exe"
                _write_fake_server(server, contract["pe_machine"])
                self.assertEqual(target._pe_machine(server), contract["pe_machine"])

    def test_pe_machine_rejects_non_pe(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            server = Path(name) / "server.exe"
            server.write_bytes(b"not a PE")
            with self.assertRaises(target.PreparationError):
                target._pe_machine(server)

    def test_verify_rejects_cross_architecture_server(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            output = Path(name)
            (output / "models").mkdir()
            _write_fake_server(output / "llama-server.exe", 0x8664)
            (output / "models" / target.MODEL_NAME).write_bytes(b"model")
            (output / "runtime-manifest.json").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(
                target.PreparationError, "architecture_mismatch"
            ):
                target.verify_runtime(output, "arm64")

    def test_manifest_contract_is_exact(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            server = root / "llama-server.exe"
            model = root / "model.gguf"
            manifest = root / "runtime-manifest.json"
            _write_fake_server(server, 0x8664)
            model.write_bytes(b"model")
            cache = "\n".join(
                (
                    "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026",
                    "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64",
                )
            )
            target._write_manifest(
                manifest,
                "x64",
                server,
                model,
                cache,
                "cmake version 4.0.0",
                {"compiler_id": "MSVC", "compiler_version": "19.50.12345"},
                {
                    "processor": "AMD64",
                    "windows_sdk_version": "10.0.26100.0",
                },
            )
            document = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertEqual(document["schema_version"], target.SCHEMA_VERSION)
            self.assertEqual(document["llama_cpp"]["tag"], "v1.0.0")
            self.assertEqual(
                document["llama_cpp"]["commit"],
                "b1b770d90d94cad9e9ea5b993bdbc383a522fb28",
            )
            self.assertEqual(
                document["cmake_string_options"]["LLAMA_BUILD_COMMIT"],
                "b1b770d90d94cad9e9ea5b993bdbc383a522fb28",
            )
            self.assertEqual(
                document["cmake_path_options"],
                {"GIT_EXE": "FALSE"},
            )
            self.assertEqual(document["architecture"], "x64")
            self.assertEqual(document["llama_server"]["pe_machine"], "0x8664")
            self.assertTrue(document["llama_server"]["static_runtime"])
            self.assertEqual(
                document["toolchain"]["compiler_version"], "19.50.12345"
            )
            self.assertEqual(
                document["toolchain"]["windows_sdk_version"],
                "10.0.26100.0",
            )

    def test_arm64_manifest_records_pinned_cross_toolchain(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            server = root / "llama-server.exe"
            model = root / "model.gguf"
            manifest = root / "runtime-manifest.json"
            _write_fake_server(server, 0xAA64)
            model.write_bytes(b"model")
            target._write_manifest(
                manifest,
                "arm64",
                server,
                model,
                "CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config\n",
                "cmake version 4.0.0",
                {
                    "compiler_id": "Clang",
                    "compiler_target": "arm64-pc-windows-msvc",
                    "compiler_version": "22.1.8",
                },
                {
                    "processor": "arm64",
                    "windows_sdk_version": "10.0.26100.0",
                },
            )
            toolchain = json.loads(
                manifest.read_text(encoding="utf-8")
            )["toolchain"]
            self.assertEqual(
                set(toolchain),
                {
                    "cmake",
                    "compiler_archive_sha256",
                    "compiler_id",
                    "compiler_target",
                    "compiler_version",
                    "generator",
                    "ninja_archive_sha256",
                    "ninja_version",
                    "system_processor",
                    "toolchain_file",
                    "windows_sdk_version",
                },
            )
            self.assertEqual(toolchain["ninja_version"], "1.13.2")
            self.assertEqual(
                toolchain["toolchain_file"],
                "cmake/arm64-windows-llvm.cmake",
            )
            self.assertNotIn("generator_platform", toolchain)

    def test_compiler_metadata_reads_exact_msvc_version(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            compiler_file = (
                Path(name) / "CMakeFiles/4.0.0/CMakeCXXCompiler.cmake"
            )
            compiler_file.parent.mkdir(parents=True)
            compiler_file.write_text(
                'set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                'set(CMAKE_CXX_COMPILER_VERSION "19.50.12345")\n',
                encoding="utf-8",
            )
            self.assertEqual(
                target._compiler_metadata(Path(name), "x64"),
                {"compiler_id": "MSVC", "compiler_version": "19.50.12345"},
            )

    def test_compiler_metadata_requires_arm64_clang_target(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            compiler_file = (
                Path(name) / "CMakeFiles/4.0.0/CMakeCXXCompiler.cmake"
            )
            compiler_file.parent.mkdir(parents=True)
            compiler_file.write_text(
                'set(CMAKE_CXX_COMPILER_ID "Clang")\n'
                'set(CMAKE_CXX_COMPILER_VERSION "22.1.8")\n',
                encoding="utf-8",
            )
            (Path(name) / "compile_commands.json").write_text(
                '[{"command":"clang++ --target=arm64-pc-windows-msvc"}]',
                encoding="utf-8",
            )
            self.assertEqual(
                target._compiler_metadata(Path(name), "arm64"),
                {
                    "compiler_id": "Clang",
                    "compiler_target": "arm64-pc-windows-msvc",
                    "compiler_version": "22.1.8",
                },
            )

            (Path(name) / "compile_commands.json").write_text(
                '[{"command":"clang++ --target=x86_64-pc-windows-msvc"}]',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                target.PreparationError, "compiler_target_invalid"
            ):
                target._compiler_metadata(Path(name), "arm64")

    def test_system_metadata_reads_generated_cmake_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            system_file = Path(name) / "CMakeFiles/4.0.0/CMakeSystem.cmake"
            system_file.parent.mkdir(parents=True)
            system_file.write_text(
                'set(CMAKE_SYSTEM_PROCESSOR "arm64")\n'
                'set(CMAKE_SYSTEM_VERSION "10.0.26100.0")\n',
                encoding="utf-8",
            )
            self.assertEqual(
                target._system_metadata(Path(name), "arm64"),
                {"processor": "arm64"},
            )

    def test_windows_sdk_metadata_is_architecture_specific(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            build = Path(name)
            project = build / "examples/server/llama-server.vcxproj"
            project.parent.mkdir(parents=True)
            project.write_text(
                "<PropertyGroup>"
                "<WindowsTargetPlatformVersion>10.0.26100.0"
                "</WindowsTargetPlatformVersion>"
                "</PropertyGroup>",
                encoding="utf-8",
            )
            self.assertEqual(
                target._windows_sdk_version(build, "x64", None),
                "10.0.26100.0",
            )
            self.assertEqual(
                target._windows_sdk_version(
                    build,
                    "arm64",
                    {"WindowsSDKVersion": "10.0.26100.0\\"},
                ),
                "10.0.26100.0",
            )

    def test_cmake_cache_contract_rejects_wrong_architecture(self) -> None:
        entries = [
            *(
                f"{name}:BOOL={value}"
                for name, value in target.REQUIRED_CMAKE_BOOL_OPTIONS.items()
            ),
            *(
                f"{name}:UNINITIALIZED=OFF"
                for name in target.LEGACY_CMAKE_OFF_OPTIONS
            ),
            *(
                f"{name}:UNINITIALIZED={value}"
                for name, value in target.REQUIRED_CMAKE_STRING_OPTIONS.items()
            ),
            *(
                f"{name}:FILEPATH={value}"
                for name, value in target.REQUIRED_CMAKE_PATH_OPTIONS.items()
            ),
            "CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config",
        ]
        with tempfile.TemporaryDirectory() as name:
            source = Path(name)
            toolchain = source / "cmake/arm64-windows-llvm.cmake"
            toolchain.parent.mkdir()
            toolchain.write_text("toolchain", encoding="utf-8")
            entries.append(f"CMAKE_TOOLCHAIN_FILE:FILEPATH={toolchain}")
            ninja = source / "ninja.exe"
            ninja.write_text("ninja", encoding="utf-8")
            entries.append(f"CMAKE_MAKE_PROGRAM:FILEPATH={ninja}")
            cache = "\n".join(entries)
            target._verify_cmake_cache(cache, "arm64", source, ninja)
            with self.assertRaisesRegex(
                target.PreparationError, "architecture_contract"
            ):
                target._verify_cmake_cache(cache, "x64", source)

    def test_arm64_configure_uses_pinned_llvm_toolchain(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = root / "source"
            build = root / "build"
            toolchain = source / "cmake/arm64-windows-llvm.cmake"
            toolchain.parent.mkdir(parents=True)
            toolchain.write_text("toolchain", encoding="utf-8")
            ninja = root / "ninja.exe"
            ninja.write_text("ninja", encoding="utf-8")
            command = target._configure_command(
                source, build, "arm64", ninja
            )
            self.assertIn("Ninja Multi-Config", command)
            self.assertNotIn("-A", command)
            self.assertIn(
                f"-DCMAKE_TOOLCHAIN_FILE:FILEPATH={toolchain.resolve()}",
                command,
            )
            self.assertIn(f"-DCMAKE_MAKE_PROGRAM:FILEPATH={ninja}", command)
            self.assertIn("-DGGML_CPU_ALL_VARIANTS=OFF", command)
            self.assertIn("-DGGML_BACKEND_DL=OFF", command)
            self.assertIn("-DGGML_OPENMP=OFF", command)

    def test_arm64_host_tools_match_update_deps_lock(self) -> None:
        update_deps = (
            target.REPOSITORY_ROOT / "src/build_tools/update_deps.py"
        ).read_text(encoding="utf-8")
        self.assertIn(target.NINJA_WIN_ARCHIVE_SHA256, update_deps)
        self.assertIn(target.LLVM_WIN_ARCHIVE_SHA256, update_deps)
        self.assertIn("'clang.exe'", update_deps)
        self.assertIn("shutil.copyfile(clang_driver", update_deps)

    def test_arm64_environment_comes_from_vcvarsall(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            llvm_bin = root / target.LLVM_BIN_RELATIVE_PATH
            llvm_bin.mkdir(parents=True)
            (llvm_bin / "clang.exe").write_text("clang", encoding="utf-8")
            (llvm_bin / "clang++.exe").write_text(
                "clang++", encoding="utf-8"
            )
            environment = {
                "Path": "C:\\toolchain",
                "WindowsSdkDir": "C:\\sdk",
                "WindowsSDKVersion": "10.0.26100.0\\",
                "VCToolsInstallDir": "C:\\vc",
            }
            with mock.patch.object(
                target.vs_util, "get_vs_env_vars", return_value=environment
            ) as get_vs_env_vars:
                result = target._build_environment("arm64", root)
            get_vs_env_vars.assert_called_once_with("amd64_arm64")
            self.assertTrue(result["Path"].startswith(str(llvm_bin)))
            self.assertIsNone(target._build_environment("x64", root))

    def test_workflow_stops_native_failures_and_prepares_arm64_crt(self) -> None:
        workflow = (target.REPOSITORY_ROOT / ".github/workflows/windows.yaml").read_text(
            encoding="utf-8"
        )
        arm64_job = workflow.split("  build_arm64:\n", 1)[1].split(
            "\n  test:\n", 1
        )[0]
        self.assertNotIn("choco install ninja", workflow)
        self.assertIn("verify_windows_crt.ps1", arm64_job)
        self.assertIn("-Architecture arm64", arm64_job)
        self.assertNotIn("\\x64\\Microsoft.VC145.CRT", arm64_job)
        self.assertIn("probe_windows_zenz_runtime.ps1", workflow)
        self.assertEqual(workflow.count("if ($LASTEXITCODE -ne 0)"), 12)
        self.assertIn("  pull_request:\n", workflow)
        self.assertIn("  push:\n    branches:\n      - main\n", workflow)
        self.assertIn("  workflow_call:\n    inputs:\n      release:\n", workflow)
        self.assertIn("        type: boolean\n        default: false\n", workflow)
        self.assertIn(
            "group: windows-${{ github.workflow }}-"
            "${{ github.head_ref || github.ref }}",
            workflow,
        )
        self.assertEqual(workflow.count("name: windows-daily-dictionary"), 6)
        self.assertEqual(
            workflow.count("if: ${{ inputs.release == true }}"),
            3,
        )
        test_job = workflow.split("\n  test:\n", 1)[1]
        self.assertNotIn("inputs.release", test_job)
        self.assertNotIn("\n  cache_deps:\n", workflow)
        self.assertIn(
            ".\\tools\\dictionary\\prepare_daily_dictionary.ps1 "
            "-ReleaseApprovedOnly",
            workflow,
        )
        release_artifacts = {
            "x64": "release-windows-x64",
            "universal": "release-windows-universal",
            "arm64": "release-windows-arm64",
        }
        for architecture, artifact_name in release_artifacts.items():
            with self.subTest(architecture=architecture):
                self.assertIn(
                    "MozkeyIbG_v${{ needs.prepare_daily_dictionary.outputs."
                    "release_version }}"
                    f"_{architecture}.msi",
                    workflow,
                )
                self.assertIn(f"name: {artifact_name}", workflow)
        self.assertEqual(workflow.count("retention-days: 7"), 5)
        bazel_commands = [
            line.strip()
            for line in workflow.splitlines()
            if line.lstrip().startswith(("bazelisk build ", "bazelisk test "))
        ]
        self.assertEqual(len(bazel_commands), 7)
        for command in bazel_commands:
            with self.subTest(command=command):
                self.assertIn(
                    "--define=mozkey_dictionary_profile=release-approved-only",
                    command,
                )

    def test_release_build_uses_only_generated_runtime(self) -> None:
        build = (
            target.REPOSITORY_ROOT
            / "src/win32/installer/zenz_runtime/BUILD.bazel"
        ).read_text(encoding="utf-8")
        installer = (
            target.REPOSITORY_ROOT / "src/win32/installer/BUILD.bazel"
        ).read_text(encoding="utf-8")
        self.assertIn("generated/x64/llama-server.exe", build)
        self.assertIn("generated/arm64/llama-server.exe", build)
        self.assertIn(
            '_TARGET_COMPATIBLE_WITH = ["@platforms//os:windows"]',
            build,
        )
        self.assertEqual(
            build.count("target_compatible_with = _TARGET_COMPATIBLE_WITH"),
            3,
        )
        self.assertNotIn('"llama-server.exe",', build)
        self.assertNotIn("zenz_runtime:ggml.dll", installer)


if __name__ == "__main__":
    unittest.main()
