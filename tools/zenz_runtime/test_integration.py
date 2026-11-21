"""Functional tests for consumer sync as used by future upgrade operators."""
import copy
import json
import pathlib
import struct
import tempfile
import unittest
from unittest.mock import patch

import runtime_lock as rl


class SyncIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lock = json.loads((pathlib.Path(__file__).resolve().parents[2] / rl.LOCK).read_text())

    def test_sync_preview_is_read_only_and_write_changes_only_target(self):
        expected = self.lock['windows']['arm64']['sha256']
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            file = root / '.github/scripts/windows_zenz_arm64_installer_lifecycle_audit.ps1'
            file.parent.mkdir(parents=True)
            prior = ('$ExpectedBaselineLlamaSha = "A826D6BC47645A9E9907FEEA49109C0FF25D49CFD268D3349AC5F9BD6DBAAEC0"\n'
                     '$ExpectedCurrentLlamaSha = "0ACD17D6EE5E361AFC06A7DCCD06EF012A00669546535889B874D5C3BB3DE81B"\n')
            file.write_text(prior, encoding='utf-8')
            rule = next(x for x in rl.rules(self.lock) if x.label == '$ExpectedCurrentLlamaSha')
            with patch.object(rl, 'rules', return_value=[rule]):
                issues, changes = rl.validate_consumers(root, self.lock, write=False)
                self.assertEqual(len(issues), 1)
                self.assertEqual(changes, {})
                self.assertEqual(file.read_text(), prior)
                issues, changes = rl.validate_consumers(root, self.lock, write=True)
                self.assertEqual(issues, [])
                self.assertEqual(len(changes), 1)
                original, updated = changes[file]
                self.assertIn(expected.encode(), updated)
                self.assertEqual(file.read_bytes(), original)

    def test_unknown_consumer_layout_fails_without_changing_it(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            file = root / '.github/workflows/windows.yaml'
            file.parent.mkdir(parents=True)
            file.write_bytes(b'# variable was renamed by upstream\n')
            rule = next(x for x in rl.rules(self.lock) if x.label == 'Windows CI installed ARM64 expected SHA')
            with patch.object(rl, 'rules', return_value=[rule]):
                issues, changes = rl.validate_consumers(root, self.lock, write=True)
                self.assertEqual(len(issues), 1)
                self.assertEqual(changes, {})
                self.assertEqual(file.read_bytes(), b'# variable was renamed by upstream\n')


if __name__ == '__main__':
    unittest.main()
