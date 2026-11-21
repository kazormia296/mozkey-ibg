import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.release import prepare_macos_zenz_runtime as target


class PrepareMacosZenzRuntimeTest(unittest.TestCase):
    def test_pins_source_and_model_digests(self):
        self.assertEqual(target.LLAMA_CPP_TAG, "b9637")
        self.assertEqual(
            target.LLAMA_CPP_ARCHIVE_SHA256,
            "762283319feb3de30886dc850d42f0e426b06600e7f9639d34e06506597309ca",
        )
        self.assertEqual(
            target.NORMALIZED_MODEL_SHA256,
            "601572033a0c231857864ab0a2ccf40fbd1abe6ee4ccecd5399bf82e3e559772",
        )
        self.assertEqual(target.ARCHITECTURES, (("arm64", "12.0"),))

    def test_cmake_configuration_is_static_cpu_accelerate_only(self):
        command = target.cmake_configure_command(
            Path("/source"),
            Path("/build"),
            "arm64",
            "12.0",
            Path("/ninja"),
        )
        required = {
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DGGML_STATIC=ON",
            "-DGGML_CPU=ON",
            "-DGGML_OPENMP=OFF",
            "-DGGML_LLAMAFILE=OFF",
            "-DGGML_ACCELERATE=ON",
            "-DGGML_METAL=OFF",
            "-DGGML_RPC=OFF",
            "-DMTMD_VIDEO=OFF",
            "-DLLAMA_CURL=OFF",
            "-DLLAMA_BUILD_TESTS=OFF",
            "-DLLAMA_BUILD_TOOLS=ON",
            "-DLLAMA_BUILD_EXAMPLES=OFF",
            "-DLLAMA_BUILD_SERVER=ON",
            "-DLLAMA_BUILD_APP=OFF",
            "-DLLAMA_BUILD_UI=OFF",
            "-DLLAMA_USE_PREBUILT_UI=OFF",
        }
        self.assertTrue(required.issubset(command))

    def test_tools_subtree_exposes_server_but_only_server_target_is_built(self):
        with tempfile.TemporaryDirectory() as temporary:
            build_root = Path(temporary) / "build"
            expected_server = build_root / "arm64/bin/llama-server"
            with mock.patch.object(target, "_run") as run, mock.patch.object(
                target, "_verify_cmake_cache"
            ) as verify_cache, mock.patch.object(
                target, "_find_built_server", return_value=expected_server
            ):
                result = target._build_architecture(
                    Path("/source"),
                    build_root,
                    "arm64",
                    "12.0",
                    Path("/ninja"),
                )

        self.assertEqual(result, expected_server)
        self.assertIn("-DLLAMA_BUILD_TOOLS=ON", run.call_args_list[0].args[0])
        verify_cache.assert_called_once_with(build_root / "arm64")
        self.assertEqual(
            run.call_args_list[1],
            mock.call(
                [
                    "cmake",
                    "--build",
                    str(build_root / "arm64"),
                    "--target",
                    "llama-server",
                    "--parallel",
                ]
            ),
        )

    def test_scorer_compile_commands_pin_each_architecture_and_target(self):
        for architecture, deployment_target in target.ARCHITECTURES:
            with self.subTest(architecture=architecture):
                command = target.scorer_compile_command(
                    Path("/repo"),
                    Path(f"/build/{architecture}/mozc_zenz_scorer"),
                    architecture,
                    deployment_target,
                )
                self.assertEqual(
                    command[:3],
                    [
                        "bazelisk",
                        "build",
                        "//zenz_scorer:mozc_zenz_scorer",
                    ],
                )
                self.assertIn("--config", command)
                self.assertIn("release_build", command)
                self.assertIn(f"--macos_cpus={architecture}", command)
                self.assertIn(
                    f"--macos_minimum_os={deployment_target}",
                    command,
                )
                self.assertIn(
                    f"--copt=-mmacosx-version-min={deployment_target}",
                    command,
                )
                self.assertIn(
                    f"--cxxopt=-mmacosx-version-min={deployment_target}",
                    command,
                )
                self.assertIn(
                    f"--linkopt=-mmacosx-version-min={deployment_target}",
                    command,
                )
                self.assertIn(
                    "--define=mozkey_dictionary_profile=release-approved-only",
                    command,
                )

    def test_darwin_scorer_spawn_is_thread_safe_and_linux_semantics_remain(self):
        source = (
            target.REPOSITORY_ROOT / "src/zenz_scorer/main.cc"
        ).read_text(encoding="utf-8")
        self.assertIn("#include <spawn.h>", source)
        self.assertIn("::posix_spawn_file_actions_addopen(", source)
        self.assertIn("::posix_spawn(\n", source)
        self.assertIn("*_NSGetEnviron()", source)
        self.assertIn("::prctl(PR_SET_PDEATHSIG, SIGTERM);", source)
        self.assertIn("::execv(options.llama_server_path.c_str()", source)
        self.assertIn("sigfillset(&sa.sa_mask);", source)
        self.assertNotIn("::sigfillset(&sa.sa_mask);", source)

    def test_arm64_scorer_rejects_extra_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            scorer = Path(temporary) / target.SCORER_NAME
            scorer.write_bytes(b"Mach-O")
            scorer.chmod(0o755)
            with mock.patch.object(
                target,
                "_run",
                return_value=SimpleNamespace(stdout="arm64 x86_64\n"),
            ):
                with self.assertRaisesRegex(
                    target.PreparationError,
                    "zenz_scorer_architectures_invalid",
                ):
                    target._verify_arm64_scorer(scorer)

    def test_arm64_scorer_rejects_wrong_deployment_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            scorer = Path(temporary) / target.SCORER_NAME
            scorer.write_bytes(b"Mach-O")
            scorer.chmod(0o755)
            with mock.patch.object(
                target,
                "_run",
                side_effect=[
                    SimpleNamespace(stdout="arm64\n"),
                    SimpleNamespace(stdout="    minos 11.0\n"),
                ],
            ):
                with self.assertRaisesRegex(
                    target.PreparationError,
                    "zenz_scorer_deployment_target_invalid",
                ):
                    target._verify_arm64_scorer(scorer)

    def test_macos_package_and_workflow_are_arm64_only(self):
        # Since the sync to upstream Mozkey v1.0.0 the macOS package ships
        # upstream's Universal Zenz runtime under Resources/ZenzRuntime.  The
        # application and installer stay arm64-only.
        server_build = (
            target.REPOSITORY_ROOT / "src/server/BUILD.bazel"
        ).read_text(encoding="utf-8")
        mac_build = (
            target.REPOSITORY_ROOT / "src/mac/BUILD.bazel"
        ).read_text(encoding="utf-8")
        runtime_build = (
            target.REPOSITORY_ROOT / "src/mac/installer/zenz_runtime/BUILD.bazel"
        ).read_text(encoding="utf-8")
        distribution = (
            target.REPOSITORY_ROOT / "src/mac/installer/distribution.xml"
        ).read_text(encoding="utf-8")
        workflow = (
            target.REPOSITORY_ROOT / ".github/workflows/macos.yaml"
        ).read_text(encoding="utf-8")

        self.assertNotIn("//mac/zenz_runtime:", server_build)
        self.assertNotIn("//mac/zenz_runtime:", mac_build)
        for runtime_target in (
            "llama_server",
            "model",
            "build_contract",
            "mozc_zenz_scorer_runtime",
            "mozc_zenz_guard_runtime",
        ):
            with self.subTest(runtime_target=runtime_target):
                self.assertIn(
                    f'"//mac/installer/zenz_runtime:{runtime_target}": '
                    '"Resources/ZenzRuntime"',
                    mac_build,
                )
        self.assertIn('"macos_zenz_runtime": "1"', runtime_build)
        self.assertIn(
            "src/mac/installer/zenz_runtime/build_universal_runtime.zsh",
            workflow,
        )
        self.assertIn(
            "src/mac/installer/zenz_runtime/stage_runtime_assets.zsh",
            workflow,
        )
        self.assertIn(
            "src/mac/installer/zenz_runtime/verify_release_package.zsh",
            workflow,
        )
        self.assertIn("MOZKEY_ZENZ_VERIFY_APP_ARCHITECTURES=arm64", workflow)
        self.assertNotIn("prepare_macos_zenz_runtime.py", workflow)
        self.assertNotIn("probe_macos_zenz_runtime", workflow)
        package_commands = [
            line.strip()
            for line in workflow.splitlines()
            if line.lstrip().startswith("bazelisk build package ")
        ]
        self.assertEqual(len(package_commands), 1)
        self.assertIn("--define=macos_zenz_runtime=1", package_commands[0])
        self.assertIn("--define=MOZKEY_INSTALLABLE=1", package_commands[0])
        self.assertIn("-o -name mozc_zenz_guard", workflow)
        self.assertIn("--macos_cpus=arm64", workflow)
        self.assertIn("  pull_request:\n", workflow)
        self.assertIn("  push:\n    branches:\n      - main\n", workflow)
        self.assertIn("  workflow_call:\n    inputs:\n      release:\n", workflow)
        self.assertIn("        type: boolean\n        default: false\n", workflow)
        self.assertIn(
            "group: macos-${{ github.workflow }}-"
            "${{ github.head_ref || github.ref }}",
            workflow,
        )
        self.assertEqual(workflow.count("name: macos-daily-dictionary"), 3)
        self.assertEqual(
            workflow.count("name: macos-zenz-runtime-universal-v0.7.0"),
            2,
        )
        self.assertEqual(
            workflow.count("if: ${{ inputs.release == true }}"),
            3,
        )
        test_job = workflow.split("\n  test:\n", 1)[1]
        self.assertNotIn("inputs.release", test_job)
        self.assertNotIn("\n  cache_deps:\n", workflow)
        self.assertIn(
            "MozkeyIbG_v${{ needs.prepare_daily_dictionary.outputs."
            "release_version }}_macos_arm64.pkg",
            workflow,
        )
        self.assertIn("name: release-macos-arm64", workflow)
        self.assertEqual(workflow.count("retention-days: 7"), 1)
        self.assertNotIn("--skip-live", workflow)
        self.assertNotIn("build_intel64:", workflow)
        self.assertNotIn("build_universal_binary:", workflow)
        self.assertNotIn("--macos_cpus=x86_64", workflow)
        self.assertIn('hostArchitectures="arm64"', distribution)
        self.assertNotIn('hostArchitectures="x86_64', distribution)
        self.assertIn(
            "./tools/dictionary/prepare_daily_dictionary.ps1 "
            "-ReleaseApprovedOnly",
            workflow,
        )
        bazel_commands = [
            line.strip()
            for line in workflow.splitlines()
            if line.lstrip().startswith(("bazelisk build ", "bazelisk test "))
        ]
        self.assertEqual(len(bazel_commands), 4)
        for command in bazel_commands:
            with self.subTest(command=command):
                self.assertIn(
                    "--define=mozkey_dictionary_profile=release-approved-only",
                    command,
                )

    def test_dependency_licenses_are_packaged_on_macos_and_windows(self):
        mac_build = (
            target.REPOSITORY_ROOT / "src/mac/BUILD.bazel"
        ).read_text(encoding="utf-8")
        mac_runtime_build = (
            target.REPOSITORY_ROOT / "src/mac/installer/zenz_runtime/BUILD.bazel"
        ).read_text(encoding="utf-8")
        mac_notices = (
            target.REPOSITORY_ROOT
            / "src/mac/installer/zenz_runtime/licenses/THIRD_PARTY_NOTICES.md"
        ).read_text(encoding="utf-8")
        windows_runtime_build = (
            target.REPOSITORY_ROOT
            / "src/win32/installer/zenz_runtime/BUILD.bazel"
        ).read_text(encoding="utf-8")
        installer_build = (
            target.REPOSITORY_ROOT / "src/win32/installer/BUILD.bazel"
        ).read_text(encoding="utf-8")
        windows_installer = (
            target.REPOSITORY_ROOT
            / "src/win32/installer/installer_oss_64bit.wxs"
        ).read_text(encoding="utf-8")

        mac_targets = {
            "cpp-httplib-MIT.txt": "cpp_httplib_license",
            "nlohmann-json-MIT.txt": "nlohmann_json_license",
        }
        for name, mac_target in mac_targets.items():
            with self.subTest(name=name):
                self.assertIn(
                    f'"//mac/installer/zenz_runtime:{mac_target}": '
                    '"Resources/ZenzRuntime"',
                    mac_build,
                )
                self.assertIn(
                    f'src = "//win32/installer/zenz_runtime:licenses/{name}"',
                    mac_runtime_build,
                )
                self.assertIn(f'out = "licenses/{name}"', mac_runtime_build)
                self.assertIn(f"`{name}`", mac_notices)
                self.assertIn(f'"licenses/{name}"', windows_runtime_build)
                self.assertIn(
                    f'"//win32/installer/zenz_runtime:licenses/{name}"',
                    installer_build,
                )
                self.assertIn(f'Name="{name}"', windows_installer)

    def test_archive_checksum_mismatch_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / "llama.tar.gz"
            archive.write_bytes(b"not-the-pinned-archive")
            with self.assertRaisesRegex(
                target.PreparationError, "llama_archive_checksum_mismatch"
            ):
                target._verify_archive(archive)

    def test_cmake_cache_requires_consumed_boolean_options(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            lines = [
                f"{key}:BOOL={value}"
                for key, value in target.REQUIRED_CMAKE_BOOL_OPTIONS.items()
            ]
            lines.append("LLAMA_CURL:UNINITIALIZED=OFF")
            lines.append("GGML_BLAS_VENDOR:STRING=Apple")
            cache = build / "CMakeCache.txt"
            cache.write_text("\n".join(lines) + "\n", encoding="utf-8")
            target._verify_cmake_cache(build)

            cache.write_text(
                cache.read_text(encoding="utf-8").replace(
                    "GGML_METAL:BOOL=OFF", "GGML_METAL:UNINITIALIZED=OFF"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                target.PreparationError, "cmake_backend_contract_invalid"
            ):
                target._verify_cmake_cache(build)

    def test_cmake_cache_accepts_deprecated_curl_switch_only_when_off(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            lines = [
                f"{key}:BOOL={value}"
                for key, value in target.REQUIRED_CMAKE_BOOL_OPTIONS.items()
            ]
            lines.extend(
                (
                    "LLAMA_CURL:UNINITIALIZED=OFF",
                    "GGML_BLAS_VENDOR:STRING=Apple",
                )
            )
            cache = build / "CMakeCache.txt"
            cache.write_text("\n".join(lines) + "\n", encoding="utf-8")
            target._verify_cmake_cache(build)

            cache.write_text(
                cache.read_text(encoding="utf-8").replace(
                    "LLAMA_CURL:UNINITIALIZED=OFF",
                    "LLAMA_CURL:UNINITIALIZED=ON",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                target.PreparationError, "cmake_backend_contract_invalid"
            ):
                target._verify_cmake_cache(build)

    def test_manifest_records_release_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "runtime.json"
            target._write_manifest(path)
            document = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(
                document["llama_cpp"]["archive_sha256"],
                target.LLAMA_CPP_ARCHIVE_SHA256,
            )
            self.assertEqual(
                document["model"]["sha256"], target.NORMALIZED_MODEL_SHA256
            )
            self.assertEqual(
                document["llama_cpp"]["built_targets"], ["llama-server"]
            )
            self.assertEqual(
                document["backend"],
                {
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
            )
            self.assertEqual(
                document["architectures"],
                {"arm64": {"deployment_target": "12.0"}},
            )
            self.assertEqual(
                document["scorer"],
                {
                    "architectures": {
                        "arm64": {"deployment_target": "12.0"},
                    },
                    "filename": target.SCORER_NAME,
                },
            )

    def test_main_redacts_unexpected_errors(self):
        stderr = io.StringIO()
        with mock.patch.object(
            target,
            "prepare_runtime",
            side_effect=RuntimeError("private build path and command"),
        ):
            with redirect_stderr(stderr):
                self.assertEqual(target.main([]), 1)
        self.assertEqual(
            stderr.getvalue(),
            "macOS Zenz runtime preparation failed: unexpected_runtime_error\n",
        )


if __name__ == "__main__":
    unittest.main()
