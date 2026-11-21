#!/usr/bin/env python3
"""Offline contract checks for the patched upstream merge_dictionaries.py.

Run after applying patches/merge_ut_mozkey_local.patch to the isolated
upstream checkout. No downloads or changes to the Mozkey source tree.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


def load_module(path: Path):
    spec = importlib.util.spec_from_file_location("mozkey_test_merge_ut", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot import staged merge script: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MozkeyDictionaryBaseContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = load_module(Path(os.environ["MOZKEY_TEST_MERGE_SCRIPT"]))

    def test_requires_explicit_mozkey_directory(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, "MOZKEY_DICTIONARY_OSS_DIR"):
                self.script.get_mozc_entry()

    def test_reads_local_dictionary_and_normalizes(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "id.def").write_text("123 名詞,一般,\n", encoding="utf-8")
            (base / "dictionary00.txt").write_text(
                "てすと\t10\t11\t7000\tＴｅｓｔ\n"
                "うみ\t10\t11\t5000\t海\n"  # 1-char surfaces are ignored
                "こわれ\t9\n",  # matches the local patch's len < 5 skip
                encoding="utf-8",
            )
            with mock.patch.dict(os.environ, {"MOZKEY_DICTIONARY_OSS_DIR": str(base)}):
                entries, noun_id = self.script.get_mozc_entry()

            self.assertEqual(noun_id, "123")
            self.assertEqual(entries, [["てすと", "10", "11", "7000", "Test"]])

            # Base Mozkey candidates should suppress duplicate UT rows.
            filtered = self.script.remove_duplicate(
                entries, [["てすと", "id_ut", "id_ut", "9000", "Test"],
                          ["ほか", "id_ut", "id_ut", "9000", "その他"]],
            )
            self.assertEqual(filtered, [["ほか", "id_ut", "id_ut", "9000", "その他"]])

    def test_missing_input_is_failure_not_fallback_to_google(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(os.environ, {"MOZKEY_DICTIONARY_OSS_DIR": directory}):
                with self.assertRaises(FileNotFoundError):
                    self.script.get_mozc_entry()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=Path, required=True)
    args = parser.parse_args()
    if not args.script.is_file():
        parser.error(f"not a file: {args.script}")
    os.environ["MOZKEY_TEST_MERGE_SCRIPT"] = str(args.script.resolve())
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(MozkeyDictionaryBaseContract)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
