#!/usr/bin/env python3
"""Mozkey Zenz runtime pin synchronization and fail-closed integrity checks.

No network access, Git writes, installer changes, or execution of binaries.
Only `sync --write` edits approved text consumers; binaries and historical
installer-baseline pins are NEVER rewritten.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import pathlib
import re
import struct
import sys
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOCK = "src/zenz_runtime/llama_runtime.lock.json"
SHA = re.compile(r"^[0-9a-fA-F]{64}$")
COMMIT = re.compile(r"^[0-9a-fA-F]{40}$")


class LockError(ValueError):
    pass


def require(ok: bool, message: str) -> None:
    if not ok:
        raise LockError(message)


def read_lock(root: pathlib.Path) -> dict:
    filename = root / LOCK
    data = json.loads(filename.read_text(encoding="utf-8-sig"))
    require(type(data) is dict and data.get("schema_version") == 1,
            "unsupported lock schema_version")
    for platform in ("windows", "macos"):
        u = data[platform]["upstream"]
        require(u["repository"] == "https://github.com/ggml-org/llama.cpp.git",
                f"{platform} upstream repository mismatch")
        require(bool(u["tag"]) and bool(COMMIT.fullmatch(u["commit"])),
                f"{platform} invalid source tag/commit")
        for field in ("patched_vocab_sha256",):
            require(bool(SHA.fullmatch(data[platform][field])), f"{platform}.{field} invalid")
    require(bool(SHA.fullmatch(data["macos"]["patched_main_sha256"])),
            "macos patched main hash invalid")
    for name, p in data["patches"].items():
        require(bool(SHA.fullmatch(p["sha256"])), f"invalid patch hash: {name}")
        require(p["file"].startswith("src/mac/installer/zenz_runtime/patches/"),
                "patch path outside allowlist")
    model = data["model"]
    require(bool(SHA.fullmatch(model["sha256"])), "model hash invalid")
    require(model["windows_file"].startswith("src/win32/installer/zenz_runtime/models/"),
            "model path outside allowlist")
    require(bool(COMMIT.fullmatch(model["revision"])), "model revision invalid")
    for arch, machine in (("x64", "0x8664"), ("arm64", "0xAA64")):
        asset = data["windows"][arch]
        require(asset["path"] == f"src/win32/installer/zenz_runtime/{arch}/llama-server.exe",
                f"unexpected Windows {arch} path")
        require(asset["pe_machine"].lower() == machine.lower(), "invalid PE machine contract")
        require(bool(SHA.fullmatch(asset["sha256"])), f"invalid {arch} hash")
        require(isinstance(asset["source_patches"], list) and
                all(p in data["patches"] for p in asset["source_patches"]),
                "unknown applied patch")
    require(data["windows"]["arm64"]["ggml_native"] is False and
            data["windows"]["arm64"]["ggml_cpu_arm_arch"] == "armv8-a",
            "ARM64 portability policy changed: requires separate review")
    return data


def sha256_file(path: pathlib.Path) -> str:
    require(path.is_file(), f"missing file: {path}")
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def pe_machine(path: pathlib.Path) -> str:
    with path.open("rb") as f:
        require(f.read(2) == b"MZ", f"invalid DOS magic: {path}")
        f.seek(0x3c)
        pe_offset = struct.unpack("<I", f.read(4))[0]
        require(0x40 <= pe_offset <= 64 * 1024 * 1024, f"invalid PE offset: {path}")
        f.seek(pe_offset)
        require(f.read(4) == b"PE\0\0", f"invalid PE signature: {path}")
        return f"0x{struct.unpack('<H', f.read(2))[0]:04X}"


def validate_assets(root: pathlib.Path, data: dict) -> list[str]:
    out = []
    for key, asset in (("tokenizer", data["patches"]["tokenizer"]),
                       ("dual_liveness", data["patches"]["dual_liveness"]),
                       ("model", {"file": data["model"]["windows_file"],
                                  "sha256": data["model"]["sha256"]})):
        h = sha256_file(root / asset["file"])
        require(h == asset["sha256"].upper(), f"{key} SHA256 mismatch: {h}")
        out.append(f"{key}: {h}")
    for arch in ("x64", "arm64"):
        a = data["windows"][arch]
        path = root / a["path"]
        h = sha256_file(path)
        require(h == a["sha256"].upper(), f"{arch} EXE SHA256 mismatch: {h}")
        require(pe_machine(path).lower() == a["pe_machine"].lower(),
                f"{arch} EXE PE machine mismatch")
        out.append(f"windows_{arch}: {h} ({a['pe_machine']})")
    return out


@dataclass(frozen=True)
class Rule:
    path: str
    label: str
    pattern: str
    value: str
    count: int = 1
    # Pattern must have prefix/value/suffix named groups.  Each rule is exact
    # count and only the `value` group may be rewritten.


def assignment(path: str, name: str, value: str, quoted: bool = True,
               count: int = 1) -> Rule:
    tail = r'(?P<suffix>")' if quoted else r'(?P<suffix>\s*$)'
    pat = rf'(?P<prefix>^[ \t]*{re.escape(name)}[ \t]*=[ \t]*' + (r'"' if quoted else '') + r')' + r'(?P<value>[^"\r\n]+)' + tail
    return Rule(path, name, pat, value, count)


def markdown(path: str, label: str, prefix: str, value: str, value_re: str = r'[^\r\n`]+') -> Rule:
    pat = rf'(?P<prefix>^{re.escape(prefix)})(?P<value>{value_re})(?P<suffix>[ \t]*$)'
    return Rule(path, label, pat, value)


def rules(data: dict) -> list[Rule]:
    w, m, p, model = data["windows"], data["macos"], data["patches"], data["model"]
    wc = "src/win32/installer/zenz_runtime/WINDOWS_ZENZ_RUNTIME_CONTRACT.md"
    wn = "src/win32/installer/zenz_runtime/licenses/THIRD_PARTY_NOTICES.md"
    mac = "src/mac/installer/zenz_runtime/"
    wb = ".github/workflows/windows.yaml"
    wl = ".github/scripts/windows_zenz_arm64_installer_lifecycle_audit.ps1"
    tag, commit = w["upstream"]["tag"], w["upstream"]["commit"]
    vocab, patch = w["patched_vocab_sha256"], p["tokenizer"]["sha256"]
    r = [
        Rule(wb, "Windows CI installed ARM64 expected SHA",
             r'(?P<prefix>^[ \t]*EXPECTED_LLAMA_SHA256:[ \t]*)(?P<value>[0-9a-fA-F]{64})(?P<suffix>[ \t]*$)', w["arm64"]["sha256"]),
        assignment(wl, "$ExpectedCurrentLlamaSha", w["arm64"]["sha256"]),
        # No rule for $ExpectedBaselineLlamaSha: historical fixture is immutable.
        Rule(wc, "Windows contract tag", r'(?P<prefix>^- tag: `)(?P<value>[^`\r\n]+)(?P<suffix>`$)', tag),
        Rule(wc, "Windows contract commit", r'(?P<prefix>^- commit: `)(?P<value>[0-9a-fA-F]{40})(?P<suffix>`$)', commit),
        Rule(wc, "Windows contract patch", r'(?P<prefix>^- tokenizer compatibility patch SHA256: `)(?P<value>[0-9a-fA-F]{64})(?P<suffix>`$)', patch),
        Rule(wc, "Windows contract vocab", r'(?P<prefix>^- patched `src/llama-vocab.cpp` SHA256: `)(?P<value>[0-9a-fA-F]{64})(?P<suffix>`$)', vocab),
        Rule(wc, "Windows contract model", r'(?P<prefix>^## Model identity\r?\n\r?\n- file: `[^`\r\n]+`\r?\n- SHA256: `)(?P<value>[0-9a-fA-F]{64})(?P<suffix>`)', model["sha256"]),
        Rule(wc, "Windows contract x64 EXE", r'(?P<prefix>^### x64\r?\n\r?\n- source-tree file: `[^`\r\n]+`\r?\n- PE: [^\r\n]+\r?\n- SHA256: `)(?P<value>[0-9a-fA-F]{64})(?P<suffix>`)', w["x64"]["sha256"]),
        Rule(wc, "Windows contract arm64 EXE", r'(?P<prefix>^### ARM64\r?\n\r?\n- source-tree file: `[^`\r\n]+`\r?\n- PE: [^\r\n]+\r?\n- SHA256: `)(?P<value>[0-9a-fA-F]{64})(?P<suffix>`)', w["arm64"]["sha256"]),
        Rule(wc, "Windows contract x64 provenance", r'(?P<prefix>^- Windows x64 \*\*rebuilt\*\* isolated MSVC candidate SHA256: `)(?P<value>[0-9a-fA-F]{64})(?P<suffix>`)', w["x64"]["sha256"]),
        Rule(wc, "Windows contract arm64 provenance", r'(?P<prefix>^- Portable ARM64 candidate SHA256: `)(?P<value>[0-9a-fA-F]{64})(?P<suffix>`)', w["arm64"]["sha256"]),
        markdown(wn, "Windows third-party source tag", "- Source tag: ", tag),
        markdown(wn, "Windows third-party source commit", "- Source commit: ", commit),
        markdown(wn, "Windows third-party patch", "- Mozkey tokenizer compatibility patch SHA256: ", patch),
        markdown(wn, "Windows third-party vocab", "- Patched src/llama-vocab.cpp SHA256: ", vocab),
        Rule(wn, "Windows notices x64 EXE", r'(?P<prefix>Windows x64 is [^\r\n(]+\(SHA256 )(?P<value>[0-9a-fA-F]{64})(?P<suffix>\))', w["x64"]["sha256"]),
        Rule(wn, "Windows notices ARM64 EXE", r'(?P<prefix>Windows ARM64 is [^\r\n(]+\(SHA256 )(?P<value>[0-9a-fA-F]{64})(?P<suffix>\))', w["arm64"]["sha256"]),
        Rule(wn, "Windows notices upstream tag in prose", r'(?P<prefix>Source is pinned to upstream )(?P<value>[^;\r\n]+)(?P<suffix>; MSI installed-lifecycle validation)', tag),
    ]
    mt, mc = m["upstream"]["tag"], m["upstream"]["commit"]
    mp, mv, md, mm = (p["tokenizer"]["sha256"], m["patched_vocab_sha256"],
                      p["dual_liveness"]["sha256"], m["patched_main_sha256"])
    build = mac + "build_universal_runtime.zsh"
    stage = mac + "stage_runtime_assets.zsh"
    verify = mac + "verify_release_package.zsh"
    for var, v in [
        ("LLAMA_TAG", mt), ("LLAMA_COMMIT", mc),
        ("PATCH_SHA256", mp.lower()), ("PATCHED_SOURCE_SHA256", mv),
        ("DUAL_LIVENESS_PATCH_SHA256", md.lower()), ("DUAL_LIVENESS_MAIN_SHA256", mm),
        ("MODEL_REPOSITORY", model["repository"]), ("MODEL_REVISION", model["revision"]),
        ("MODEL_REMOTE_FILE", model["remote_file"]), ("MODEL_OUTPUT_FILE", model["file"]),
        ("MODEL_SHA256", model["sha256"].lower()),
        ("MINIMUM_MACOS", m["minimum_os"]),
    ]:
        r.append(assignment(build, var, v))
    r.append(Rule(build, "macOS build-info short commit",
                  r'(?P<prefix>\x27LLAMA_COMMIT = \")(?P<value>[0-9a-fA-F]{7})(?P<suffix>\"\x27)',
                  mc[:7], 2))
    for var, v in [
        ("EXPECTED_LLAMA_TAG", mt), ("EXPECTED_LLAMA_COMMIT", mc),
        ("EXPECTED_PATCH_SHA256", mp.lower()), ("EXPECTED_PATCHED_SOURCE_SHA256", mv),
        ("EXPECTED_MODEL_REPOSITORY", model["repository"]),
        ("EXPECTED_MODEL_REVISION", model["revision"]),
        ("EXPECTED_MODEL_REMOTE_FILE", model["remote_file"]),
        ("EXPECTED_MODEL_OUTPUT_FILE", model["file"]),
        ("EXPECTED_MODEL_SHA256", model["sha256"].lower()),
        ("EXPECTED_MINIMUM_MACOS", m["minimum_os"]),
        ("EXPECTED_ARCHITECTURES", m["architectures"]),
    ]:
        r.append(assignment(stage, var, v))
    for var, v in [
        ("EXPECTED_LLAMA_TAG", mt), ("EXPECTED_LLAMA_COMMIT", mc),
        ("EXPECTED_PATCH_SHA", mp.lower()), ("EXPECTED_PATCHED_SOURCE_SHA", mv),
        ("EXPECTED_DUAL_LIVENESS_PATCH_SHA", md.lower()),
        ("EXPECTED_PATCHED_LLAMA_MAIN_SHA", mm),
        ("EXPECTED_MODEL_REPOSITORY", model["repository"]),
        ("EXPECTED_MODEL_REVISION", model["revision"]),
        ("EXPECTED_MODEL_SHA", model["sha256"].lower()),
        ("EXPECTED_MINIMUM_MACOS", m["minimum_os"]),
        ("EXPECTED_ARCHITECTURES", m["architectures"]),
    ]:
        r.append(assignment(verify, var, v))
    # The macOS README is documentation only; do not rewrite the full content
    # automatically.  Require pinned source identity to match, however.
    mr = mac + "README.md"
    r.append(Rule(mr, "macOS README source pin",
                  r'(?P<prefix>^Tag:\r?\n)(?P<value>[^\r\n]+)(?P<suffix>\r?\n\r?\nCommit:)', mt))
    r.append(Rule(mr, "macOS README commit pin",
                  r'(?P<prefix>^Commit:\r?\n)(?P<value>[0-9a-fA-F]{40})(?P<suffix>\r?\n```)', mc))
    r.append(Rule(mr, "macOS README tokenizer SHA",
                  r'(?P<prefix>^patches/llama-vocab-gpt2-small-japanese-char\.patch\r?\nSHA-256:\r?\n)(?P<value>[0-9a-fA-F]{64})(?P<suffix>\r?\n```)', mp.lower()))
    r.append(Rule(mr, "macOS README patched vocab SHA",
                  r'(?P<prefix>^The patched `src/llama-vocab.cpp` identity is:\r?\n\r?\n```text\r?\n)(?P<value>[0-9a-fA-F]{64})(?P<suffix>\r?\n```)', mv))
    return r


def apply_rules(content: str, group: list[Rule], write: bool) -> tuple[str, list[str]]:
    errors = []
    for rule in group:
        rx = re.compile(rule.pattern, re.MULTILINE)
        matches = list(rx.finditer(content))
        if len(matches) != rule.count:
            errors.append(f"{rule.path} {rule.label}: expected {rule.count} matches, got {len(matches)}")
            continue
        for match in matches:
            got = match.group("value")
            if got != rule.value:
                errors.append(f"{rule.path} {rule.label}: stale {got!r}, lock has {rule.value!r}")
        if write and any(m.group("value") != rule.value for m in matches):
            # Replace from right to left, so match offsets remain valid.
            for match in reversed(matches):
                content = content[:match.start("value")] + rule.value + content[match.end("value"):]
    return content, errors


def validate_consumers(root: pathlib.Path, data: dict, write: bool) -> tuple[list[str], dict]:
    groups: dict[str, list[Rule]] = {}
    for rule in rules(data):
        groups.setdefault(rule.path, []).append(rule)
    errors: list[str] = []
    changes: dict[pathlib.Path, tuple[bytes, bytes]] = {}
    for relative, rule_list in groups.items():
        path = root / relative
        require(path.is_file(), f"missing consumer: {relative}")
        before = path.read_bytes()
        require(not before.startswith(b"\xff\xfe"), f"unsupported UTF-16 consumer: {relative}")
        require(b"\x00" not in before, f"binary consumer forbidden: {relative}")
        text = before.decode("utf-8-sig")
        after, issues = apply_rules(text, rule_list, write)
        if write:
            # Rule-count mismatch is a hard failure, never update a partial set.
            structure = [e for e in issues if "expected " in e and "matches" in e]
            errors.extend(structure)
        else:
            errors.extend(issues)
        new_bytes = (b"\xef\xbb\xbf" if before.startswith(b"\xef\xbb\xbf") else b"") + after.encode("utf-8")
        if write and new_bytes != before:
            changes[path] = (before, new_bytes)
    return errors, changes


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=("check", "sync"))
    ap.add_argument("--repo", type=pathlib.Path, default=ROOT)
    ap.add_argument("--write", action="store_true", help="sync only: apply pinned-text changes")
    ap.add_argument("--diff", action="store_true", help="show sync diff; without --write files remain untouched")
    ns = ap.parse_args(argv)
    require(not ns.write or ns.action == "sync", "--write valid only with sync")
    root = ns.repo.resolve()
    data = read_lock(root)
    print("===== MOZKEY ZENZ RUNTIME LOCK =====")
    print(f"Action={ns.action} Write={ns.write} Schema={data['schema_version']}")
    print("WindowsPin=" + data["windows"]["upstream"]["tag"] + "@" + data["windows"]["upstream"]["commit"])
    print("macOSPin=" + data["macos"]["upstream"]["tag"] + "@" + data["macos"]["upstream"]["commit"])
    # Refuse writes unless all referenced assets match the *new* lock.
    # During a future upgrade: replace tested candidate binaries, then update lock,
    # then sync.  No automatic download or unvalidated hash adoption.
    assets = validate_assets(root, data)
    for row in assets:
        print("Verified=" + row)
    problems, changes = validate_consumers(root, data, ns.action == "sync")
    if problems:
        for issue in problems:
            print("DRIFT=" + issue, file=sys.stderr)
        if ns.action == "check":
            raise LockError(f"{len(problems)} pinned consumer(s) not synchronized")
        raise LockError(f"{len(problems)} structural problem(s), sync stopped")
    if ns.action == "sync":
        print("ChangedConsumers=" + str(len(changes)))
        if ns.diff:
            for path, (old, new) in changes.items():
                before = old.decode("utf-8-sig").splitlines(keepends=True)
                after = new.decode("utf-8-sig").splitlines(keepends=True)
                print("".join(difflib.unified_diff(before, after,
                                                     fromfile=str(path.relative_to(root)),
                                                     tofile=str(path.relative_to(root)) + " (locked)")))
        if ns.write:
            # Both file bytes and mode are retained; text operations only.
            # Create no extra repository files.  Each write is checked against
            # its already-read bytes to prevent unintended concurrent edits.
            applied = []
            try:
                for path, (original, output) in changes.items():
                    require(path.read_bytes() == original, f"concurrent modification: {path}")
                    path.write_bytes(output)
                    applied.append((path, original))
            except Exception:
                for path, original in reversed(applied):
                    path.write_bytes(original)
                raise
    print("MOZKEY_ZENZ_RUNTIME_LOCK=PASS")
    print("HistoricBaselinePins=NOT_TOUCHED")
    print("InstallerAndRuntimeState=NOT_MODIFIED")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (LockError, OSError, ValueError, KeyError, UnicodeError) as error:
        print(f"MOZKEY_ZENZ_RUNTIME_LOCK=FAIL: {error}", file=sys.stderr)
        sys.exit(1)
