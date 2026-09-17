"""Print every thread's call stack of a running ds.exe, as module+offset, using dbghelp's StackWalk64.

    python tools/rtti/stacks.py [pid]
"""
import ctypes
import ctypes.wintypes as w
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)

k32.OpenProcess.restype = w.HANDLE
k32.OpenThread.restype = w.HANDLE
k32.CreateToolhelp32Snapshot.restype = w.HANDLE
psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), w.DWORD]
psapi.GetModuleBaseNameW.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, w.DWORD]
psapi.GetModuleInformation.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, w.DWORD]
k32.SuspendThread.argtypes = [w.HANDLE]
k32.ResumeThread.argtypes = [w.HANDLE]
k32.CloseHandle.argtypes = [w.HANDLE]
k32.GetThreadContext.argtypes = [w.HANDLE, ctypes.c_void_p]
dbghelp.SymInitialize.argtypes = [w.HANDLE, ctypes.c_void_p, w.BOOL]
PROCESS_ALL = 0x1F0FFF
THREAD_ALL = 0x1F03FF
TH32CS_SNAPTHREAD = 4
CONTEXT_FULL = 0x10000B
IMAGE_FILE_MACHINE_AMD64 = 0x8664


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ThreadID", w.DWORD),
                ("th32OwnerProcessID", w.DWORD), ("tpBasePri", w.LONG), ("tpDeltaPri", w.LONG),
                ("dwFlags", w.DWORD)]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_uint64), ("High", ctypes.c_int64)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [("P1Home", ctypes.c_uint64), ("P2Home", ctypes.c_uint64), ("P3Home", ctypes.c_uint64),
                ("P4Home", ctypes.c_uint64), ("P5Home", ctypes.c_uint64), ("P6Home", ctypes.c_uint64),
                ("ContextFlags", w.DWORD), ("MxCsr", w.DWORD),
                ("SegCs", w.WORD), ("SegDs", w.WORD), ("SegEs", w.WORD), ("SegFs", w.WORD), ("SegGs", w.WORD),
                ("SegSs", w.WORD), ("EFlags", w.DWORD),
                ("Dr0", ctypes.c_uint64), ("Dr1", ctypes.c_uint64), ("Dr2", ctypes.c_uint64), ("Dr3", ctypes.c_uint64),
                ("Dr6", ctypes.c_uint64), ("Dr7", ctypes.c_uint64),
                ("Rax", ctypes.c_uint64), ("Rcx", ctypes.c_uint64), ("Rdx", ctypes.c_uint64), ("Rbx", ctypes.c_uint64),
                ("Rsp", ctypes.c_uint64), ("Rbp", ctypes.c_uint64), ("Rsi", ctypes.c_uint64), ("Rdi", ctypes.c_uint64),
                ("R8", ctypes.c_uint64), ("R9", ctypes.c_uint64), ("R10", ctypes.c_uint64), ("R11", ctypes.c_uint64),
                ("R12", ctypes.c_uint64), ("R13", ctypes.c_uint64), ("R14", ctypes.c_uint64), ("R15", ctypes.c_uint64),
                ("Rip", ctypes.c_uint64),
                ("FltSave", ctypes.c_byte * 512),
                ("VectorRegister", M128A * 26), ("VectorControl", ctypes.c_uint64),
                ("DebugControl", ctypes.c_uint64), ("LastBranchToRip", ctypes.c_uint64),
                ("LastBranchFromRip", ctypes.c_uint64), ("LastExceptionToRip", ctypes.c_uint64),
                ("LastExceptionFromRip", ctypes.c_uint64)]


class ADDRESS64(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", w.WORD), ("Mode", ctypes.c_int)]


class STACKFRAME64(ctypes.Structure):
    _fields_ = [("AddrPC", ADDRESS64), ("AddrReturn", ADDRESS64), ("AddrFrame", ADDRESS64),
                ("AddrStack", ADDRESS64), ("AddrBStore", ADDRESS64), ("FuncTableEntry", ctypes.c_void_p),
                ("Params", ctypes.c_uint64 * 4), ("Far", w.BOOL), ("Virtual", w.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", ctypes.c_byte * 0x60)]


def modules(proc):
    arr = (ctypes.c_void_p * 1024)()
    need = w.DWORD()
    psapi.EnumProcessModulesEx(proc, arr, ctypes.sizeof(arr), ctypes.byref(need), 3)
    out = []
    for i in range(need.value // 8):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(proc, arr[i], name, 260)
        info = (ctypes.c_void_p * 3)()
        psapi.GetModuleInformation(proc, arr[i], info, ctypes.sizeof(info))
        out.append((info[0] or 0, info[1] or 0, name.value))
    return out


def label(mods, addr):
    for base, size, name in mods:
        if base <= addr < base + size:
            return "%s+%#x" % (name, addr - base)
    return "%#x" % addr


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else int(subprocess.check_output(
        ["powershell", "-NoProfile", "-Command", "(Get-Process ds).Id"]).strip())
    proc = k32.OpenProcess(PROCESS_ALL, False, pid)
    dbghelp.SymInitialize(proc, None, True)
    dbghelp.SymFunctionTableAccess64.restype = ctypes.c_void_p
    dbghelp.SymGetModuleBase64.restype = ctypes.c_uint64
    fta = ctypes.CFUNCTYPE(ctypes.c_void_p, w.HANDLE, ctypes.c_uint64)(lambda h, a: dbghelp.SymFunctionTableAccess64(h, ctypes.c_uint64(a)))
    gmb = ctypes.CFUNCTYPE(ctypes.c_uint64, w.HANDLE, ctypes.c_uint64)(lambda h, a: dbghelp.SymGetModuleBase64(h, ctypes.c_uint64(a)))
    mods = modules(proc)
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            th = k32.OpenThread(THREAD_ALL, False, te.th32ThreadID)
            k32.SuspendThread(th)
            ctx = CONTEXT(); ctx.ContextFlags = CONTEXT_FULL
            buf = ctypes.create_string_buffer(ctypes.sizeof(CONTEXT) + 64)
            addr = (ctypes.addressof(buf) + 15) & ~15
            ctypes.memmove(addr, ctypes.addressof(ctx), ctypes.sizeof(ctx))
            if k32.GetThreadContext(th, ctypes.c_void_p(addr)):
                ctx = CONTEXT.from_address(addr)
                frame = STACKFRAME64()
                frame.AddrPC.Offset, frame.AddrPC.Mode = ctx.Rip, 3
                frame.AddrFrame.Offset, frame.AddrFrame.Mode = ctx.Rbp, 3
                frame.AddrStack.Offset, frame.AddrStack.Mode = ctx.Rsp, 3
                frames = []
                for _ in range(24):
                    if not dbghelp.StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, ctypes.byref(frame),
                                               ctypes.c_void_p(addr), None, fta, gmb, None):
                        break
                    if frame.AddrPC.Offset == 0:
                        break
                    frames.append(label(mods, frame.AddrPC.Offset))
                print("thread %d: %s" % (te.th32ThreadID, " <- ".join(frames)))
                if any("0x17481" in fr or "0x17482" in fr or "0x17483" in fr for fr in frames[:1]):
                    print("   rbx %x rcx %x rdi %x rsi %x" % (ctx.Rbx, ctx.Rcx, ctx.Rdi, ctx.Rsi))
            k32.ResumeThread(th)
            k32.CloseHandle(th)
        ok = k32.Thread32Next(snap, ctypes.byref(te))


if __name__ == "__main__":
    main()
