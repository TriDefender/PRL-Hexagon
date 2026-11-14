#!/usr/bin/env python3
"""Reject missing static HexKL code and invalid initializer mappings in DSP ELFs.

QuRT/HAP/libc imports are supplied by the device and intentionally remain
unresolved. HexKL is linked from our static archive and must be defined here.
Uses only the Python standard library so release builds need no host readelf.
"""
from pathlib import Path
import struct
import sys


def check(path, expected_arch=None):
    data = Path(path).read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected ELF32 little-endian")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if header[1:3] != (3, 164):  # ET_DYN, EM_HEXAGON
        raise ValueError("expected Hexagon shared library")
    if expected_arch is not None and (header[7] & 0xFF) != expected_arch:
        # e_flags carries the compute target (0x75 = V75, 0x79 = V79, ...).
        # A payload packaged under the wrong arch suffix would fail to load
        # only at self-check time on the device; reject it here instead.
        raise ValueError(f"ELF arch 0x{header[7] & 0xFF:x}, expected 0x{expected_arch:x}")

    def records(offset, count, stride, fmt):
        size = struct.calcsize(fmt)
        if stride < size or offset + count * stride > len(data):
            raise ValueError("invalid ELF table extent")
        return [struct.unpack_from(fmt, data, offset + i * stride) for i in range(count)]

    programs = records(header[5], header[10], header[9], "<8I")
    executable = [p for p in programs if p[0] == 1 and p[6] & 1]
    if not executable:
        raise ValueError("missing executable PT_LOAD")
    sections = records(header[6], header[12], header[11], "<10I")
    imports = []
    symbol_tables = 0
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        symbol_tables += 1
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for symbol in records(section[4], section[5] // section[9], section[9], "<IIIBBH"):
            name = names[symbol[0]:].split(b"\0", 1)[0].decode("ascii")
            if symbol[5] == 0 and name.startswith("hexkl_"):
                imports.append(name)
    if not symbol_tables:
        raise ValueError("missing dynamic symbol table")
    if imports:
        raise ValueError("unresolved static HexKL imports: " + ", ".join(sorted(imports)))
    for program in programs:
        if program[0] != 2:  # PT_DYNAMIC
            continue
        for tag, value in records(program[1], program[4] // 8, 8, "<II"):
            if tag == 0:
                break
            if tag in (12, 13) and value:  # DT_INIT, DT_FINI
                if not any(p[2] <= value < p[2] + p[4] for p in executable):
                    raise ValueError(f"initializer/finalizer VA 0x{value:x} outside file-backed RX segment")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", type=lambda s: int(s, 0), default=None,
                        help="expected compute target in e_flags, e.g. 0x75 or 0x79")
    parser.add_argument("libraries", nargs="+")
    args = parser.parse_args()
    for filename in args.libraries:
        try:
            check(filename, args.arch)
        except (ValueError, IndexError, ZeroDivisionError, struct.error) as error:
            sys.exit(f"DSP_ELF_FAIL {filename}: {error}")
        print(f"DSP_ELF_PASS {filename}")
