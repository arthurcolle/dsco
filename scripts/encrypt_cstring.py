#!/usr/bin/env python3
"""encrypt_cstring.py — encrypt the (relocated) __DATA,__cstring section in place.

Run after link+strip, before codesign. Parses the thin arm64 Mach-O, locates the
__DATA,__cstring section (moved there by the hardened link's -rename_section),
and XORs it with the HMAC-SHA256-CTR keystream keyed by build/.cstring_key — the
same key src/cstring_unlock.c reconstructs to decrypt at load.

Usage: encrypt_cstring.py <binary> [key_file]
"""
import hashlib
import hmac
import struct
import sys

binary = sys.argv[1]
key_file = sys.argv[2] if len(sys.argv) > 2 else "build/.cstring_key"

with open(key_file, "rb") as fh:
    km = fh.read()
key, nonce = km[:32], km[32:48]

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19


def find_section(data, seg_want, sect_want):
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != MH_MAGIC_64:
        raise SystemExit("encrypt_cstring: not a thin arm64 Mach-O (magic=0x%x)" % magic)
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off = 32  # sizeof(mach_header_64)
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            segname = data[off + 8:off + 24].rstrip(b"\0").decode("ascii", "replace")
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            soff = off + 72  # sizeof(segment_command_64)
            for _s in range(nsects):
                sectname = data[soff:soff + 16].rstrip(b"\0").decode("ascii", "replace")
                seg2 = data[soff + 16:soff + 32].rstrip(b"\0").decode("ascii", "replace")
                sect_addr, sect_size = struct.unpack_from("<QQ", data, soff + 32)
                sect_off = struct.unpack_from("<I", data, soff + 48)[0]
                if (segname == seg_want or seg2 == seg_want) and sectname == sect_want:
                    return sect_off, sect_size
                soff += 80  # sizeof(section_64)
        off += cmdsize
    return None, None


def keystream_xor(key, nonce, buf):
    out = bytearray(buf)
    off, ctr = 0, 0
    while off < len(out):
        block = hmac.new(key, nonce + struct.pack("<Q", ctr), hashlib.sha256).digest()
        n = min(32, len(out) - off)
        for i in range(n):
            out[off + i] ^= block[i]
        off += n
        ctr += 1
    return bytes(out)


with open(binary, "rb") as fh:
    data = bytearray(fh.read())

foff, fsize = find_section(data, "__DATA", "__cstring")
if foff is None or not fsize:
    raise SystemExit("encrypt_cstring: __DATA,__cstring not found — did the rename_section link run?")

enc = keystream_xor(key, nonce, bytes(data[foff:foff + fsize]))
data[foff:foff + fsize] = enc
with open(binary, "wb") as fh:
    fh.write(data)

print("  [cstring] encrypted __DATA,__cstring: %d bytes @ 0x%x" % (fsize, foff))
