from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class ProductIdentityContractTest(unittest.TestCase):
    def test_common_runtime_and_consumer_namespaces_are_fork_specific(self) -> None:
        constants = read("src/base/const.h")
        system_util = read("src/base/system_util.cc")
        ipc = read("src/ipc/ipc_path_manager.cc")
        consumers = read("src/grimodex/consumer_handshake.h")
        zenz_endpoint = read("src/session/zenz_named_pipe_endpoint.h")

        for required in (
            'kProductNameInEnglish[] = "Mozkey IbG"',
            '#define kProductPrefix "MozkeyIbG"',
            '"Local\\\\MozkeyIbG.event."',
            '"Local\\\\MozkeyIbG.mutex."',
            '"\\\\\\\\.\\\\pipe\\\\mozkey-ibg."',
            'L"Software\\\\Grimodex\\\\MozkeyIbG"',
        ):
            self.assertIn(required, constants)

        self.assertIn('"/usr/lib/mozkey-ibg"', system_util)
        self.assertIn('"MozkeyIbG"', system_util)
        self.assertIn('"mozkey-ibg"', system_util)
        self.assertIn('"/tmp/.mozkey-ibg."', ipc)
        for consumer_id in (
            "fcitx5-mozkey-ibg",
            "tsf-mozkey-ibg",
            "imkit-mozkey-ibg",
        ):
            self.assertIn(consumer_id, consumers)
        self.assertIn(
            r"\\.\pipe\mozkey-ibg_zenz_scorer",
            zenz_endpoint,
        )

    def test_linux_install_and_fcitx_identities_are_fork_specific(self) -> None:
        addon = read("src/unix/fcitx5/mozkey-ibg-addon.conf")
        input_method = read("src/unix/fcitx5/mozkey-ibg.conf")
        appstream = read(
            "src/unix/fcitx5/io.github.kazormia296.MozkeyIbG.metainfo.xml.in"
        )
        build = read("src/unix/fcitx5/BUILD.bazel")
        manifest = read("scripts/mozkey_fcitx5_install_manifest.txt")
        uninstaller = read("scripts/uninstall_mozkey_fcitx5")
        aur = read("packaging/aur/PKGBUILD")
        deb = read("scripts/package_mozkey_linux_deb")
        rpm = read("scripts/package_mozkey_linux_rpm")
        launcher = read("scripts/launch_fcitx5_mozkey_e2e")
        profile_relative = "fixtures/fcitx5-mozkey-ibg-e2e/profile"
        profile_path = ROOT / "scripts" / profile_relative

        self.assertIn("Library=fcitx5-mozkey-ibg", addon)
        self.assertIn("Addon=mozkey-ibg", input_method)
        self.assertIn("<id>io.github.kazormia296.MozkeyIbG</id>", appstream)
        self.assertIn('name = "fcitx5-mozkey-ibg.so"', build)
        for path in (
            "/lib/mozkey-ibg/mozc_server",
            "/share/fcitx5/addon/mozkey-ibg.conf",
            "/share/fcitx5/inputmethod/mozkey-ibg.conf",
            "/share/mozkey-ibg/fcitx5-addon-dir",
        ):
            self.assertIn(path, manifest)

        self.assertNotIn("fcitx5-mozkey.so", uninstaller)
        self.assertNotIn("/fcitx5/addon/mozkey.conf", manifest)
        self.assertNotIn("/fcitx5/inputmethod/mozkey.conf", manifest)
        self.assertNotIn("conflicts=('mozkey')", aur)
        self.assertNotRegex(deb, r"(?m)^(?:Breaks|Conflicts|Replaces): mozkey$")
        self.assertNotRegex(rpm, r"(?m)^(?:Conflicts|Obsoletes): +mozkey$")

        self.assertIn(
            f'profile_fixture="${{script_directory}}/{profile_relative}"',
            launcher,
        )
        self.assertTrue(profile_path.is_file())
        profile = profile_path.read_text(encoding="utf-8")
        self.assertIn("DefaultIM=mozkey-ibg", profile)
        self.assertIn("Name=mozkey-ibg", profile)
        self.assertFalse(
            (ROOT / "scripts/fixtures/fcitx5-mozkey-e2e/profile").exists()
        )

    def test_windows_registration_is_independent_from_upstream_mozkey(self) -> None:
        profile = read("src/win32/base/tsf_profile.cc")
        installer_builder = read("src/win32/installer/build_installer.py")
        installer = read("src/win32/installer/installer_oss_64bit.wxs")
        constants = read("src/base/const.h")

        self.assertIn("2D046FEA-2B23-4E77-946B-FC2AF48219DC", profile)
        self.assertIn("A5F4AF8E-7338-4A5C-9186-FF5B05B28393", profile)
        self.assertIn("422E6070-C76C-4F9B-96BE-FD9569E4B762", installer_builder)
        self.assertIn('Directory Id="MozkeyIbGDir" Name="MozkeyIbG"', installer)
        self.assertIn('Name="MozkeyIbGCacheService"', installer)
        self.assertIn('Name="Mozkey IbG Prelauncher"', installer)
        self.assertIn("kGrimodexConsumerBrokerShutdownEvent", constants)
        self.assertNotIn("Terminal Server\\SysProcs", installer)
        self.assertIn(
            'kProductInstallDirectoryName[] = "MozkeyIbG"', constants
        )
        self.assertIn('L"MozkeyIbGCacheService"', constants)
        system_util = read("src/base/system_util.cc")
        self.assertIn("kProductInstallDirectoryName", system_util)
        self.assertIn("FileUtil::FileExists(tip_path).ok()", system_util)

        for upstream_identifier in (
            "10A67BC8-22FA-4A59-90DC-2546652C56BF",
            "186F700C-71CF-43FE-A00E-AACB1D9E6D3D",
        ):
            self.assertNotIn(upstream_identifier, profile)
        self.assertNotIn(
            "DD94B570-B5E2-4100-9D42-61930C611D8A",
            installer_builder,
        )

    def test_windows_heartbeat_is_owned_by_normal_integrity_broker(self) -> None:
        prelauncher = read("src/win32/broker/prelauncher.cc")
        server = read("src/server/mozc_server.cc")

        start = prelauncher.index("StartDesktopConsumerHeartbeat()")
        launch_server = prelauncher.index("EnsureConnection()")
        wait = prelauncher.index("shutdown_listener.Wait")
        self.assertLess(start, launch_server)
        self.assertLess(launch_server, wait)
        self.assertIn("kGrimodexConsumerBrokerShutdownEvent", prelauncher)
        self.assertIn("#if defined(__APPLE__)", server)
        self.assertNotIn("#if defined(_WIN32)\n  std::unique_ptr<grimodex", server)

    def test_macos_bundle_receipt_and_launch_agents_are_fork_specific(self) -> None:
        config = read("src/config.bzl")
        package_builder = read("src/mac/build_package.py")
        distribution = read("src/mac/installer/distribution.xml")
        mac_build = read("src/mac/BUILD.bazel")

        bundle_prefix = "io.github.kazormia296.mozkey-ibg.inputmethod.Japanese"
        receipt = "io.github.kazormia296.mozkey-ibg.pkg.JapaneseInput"
        self.assertIn(bundle_prefix, config)
        self.assertIn(receipt, package_builder)
        self.assertIn(receipt, distribution)
        self.assertIn('outs = ["MozkeyIbG.pkg"]', mac_build)

        launch_agents = ROOT / "src/mac/installer/LaunchAgents"
        for helper in ("Converter", "Renderer"):
            path = launch_agents / f"{bundle_prefix}.{helper}.plist"
            self.assertTrue(path.is_file())
            self.assertIn(f"{bundle_prefix}.{helper}", path.read_text())
        self.assertFalse(
            (launch_agents / "org.mozc.inputmethod.Japanese.Converter.plist").exists()
        )

    def test_macos_installer_lifecycle_scripts_only_name_the_fork(self) -> None:
        # Upstream v1.0.0 added a preinstall/postinstall lifecycle that names
        # /Library/Input Methods/Mozc.app and org.mozc.*.  The fork's copies
        # must gate on, verify and signal Mozkey IbG only.
        scripts = {
            path: read(path)
            for path in (
                "src/mac/preflight_mozkey_release.sh",
                "src/mac/preflight_mozkey_stage1.sh",
                "src/mac/postflight_mozkey_oss_cleanup.sh",
                "src/mac/postflight_mozkey_oss_filter.py",
                "src/mac/installer/preflight_template.sh",
                "src/mac/installer/postflight_template.sh",
            )
        }
        for path, text in scripts.items():
            for upstream_identifier in (
                "org.mozc.",
                "/Mozc.app",
                "/Applications/Mozc",
                "UninstallMozc",
                "killall Mozc",
                "MozcConverter",
                "MozcRenderer",
                "killall mozc_zenz_scorer",
            ):
                self.assertNotIn(upstream_identifier, text, path)

        preflight = scripts["src/mac/preflight_mozkey_release.sh"]
        for required in (
            "_mozkey_pkg_id='io.github.kazormia296.mozkey-ibg.pkg.JapaneseInput'",
            "_mozkey_bundle_id='io.github.kazormia296.mozkey-ibg.inputmethod.Japanese'",
            "_mozkey_app='/Library/Input Methods/MozkeyIbG.app'",
            "_mozkey_uninstaller='/Applications/MozkeyIbG/UninstallMozkeyIbG.app'",
        ):
            self.assertIn(required, preflight)
        self.assertIn(
            "_mozkey_app='/Library/Input Methods/MozkeyIbG.app'",
            scripts["src/mac/postflight_mozkey_oss_cleanup.sh"],
        )
        self.assertIn(
            "MOZKEY_IBG_INSTALLER_SCRIPT_IDENTITY_GUARD",
            read("src/mac/BUILD.bazel"),
        )
        self.assertIn(
            "--define=MOZKEY_INSTALLABLE=1",
            read(".github/workflows/macos.yaml"),
        )

    def test_upstream_v1_runtime_surfaces_are_fork_specific(self) -> None:
        custom_action = read("src/win32/custom_action/custom_action.cc")
        self.assertNotIn('L"Mozc Offline - ', custom_action)
        for program in (
            "mozc_server",
            "mozc_tool",
            "mozc_renderer",
            "mozc_broker",
            "mozc_cache_service",
            "mozc_zenz_scorer",
            "llama-server",
        ):
            self.assertIn(
                f'L"Mozkey IbG Offline - Block {program} outbound"',
                custom_action,
            )

        # macOS scorer endpoint (socket and lock directory).
        socket_path = read("src/zenz/zenz_unix_socket_path.cc")
        self.assertIn('"/tmp/mozkey-ibg_zenz_"', socket_path)
        self.assertNotIn('"/tmp/mozc_zenz_"', socket_path)

        # TSF display attribute GUIDs added by upstream v1.0.0.
        display_attributes = read(
            "src/win32/tip/tip_display_attributes.cc"
        ).lower()
        for upstream_guid in (
            "ba4e3a96-8078-41fa-a1bb-72a64c3ed494",
            "519662d0-bef1-459d-bd03-b02f1ef8fd88",
            "84ca1e7e-3020-4d1c-8968-dda372d1e067",
            "8a4028e5-2dcd-4365-a5dc-71f67e797437",
        ):
            self.assertNotIn(upstream_guid, display_attributes)
            self.assertNotIn(
                "0x" + upstream_guid.split("-")[0] + ",", display_attributes
            )

        # Renderer window class, callback message and per-user registry state.
        self.assertIn(
            'L"MozkeyIbGConversionStatusWindow"',
            read("src/renderer/win32/conversion_status_window.h"),
        )
        self.assertIn(
            'L"mozkeyibg.renderer.submit.message"', read("src/base/const.h")
        )
        config_dialog = read("src/gui/config_dialog/config_dialog.cc")
        self.assertNotIn("Software\\\\Mozkey\\\\", config_dialog)
        self.assertNotIn("koyasi777/mozkey", read("src/gui/about_dialog/about_dialog.cc"))

        for verifier in (
            "tools/release/verify_source_contract.py",
            "tools/release/Verify-WindowsMsiCandidate.ps1",
            ".github/scripts/windows_zenz_arm64_installer_lifecycle_audit.ps1",
        ):
            text = read(verifier)
            self.assertIn("422E6070-C76C-4F9B-96BE-FD9569E4B762", text, verifier)
            self.assertNotIn("Mozc Offline - ", text, verifier)
        self.assertNotIn(
            "'DD94B570-B5E2-4100-9D42-61930C611D8A')",
            read("tools/release/Verify-WindowsMsiCandidate.ps1"),
        )

    def test_release_asset_names_use_the_fork_identity(self) -> None:
        windows = read(".github/workflows/windows.yaml")
        macos = read(".github/workflows/macos.yaml")
        linux = read(".github/workflows/linux.yaml")
        release = read(".github/workflows/release.yaml")

        self.assertIn("MozkeyIbG_v", windows)
        self.assertNotIn("Mozkey_v", windows)
        self.assertIn("MozkeyIbG_v", macos)
        self.assertIn("src/bazel-bin/mac/MozkeyIbG.pkg", macos)
        self.assertNotIn("Mozkey_v", macos)
        self.assertIn("dist/mozkey-ibg-v*", linux)
        self.assertIn('"mozkey-ibg-v${RELEASE_VERSION}', release)
        self.assertIn('--title "Mozkey IbG v${RELEASE_VERSION}"', release)
        self.assertNotIn('--title "Mozkey v${RELEASE_VERSION}"', release)


if __name__ == "__main__":
    unittest.main()
