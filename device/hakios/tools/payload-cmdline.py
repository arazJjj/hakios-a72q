#!/usr/bin/env python3
"""Rewrite the cmdline entry of an XNUP payload: payload-cmdline.py IN OUT "new cmdline" (prints the old one)."""
import struct, sys
src, dst, new = sys.argv[1:4]
d = open(src, 'rb').read()
magic, n = struct.unpack_from('<II', d)
assert magic == 0x50554E58, 'not an XNUP payload'
body, i, out = d[8:8 + n], 0, b''
while i + 24 <= len(body):
    name, size, _ = struct.unpack_from('<16sII', body, i)
    if not name.strip(b'\0'): break
    data = body[i + 24:i + 24 + size]; i += 24 + size + (-size % 16)
    if name.rstrip(b'\0') == b'cmdline':
        print('old:', data.rstrip(b'\0').decode()); data = new.encode() + b'\0'
    out += struct.pack('<16sII', name, len(data), 0) + data + b'\0' * (-len(data) % 16)
out += b'\0' * 24
open(dst, 'wb').write(struct.pack('<II', 0x50554E58, len(out)) + out)
print('new:', new, '->', dst, len(out) + 8, 'bytes')
