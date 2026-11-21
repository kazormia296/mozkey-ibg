import itertools
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from mac import tweak_installer_files as target


def _create_runtime(top_dir: Path, *, oss: bool):
    name = "MozkeyIbG" if oss else "GoogleJapaneseInput"
    resources = (
        top_dir
        / "root/Library/Input Methods"
        / f"{name}.app/Contents/Resources"
        / "ZenzRuntime"
    )
    models = resources / "models"
    licenses = resources / "licenses"
    models.mkdir(parents=True)
    licenses.mkdir()

    expected = {
        resources / "llama-server": 0o755,
        resources / "mozc_zenz_scorer": 0o755,
        resources / "mozc_zenz_guard": 0o755,
        models / target._ZENZ_MODEL_NAME: 0o644,
        resources / "BUILD-CONTRACT.txt": 0o644,
    }
    expected.update(
        {
            licenses / license_name: 0o644
            for license_name in target._ZENZ_RUNTIME_LICENSE_NAMES
        }
    )
    for path, expected_mode in expected.items():
        path.write_bytes(b"runtime fixture")
        path.chmod(0o644 if expected_mode == 0o755 else 0o755)
    return resources, expected


class TweakInstallerFilesRuntimeLayoutTest(unittest.TestCase):
    def test_normalizes_exact_modes_at_canonical_brand_paths(self):
        for oss in (True, False):
            with self.subTest(oss=oss), tempfile.TemporaryDirectory() as temporary:
                top_dir = Path(temporary) / "installer"
                resources, expected = _create_runtime(top_dir, oss=oss)

                target.NormalizeAndValidateZenzRuntime(str(top_dir), oss)

                expected_brand = "MozkeyIbG" if oss else "GoogleJapaneseInput"
                self.assertEqual(
                    resources,
                    top_dir
                    / "root/Library/Input Methods"
                    / f"{expected_brand}.app/Contents/Resources"
                    / "ZenzRuntime",
                )
                for path, expected_mode in expected.items():
                    self.assertEqual(path.stat().st_mode & 0o777, expected_mode)

    def test_rejects_missing_empty_and_symlinked_runtime_files(self):
        mutations = ("missing", "empty", "symlink")
        names = ("llama-server", "mozc_zenz_guard", "BUILD-CONTRACT.txt")
        for mutation, runtime_name in itertools.product(mutations, names):
            with (
                self.subTest(mutation=mutation, runtime_name=runtime_name),
                tempfile.TemporaryDirectory() as temporary,
            ):
                top_dir = Path(temporary) / "installer"
                resources, _ = _create_runtime(top_dir, oss=True)
                server = resources / runtime_name
                if mutation == "missing":
                    server.unlink()
                elif mutation == "empty":
                    server.write_bytes(b"")
                else:
                    server.unlink()
                    symlink_target = Path(temporary) / "outside-runtime"
                    symlink_target.write_bytes(b"runtime fixture")
                    os.symlink(symlink_target, server)

                with self.assertRaisesRegex(
                    ValueError, "invalid Zenz runtime file"
                ):
                    target.NormalizeAndValidateZenzRuntime(str(top_dir), True)

    def test_validation_runs_after_productbuild_and_before_codesign(self):
        with tempfile.TemporaryDirectory() as temporary:
            events = []
            args = SimpleNamespace(
                input="input.zip",
                output=str(Path(temporary) / "output.zip"),
                noqt=True,
                productbuild=True,
                oss=True,
                channel="dev",
                codesign_identity="-",
            )
            with (
                mock.patch.object(target.util, "RunOrDie"),
                mock.patch.object(
                    target,
                    "TweakForProductbuild",
                    side_effect=lambda *unused: events.append("productbuild"),
                ),
                mock.patch.object(
                    target,
                    "NormalizeAndValidateZenzRuntime",
                    side_effect=lambda *unused: events.append("validate"),
                ),
                mock.patch.object(
                    target,
                    "Codesign",
                    side_effect=lambda *unused: events.append("codesign"),
                ),
            ):
                target.TweakInstallerFiles(args, temporary)

            self.assertEqual(events, ["productbuild", "validate", "codesign"])

    def test_validation_failure_aborts_codesign(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = SimpleNamespace(
                input="input.zip",
                output=str(Path(temporary) / "output.zip"),
                noqt=True,
                productbuild=True,
                oss=True,
                channel="dev",
                codesign_identity="-",
            )
            with (
                mock.patch.object(target.util, "RunOrDie"),
                mock.patch.object(target, "TweakForProductbuild"),
                mock.patch.object(
                    target,
                    "NormalizeAndValidateZenzRuntime",
                    side_effect=ValueError("invalid Zenz runtime file"),
                ),
                mock.patch.object(target, "Codesign") as codesign,
            ):
                with self.assertRaisesRegex(
                    ValueError, "invalid Zenz runtime file"
                ):
                    target.TweakInstallerFiles(args, temporary)

            codesign.assert_not_called()


class TweakInstallerFilesCodesignTest(unittest.TestCase):
    def test_signs_nested_runtime_before_enclosing_app(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "MozkeyIbG.app"
            resources = app / "Contents/Resources/ZenzRuntime"
            resources.mkdir(parents=True)
            server = resources / "llama-server"
            scorer = resources / "mozc_zenz_scorer"
            guard = resources / "mozc_zenz_guard"
            for executable in (server, scorer, guard):
                executable.write_bytes(b"mach-o fixture")
                executable.chmod(0o755)

            with mock.patch.object(target.util, "RunOrDie") as run:
                target.Codesign(temporary, "-")

            commands = [call.args[0] for call in run.call_args_list]
            signed_targets = [Path(command[-1]) for command in commands]
            # Each helper is signed exactly once, before the enclosing app.
            self.assertEqual(len(signed_targets), 4)
            self.assertEqual(set(signed_targets[:3]), {server, scorer, guard})
            self.assertEqual(signed_targets[-1], app)

    def test_release_identity_enables_timestamped_hardened_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "MozkeyIbGConverter.app"
            resources = app / "Contents/Resources"
            resources.mkdir(parents=True)
            server = resources / "llama-server"
            server.write_bytes(b"mach-o fixture")
            server.chmod(0o755)

            with mock.patch.object(target.util, "RunOrDie") as run:
                target.Codesign(temporary, "Developer ID Application: Example")

            for call in run.call_args_list:
                command = call.args[0]
                self.assertIn("--timestamp", command)
                self.assertIn("--options=runtime", command)

    def test_release_identity_uses_explicit_temporary_keychain(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "MozkeyIbGConverter.app"
            (app / "Contents/MacOS").mkdir(parents=True)
            keychain = "/private/tmp/mozkey-signing.keychain-db"

            with mock.patch.object(target.util, "RunOrDie") as run:
                target.Codesign(
                    temporary,
                    "Developer ID Application: Example",
                    keychain,
                )

            self.assertGreaterEqual(run.call_count, 1)
            for call in run.call_args_list:
                command = call.args[0]
                self.assertEqual(command[command.index("--keychain") + 1], keychain)

    def test_signs_every_nested_dylib_before_enclosing_app(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "ConfigDialog.app"
            plugins = app / "Contents/PlugIns/imageformats"
            plugins.mkdir(parents=True)
            plugin = plugins / "libqjpeg.dylib"
            plugin.write_bytes(b"mach-o fixture")
            plugin.chmod(0o644)

            with mock.patch.object(target.util, "RunOrDie") as run:
                target.Codesign(temporary, "-")

            signed_targets = [Path(call.args[0][-1]) for call in run.call_args_list]
            self.assertEqual(signed_targets, [plugin, app])

    def test_refuses_nested_runtime_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            resources = (
                Path(temporary) / "MozkeyIbGConverter.app/Contents/Resources"
            )
            resources.mkdir(parents=True)
            target_file = resources / "other"
            target_file.write_bytes(b"fixture")
            os.symlink(target_file.name, resources / "llama-server")

            with self.assertRaisesRegex(ValueError, "refusing to codesign symlink"):
                target.Codesign(temporary, "-")


if __name__ == "__main__":
    unittest.main()
