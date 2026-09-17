#include "script/ffi.h"

#include <Windows.h>

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <format>
#include <mutex>

#include "core/guard.h"

extern "C" {
std::uint64_t bridger_ffi_call(void* fn, const std::uint64_t* ints, const double* xmms,
                               std::uint64_t count, double* xmm0_out);
extern const void* const bridger_ffi_hook_table[];
std::uint64_t bridger_ffi_dispatch(std::uint32_t slot, std::uint64_t* ints, double* xmms,
                                   double* xmm0_out);
}

namespace bridger::script::ffi {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

bool identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '<'
        || c == '>' || c == '.';
}

struct Primitive {
    std::string_view name;
    Kind kind;
    std::uint32_t size;
};

constexpr Primitive kPrimitives[] = {
    {"void", Kind::Void, 0},
    {"bool", Kind::Bool, 1},
    {"int8", Kind::I8, 1},       {"i8", Kind::I8, 1},       {"char", Kind::I8, 1},
    {"tchar", Kind::I8, 1},
    {"uint8", Kind::U8, 1},      {"u8", Kind::U8, 1},       {"uchar", Kind::U8, 1},
    {"byte", Kind::U8, 1},
    {"int16", Kind::I16, 2},     {"i16", Kind::I16, 2},     {"short", Kind::I16, 2},
    {"uint16", Kind::U16, 2},    {"u16", Kind::U16, 2},     {"wchar", Kind::U16, 2},
    {"wchar_t", Kind::U16, 2},
    {"int", Kind::I32, 4},       {"int32", Kind::I32, 4},   {"i32", Kind::I32, 4},
    {"long", Kind::I32, 4},
    {"uint", Kind::U32, 4},      {"uint32", Kind::U32, 4},  {"u32", Kind::U32, 4},
    {"ulong", Kind::U32, 4},     {"ucs4", Kind::U32, 4},
    {"int64", Kind::I64, 8},     {"i64", Kind::I64, 8},     {"longlong", Kind::I64, 8},
    {"uint64", Kind::U64, 8},    {"u64", Kind::U64, 8},     {"size_t", Kind::U64, 8},
    {"uintptr", Kind::U64, 8},   {"uintptr_t", Kind::U64, 8},
    {"float", Kind::F32, 4},     {"f32", Kind::F32, 4},
    {"double", Kind::F64, 8},    {"f64", Kind::F64, 8},
    {"ptr", Kind::Pointer, 8},   {"pointer", Kind::Pointer, 8},
};

