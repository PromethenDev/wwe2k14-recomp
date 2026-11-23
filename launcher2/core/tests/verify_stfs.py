#!/usr/bin/env python3
# Independent check of an exported single-file STFS "CON " package (does not share code with the C++ writer/reader).
# Read/write format, two copies per hash table: [L0 #0][data 0..169][L1 #0][L0 #1][data 170..339][L0 #2]...; level 1
# exists past 170 blocks, level 2 past 170 * 170 (layout from a game-written 177-block arena and the Xenia reader).
import hashlib, struct, sys
d = open(sys.argv[1], 'rb').read()
exp_title = int(sys.argv[2], 16); exp_name = sys.argv[3]; exp_size = int(sys.argv[4])
payload = open(sys.argv[5], 'rb').read() if len(sys.argv) > 5 else None
assert d[:4] == b'CON ', 'magic'
hs = struct.unpack('>I', d[0x340:0x344])[0]
assert hs == 0xAD0E, 'header size'
assert struct.unpack('>I', d[0x344:0x348])[0] == 1, 'content type'
assert struct.unpack('>I', d[0x360:0x364])[0] == exp_title, 'title id'
name = d[0x411:0x411 + 0x80].decode('utf-16-be').split('\0')[0]
assert name == exp_name, 'display name %r' % name
assert hashlib.sha1(d[0x344:hs]).digest() == d[0x32C:0x340], 'content id'
v = d[0x379:0x379 + 0x24]
assert v[0] == 0x24 and v[2] == 0 and struct.unpack('<H', v[3:5])[0] == 1, 'volume descriptor'
total = struct.unpack('>I', v[0x1C:0x20])[0]
data_blocks = (exp_size + 0xFFF) // 0x1000
assert total == 1 + data_blocks, 'total blocks'
base = 0xB000
levels = 1 if total <= 170 else 2 if total <= 170 * 170 else 3


def data_phys(k):
    # Xenia BlockToOffsetSTFS: one table slot (2 blocks) per level boundary passed.
    p, base = k, 170
    for _ in range(3):
        p += ((k + base) // base) * 2
        if k < base:
            break
        base *= 170
    return p


def table_phys(k, level):
    # Xenia BlockToHashBlockNumberSTFS with two blocks per table.
    step0, step1 = 172, 170 * 170 + 171 * 2
    if level == 0:
        if k < 170:
            return 0
        n = (k // 170) * step0 + (k // (170 * 170) + 1) * 2
        return n if k < 170 * 170 else n + 2
    if level == 1:
        return step0 if k < 170 * 170 else (k // (170 * 170)) * step1 + 2
    return step1


def block(p):
    return d[base + p * 0x1000: base + (p + 1) * 0x1000]


top = block(table_phys(0, levels - 1))
assert hashlib.sha1(top).digest() == v[8:28], 'top hash'
assert len(d) == base + (data_phys(total - 1) + 1) * 0x1000, 'file length'
for level in range(1, levels):
    span = 170 if level == 1 else 170 * 170
    for first in range(0, total, span):
        rec = block(table_phys(first, level))[((first // span) % 170) * 0x18:((first // span) % 170 + 1) * 0x18]
        assert hashlib.sha1(block(table_phys(first, level - 1))).digest() == rec[:20], 'level-%d hash %d' % (level, first)
        assert rec[20:24] == b'\x80\x00\x00\x00', 'level-%d info %d' % (level, first)
content = bytearray()
for k in range(total):
    rec = block(table_phys(k, 0))[(k % 170) * 0x18:(k % 170 + 1) * 0x18]
    assert hashlib.sha1(block(data_phys(k))).digest() == rec[:20], 'block hash %d' % k
    nxt = int.from_bytes(rec[21:24], 'big')
    want = 0xFFFFFF if (k == 0 or k == total - 1) else k + 1
    assert nxt == want, 'chain %d' % k
    if k:
        content += block(data_phys(k))
last = block(table_phys(total - 1, 0))
used = (total - 1) % 170 + 1
assert last[used * 0x18:] == bytes(0x1000 - used * 0x18), 'unused table bytes'
ft = block(data_phys(0))[:0x40]
nlen = ft[0x28] & 0x3F
assert ft[:nlen] == b'SaveData.Dat' and nlen == 12, 'file name'
assert int.from_bytes(ft[0x2F:0x32], 'little') == 1 and int.from_bytes(ft[0x2C:0x2F], 'little') == data_blocks
assert struct.unpack('>I', ft[0x34:0x38])[0] == exp_size, 'file size'
if payload is not None:
    assert bytes(content[:exp_size]) == payload, 'payload bytes'
print('OK')
