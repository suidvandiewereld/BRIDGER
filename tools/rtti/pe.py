import struct


class PE:
    def __init__(self, path):
        self.blob = open(path, "rb").read()
        blob = self.blob
        self.pe = struct.unpack_from("<I", blob, 0x3C)[0]
        if blob[self.pe:self.pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: not a PE image")
        self.opt = self.pe + 24
        self.image_base = struct.unpack_from("<Q", blob, self.opt + 24)[0]

        count = struct.unpack_from("<H", blob, self.pe + 6)[0]
        table = self.opt + struct.unpack_from("<H", blob, self.pe + 20)[0]
        self.sections = []
        for i in range(count):
            entry = table + i * 40
            name = blob[entry:entry + 8].rstrip(b"\0").decode()
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", blob, entry + 8)
            self.sections.append((name, vaddr, vsize, raddr, rsize))

    def section_of(self, rva):
        for name, vaddr, vsize, raddr, rsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return name
        return None

    def range_of(self, name):
        for section, vaddr, vsize, raddr, rsize in self.sections:
            if section == name:
                return vaddr, vsize, raddr, rsize
        return None

    def offset(self, rva):
        for name, vaddr, vsize, raddr, rsize in self.sections:
            if vaddr <= rva < vaddr + rsize:
                return raddr + (rva - vaddr)
        return None

    def u8(self, rva):
        o = self.offset(rva)
        return None if o is None else self.blob[o]

    def u16(self, rva):
        o = self.offset(rva)
        return None if o is None else struct.unpack_from("<H", self.blob, o)[0]

    def u32(self, rva):
        o = self.offset(rva)
        return None if o is None else struct.unpack_from("<I", self.blob, o)[0]

    def i32(self, rva):
        o = self.offset(rva)
        return None if o is None else struct.unpack_from("<i", self.blob, o)[0]

    def u64(self, rva):
        o = self.offset(rva)
        return None if o is None else struct.unpack_from("<Q", self.blob, o)[0]

    def pointer(self, rva):
        value = self.u64(rva)
        if not value:
            return None
        target = value - self.image_base
        return target if self.section_of(target) is not None else None

    def cstring(self, rva, limit=256):
        o = self.offset(rva)
        if o is None:
            return None
        end = self.blob.find(b"\0", o, o + limit)
        if end < 0:
            return None
        try:
            return self.blob[o:end].decode("ascii")
        except UnicodeDecodeError:
            return None