const Primitive* primitive(std::string_view name) {
    for (const auto& p : kPrimitives) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

bool non_trivial(std::string_view name) {
    if (name == "String" || name == "WString") {
        return true;
    }
    constexpr std::string_view kTemplates[] = {"Ref<",    "WeakPtr<", "StreamingRef<",
                                               "UUIDRef<", "Array<",  "HashMap<",
                                               "HashSet<", "Array_"};
    return std::any_of(std::begin(kTemplates), std::end(kTemplates),
                       [&](std::string_view prefix) { return name.starts_with(prefix); });
}

Type classify_impl(std::string_view token, const TypeInfo& info, bool as_result) {
    Type type;
    std::string spaced;
    spaced.reserve(token.size() + 8);
    for (const char c : token) {
        if (c == '*' || c == '&') {
            spaced.push_back(' ');
            spaced.push_back(c);
            spaced.push_back(' ');
        } else {
            spaced.push_back(c);
        }
    }

    std::vector<std::string_view> words;
    std::string_view rest = spaced;
    while (!rest.empty()) {
        rest = trim(rest);
        if (rest.empty()) {
            break;
        }
        std::size_t length = 1;
        if (identifier_char(rest.front())) {
            int depth = 0;
            length = 0;
            while (length < rest.size()) {
                const char c = rest[length];
                if (c == '<') {
                    ++depth;
                } else if (c == '>') {
                    --depth;
                } else if (depth == 0 && !identifier_char(c)) {
                    break;
                }
                ++length;
            }
        }
        words.push_back(rest.substr(0, length));
        rest.remove_prefix(length);
    }

    std::string base;
    bool is_unsigned = false;
    for (const auto word : words) {
        if (word == "*") {
            ++type.depth;
        } else if (word == "&" ) {
            type.reference = true;
        } else if (word == "const" || word == "volatile" || word == "struct" || word == "class"
                   || word == "enum") {
            type.is_const = type.is_const || word == "const";
        } else if (word == "unsigned") {
            is_unsigned = true;
        } else if (word == "signed") {
        } else if (base.empty()) {
            base = std::string(word);
        }
    }
    if (base.empty() && is_unsigned) {
        base = "uint";
    } else if (is_unsigned) {
        if (base == "int" || base == "long") base = "uint";
        else if (base == "char") base = "uint8";
        else if (base == "short") base = "uint16";
    }
    type.name = base;

    if (type.depth > 0 || type.reference) {
        type.kind = Kind::Pointer;
        type.size = 8;
        if (base == "void") {
            type.name.clear();
        }
        return type;
    }
    if (const auto* p = primitive(base); p != nullptr) {
        type.kind = p->kind;
        type.size = p->size;
        if (p->kind == Kind::Pointer) {
            type.name.clear();
        }
        return type;
    }

    bool is_enum = false;
    const std::uint32_t size = info ? info(base, is_enum) : 0;
    if (is_enum) {
        type.kind = Kind::Enum;
        type.size = size != 0 ? size : 4;
        return type;
    }
    type.size = size;
    const bool register_sized = size == 1 || size == 2 || size == 4 || size == 8;
    if (register_sized && !(as_result && non_trivial(base))) {
        type.kind = Kind::StructValue;
    } else {
        type.kind = Kind::StructMemory;
    }
    return type;
}

std::string kind_text(const Type& type) {
    switch (type.kind) {
        case Kind::Void: return "void";
        case Kind::Bool: return "bool";
        case Kind::I8: return "int8";
        case Kind::U8: return "uint8";
        case Kind::I16: return "int16";
        case Kind::U16: return "uint16";
        case Kind::I32: return "int32";
        case Kind::U32: return "uint32";
        case Kind::I64: return "int64";
        case Kind::U64: return "uint64";
        case Kind::F32: return "float";
        case Kind::F64: return "double";
        case Kind::Pointer: {
            std::string text = type.name.empty() ? "void" : type.name;
            if (type.reference) {
                return text + "&";
            }
            for (int i = 0; i < std::max(type.depth, 1); ++i) {
                text += "*";
            }
            return text;
        }
        case Kind::Enum:
        case Kind::StructValue:
        case Kind::StructMemory: return type.name;
    }
    return "?";
}

}

std::string Signature::describe() const {
    std::string text = kind_text(result) + "(";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += kind_text(params[i]);
    }
    return text + ")";
}

Type classify(std::string_view token, const TypeInfo& info) {
    return classify_impl(token, info, false);
}

Signature from_tokens(const std::vector<std::string>& tokens, const TypeInfo& info) {
    Signature signature;
    if (tokens.empty()) {
        return signature;
    }
    signature.result = classify_impl(tokens.front(), info, true);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        signature.params.push_back(classify_impl(tokens[i], info, false));
    }
    return signature;
}

