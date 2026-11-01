#!/usr/bin/env python3
"""Put a new loader.efi into an a72q U-Boot boot.img (HKUB container in the ramdisk), keeping U-Boot and the payload.
usage: swap-loader.py in-boot.img loader.efi out-boot.img mkbootimg.py"""
import struct, subprocess, sys, zlib, os
src, loader, out, mkbootimg = sys.argv[1:5]
d = open(src, 'rb').read()
assert d[:8] == b'ANDROID!'
ks, ka, rs, ra, ss, sa, ta, ps, hv = struct.unpack_from('<9I', d, 8)
kernel = d[ps:ps + ks]
roff = ps + (ks + ps - 1) // ps * ps
rd = d[roff:roff + rs]
magic, ver, cnt, hsz = struct.unpack_from('<4sIII', rd)
assert magic == b'HKUB'
entries = []
for i in range(cnt):
    name, off, size, crc, _ = struct.unpack_from('<16sIIII', rd, 16 + 32 * i)
    name = name.rstrip(b'\0').decode()
    data = rd[off:off + size]
    assert zlib.crc32(data) == crc, name
    entries.append((name, data))
efi = open(loader, 'rb').read()
assert efi[:2] == b'MZ'
entries = [(n, efi if n == 'loader' else x) for n, x in entries]
hs = 16 + 32 * len(entries); o = (hs + 0xfff) & ~0xfff
table = body = b''
for n, x in entries:
    table += struct.pack('<16sIIII', n.encode(), o + len(body), len(x), zlib.crc32(x), 0)
    body += x + b'\0' * (-len(x) % 0x1000)
head = struct.pack('<4sIII', b'HKUB', 1, len(entries), hs) + table
hkub = head + b'\0' * (o - len(head)) + body
open('k~', 'wb').write(kernel); open('r~', 'wb').write(hkub)
subprocess.check_call([sys.executable, mkbootimg, '--kernel', 'k~', '--ramdisk', 'r~', '--header_version', '1',
    '--base', '0x10000000', '--kernel_offset', '0x8000', '--ramdisk_offset', '0x1000000',
    '--tags_offset', '0x100', '--pagesize', str(ps), '-o', out])
os.unlink('k~'); os.unlink('r~')
print(out, os.path.getsize(out), [(n, len(x)) for n, x in entries])
