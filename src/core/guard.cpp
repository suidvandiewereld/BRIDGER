#include "core/guard.h"
#include "core/log.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace bridger {
namespace {

struct Recovery {
    CONTEXT context;
    volatile std::uint32_t code;
    volatile bool armed;
};

thread_local Recovery* t_recovery = nullptr;
thread_local FaultInfo t_last_fault{};

void record_fault(const EXCEPTION_POINTERS* info) {
    FaultInfo fault{};
    fault.code = info->ExceptionRecord->ExceptionCode;
    fault.instruction = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    if (fault.code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        fault.accessed = info->ExceptionRecord->ExceptionInformation[1];
    }
    CONTEXT context = *info->ContextRecord;
    for (std::uint32_t i = 0; i < std::size(fault.frames); ++i) {
        DWORD64 image_base = 0;
        auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (entry == nullptr) {
            break;
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data,
                         &establisher, nullptr);
        if (context.Rip == 0) {
            break;
        }
        fault.frames[fault.frame_count++] = context.Rip;
    }
    t_last_fault = fault;
}

std::string module_relative(std::uintptr_t address) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) && module != nullptr) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        char buffer[MAX_PATH + 32]{};
        std::snprintf(buffer, sizeof buffer, "%s+%#llx", name != nullptr ? name + 1 : path,
                      static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
        return buffer;
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof buffer, "%#llx", static_cast<unsigned long long>(address));
    return buffer;
}

bool recoverable(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

LONG CALLBACK recover(EXCEPTION_POINTERS* info) {
    {
        static std::atomic<int> logged{0};
        const auto code = info->ExceptionRecord->ExceptionCode;
        const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION
                        || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO
                        || code == EXCEPTION_PRIV_INSTRUCTION || code == 0xC0000409
                        || code == EXCEPTION_BREAKPOINT;
        if (fatal && (t_recovery == nullptr || !t_recovery->armed) && logged.fetch_add(1) < 3) {
            record_fault(info);
            log::error("unhandled exception {:#x} on thread {}: {}", code, GetCurrentThreadId(), describe_last_fault());
        }
    }
    Recovery* const recovery = t_recovery;
    if (recovery == nullptr || !recovery->armed || !recoverable(info->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    recovery->armed = false;
    record_fault(info);
    recovery->code = info->ExceptionRecord->ExceptionCode;
    *info->ContextRecord = recovery->context;
    return EXCEPTION_CONTINUE_EXECUTION;
}

std::mutex g_handler_mutex;
PVOID g_handler = nullptr;

}

void promote_fault_handler() {
    std::scoped_lock lock(g_handler_mutex);
    if (g_handler != nullptr) {
        RemoveVectoredExceptionHandler(g_handler);
    }
    g_handler = AddVectoredExceptionHandler(1, recover);
}

std::uint32_t guarded_call(GuardedFn function, void* context) {
    if (g_handler == nullptr) {
        promote_fault_handler();
    }
    Recovery recovery{};
    Recovery* const previous = t_recovery;
    RtlCaptureContext(&recovery.context);
    if (recovery.code != 0) {
        t_recovery = previous;
        return recovery.code;
    }
    recovery.armed = true;
    t_recovery = &recovery;
    __try {
        function(context);
    } __except (recoverable(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        recovery.code = GetExceptionCode();
    }
    recovery.armed = false;
    t_recovery = previous;
    return recovery.code;
}

const FaultInfo& last_fault() {
    return t_last_fault;
}

std::string describe_last_fault() {
    const FaultInfo& fault = t_last_fault;
    if (fault.code == 0) {
        return {};
    }
    std::string text = "at " + module_relative(fault.instruction);
    if (fault.accessed != 0 || fault.code == EXCEPTION_ACCESS_VIOLATION) {
        char buffer[48]{};
        std::snprintf(buffer, sizeof buffer, " touching %#llx", static_cast<unsigned long long>(fault.accessed));
        text += buffer;
    }
    for (std::uint32_t i = 0; i < fault.frame_count; ++i) {
        text += " <- " + module_relative(fault.frames[i]);
    }
    return text;
}

GuardSuspension::GuardSuspension() : saved_(t_recovery) {
    t_recovery = nullptr;
}

GuardSuspension::~GuardSuspension() {
    t_recovery = static_cast<Recovery*>(saved_);
}

bool safe_read(const void* address, void* destination, std::size_t bytes) {
    if (address == nullptr || destination == nullptr || bytes == 0) {
        return false;
    }
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, destination, bytes, &copied) != 0
        && copied == bytes;
}

}
