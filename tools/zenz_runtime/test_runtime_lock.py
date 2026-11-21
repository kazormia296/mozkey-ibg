import copy
import json
import pathlib
import struct
import tempfile
import unittest
from unittest.mock import patch

import runtime_lock as rl


class RuntimeLockTests(unittest.TestCase):
    def setUp(self):
        self.data = json.loads((rl.ROOT / rl.LOCK).read_text(encoding="utf-8")) if (rl.ROOT / rl.LOCK).exists() else None
        # In a ZIP installed at repository root, ROOT points to actual checkout.
        # When running directly in the staging tree, that also works.
        if self.data is None:
            self.data = json.loads((pathlib.Path(__file__).resolve().parents[2] / rl.LOCK).read_text())

    def test_lock_schema_and_platform_independence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / rl.LOCK
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(self.data), encoding="utf-8")
            loaded = rl.read_lock(root)
            self.assertEqual(loaded["windows"]["upstream"], self.data["windows"]["upstream"])
            self.assertEqual(loaded["macos"]["upstream"], self.data["macos"]["upstream"])
            self.assertIsNot(loaded["windows"]["upstream"], loaded["macos"]["upstream"])
            loaded["macos"]["upstream"]["tag"] = "independent-macos-test-tag"
            self.assertEqual(loaded["windows"]["upstream"], self.data["windows"]["upstream"])

    def test_reject_moving_revision_and_unknown_patch(self):
        changed = copy.deepcopy(self.data)
        changed["windows"]["upstream"]["commit"] = "main"
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / rl.LOCK
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaises(rl.LockError):
                rl.read_lock(root)
        changed = copy.deepcopy(self.data)
        changed["windows"]["arm64"]["source_patches"] = ["untracked"]
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = root / rl.LOCK
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaises(rl.LockError):
                rl.read_lock(root)

    def test_win_current_hash_assignment_never_touches_historical_baseline(self):
        rule = next(r for r in rl.rules(self.data) if r.label == "$ExpectedCurrentLlamaSha")
        src = ('$ExpectedBaselineLlamaSha = "A826D6BC47645A9E9907FEEA49109C0FF25D49CFD268D3349AC5F9BD6DBAAEC0"\r\n'
               '$ExpectedCurrentLlamaSha = "0ACD17D6EE5E361AFC06A7DCCD06EF012A00669546535889B874D5C3BB3DE81B"\r\n')
        output, issues = rl.apply_rules(src, [rule], write=True)
        self.assertEqual(len(issues), 1)
        self.assertIn(self.data["windows"]["arm64"]["sha256"], output)
        self.assertIn('A826D6BC47645A9E9907FEEA49109C0FF25D49CFD268D3349AC5F9BD6DBAAEC0', output)
        self.assertEqual(output.count('\r\n'), 2)

    def test_win_document_arm64_two_distinct_fields(self):
        rules = [r for r in rl.rules(self.data) if r.label in
                 ("Windows contract arm64 EXE", "Windows contract arm64 provenance")]
        text = ("### ARM64\n\n- source-tree file: `arm64/llama-server.exe`\n- PE: ARM64 / 0xAA64\n"
                "- SHA256: `0ACD17D6EE5E361AFC06A7DCCD06EF012A00669546535889B874D5C3BB3DE81B`\n"
                "- Portable ARM64 candidate SHA256: `0ACD17D6EE5E361AFC06A7DCCD06EF012A00669546535889B874D5C3BB3DE81B`.\n")
        changed, issues = rl.apply_rules(text, rules, write=True)
        self.assertEqual(len(issues), 2)
        self.assertEqual(changed.count(self.data["windows"]["arm64"]["sha256"]), 2)

    def test_mac_generated_build_info_pin_is_checked_twice(self):
        rule = next(r for r in rl.rules(self.data) if r.label == "macOS build-info short commit")
        short = self.data["macos"]["upstream"]["commit"][:7]
        one = f"'LLAMA_COMMIT = \"{short}\"'\n"
        text = one * 2
        _, issues = rl.apply_rules(text, [rule], write=False)
        self.assertEqual(issues, [])
        invalid = "0000000" if short != "0000000" else "1111111"
        stale = text.replace(short, invalid, 1)
        _, issues = rl.apply_rules(stale, [rule], write=False)
        self.assertEqual(len(issues), 1)
        self.assertIn("stale", issues[0])
        repaired, issues = rl.apply_rules(stale, [rule], write=True)
        self.assertEqual(len(issues), 1)
        self.assertEqual(repaired, text)

    def test_missing_duplicate_consumer_fails_closed(self):
        rule = next(r for r in rl.rules(self.data) if r.label == "macOS build-info short commit")
        short = self.data["macos"]["upstream"]["commit"][:7]
        text = f"'LLAMA_COMMIT = \"{short}\"'\n"
        updated, issues = rl.apply_rules(text, [rule], write=True)
        self.assertEqual(updated, text)
        self.assertEqual(len(issues), 1)
        self.assertIn("expected 2 matches", issues[0])

    def test_mac_hash_consumer_case_matches_existing_shasum_convention(self):
        """macOS shasum outputs lowercase: uppercase would break strict shell compares."""
        for label in ("PATCH_SHA256", "DUAL_LIVENESS_PATCH_SHA256",
                      "EXPECTED_PATCH_SHA256", "EXPECTED_PATCH_SHA",
                      "EXPECTED_DUAL_LIVENESS_PATCH_SHA"):
            rule = next(r for r in rl.rules(self.data) if r.label == label)
            self.assertEqual(rule.value, rule.value.lower())
            sample = f'{label}="{rule.value}"\n'
            output, errors = rl.apply_rules(sample, [rule], write=False)
            self.assertEqual(errors, [], label)
            self.assertEqual(output, sample)
            uppercase = sample.replace(rule.value, rule.value.upper())
            _, errors = rl.apply_rules(uppercase, [rule], write=False)
            self.assertEqual(len(errors), 1, label)

    def test_mac_readme_two_sha256_sections_are_not_ambiguous(self):
        """The tokenizer SHA and model SHA share the same heading in README."""
        rule = next(r for r in rl.rules(self.data) if r.label == "macOS README tokenizer SHA")
        tokenizer = self.data['patches']['tokenizer']['sha256'].lower()
        model = self.data['model']['sha256'].lower()
        content = ("```text\npatches/llama-vocab-gpt2-small-japanese-char.patch\n"
                   f"SHA-256:\n{tokenizer}\n```\n"
                   f"```text\nmodel-file.gguf\nSHA-256:\n{model}\n```\n")
        output, errors = rl.apply_rules(content, [rule], write=False)
        self.assertEqual(errors, [])
        self.assertEqual(output, content)
        # Update only tokenizer while model hash remains untouched.
        stale = content.replace(tokenizer, '0' * 64, 1)
        output, errors = rl.apply_rules(stale, [rule], write=True)
        self.assertEqual(len(errors), 1)
        self.assertEqual(output, content)
        self.assertIn(model, output)
        # Absence of the named tokenizer path is structural drift, not a match
        # against the unrelated model SHA-256 section.
        wrong = content.replace('patches/llama-vocab-gpt2-small-japanese-char.patch', 'patches/other.patch')
        _, errors = rl.apply_rules(wrong, [rule], write=False)
        self.assertEqual(len(errors), 1)
        self.assertIn('expected 1 matches', errors[0])

    def test_pe_machine_reader(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "test.exe"
            raw = bytearray(512)
            raw[:2] = b"MZ"
            struct.pack_into("<I", raw, 0x3c, 0x80)
            raw[0x80:0x84] = b"PE\0\0"
            struct.pack_into("<H", raw, 0x84, 0xAA64)
            path.write_bytes(raw)
            self.assertEqual(rl.pe_machine(path), "0xAA64")
            raw[0x80] = 0
            path.write_bytes(raw)
            with self.assertRaises(rl.LockError):
                rl.pe_machine(path)

    def test_model_and_patch_sha_check_refuses_incorrect_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            data = copy.deepcopy(self.data)
            for key in ("tokenizer", "dual_liveness"):
                field = data["patches"][key]
                path = root / field["file"]
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"test fixture")
                field["sha256"] = rl.sha256_file(path)
            model = root / data["model"]["windows_file"]
            model.parent.mkdir(parents=True, exist_ok=True)
            model.write_bytes(b"GGUF test fixture")
            data["model"]["sha256"] = rl.sha256_file(model)
            for arch, machine in (("x64", 0x8664), ("arm64", 0xAA64)):
                path = root / data["windows"][arch]["path"]
                path.parent.mkdir(parents=True, exist_ok=True)
                raw = bytearray(256)
                raw[:2] = b"MZ"
                struct.pack_into("<I", raw, 0x3c, 0x80)
                raw[0x80:0x84] = b"PE\0\0"
                struct.pack_into("<H", raw, 0x84, machine)
                path.write_bytes(raw)
                data["windows"][arch]["sha256"] = rl.sha256_file(path)
            self.assertEqual(len(rl.validate_assets(root, data)), 5)
            model.write_bytes(b"altered")
            with self.assertRaisesRegex(rl.LockError, 'model SHA256 mismatch'):
                rl.validate_assets(root, data)

    def test_windows_workflow_pin_stale_detection(self):
        rule = next(r for r in rl.rules(self.data) if r.label == "Windows CI installed ARM64 expected SHA")
        text = '          EXPECTED_LLAMA_SHA256: 0ACD17D6EE5E361AFC06A7DCCD06EF012A00669546535889B874D5C3BB3DE81B\n'
        _, issues = rl.apply_rules(text, [rule], write=False)
        self.assertEqual(len(issues), 1)
        updated, _ = rl.apply_rules(text, [rule], write=True)
        _, issues = rl.apply_rules(updated, [rule], write=False)
        self.assertEqual(issues, [])


if __name__ == '__main__':
    unittest.main()