bool parse(std::string_view text, const TypeInfo& info, Signature& out, std::string& error) {
    text = trim(text);
    const auto open = text.find('(');
    const auto close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        error = std::format("'{}' is not a signature; write it as \"result(param, param)\"", text);
        return false;
    }
    Signature signature;

    auto head = trim(text.substr(0, open));
    if (!head.empty() && identifier_char(head.back())) {
        const auto space = head.find_last_of(" \t*&");
        if (space != std::string_view::npos) {
            const auto name = head.substr(space + 1);
            if (!primitive(name) && name != "const") {
                head = trim(head.substr(0, space + 1));
            }
        }
    }
    signature.result = head.empty() ? Type{} : classify_impl(head, info, true);

    const auto body = trim(text.substr(open + 1, close - open - 1));
    if (!body.empty() && body != "void") {
        int depth = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= body.size(); ++i) {
            const char c = i < body.size() ? body[i] : ',';
            if (c == '<') {
                ++depth;
            } else if (c == '>') {
                --depth;
            } else if (c == ',' && depth == 0) {
                auto param = trim(body.substr(start, i - start));
                start = i + 1;
                if (param.empty()) {
                    error = std::format("empty parameter in '{}'", text);
                    return false;
                }
                const auto split = param.find_last_of(" \t*&");
                if (split != std::string_view::npos && identifier_char(param.back())) {
                    const auto name = param.substr(split + 1);
                    const auto before = trim(param.substr(0, split + 1));
                    if (!before.empty() && before != "const" && before != "unsigned"
                        && !primitive(name) && name != "const") {
                        param = before;
                    }
                }
                auto type = classify_impl(param, info, false);
                if (type.kind == Kind::Void) {
                    error = std::format("parameter '{}' cannot be void", param);
                    return false;
                }
                if (type.kind == Kind::StructMemory && type.size == 0) {
                    error = std::format("'{}' is not a primitive or a known engine type", param);
                    return false;
                }
                signature.params.push_back(std::move(type));
            }
        }
    }
    if (signature.result.kind == Kind::StructMemory && signature.result.size == 0) {
        error = std::format("'{}' is not a primitive or a known engine type", head);
        return false;
    }
    out = std::move(signature);
    return true;
}

std::uint64_t pack_f32(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

std::uint64_t pack_f64(double value) {
    return std::bit_cast<std::uint64_t>(value);
}

float unpack_f32(std::uint64_t bits) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
}

double unpack_f64(std::uint64_t bits) {
    return std::bit_cast<double>(bits);
}

namespace {

struct CallWork {
    void* fn;
    const std::uint64_t* ints;
    const double* xmms;
    std::size_t count;
    Result* out;
};

void call_body(void* raw) {
    auto& work = *static_cast<CallWork*>(raw);
    work.out->rax = bridger_ffi_call(work.fn, work.ints, work.xmms, work.count, &work.out->xmm0);
}

}

std::uint32_t call(void* fn, const std::uint64_t* ints, const double* xmms, std::size_t count,
                   Result& out) {
    if (fn == nullptr || ints == nullptr || xmms == nullptr) {
        return EXCEPTION_ACCESS_VIOLATION;
    }
    CallWork work{fn, ints, xmms, count, &out};
    return guarded_call(call_body, &work);
}

std::uint32_t Frame::call_original(Result& out) const {
    return call(trampoline, ints, xmms, slots, out);
}

namespace {

struct Slot {
    std::atomic<Handler> handler{nullptr};
    Handler installed = nullptr;
    void* user = nullptr;
    void* target = nullptr;
    void* trampoline = nullptr;
    std::size_t slots = 0;
    std::atomic<int> in_flight{0};
    bool used = false;
    bool leaked = false;
};

std::array<Slot, kHookSlots> g_slots;
std::mutex g_mutex;

}

int create_hook(void* target, std::size_t slots, Handler handler, void* user, std::string& error) {
    if (target == nullptr || handler == nullptr) {
        error = "no target";
        return -1;
    }
    std::scoped_lock lock(g_mutex);
    int index = -1;
    for (std::size_t i = 0; i < kHookSlots; ++i) {
        if (!g_slots[i].used) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) {
        error = std::format("all {} script hooks are in use", kHookSlots);
        return -1;
    }
    auto& slot = g_slots[static_cast<std::size_t>(index)];
    void* trampoline = nullptr;
    const auto status = MH_CreateHook(target, const_cast<void*>(bridger_ffi_hook_table[index]),
                                      &trampoline);
    if (status != MH_OK) {
        error = status == MH_ERROR_ALREADY_CREATED
                    ? std::string("that function is already hooked, by a mod or another script")
                    : std::format("MinHook refused the target: {}", MH_StatusToString(status));
        return -1;
    }
    slot.used = true;
    slot.leaked = false;
    slot.target = target;
    slot.trampoline = trampoline;
    slot.slots = slots;
    slot.user = user;
    slot.installed = handler;
    slot.handler.store(nullptr, std::memory_order_release);
    return index;
}

