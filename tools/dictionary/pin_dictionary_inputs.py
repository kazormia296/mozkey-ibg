#!/usr/bin/env python3
"""Enforce pinned merge-ut inputs in a disposable checkout (not repo files)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import urllib.request


def read_lock(path: Path) -> dict:
    data = json.loads(path.read_text(encoding='utf-8-sig'))
    if data.get('schema_version') != 1:
        raise ValueError('Unsupported dictionary input lock schema')
    if not re.fullmatch('[a-f0-9]{40}', data['merge_ut']['revision']):
        raise ValueError('Malformed pinned parent revision')
    for child in data['merge_ut']['children']:
        if not re.fullmatch('[a-f0-9]{40}', child['revision']):
            raise ValueError('Malformed pinned child revision')
        if not re.fullmatch(r'mozcdic-ut-[a-z-]+', child['name']):
            raise ValueError('Unexpected child repository name')
        if child['repository'] != 'utuhiro78/' + child['name']:
            raise ValueError('Unexpected child repository identity')
    for name in ('wikipedia', 'nico_pixiv', 'personal_names'):
        if not re.fullmatch('[0-9A-Fa-f]{64}', data[name]['sha256']):
            raise ValueError(f'Malformed {name} SHA256')
        if not isinstance(data[name]['bytes'], int) or data[name]['bytes'] <= 0:
            raise ValueError(f'Malformed {name} byte count')
    if data['wikipedia']['filename'] != f"jawiki-{data['wikipedia']['snapshot']}-pages-articles-multistream-index.txt.bz2":
        raise ValueError('Wikipedia filename/snapshot mismatch')
    if data['wikipedia']['url'] != f"https://dumps.wikimedia.org/jawiki/{data['wikipedia']['snapshot']}/{data['wikipedia']['filename']}":
        raise ValueError('Wikipedia must use a dated snapshot, not latest')
    return data


def fingerprint(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest().upper()


def verify(path: Path, spec: dict) -> bool:
    return path.is_file() and fingerprint(path) == (spec['bytes'], spec['sha256'].upper())


def stage_wiki(repo: Path, lock: dict) -> Path:
    spec = lock['wikipedia']
    filename = spec['filename']
    target = repo / 'dist' / 'dictionary' / 'locked-sources' / filename
    if verify(target, spec):
        return target.resolve()
    if target.exists():
        raise RuntimeError(f'Pinned Wikipedia cache has wrong SHA256, refusing overwrite: {target}')

    # Existing caches are candidates only, validated by content identity.
    work = repo / 'dist' / 'dictionary' / 'merge-ut'
    candidates = [work / 'merge-ut-dictionaries' / 'src' / 'merge' / filename]
    candidates.extend(sorted(work.glob('run-*/merge-ut-dictionaries/src/merge/' + filename)))
    target.parent.mkdir(parents=True, exist_ok=True)
    for candidate in candidates:
        if verify(candidate, spec):
            with tempfile.NamedTemporaryFile(dir=target.parent, suffix='.part', delete=False) as tf:
                tmp = Path(tf.name)
            try:
                shutil.copyfile(candidate, tmp)
                if not verify(tmp, spec):
                    raise RuntimeError(f'Wikipedia cache changed during copy: {candidate}')
                os.replace(tmp, target)
                return target.resolve()
            finally:
                tmp.unlink(missing_ok=True)

    # Do not touch generated dictionaries if a stable snapshot is unavailable.
    with tempfile.NamedTemporaryFile(dir=target.parent, suffix='.part', delete=False) as tf:
        tmp = Path(tf.name)
    try:
        req = urllib.request.Request(spec['url'], headers={'User-Agent': 'Mozkey-dictionary-reproducibility/1.0'})
        total = 0
        with urllib.request.urlopen(req, timeout=60) as response, tmp.open('wb') as output:
            while True:
                block = response.read(1024 * 1024)
                if not block:
                    break
                total += len(block)
                if total > spec['bytes']:
                    raise RuntimeError('Downloaded Wikipedia index exceeds locked byte count')
                output.write(block)
        if not verify(tmp, spec):
            raise RuntimeError(f'Wikipedia downloaded snapshot differs from locked SHA256: {spec["url"]}')
        os.replace(tmp, target)
    finally:
        tmp.unlink(missing_ok=True)
    return target.resolve()


def patch_merge_make(text: str, lock: dict) -> str:
    for child in lock['merge_ut']['children']:
        name, rev = child['name'], child['revision']
        url = 'https://github.com/' + child['repository'] + '.git'
        needle = 'git clone --depth 1 ' + url
        if text.count(needle) != 1:
            raise RuntimeError(f'Expected exactly one clone command for {name}')
        # Parent make.sh does rm -rf mozcdic-ut* before these commands.
        replacement = (f'git init -q {name} &&\n'
                       f'    git -C {name} remote add origin {url} &&\n'
                       f'    git -C {name} fetch --depth 1 origin {rev} &&\n'
                       f'    git -C {name} checkout -q --detach FETCH_HEAD &&\n'
                       f'    test "$(git -C {name} rev-parse HEAD)" = "{rev}"')
        text = text.replace(needle, replacement)
    return text


def patch_merge_python(text: str) -> str:
    if 'import os\n' not in text or 'MOZKEY_DICTIONARY_OSS_DIR' not in text:
        raise RuntimeError('Expected managed merge-ut/Mozkey base dictionary patch is missing')
    header = 'def generate_jawiki_hit_dict():\n'
    begin = text.find(header)
    if begin < 0 or text.count(header) != 1:
        raise RuntimeError('Missing/duplicated Wikipedia function in patched script')
    end = text.find('    jawiki_index = []\n', begin)
    if end < 0 or text.count('    jawiki_index = []\n') != 1:
        raise RuntimeError('Cannot locate Wikipedia index parsing code')
    original = text[begin:end]
    if 'jawiki-latest' not in original or 'jawiki_index_file' not in original:
        raise RuntimeError('Unexpected upstream Wikipedia fetch block; manual review required')
    replacement = (header +
                   '    # Mozkey: never consult latest RSS; index is pinned and hash-verified.\n'
                   '    index_path = os.environ.get("MOZKEY_JAWIKI_INDEX_BZ2")\n'
                   '    if not index_path:\n'
                   '        raise RuntimeError("MOZKEY_JAWIKI_INDEX_BZ2 is required")\n'
                   '    jawiki_index_file = Path(index_path)\n'
                   '    if not jawiki_index_file.is_file():\n'
                   '        raise FileNotFoundError(jawiki_index_file)\n\n')
    text = text[:begin] + replacement + text[end:]
    assert 'jawiki-latest' not in text
    return text


def patch_staged(make_path: Path, python_path: Path, lock: dict) -> None:
    # Compute both transformations before mutating either isolated file.
    make_text = make_path.read_text(encoding='utf-8')
    py_text = python_path.read_text(encoding='utf-8')
    new_make = patch_merge_make(make_text, lock)
    new_python = patch_merge_python(py_text)
    make_path.write_text(new_make, encoding='utf-8', newline='\n')
    python_path.write_text(new_python, encoding='utf-8', newline='\n')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--lock', type=Path, required=True)
    sub = ap.add_subparsers(dest='action', required=True)
    sub.add_parser('validate')
    s = sub.add_parser('stage-wiki')
    s.add_argument('--repo-root', type=Path, required=True)
    s = sub.add_parser('patch-staged-merge')
    s.add_argument('--make', type=Path, required=True)
    s.add_argument('--merge-script', type=Path, required=True)
    args = ap.parse_args()
    lock = read_lock(args.lock)
    if args.action == 'validate':
        print('LOCK_SCHEMA=PASS')
    elif args.action == 'stage-wiki':
        print(stage_wiki(args.repo_root, lock))
    elif args.action == 'patch-staged-merge':
        patch_staged(args.make, args.merge_script, lock)
        print('PINNED_DEPENDENT_CLONES_AND_WIKIPEDIA_CODE=PASS')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError, OSError) as e:
        print(f'MOZKEY_INPUT_LOCK_ERROR: {e}', file=sys.stderr)
        raise SystemExit(1)
