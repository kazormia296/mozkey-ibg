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