bool enable_hook(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kHookSlots) {
        return false;
    }
    auto& slot = g_slots[static_cast<std::size_t>(index)];
    if (!slot.used) {
        return false;
    }
    slot.handler.store(slot.installed, std::memory_order_release);
    const auto status = MH_EnableHook(slot.target);
    if (status != MH_OK && status != MH_ERROR_ENABLED) {
        slot.handler.store(nullptr, std::memory_order_release);
        return false;
    }
    return true;
}

bool disable_hook(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kHookSlots) {
        return false;
    }
    auto& slot = g_slots[static_cast<std::size_t>(index)];
    if (!slot.used) {
        return false;
    }
    slot.handler.store(nullptr, std::memory_order_release);
    return MH_DisableHook(slot.target) == MH_OK;
}

bool remove_hook(int index, unsigned wait_ms) {
    if (index < 0 || static_cast<std::size_t>(index) >= kHookSlots) {
        return false;
    }
    auto& slot = g_slots[static_cast<std::size_t>(index)];
    if (!slot.used) {
        return false;
    }
    disable_hook(index);
    const auto deadline = GetTickCount64() + wait_ms;
    while (slot.in_flight.load(std::memory_order_acquire) > 0 && GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (slot.in_flight.load(std::memory_order_acquire) > 0) {
        slot.leaked = true;
        return false;
    }
    std::scoped_lock lock(g_mutex);
    MH_RemoveHook(slot.target);
    slot.used = false;
    slot.target = nullptr;
    slot.trampoline = nullptr;
    slot.user = nullptr;
    slot.installed = nullptr;
    slot.slots = 0;
    return true;
}

int create_callback(std::size_t slots, Handler handler, void* user, void* fallback,
                    void*& address, std::string& error) {
    if (handler == nullptr) {
        error = "no handler";
        return -1;
    }
    std::scoped_lock lock(g_mutex);
    for (std::size_t i = 0; i < kHookSlots; ++i) {
        auto& slot = g_slots[i];
        if (slot.used) {
            continue;
        }
        slot.used = true;
        slot.leaked = false;
        slot.target = nullptr;
        slot.trampoline = fallback;
        slot.slots = slots;
        slot.user = user;
        slot.installed = handler;
        slot.handler.store(handler, std::memory_order_release);
        address = const_cast<void*>(bridger_ffi_hook_table[i]);
        return static_cast<int>(i);
    }
    error = std::format("all {} script hooks and callbacks are in use", kHookSlots);
    return -1;
}

bool release_callback(int index, unsigned wait_ms) {
    if (index < 0 || static_cast<std::size_t>(index) >= kHookSlots) {
        return false;
    }
    auto& slot = g_slots[static_cast<std::size_t>(index)];
    if (!slot.used || slot.target != nullptr) {
        return false;
    }
    slot.handler.store(nullptr, std::memory_order_release);
    const auto deadline = GetTickCount64() + wait_ms;
    while (slot.in_flight.load(std::memory_order_acquire) > 0 && GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (slot.in_flight.load(std::memory_order_acquire) > 0) {
        slot.leaked = true;
        return false;
    }
    slot.leaked = true;
    return true;
}

int in_flight(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kHookSlots) {
        return 0;
    }
    return g_slots[static_cast<std::size_t>(index)].in_flight.load(std::memory_order_acquire);
}

void* hook_target(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kHookSlots) {
        return nullptr;
    }
    return g_slots[static_cast<std::size_t>(index)].target;
}

}

std::uint64_t bridger_ffi_dispatch(std::uint32_t index, std::uint64_t* ints, double* xmms,
                                   double* xmm0_out) {
    using namespace bridger::script::ffi;
    auto& slot = g_slots[index];
    const bridger::GuardSuspension suspended;
    slot.in_flight.fetch_add(1, std::memory_order_acq_rel);
    Frame frame{ints, xmms, xmm0_out, slot.trampoline, slot.slots};
    std::uint64_t rax = 0;
    if (const auto handler = slot.handler.load(std::memory_order_acquire); handler != nullptr) {
        rax = handler(slot.user, frame);
    } else if (slot.trampoline != nullptr) {
        rax = bridger_ffi_call(slot.trampoline, ints, xmms, slot.slots, xmm0_out);
    } else {
        *xmm0_out = 0.0;
    }
    slot.in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return rax;
}
