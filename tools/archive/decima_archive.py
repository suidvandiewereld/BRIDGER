"""Read Death Stranding's .bin pack files: decrypt tables and chunks, decompress with the game's
own Oodle, and extract files by id or by path.

Encryption follows Wunkolo/DecimaTools (MIT) and Jayveer/Decima-Explorer: MurmurHash3 x64-128
(seed 42) keyed tables, and per-chunk XOR with an MD5 of the chunk entry mixed with a salt.

    python tools/archive/decima_archive.py info  <archive.bin>
    python tools/archive/decima_archive.py find  <path>                  which archives hold it
    python tools/archive/decima_archive.py extract <path> <out_file>
"""
import ctypes
import glob
import hashlib
import os
import struct
import sys

import sys as _sys, os as _os
_sys.path.insert(0, _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
import gamedir
GAME = gamedir.require()
SALT1 = (0x0FA3A9443, 0x0F41CAB62, 0x0F376811C, 0x0D2A89E3E)
SALT2 = (0x06C084A37, 0x07E159D95, 0x03D5AF7E8, 0x018AA7D3F)
SEED = 42
ENCRYPTED = 0x21304050
UNENCRYPTED = 0x20304050
M64 = (1 << 64) - 1


def _rotl64(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def _fmix64(k):
    k ^= k >> 33
    k = (k * 0xFF51AFD7ED558CCD) & M64
    k ^= k >> 33
    k = (k * 0xC4CEB9FE1A85EC53) & M64
    k ^= k >> 33
    return k


def murmur3_x64_128(data, seed=SEED):
    c1, c2 = 0x87C37B91114253D5, 0x4CF5AD432745937F
    h1 = h2 = seed
    n = len(data) // 16
    for i in range(n):
        k1, k2 = struct.unpack_from("<QQ", data, i * 16)
        k1 = (k1 * c1) & M64; k1 = _rotl64(k1, 31); k1 = (k1 * c2) & M64; h1 ^= k1
        h1 = _rotl64(h1, 27); h1 = (h1 + h2) & M64; h1 = (h1 * 5 + 0x52DCE729) & M64
        k2 = (k2 * c2) & M64; k2 = _rotl64(k2, 33); k2 = (k2 * c1) & M64; h2 ^= k2
        h2 = _rotl64(h2, 31); h2 = (h2 + h1) & M64; h2 = (h2 * 5 + 0x38495AB5) & M64
    tail = data[n * 16:]
    k1 = k2 = 0
    for i in range(len(tail) - 1, -1, -1):
        if i >= 8:
            k2 ^= tail[i] << ((i - 8) * 8)
        else:
            k1 ^= tail[i] << (i * 8)
    if len(tail) > 8:
        k2 = (k2 * c2) & M64; k2 = _rotl64(k2, 33); k2 = (k2 * c1) & M64; h2 ^= k2
    if len(tail) > 0:
        k1 = (k1 * c1) & M64; k1 = _rotl64(k1, 31); k1 = (k1 * c2) & M64; h1 ^= k1
    h1 ^= len(data); h2 ^= len(data)
    h1 = (h1 + h2) & M64; h2 = (h2 + h1) & M64
    h1 = _fmix64(h1); h2 = _fmix64(h2)
    h1 = (h1 + h2) & M64; h2 = (h2 + h1) & M64
    return struct.pack("<QQ", h1, h2)


def _key_block(first):
    vec = struct.pack("<4I", first & 0xFFFFFFFF, *SALT1[1:])
    return struct.unpack("<4I", murmur3_x64_128(vec)), vec


def _xor_words(words, key):
    return [w ^ k for w, k in zip(words, key)]


def decrypt_header(raw):
    w = list(struct.unpack_from("<10I", raw, 0))
    key = w[1]
    h, _ = _key_block(key)
    w[2:6] = _xor_words(w[2:6], h)
    h2, _ = _key_block(key + 1)
    w[6:10] = _xor_words(w[6:10], h2)
    return w


def decrypt_entry(raw, key_word_index, second_key_word_index):
    """Entry of 32 bytes: words 0-3 xor hash(salt with word[key]), words 4-7 xor hash(salt with word[second])."""
    w = list(struct.unpack("<8I", raw))
    h, _ = _key_block(w[key_word_index])
    first = _xor_words(w[0:4], h)
    h2, _ = _key_block(w[second_key_word_index])
    second = _xor_words(w[4:8], h2)
    out = first + second
    out[key_word_index] = w[key_word_index]
    out[second_key_word_index] = w[second_key_word_index]
    return out


class Oodle:
    def __init__(self):
        self.dll = ctypes.WinDLL(os.path.join(GAME, "oo2core_7_win64.dll"))
        self.fn = self.dll.OodleLZ_Decompress
        self.fn.restype = ctypes.c_int64
        self.fn.argtypes = [ctypes.c_char_p, ctypes.c_int64, ctypes.c_char_p, ctypes.c_int64,
                            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int64,
                            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int64, ctypes.c_int]

    def decompress(self, data, size):
        out = ctypes.create_string_buffer(size)
        got = self.fn(data, len(data), out, size, 0, 0, 0, None, 0, None, None, None, 0, 3)
        if got != size:
            raise RuntimeError("oodle returned %d, expected %d" % (got, size))
        return out.raw


class Archive:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        raw = self.f.read(0x28)
        version = struct.unpack_from("<I", raw)[0]
        if version not in (ENCRYPTED, UNENCRYPTED):
            raise ValueError("not a Decima archive: %s" % path)
        self.encrypted = version == ENCRYPTED
        w = decrypt_header(raw) if self.encrypted else list(struct.unpack("<10I", raw))
        self.file_size = w[2] | (w[3] << 32)
        self.data_size = w[4] | (w[5] << 32)
        self.file_count = w[6] | (w[7] << 32)
        self.chunk_count = w[8]
        self.max_chunk = w[9]
        self.files = {}
        table = self.f.read(0x20 * self.file_count)
        for i in range(self.file_count):
            e = table[i * 0x20:(i + 1) * 0x20]
            w8 = decrypt_entry(e, 1, 7) if self.encrypted else list(struct.unpack("<8I", e))
            path_hash = w8[2] | (w8[3] << 32)
            offset = w8[4] | (w8[5] << 32)
            self.files[path_hash] = (w8[0], offset, w8[6])
        table = self.f.read(0x20 * self.chunk_count)
        self.chunks = []
        for i in range(self.chunk_count):
            e = table[i * 0x20:(i + 1) * 0x20]
            w8 = decrypt_entry(e, 3, 7) if self.encrypted else list(struct.unpack("<8I", e))
            self.chunks.append((w8[0] | (w8[1] << 32), w8[2], w8[3],
                                w8[4] | (w8[5] << 32), w8[6], w8[7],
                                e))
        self.chunks.sort(key=lambda c: c[0])
        self._oodle = None

    def _chunk_data(self, chunk):
        u_off, u_size, u_hash, c_off, c_size, c_hash, raw_entry = chunk
        self.f.seek(c_off)
        data = bytearray(self.f.read(c_size))
        if self.encrypted:
            plain_entry = struct.pack("<QII", u_off, u_size, u_hash)
            key = struct.unpack("<4I", murmur3_x64_128(plain_entry))
            key = struct.pack("<4I", *_xor_words(key, SALT2))
            digest = hashlib.md5(key).digest()
            for i in range(len(data)):
                data[i] ^= digest[i % 16]
        if self._oodle is None:
            self._oodle = Oodle()
        return self._oodle.decompress(bytes(data), u_size)

    def read(self, path_hash):
        _, offset, size = self.files[path_hash]
        out = bytearray()
        pos = offset
        import bisect
        starts = [c[0] for c in self.chunks]
        while len(out) < size:
            i = bisect.bisect_right(starts, pos) - 1
            chunk = self.chunks[i]
            data = self._chunk_data(chunk)
            start = pos - chunk[0]
            take = min(len(data) - start, size - len(out))
            out += data[start:start + take]
            pos += take
        return bytes(out)


class OodleCompressor:
    KRAKEN = 8

    def __init__(self):
        self.dll = ctypes.WinDLL(os.path.join(GAME, "oo2core_7_win64.dll"))
        self.fn = self.dll.OodleLZ_Compress
        self.fn.restype = ctypes.c_int64
        self.fn.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int64, ctypes.c_char_p, ctypes.c_int,
                            ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64, ctypes.c_void_p, ctypes.c_int64]

    def compress(self, data, level=4):
        out = ctypes.create_string_buffer(len(data) + 0x10000)
        n = self.fn(self.KRAKEN, data, len(data), out, level, None, 0, 0, None, 0)
        if n <= 0:
            raise RuntimeError("oodle compress failed")
        return out.raw[:n]


def write_archive(path, files, chunk_size=0x40000):
    """Write an unencrypted archive (version 0x20304050) holding {asset path: bytes}.

    Layout matches the game's own archives: header, file table sorted by path hash, chunk table,
    then Oodle Kraken chunks of the concatenated file data.
    """
    entries = sorted(((path_hash(p), data) for p, data in files.items()), key=lambda e: e[0])
    blob = bytearray()
    table = []
    for file_id, (h, data) in enumerate(entries):
        table.append((file_id, h, len(blob), len(data)))
        blob += data
    comp = OodleCompressor()
    chunks = []
    for u_off in range(0, len(blob), chunk_size):
        raw = bytes(blob[u_off:u_off + chunk_size])
        chunks.append((u_off, len(raw), comp.compress(raw)))
    data_start = 0x28 + 0x20 * len(table) + 0x20 * len(chunks)
    out = bytearray()
    c_off = data_start
    chunk_rows = bytearray()
    for u_off, u_size, c in chunks:
        chunk_rows += struct.pack("<QIIQII", u_off, u_size, 0, c_off, len(c), 0)
        c_off += len(c)
    file_rows = bytearray()
    for file_id, h, off, size in table:
        file_rows += struct.pack("<IIQQII", file_id, 0, h, off, size, 0)
    header = struct.pack("<IIQQQII", UNENCRYPTED, 0, c_off, len(blob), len(table), len(chunks), chunk_size)
    out += header + file_rows + chunk_rows
    for _, _, c in chunks:
        out += c
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def path_hash(path):
    """Decima file ids: MurmurHash3 x64-128 of the lowercase path with '.core' and a NUL, low 64 bits."""
    p = path.replace("\\", "/").lower()
    if not p.endswith(".core") and not p.endswith(".stream"):
        p += ".core"
    return struct.unpack_from("<Q", murmur3_x64_128(p.encode() + b"\0"))[0]


def archives():
    return sorted(glob.glob(os.path.join(GAME, "data", "*.bin")), key=os.path.getsize)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    cmd = argv[1]
    if cmd == "info":
        a = Archive(argv[2])
        print("encrypted %s files %d chunks %d max chunk %d data %d" % (a.encrypted, a.file_count, a.chunk_count, a.max_chunk, a.data_size))
        for h, (fid, off, size) in list(a.files.items())[:5]:
            print("  %016x id %d offset %d size %d" % (h, fid, off, size))
    elif cmd in ("find", "extract"):
        h = path_hash(argv[2])
        print("path hash %016x" % h)
        for path in archives():
            a = Archive(path)
            if h in a.files:
                print("found in", os.path.basename(path), a.files[h])
                if cmd == "extract":
                    data = a.read(h)
                    os.makedirs(os.path.dirname(os.path.abspath(argv[3])), exist_ok=True)
                    open(argv[3], "wb").write(data)
                    print("wrote %d bytes to %s" % (len(data), argv[3]))
                return
        print("not found")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
