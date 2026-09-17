"""Replace every discard instruction in a DXBC pixel shader with no-ops.

    python tools/dxbc_nop_discard.py input.dxbc output.dxbc

The instruction stream is position independent, so a discard (opcode 0x0d) is overwritten in
place with NOP tokens (opcode 0x3a, length 1) of the same total length. The container checksum is
left stale on purpose: Bridger re-signs a .dxbc replacement when it loads it.
"""

import struct
import sys

OP_DISCARD = 0x0D
OP_NOP = 0x3A
OP_CUSTOMDATA = 0x35


def patch(data: bytearray) -> int:
    if data[:4] != b'DXBC':
        raise ValueError('not a DXBC container')
    part_count = struct.unpack_from('<I', data, 28)[0]
    offsets = struct.unpack_from(f'<{part_count}I', data, 32)
    patched = 0
    for off in offsets:
        tag = bytes(data[off:off + 4])
        if tag not in (b'SHEX', b'SHDR'):
            continue
        size = struct.unpack_from('<I', data, off + 4)[0]
        body = off + 8
        total_tokens = struct.unpack_from('<I', data, body + 4)[0]
        end = min(body + total_tokens * 4, body + size)
        pos = body + 8
        while pos < end:
            token = struct.unpack_from('<I', data, pos)[0]
            opcode = token & 0x7FF
            if opcode == OP_CUSTOMDATA:
                length = struct.unpack_from('<I', data, pos + 4)[0]
            else:
                length = (token >> 24) & 0x7F
            if length == 0:
                raise ValueError(f'zero-length instruction at {pos - body:#x}')
            if opcode == OP_DISCARD:
                for i in range(length):
                    struct.pack_into('<I', data, pos + i * 4, OP_NOP | (1 << 24))
                patched += 1
            pos += length * 4
    return patched


def main() -> None:
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, 'rb').read())
    count = patch(data)
    open(dst, 'wb').write(data)
    print(f'{src}: {count} discard(s) replaced -> {dst}')


if __name__ == '__main__':
    main()
