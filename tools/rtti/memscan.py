"""Scan a running ds.exe's committed memory for a byte pattern and print hits with context.

    python tools/rtti/memscan.py <hex bytes> [context bytes]
"""
import ctypes
import ctypes.wintypes as w
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = w.HANDLE
k32.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64), ("AllocationProtect", w.DWORD),
                ("PartitionId", w.WORD), ("RegionSize", ctypes.c_uint64), ("State", w.DWORD), ("Protect", w.DWORD),
                ("Type", w.DWORD)]


def main():
    pattern = bytes.fromhex(sys.argv[1])
    ctx = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    pid = int(subprocess.check_output(["powershell", "-NoProfile", "-Command", "(Get-Process ds).Id"]).strip())
    proc = k32.OpenProcess(0x0410, False, pid)
    addr, hits = 0, 0
    mbi = MBI()
    while addr < 0x7FFFFFFFFFFF and k32.VirtualQueryEx(proc, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x02, 0x40, 0x20) and mbi.RegionSize < 0x40000000:
            buf = ctypes.create_string_buffer(mbi.RegionSize)
            got = ctypes.c_size_t()
            if k32.ReadProcessMemory(proc, ctypes.c_void_p(mbi.BaseAddress), buf, mbi.RegionSize, ctypes.byref(got)):
                data = buf.raw[:got.value]
                i = data.find(pattern)
                while i >= 0 and hits < 40:
                    lo = max(0, i - ctx)
                    print("%#x: %s | %s" % (mbi.BaseAddress + i, data[lo:i].hex(), data[i:i + len(pattern) + ctx].hex()))
                    hits += 1
                    i = data.find(pattern, i + 1)
        addr = mbi.BaseAddress + mbi.RegionSize
    print("hits", hits)


if __name__ == "__main__":
    main()
