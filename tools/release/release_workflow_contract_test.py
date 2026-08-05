from __future__ import annotations

from collections import Counter
from pathlib import Path
import re
import unittest


SUPPORTED_PLATFORMS = ("linux", "macos", "windows")
MOBILE_PLATFORMS = ("android", "ios")
ROUTINE_CI_WORKFLOWS = ("lint", *SUPPORTED_PLATFORMS, "secure-offline")


class ReleaseWorkflowContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repository = Path(__file__).resolve().parents[2]
        cls.workflow_directory = cls.repository / ".github" / "workflows"
        cls.workflow_paths = sorted(
            {
                *cls.workflow_directory.glob("*.yaml"),
                *cls.workflow_directory.glob("*.yml"),
            }
        )
        cls.workflow_path = cls.workflow_directory / "release.yaml"
        cls.workflow = cls.workflow_path.read_text(encoding="utf-8")
        cls.jobs = cls._split_job_blocks(cls.workflow)
        cls.root_build = (cls.repository / "src" / "BUILD.bazel").read_text(
            encoding="utf-8"
        )

    @staticmethod
    def _split_job_blocks(workflow: str) -> dict[str, str]:
        jobs = workflow.split("\njobs:\n", maxsplit=1)[1]
        matches = list(
            re.finditer(r"^  ([a-z][a-z0-9_-]*):\n", jobs, re.MULTILINE)
        )
        return {
            match.group(1): jobs[
                match.start() : matches[index + 1].start()
                if index + 1 < len(matches)
                else len(jobs)
            ]
            for index, match in enumerate(matches)
        }

    def _platform_workflow(self, platform: str) -> str:
        return (self.workflow_directory / f"{platform}.yaml").read_text(
            encoding="utf-8"
        )

    def test_release_is_tag_only_and_gate_validates_dispatch_refs(self) -> None:
        trigger = self.workflow.split("\nconcurrency:\n", maxsplit=1)[0]
        self.assertIn('      - "v*.*.*"', trigger)
        self.assertIn("  workflow_dispatch: {}", trigger)
        self.assertNotIn("branches:", trigger)

        gate = self.jobs["release-gate"]
        self.assertIn("fetch-depth: 0", gate)
        self.assertIn("RELEASE_REF_TYPE: ${{ github.ref_type }}", gate)
        self.assertIn("tools/release/validate_mozkey_release.py", gate)
        self.assertIn("--main-ref origin/main", gate)
        self.assertIn(
            "Refuse an already published release before product builds",
            gate,
        )

    def test_only_publish_has_write_permission(self) -> None:
        writers = [
            name
            for name, block in self.jobs.items()
            if re.search(r"(?m)^      contents: write$", block)
        ]
        self.assertEqual(writers, ["publish"])
        self.assertIn("permissions:\n  contents: read", self.workflow)

    def test_supported_platform_release_jobs_are_gated(self) -> None:
        for platform in SUPPORTED_PLATFORMS:
            with self.subTest(platform=platform):
                block = self.jobs[platform]
                self.assertIn("needs: [release-gate, preflight]", block)
                self.assertIn(
                    f"uses: ./.github/workflows/{platform}.yaml",
                    block,
                )
                self.assertIn("release: true", block)
                self.assertNotIn("secrets: inherit", block)

                self.assertRegex(
                    self._platform_workflow(platform),
                    r"(?ms)^  workflow_call:\n    inputs:\n      release:\n"
                    r".*?        type: boolean",
                )

        self.assertIn(
            "needs: [release-gate, preflight, linux, macos, windows, "
            "secure_offline]",
            self.jobs["publish"],
        )

    def test_secure_offline_gate_runs_before_publish(self) -> None:
        gate = self.jobs["secure_offline"]
        self.assertIn("needs: [release-gate, preflight]", gate)
        self.assertIn("uses: ./.github/workflows/secure-offline.yaml", gate)
        self.assertIn("contents: read", gate)
        self.assertIn("secure_offline", self.jobs["publish"])

    def test_short_preflight_gates_every_expensive_release_job(self) -> None:
        gate = self.jobs["preflight"]
        self.assertIn("needs: release-gate", gate)
        self.assertIn(
            "uses: ./.github/workflows/release-preflight.yaml",
            gate,
        )
        self.assertIn(
            "candidate_tag: ${{ needs.release-gate.outputs.tag }}",
            gate,
        )

        preflight = self._workflow("release-preflight")
        self.assertIn("  pull_request:", preflight)
        self.assertIn("  workflow_dispatch:", preflight)
        self.assertIn("  workflow_call:", preflight)
        preflight_trigger = preflight.split("\npermissions:\n", maxsplit=1)[0]
        self.assertNotRegex(preflight_trigger, r"(?m)^  push:")
        # Only the full-history portable job needs the extended checkout budget.
        preflight_jobs = self._split_job_blocks(preflight)
        self.assertIn("timeout-minutes: 10", preflight_jobs["portable"])
        self.assertNotIn("timeout-minutes: 5", preflight_jobs["portable"])
        self.assertIn("timeout-minutes: 5", preflight_jobs["archlinux"])
        self.assertNotIn("timeout-minutes: 10", preflight_jobs["archlinux"])
        self.assertIn("timeout-minutes: 5", preflight_jobs["fedora"])
        self.assertNotIn("timeout-minutes: 10", preflight_jobs["fedora"])
        self.assertIn("preflight_release_identity.py", preflight)
        self.assertIn("preflight_release_inputs.py", preflight)
        self.assertIn("check_powershell_preflight.ps1", preflight)
        self.assertIn("--phase pull-request", preflight)
        self.assertIn("--phase pre-tag", preflight)
        self.assertIn("--phase tag", preflight)
        self.assertIn("pacman -Si -- \"$package\"", preflight)
        self.assertIn("dnf -q repoquery --available", preflight)
        self.assertIn("-Wa,--gsframe=no", preflight)
        self.assertNotIn("contents: write", preflight)

    def test_windows_release_checks_built_and_extracted_msi_payloads(self) -> None:
        windows = self._platform_workflow("windows")
        secure_offline = self._workflow("secure-offline")
        self.assertEqual(windows.count("check_windows_msi_offline.ps1"), 3)
        self.assertEqual(windows.count("probe_windows_zenz_runtime.ps1"), 2)
        self.assertNotIn(r"Visual Studio\18\Community", windows)
        self.assertNotIn(r"Visual Studio\18\Community", secure_offline)
        self.assertNotIn("vs_util.py --arch", windows)
        self.assertNotIn("check_windows_msi_offline.ps1", secure_offline)
        self.assertIn("probe_windows_zenz_runtime.ps1", secure_offline)

        windows_jobs = self._split_job_blocks(windows)
        for job, upload_step in (
            ("build_x64", "Upload versioned x64 MSI"),
            ("build_universal", "Upload versioned universal MSI"),
            ("build_arm64", "Upload versioned ARM64 MSI"),
        ):
            with self.subTest(job=job):
                block = windows_jobs[job]
                self.assertLess(
                    block.index("check_windows_msi_offline.ps1"),
                    block.index(upload_step),
                )

        binary_check = self._split_job_blocks(secure_offline)["binary_check"]
        self.assertNotIn("github.event_name == 'workflow_dispatch'", binary_check)
        self.assertNotIn("Build Qt", binary_check)
        self.assertNotRegex(binary_check, r"bazelisk build .*\bpackage\b")
        self.assertNotIn("actions/upload-artifact", binary_check)
        self.assertIn("//zenz_scorer:mozc_zenz_scorer", binary_check)
        self.assertIn("check_no_network_imports.py", binary_check)
        self.assertIn("check_no_network_strings.py", binary_check)
        self.assertIn("mozc_zenz_scorer.exe", binary_check)
        self.assertIn("llama-server.exe", binary_check)

    def test_secure_offline_reuses_bazel_repository_downloads(self) -> None:
        secure_offline = self._workflow("secure-offline")
        jobs = self._split_job_blocks(secure_offline)
        for job in ("config_tests", "binary_check"):
            with self.subTest(job=job):
                self.assertIn("Restore Bazel repository cache", jobs[job])
                self.assertIn(
                    "path: ${{ runner.temp }}/bazel-repository-cache",
                    jobs[job],
                )
                self.assertIn(
                    "--repository_cache=",
                    jobs[job],
                )
                self.assertIn("RUNNER_TEMP", jobs[job])

        config_tests = jobs["config_tests"]
        self.assertIn("Hydrate Bazel repositories", config_tests)
        self.assertIn(
            "for ($attempt = 1; $attempt -le 3; $attempt++)",
            config_tests,
        )

    def test_windows_msi_checker_uses_the_right_exit_status_contract(self) -> None:
        checker = (
            self.repository / "tools/release/check_windows_msi_offline.ps1"
        ).read_text(encoding="utf-8")
        script_verification = checker.split("function Invoke-NetworkCheck", 1)[0]
        self.assertIn("if (-not $?)", script_verification)
        self.assertNotIn("$LASTEXITCODE", script_verification)
        self.assertEqual(checker.count("if ($LASTEXITCODE -ne 0)"), 1)
        self.assertIn("[System.Diagnostics.ProcessStartInfo]::new()", checker)
        self.assertIn("$startInfo.ArgumentList.Add($argument)", checker)
        self.assertIn("$process.WaitForExit()", checker)
        self.assertIn("$process.ExitCode", checker)
        self.assertNotIn("& msiexec.exe", checker)

    def test_windows_crt_source_is_toolchain_selected_and_verified(self) -> None:
        windows = self._platform_workflow("windows")
        secure_offline = self._workflow("secure-offline")
        installer = (
            self.repository / "src/win32/installer/build_installer.py"
        ).read_text(encoding="utf-8")
        verifier = (
            self.repository / "tools/release/verify_windows_crt.ps1"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "EXPECTED_CRT_REDIST_VERSION_PATTERN",
            installer,
        )
        self.assertIn("VCTOOLSREDISTDIR", installer)
        self.assertIn("VCToolsRedistDir", verifier)
        self.assertIn('$expectedFileVersion = "14.51.36247.0"', verifier)
        self.assertIn("Get-AuthenticodeSignature", verifier)
        self.assertIn("Get-FileHash", verifier)
        self.assertIn("GetVersionInfo", verifier)
        self.assertEqual(windows.count("verify_windows_crt.ps1"), 3)
        self.assertIn("verify_windows_crt.ps1", secure_offline)

    def _workflow(self, name: str) -> str:
        return (self.workflow_directory / f"{name}.yaml").read_text(
            encoding="utf-8"
        )

    def test_mobile_platforms_are_not_product_or_release_targets(self) -> None:
        for platform in MOBILE_PLATFORMS:
            with self.subTest(platform=platform):
                self.assertNotIn(platform, self.jobs)
                for suffix in ("yaml", "yml"):
                    self.assertFalse(
                        (self.workflow_directory / f"{platform}.{suffix}").exists()
                    )
                self.assertNotIn(
                    f"uses: ./.github/workflows/{platform}.yaml",
                    self.workflow,
                )

        package = re.search(
            r'(?ms)^filegroup\(\n    name = "package",.*?^\)\n',
            self.root_build,
        )
        self.assertIsNotNone(package)
        assert package is not None
        self.assertNotRegex(package.group(0), r"(?m)^\s+(android|ios)\s*=")
        self.assertIn('linux = ["//unix:package"]', package.group(0))
        self.assertIn('macos = ["//mac:package"]', package.group(0))
        self.assertIn('windows = ["//win32/installer"]', package.group(0))

    def test_release_artifact_names_are_unique(self) -> None:
        names: list[str] = []
        for platform in SUPPORTED_PLATFORMS:
            platform_names = re.findall(
                r"(?m)^\s+name: (release-[A-Za-z0-9_.-]+)\s*$",
                self._platform_workflow(platform),
            )
            self.assertTrue(platform_names, f"{platform} has no release-* artifact")
            names.extend(platform_names)

        duplicates = [name for name, count in Counter(names).items() if count > 1]
        self.assertEqual(duplicates, [])
        self.assertIn("pattern: release-*", self.jobs["publish"])

    def test_routine_ci_skips_all_product_build_jobs(self) -> None:
        gated_jobs = {
            "linux": ("build", "package_fedora", "package_archlinux"),
            "macos": ("prepare_macos_zenz_runtime", "build_arm64"),
            "windows": ("build_x64", "build_universal", "build_arm64"),
        }
        for platform, expected_jobs in gated_jobs.items():
            with self.subTest(platform=platform):
                workflow = self._platform_workflow(platform)
                jobs = self._split_job_blocks(workflow)
                for job in expected_jobs:
                    self.assertIn(
                        "if: ${{ inputs.release == true }}",
                        jobs[job],
                        f"{platform}/{job} must be release-only",
                    )

                trigger = workflow.split("\npermissions:\n", maxsplit=1)[0]
                self.assertIn("  pull_request:", trigger)
                self.assertIn("  push:\n    branches:\n      - main", trigger)

    def test_routine_ci_bypasses_only_version_file_pull_requests(self) -> None:
        for name in ROUTINE_CI_WORKFLOWS:
            with self.subTest(workflow=name):
                trigger = self._workflow(name).split(
                    "\npermissions:\n", maxsplit=1
                )[0]
                event_match = re.search(
                    r"(?ms)^  pull_request:\n(.*?)(?=^  [a-z_]+:|\Z)",
                    trigger,
                )
                self.assertIsNotNone(event_match)
                assert event_match is not None
                paths_match = re.search(
                    r"(?m)^    paths-ignore:\n"
                    r"((?:      - [^\n]+\n?)+)",
                    event_match.group(1),
                )
                self.assertIsNotNone(paths_match)
                assert paths_match is not None
                self.assertEqual(
                    re.findall(r"(?m)^      - ([^\n]+)$", paths_match.group(1)),
                    ["src/version.bzl"],
                )

        preflight_trigger = self._workflow("release-preflight").split(
            "\npermissions:\n", maxsplit=1
        )[0]
        self.assertNotIn("paths-ignore:", preflight_trigger)

    def test_consecutive_version_only_main_pushes_run_routine_ci(self) -> None:
        # With no push path filter, a later bump still tests any unsafe state
        # inherited from an earlier, possibly cancelled, main-branch run.
        for name in ROUTINE_CI_WORKFLOWS:
            with self.subTest(workflow=name):
                trigger = self._workflow(name).split(
                    "\npermissions:\n", maxsplit=1
                )[0]
                event_match = re.search(
                    r"(?ms)^  push:\n(.*?)(?=^  [a-z_]+:|\Z)",
                    trigger,
                )
                self.assertIsNotNone(event_match)
                assert event_match is not None
                push = event_match.group(1)
                self.assertIn("    branches:\n      - main", push)
                self.assertNotRegex(push, r"(?m)^    paths(?:-ignore)?:")

    def test_intermediate_artifacts_are_namespaced_and_short_lived(self) -> None:
        artifact_names: list[str] = []
        workflows: dict[str, str] = {}
        for platform in SUPPORTED_PLATFORMS:
            workflow = self._platform_workflow(platform)
            workflows[platform] = workflow
            artifact_names.extend(
                re.findall(
                    r"(?m)^\s+uses: actions/upload-artifact@[0-9a-f]{40}  "
                    r"# v[0-9]+(?:\.[0-9]+){0,2}\n"
                    r"\s+with:\n\s+name: ([^\s]+)$",
                    workflow,
                )
            )
            upload_blocks = workflow.split("uses: actions/upload-artifact@")[1:]
            self.assertTrue(upload_blocks)
            for index, upload in enumerate(upload_blocks):
                with self.subTest(platform=platform, upload=index):
                    self.assertIn("overwrite: true", upload.split("\n\n", 1)[0])

        duplicates = [
            name for name, count in Counter(artifact_names).items() if count > 1
        ]
        self.assertEqual(duplicates, [])
        self.assertNotIn("name: mozc.zip", workflows["linux"])
        self.assertRegex(
            workflows["linux"],
            r"(?ms)name: linux-release-approved-dictionary\n.*?retention-days: 1",
        )
        self.assertRegex(
            workflows["macos"],
            r"(?ms)name: macos-daily-dictionary\n.*?retention-days: 1",
        )
        self.assertRegex(
            workflows["windows"],
            r"(?ms)name: windows-daily-dictionary\n.*?retention-days: 1",
        )

    def test_public_product_filenames_are_versioned(self) -> None:
        expected = {
            "linux": (
                "dist/mozkey-ibg-v*-archlinux-x86_64.tar.xz",
                "dist/mozkey-ibg_*_amd64.deb",
                "dist/mozkey-ibg-*-1.x86_64.rpm",
            ),
            "macos": (
                "MozkeyIbG_v${{ needs.prepare_daily_dictionary.outputs.release_version }}_macos_arm64.pkg",
            ),
            "windows": (
                "MozkeyIbG_v${{ needs.prepare_daily_dictionary.outputs.release_version }}_x64.msi",
            ),
        }
        for platform, filenames in expected.items():
            with self.subTest(platform=platform):
                for filename in filenames:
                    self.assertIn(filename, self._platform_workflow(platform))

        all_workflows = "\n".join(
            path.read_text(encoding="utf-8")
            for path in self.workflow_paths
        )
        self.assertNotIn("android-native-libs", all_workflows)
        self.assertNotRegex(all_workflows, r"(?i)release[-_/].*ios")

    def test_every_external_action_is_pinned_to_a_full_commit(self) -> None:
        for path in self.workflow_paths:
            workflow = path.read_text(encoding="utf-8")
            for line_number, line in enumerate(workflow.splitlines(), start=1):
                stripped = line.strip()
                if not stripped.startswith("uses:"):
                    continue
                action = stripped.removeprefix("uses:").strip()
                if action.startswith("./"):
                    continue
                with self.subTest(path=path.name, line=line_number):
                    self.assertRegex(
                        line,
                        r"^\s+uses: [A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@"
                        r"[0-9a-f]{40}  # v[0-9]+(?:\.[0-9]+){0,2}$",
                    )

        all_workflows = "\n".join(
            path.read_text(encoding="utf-8")
            for path in self.workflow_paths
        )
        self.assertNotIn("openai/codex-action", all_workflows)
        self.assertNotIn("OPENAI_API_KEY", all_workflows)

    def test_release_notes_use_only_github_generation(self) -> None:
        self.assertNotIn("release-notes", self.jobs)
        publish = self.jobs["publish"]
        self.assertIn("Generate release notes with GitHub", publish)
        self.assertIn("releases/generate-notes", publish)
        self.assertIn(
            "release-notes-generator: github-generate-notes",
            publish,
        )
        self.assertNotIn("CODEX_RELEASE_NOTES", publish)

    def test_macos_release_is_signed_notarized_and_receives_only_named_secrets(
        self,
    ) -> None:
        caller = self.jobs["macos"]
        workflow = self._platform_workflow("macos")
        required_secrets = {
            "APPLE_CERTIFICATE",
            "APPLE_CERTIFICATE_PASSWORD",
            "APPLE_INSTALLER_CERTIFICATE",
            "APPLE_INSTALLER_CERTIFICATE_PASSWORD",
            "KEYCHAIN_PASSWORD",
            "APPLE_API_KEY_BASE64",
            "APPLE_API_KEY",
            "APPLE_API_ISSUER",
        }
        for secret in required_secrets:
            with self.subTest(secret=secret):
                self.assertIn(f"      {secret}: ${{{{ secrets.{secret} }}}}", caller)
                self.assertRegex(
                    workflow,
                    rf"(?m)^      {secret}:\n(?:        .*\n)*?        required: true$",
                )

        self.assertNotIn("secrets: inherit", caller)
        macos_jobs = self._split_job_blocks(workflow)
        self.assertIn(
            "if: ${{ inputs.release == true }}",
            macos_jobs["validate_release_signing"],
        )
        self.assertIn("Build Qt", workflow)
        self.assertLess(
            workflow.index("Probe packaged arm64 scorer"),
            workflow.index("Import Developer ID identities"),
        )
        self.assertIn("Developer ID Application", workflow)
        self.assertIn("Developer ID Installer", workflow)
        self.assertIn(
            'security list-keychains -d user -s "$SIGNING_KEYCHAIN"',
            workflow,
        )
        self.assertRegex(
            workflow,
            r"python3 src/mac/build_package\.py \\\n\s+--oss \\",
        )
        self.assertIn("xcrun notarytool submit", workflow)
        self.assertIn("xcrun stapler staple", workflow)
        self.assertIn("spctl --assess --type install", workflow)
        self.assertIn("if: always()", workflow)

    def test_aur_publication_is_serial_and_monotonic(self) -> None:
        workflow = self._platform_workflow("aur")
        self.assertIn("group: mozkey-aur-publish", workflow)
        self.assertNotIn("group: mozkey-aur-${{", workflow)
        self.assertIn('vercmp "$RELEASE_VERSION" "$current_version"', workflow)
        self.assertIn("Refusing AUR package downgrade", workflow)
        self.assertIn("Refusing AUR pkgrel downgrade", workflow)
        self.assertIn("next_pkgrel == current_pkgrel", workflow)
        self.assertIn(
            "AUR metadata changed without a pkgrel increase",
            workflow,
        )

    def test_native_linux_packages_preserve_attested_stage(self) -> None:
        linux = self._platform_workflow("linux")
        smoke = (self.repository / "scripts/smoke_test_mozkey_fcitx5_install").read_text(
            encoding="utf-8"
        )
        rpm = (self.repository / "scripts/package_mozkey_linux_rpm").read_text(
            encoding="utf-8"
        )
        deb = (self.repository / "scripts/package_mozkey_linux_deb").read_text(
            encoding="utf-8"
        )
        self.assertIn("MOZKEY_ZENZ_LLAMA_SERVER_SOURCE", smoke)
        self.assertIn("cmp -s --", smoke)
        self.assertIn("%global __os_install_post %{nil}", rpm)
        self.assertIn("verify_staged_linux_payload.py", rpm)
        self.assertIn("verify_staged_linux_payload.py", deb)
        self.assertIn("tools.release.verify_staged_linux_payload_test", linux)
        self.assertIn(
            'runtime="${GITHUB_WORKSPACE}/dist/zenz/linux/runtime/llama-server"',
            linux,
        )
        self.assertNotIn(
            'runtime="$(pwd)/../dist/zenz/linux/runtime/llama-server"',
            linux,
        )

    def test_linux_release_build_inputs_use_fixed_snapshots(self) -> None:
        linux = self._platform_workflow("linux")
        preflight = self._workflow("release-preflight")
        self.assertIn(
            "container: fedora@sha256:"
            "99e203b80b1c3d8f7e161ec10a68fd02b081ef83a3963553e513c82846b97814",
            linux,
        )
        self.assertIn("container: archlinux/archlinux@sha256:", linux)
        fedora_snapshot = (
            self.repository / "tools/release/use_fedora_snapshot.sh"
        ).read_text(encoding="utf-8")
        arch_snapshot = (
            self.repository / "tools/release/use_archlinux_snapshot.sh"
        ).read_text(encoding="utf-8")
        self.assertIn('readonly FEDORA_RELEASE="42"', fedora_snapshot)
        self.assertIn("Fedora 42 Everything archive", linux)
        self.assertIn("Fedora 42 updates archive", linux)
        self.assertIn("fedora/linux/releases/42/Everything/", linux)
        self.assertIn("fedora/linux/updates/42/Everything/", linux)
        self.assertIn("RPM-GPG-KEY-fedora-42-primary", linux)
        self.assertIn('readonly SNAPSHOT_DATE="2026/07/17"', arch_snapshot)
        self.assertIn(
            "pacman -Syy --noconfirm --disable-download-timeout", linux
        )
        self.assertIn(
            "pacman -Syy --noconfirm --disable-download-timeout", preflight
        )
        self.assertIn(
            "pacman -Syy --noconfirm --disable-download-timeout", arch_snapshot
        )
        for command, workflow in (
            (
                "pacman -S --noconfirm --disable-download-timeout git",
                linux,
            ),
            (
                "pacman -S --noconfirm --disable-download-timeout --needed "
                '"${release_packages[@]}"',
                linux,
            ),
            (
                "pacman -S --noconfirm --disable-download-timeout --needed git",
                preflight,
            ),
            (
                "pacman -S --noconfirm --disable-download-timeout --needed "
                "curl gcc binutils",
                preflight,
            ),
        ):
            with self.subTest(command=command):
                self.assertIn(command, workflow)
        self.assertIn(
            "Server = https://archive.archlinux.org/repos/2026/07/17/"
            "$repo/os/$arch",
            linux,
        )
        self.assertNotIn(r"\$repo", linux)
        self.assertIn(
            r"Server = https://archive.archlinux.org/repos/${SNAPSHOT_DATE}/"
            r"\$repo/os/\$arch",
            arch_snapshot,
        )
        self.assertNotIn(r"\\$repo", arch_snapshot)
        for script in (
            "use_ubuntu_snapshot.sh",
            "use_fedora_snapshot.sh",
            "use_archlinux_snapshot.sh",
        ):
            with self.subTest(script=script):
                self.assertIn(script, linux)
        self.assertNotIn("apt-get update", linux)
        self.assertNotIn("pacman -Syu", linux)
        self.assertIn("2026/07/17", preflight)
        self.assertIn("Fedora 42", preflight)

        package_contracts = {
            "archlinux": (
                "archlinux_release_packages.txt",
                {"binutils", "gcc", "llama-cpp", "qt6-base"},
            ),
            "fedora": (
                "fedora_release_packages.txt",
                {"binutils", "gcc-c++", "qt6-qtbase-devel", "rpm-build"},
            ),
        }
        for platform, (filename, required) in package_contracts.items():
            with self.subTest(platform=platform):
                path = self.repository / "tools/release" / filename
                packages = path.read_text(encoding="utf-8").splitlines()
                self.assertEqual(packages, sorted(set(packages)))
                self.assertTrue(required.issubset(packages))
                self.assertIn(filename, linux)
                self.assertIn(filename, preflight)

    def test_ubuntu_snapshot_update_is_strict_and_retryable(self) -> None:
        snapshot = (
            self.repository / "tools/release/use_ubuntu_snapshot.sh"
        ).read_text(encoding="utf-8")
        self.assertIn('readonly APT_UPDATE_ATTEMPTS=5', snapshot)
        self.assertIn("/etc/apt/sources.list.d/*.list", snapshot)
        self.assertIn("/etc/apt/sources.list.d/*.sources", snapshot)
        self.assertIn("APT::Update::Error-Mode=any", snapshot)
        self.assertIn(
            "for ((attempt = 1; attempt <= APT_UPDATE_ATTEMPTS; attempt++))",
            snapshot,
        )

    def test_publish_replaces_and_verifies_exact_asset_set(self) -> None:
        publish = self.jobs["publish"]
        expected = {
            "MozkeyIbG_v${RELEASE_VERSION}_arm64.msi",
            "MozkeyIbG_v${RELEASE_VERSION}_macos_arm64.pkg",
            "MozkeyIbG_v${RELEASE_VERSION}_universal.msi",
            "MozkeyIbG_v${RELEASE_VERSION}_x64.msi",
            "archlinux-build-packages.txt",
            "fedora-build-packages.txt",
            "mozkey-ibg-${RELEASE_VERSION}-1.x86_64.rpm",
            "mozkey-ibg_${RELEASE_VERSION}_amd64.deb",
            "mozkey-ibg-v${RELEASE_VERSION}-archlinux-x86_64.build-attestation.json",
            "mozkey-ibg-v${RELEASE_VERSION}-archlinux-x86_64.spdx.json",
            "mozkey-ibg-v${RELEASE_VERSION}-archlinux-x86_64.tar.xz",
            "mozkey-ibg-v${RELEASE_VERSION}-archlinux-x86_64.tar.xz.sha256",
            "mozkey-ibg-v${RELEASE_VERSION}-fedora-x86_64.build-attestation.json",
            "mozkey-ibg-v${RELEASE_VERSION}-fedora-x86_64.spdx.json",
            "mozkey-ibg-v${RELEASE_VERSION}-ubuntu-x86_64.build-attestation.json",
            "mozkey-ibg-v${RELEASE_VERSION}-ubuntu-x86_64.spdx.json",
            "ubuntu-build-packages.txt",
        }
        allowlist = re.search(
            r"(?ms)expected_product_assets=\(\n(?P<body>.*?)^          \)",
            publish,
        )
        self.assertIsNotNone(allowlist)
        assert allowlist is not None
        self.assertEqual(
            set(re.findall(r'^\s+"([^"]+)"$', allowlist.group("body"), re.MULTILINE)),
            expected,
        )

        self.assertIn("expected-product-assets.txt", publish)
        self.assertIn("expected-release-assets.txt", publish)
        self.assertIn("pre-upload-release-assets.txt", publish)
        self.assertIn("post-upload-release-assets.txt", publish)
        self.assertIn("releases/assets/${asset_id}", publish)
        self.assertIn("--method DELETE", publish)
        self.assertIn("require_mutable_draft", publish)
        self.assertIn(".immutable // false", publish)
        self.assertGreaterEqual(publish.count("require_mutable_draft"), 6)
        edit_block = publish.split('gh release edit "$RELEASE_TAG"', 1)[1].split(
            'gh release create "$RELEASE_TAG"', 1
        )[0]
        self.assertNotIn("--draft", edit_block)
        self.assertGreaterEqual(publish.count("diff -u"), 4)
        self.assertNotIn("--clobber", publish)

    def test_publish_resolves_draft_releases_from_the_release_list(self) -> None:
        publish = self.jobs["publish"]
        self.assertIn("find_draft_release_id()", publish)
        self.assertIn("gh api --paginate --slurp", publish)
        self.assertIn('"repos/${GITHUB_REPOSITORY}/releases?per_page=100"', publish)
        self.assertIn(
            "select(.tag_name == $tag and .draft == true) | .id",
            publish,
        )
        self.assertIn('release_id="$(find_draft_release_id)"', publish)
        self.assertIn("wait_for_draft_release_id()", publish)
        self.assertIn('release_id="$(wait_for_draft_release_id)"', publish)
        self.assertIn("sleep 2", publish)
        self.assertIn(
            'Unable to resolve the draft release ${RELEASE_TAG}',
            publish,
        )

    def test_publish_refuses_published_release_and_builds_checksums(self) -> None:
        publish = self.jobs["publish"]
        self.assertIn("Refusing to mutate published release", publish)
        self.assertIn("pattern: release-*", publish)
        self.assertIn("SHA256SUMS", publish)
        self.assertIn("id-token: write", publish)
        self.assertIn("attestations: write", publish)
        self.assertIn("actions/attest@f7c74d28b9d84cb8768d0b8ca14a4bac6ef463e6", publish)
        self.assertIn("subject-path: publish-assets/*", publish)
        self.assertIn("--draft", publish)
        self.assertIn("--prerelease", publish)


if __name__ == "__main__":
    unittest.main()
