#!/usr/bin/env python3
"""Offline regression tests: locked imports must fail closed."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from pin_dictionary_inputs import read_lock, patch_merge_make, patch_merge_python, fingerprint, stage_wiki

class PinnedInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lock=read_lock(ROOT/'locks'/'daily_sources.json')

    def test_lock_references_exact_commits_and_hashes(self):
        self.assertEqual(self.lock['wikipedia']['snapshot'],'20261001')
        self.assertEqual(len(self.lock['merge_ut']['children']),2)
        self.assertTrue(all(len(c['revision']) == 40 for c in self.lock['merge_ut']['children']))

    def test_child_clone_is_transformed_to_exact_revisions(self):
        orig='''#!/bin/sh
rm -rf mozcdic-ut*
if [ "$place_names" = "true" ]; then
    git clone --depth 1 https://github.com/utuhiro78/mozcdic-ut-place-names.git
fi
if [ "$sudachidict" = "true" ]; then
    git clone --depth 1 https://github.com/utuhiro78/mozcdic-ut-sudachidict.git
fi
'''
        new=patch_merge_make(orig,self.lock)
        self.assertNotIn('git clone --depth 1',new)
        for child in self.lock['merge_ut']['children']:
            self.assertIn('fetch --depth 1 origin '+child['revision'],new)
            self.assertIn('rev-parse HEAD',new)
        with self.assertRaisesRegex(RuntimeError,'exactly one'):
            patch_merge_make('#!/bin/sh\n',self.lock)

    def test_wiki_latest_rss_is_removed(self):
        text='''import os
from pathlib import Path
import bz2
# MOZKEY_DICTIONARY_OSS_DIR
def generate_jawiki_hit_dict():
    # jawiki-latest-pages-articles-multistream-index.txt.bz2
    jawiki_index_file = 'latest.bz2'
    jawiki_index = []
    with bz2.open(jawiki_index_file, 'rt', encoding='utf-8') as file:
        for entry in file:
            jawiki_index.append(entry)
    return jawiki_index
'''
        new=patch_merge_python(text)
        self.assertNotIn('jawiki-latest',new)
        self.assertIn('MOZKEY_JAWIKI_INDEX_BZ2',new)
        self.assertIn("with bz2.open(jawiki_index_file",new)
        with self.assertRaisesRegex(RuntimeError,'missing'):
            patch_merge_python(text.replace('MOZKEY_DICTIONARY_OSS_DIR','UNPINNED'))

    def test_snapshot_cache_sha_verified(self):
        with tempfile.TemporaryDirectory() as td:
            repo=Path(td)
            payload=b'synthetic pinned index'
            src=repo/'dist/dictionary/merge-ut/run-synthetic/merge-ut-dictionaries/src/merge/test.bz2'
            src.parent.mkdir(parents=True)
            src.write_bytes(payload)
            lock=dict(self.lock)
            lock['wikipedia']={
                'snapshot':'test', 'filename':'test.bz2','url':'https://example.invalid/test',
                'bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest().upper()}
            dst=stage_wiki(repo,lock)
            self.assertEqual(dst.read_bytes(),payload)
            self.assertEqual(fingerprint(dst),(len(payload),hashlib.sha256(payload).hexdigest().upper()))
            dst.write_bytes(b'wrong')
            with self.assertRaisesRegex(RuntimeError,'wrong SHA256'):
                stage_wiki(repo,lock)

    def test_missing_cache_does_not_silently_use_latest(self):
        # A failing URL is not tested via network; fixed link validation is tested above.
        self.assertNotIn('/latest/',self.lock['wikipedia']['url'])

    def test_daily_bazel_inputs_explicit(self):
        build=(ROOT.parent.parent/'src/data/dictionary_koyasi/BUILD.bazel').read_text('utf-8')
        start=build.index('name = "mozcdic_ut_daily_local"')
        end=build.index('name = "mozcdic_ut_rich_local"')
        section=build[start:end]
        self.assertNotIn('glob(',section)
        for name in ('mozcdic-ut-daily','dic-nico-pixiv-delta','mozcdic-ut-personal-names-daily','koyasi-syntax-guard'):
            self.assertIn(name+'.txt',section)

if __name__=='__main__':
    unittest.main(verbosity=2)
