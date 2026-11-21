#!/usr/bin/env python3
"""Check normal and delay-load DLL imports in a PE32+ ARM64 executable.

Only OS DLLs, Windows API sets, and the Microsoft CRT are permitted.
Fail closed if the import table cannot be safely parsed.  This is an
import audit, not proof of compatible CPU instructions or all runtime loads.
"""
import json
import struct
import sys
from pathlib import Path

ALLOWED = {
    'advapi32.dll', 'bcrypt.dll', 'bcryptprimitives.dll', 'comctl32.dll',
    'comdlg32.dll', 'concrt140.dll', 'crypt32.dll', 'cryptbase.dll',
    'dbghelp.dll', 'dnsapi.dll', 'dwrite.dll', 'gdi32.dll', 'gdi32full.dll',
    'imm32.dll', 'iphlpapi.dll', 'kernel32.dll', 'kernelbase.dll',
    'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcrt.dll',
    'normaliz.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'psapi.dll',
    'rpcrt4.dll', 'secur32.dll', 'setupapi.dll', 'shell32.dll', 'shcore.dll',
    'shlwapi.dll', 'ucrtbase.dll', 'user32.dll', 'userenv.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'version.dll',
    'winhttp.dll', 'wininet.dll', 'winmm.dll', 'ws2_32.dll',
    'wtsapi32.dll',
}


def parse_pe(data):
    def fail(msg):
        raise ValueError(msg)

    def span(off, size):
        if off < 0 or size < 0 or off + size > len(data):
            fail(f'out-of-bounds PE read at {off} length {size}')
        return data[off:off + size]

    def u16(off):
        return struct.unpack('<H', span(off, 2))[0]

    def u32(off):
        return struct.unpack('<I', span(off, 4))[0]

    def u64(off):
        return struct.unpack('<Q', span(off, 8))[0]

    if len(data) < 128 or span(0, 2) != b'MZ':
        fail('not a DOS/PE executable')
    coff = u32(0x3c)
    if span(coff, 4) != b'PE\0\0':
        fail('invalid PE signature')
    machine = u16(coff + 4)
    if machine != 0xAA64:
        fail(f'not ARM64 PE (machine=0x{machine:04X})')
    nsec = u16(coff + 6)
    if not (1 <= nsec <= 96):
        fail('unexpected number of sections')
    opts = coff + 24
    optlen = u16(coff + 20)
    if optlen < 112 + 14 * 8 or u16(opts) != 0x20B:
        fail('expected PE32+ with import/delay data directories')
    if u32(opts + 108) <= 13:
        fail('required PE data directories are absent')
    image_base = u64(opts + 24)
    header_size = u32(opts + 60)
    secloc = opts + optlen
    sections = []
    for i in range(nsec):
        off = secloc + i * 40
        span(off, 40)
        virt_size, virt_addr, raw_size, raw_off = (u32(off+8), u32(off+12),
                                                   u32(off+16), u32(off+20))
        sections.append((virt_addr, max(virt_size, raw_size), raw_size, raw_off))

    def rva_to_off(rva, size=1):
        if rva < header_size and rva + size <= header_size:
            span(rva, size)
            return rva
        for vaddr, vsize, raw_size, raw_off in sections:
            if vaddr <= rva < vaddr + vsize:
                delta = rva - vaddr
                if delta + size > raw_size:
                    fail(f'RVA 0x{rva:X} lies in a zero-filled section')
                span(raw_off + delta, size)
                return raw_off + delta
        fail(f'RVA 0x{rva:X} has no file backing')

    def dll_name(rva):
        off = rva_to_off(rva)
        end = data.find(b'\0', off, min(len(data), off + 260))
        if end < 0:
            fail('unterminated DLL name')
        try:
            name = data[off:end].decode('ascii').lower()
        except UnicodeError:
            fail('non-ASCII DLL name')
        if not name or '/' in name or '\\' in name or not name.endswith('.dll'):
            fail(f'invalid imported DLL name: {name!r}')
        return name

    def directory(index):
        at = opts + 112 + index * 8
        rva, size = u32(at), u32(at + 4)
        if bool(rva) != bool(size):
            fail(f'inconsistent directory {index}')
        if size > len(data):
            fail(f'implausible directory {index} size')
        return rva, size

    imports = set()
    rva, length = directory(1)
    if rva:
        found_end = False
        for i in range(min(4096, length // 20 + 1)):
            entry = span(rva_to_off(rva + i * 20, 20), 20)
            if entry == b'\0' * 20:
                found_end = True
                break
            name_rva = u32(rva_to_off(rva + i * 20 + 12, 4))
            imports.add(dll_name(name_rva))
        if not found_end:
            fail('import directory missing terminator')
    if not imports:
        fail('no normal PE imports found: cannot establish dependency contract')

    delayed = set()
    rva, length = directory(13)
    if rva:
        found_end = False
        for i in range(min(4096, length // 32 + 1)):
            entry = span(rva_to_off(rva + i * 32, 32), 32)
            if entry == b'\0' * 32:
                found_end = True
                break
            entry_off = rva_to_off(rva + i * 32, 32)
            attr = u32(entry_off)
            name = u32(entry_off + 4)
            if not attr & 1:
                # Legacy VA-based descriptor; 32-bit name field cannot
                # represent large image bases. Refuse rather than guess.
                if image_base > 0xFFFFFFFF or name < image_base:
                    fail('unsupported VA-based delay import descriptor')
                name -= image_base
            delayed.add(dll_name(name))
        if not found_end:
            fail('delay import directory missing terminator')

    dlls = imports | delayed
    unknown = sorted(d for d in dlls if d not in ALLOWED and
                     not d.startswith('api-ms-win-') and
                     not d.startswith('ext-ms-win-'))
    if unknown:
        fail('non-OS/CRT or unreviewed imports: ' + ', '.join(unknown))
    return {
        'machine': machine,
        'policy': 'OS_OR_MSVC_CRT_ONLY',
        'imports': sorted(imports),
        'delay_imports': sorted(delayed),
        'caution': 'Import table check does not prove static CPU baseline or dynamic LoadLibrary behavior',
    }


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: pe_import_audit.py FILE.exe report.json')
    binary, report = map(Path, sys.argv[1:])
    data = binary.read_bytes()
    result = parse_pe(data)
    report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('PE_MACHINE=0xAA64')
    print('DEPENDENCY_AUDIT=PASS_NORMAL_AND_DELAY_IMPORTS')
    print('IMPORTED_DLLS=' + ','.join(sorted(set(result['imports'] + result['delay_imports']))))


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('PE_IMPORT_AUDIT_FAIL=' + str(exc), file=sys.stderr)
        raise SystemExit(1)
