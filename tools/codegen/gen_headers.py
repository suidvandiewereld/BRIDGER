import argparse
import gzip
import json
import keyword
import os
import re
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TYPES = os.path.join(ROOT, "data", "rtti", "ds", "static.json")
SYMBOLS = os.path.join(ROOT, "data", "rtti", "ds", "symbols.json")
OUT = os.path.join(ROOT, "include", "decima")

ATOMS = {
    "bool": ("bool", 1),
    "int8": ("std::int8_t", 1),
    "uint8": ("std::uint8_t", 1),
    "int16": ("std::int16_t", 2),
    "uint16": ("std::uint16_t", 2),
    "int": ("std::int32_t", 4),
    "int32": ("std::int32_t", 4),
    "uint": ("std::uint32_t", 4),
    "uint32": ("std::uint32_t", 4),
    "int64": ("std::int64_t", 8),
    "uint64": ("std::uint64_t", 8),
    "intptr": ("std::intptr_t", 8),
    "uintptr": ("std::uintptr_t", 8),
    "float": ("float", 4),
    "double": ("double", 8),
    "HalfFloat": ("decima::HalfFloat", 2),
    "String": ("decima::String", 8),
    "WString": ("decima::WString", 8),
}

WRAPPERS = {
    "Ref": ("decima::Ref", 8),
    "cptr": ("decima::cptr", 8),
    "StreamingRef": ("decima::StreamingRef", 8),
    "UUIDRef": ("decima::UUIDRef", 16),
    "WeakPtr": ("decima::WeakPtr", 24),
    "Array": ("decima::Array", 16),
    "HashMap": ("decima::HashMap", 16),
    "HashSet": ("decima::HashSet", 16),
}

RESERVED = set(keyword.kwlist) | {
    "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
    "catch", "char", "class", "const", "consteval", "constexpr", "constinit", "const_cast",
    "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do",
    "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
    "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
    "noexcept", "not", "nullptr", "operator", "or", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
    "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor",
}

MACRO_GUARDS = [
    "ERROR", "IN", "OUT", "DELETE", "NONE", "OPTIONAL", "NEAR", "FAR", "SEVERITY_ERROR",
    "min", "max", "TRUE", "FALSE", "CONST", "INFINITE", "NO_ERROR", "OVERFLOW", "UNDERFLOW",
    "DOMAIN", "SING", "TRANSPARENT", "RELATIVE", "ABSOLUTE", "small", "interface",
]

GENERIC = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)<(.+)>$")
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def sanitize(name, used):
    if not name or not IDENTIFIER.match(name):
        name = "field"
    if name in RESERVED:
        name = name + "_"
    candidate = name
    index = 1
    while candidate in used:
        candidate = f"{name}_{index}"
        index += 1
    used.add(candidate)
    return candidate


class Generator:
    def __init__(self, types, symbols):
        self.classes = {n: t for n, t in types.items() if t["kind"] == "class"}
        self.enums = {n: t for n, t in types.items() if t["kind"] != "class"}
        self.symbols = symbols
        self.stats = defaultdict(int)
        self.inferred = self.infer_sizes()

    def infer_sizes(self):
        gaps = defaultdict(list)
        for entry in self.classes.values():
            members = sorted((m for m in entry["members"] if not m.get("property")),
                             key=lambda m: m["offset"])
            for index, member in enumerate(members):
                following = (members[index + 1]["offset"] if index + 1 < len(members)
                             else entry["size"])
                if following and following > member["offset"]:
                    gaps[member["type"]].append(following - member["offset"])
        return {name: min(values) for name, values in gaps.items()}

    def enum_underlying(self, entry):
        signed = any(v["value"] < 0 for v in entry["values"])
        width = entry["size"] or 4
        table = {
            1: ("std::int8_t", "std::uint8_t"),
            2: ("std::int16_t", "std::uint16_t"),
            4: ("std::int32_t", "std::uint32_t"),
            8: ("std::int64_t", "std::uint64_t"),
        }
        pair = table.get(width, table[4])
        return pair[0] if signed else pair[1]

    def size_of(self, name):
        if name in ATOMS:
            return ATOMS[name][1]
        if name in self.enums:
            return self.enums[name]["size"] or 4
        if name in self.classes:
            return self.classes[name]["size"]
        return None

    def resolve(self, text):
        if not text:
            return None
        match = GENERIC.match(text)
        if match:
            outer, inner = match.group(1), match.group(2)
            if outer not in WRAPPERS:
                return None
            template, size = WRAPPERS[outer]
            item = self.resolve_name(inner)
            return (f"{template}<{item}>", size)
        if text in ATOMS:
            return ATOMS[text]
        if text in self.enums:
            return (f"decima::{text}", self.enums[text]["size"] or 4)
        if text in self.classes and self.classes[text]["size"]:
            return (f"decima::{text}", self.classes[text]["size"])
        return None

    def raw_class(self, text):
        if text and not GENERIC.match(text) and text in self.classes:
            return text
        return None

    def resolve_name(self, text):
        match = GENERIC.match(text)
        if match:
            outer, inner = match.group(1), match.group(2)
            if outer in WRAPPERS:
                return f"{WRAPPERS[outer][0]}<{self.resolve_name(inner)}>"
            return "void"
        if text in ATOMS:
            return ATOMS[text][0]
        if text in self.classes or text in self.enums:
            return f"decima::{text}"
        return "void"

    def dependencies(self, name):
        entry = self.classes[name]
        out = set()
        for base in entry["bases"]:
            if base["name"] in self.classes and self.classes[base["name"]]["size"]:
                out.add(base["name"])
        for member in entry["members"]:
            embedded = self.raw_class(member["type"])
            if embedded and self.classes[embedded]["size"]:
                out.add(embedded)
        out.discard(name)
        return out

    def order(self):
        pending = dict((n, self.dependencies(n)) for n in self.classes)
        done = set()
        result = []
        while pending:
            ready = sorted(n for n, deps in pending.items() if deps <= done)
            if not ready:
                stuck = sorted(pending)
                self.stats["cyclic"] += len(stuck)
                result.extend(stuck)
                break
            for name in ready:
                result.append(name)
                done.add(name)
                del pending[name]
        return result

    def layout(self, name):
        entry = self.classes[name]
        total = entry["size"]
        items = []

        for base in entry["bases"]:
            base_name = base["name"]
            if base_name not in self.classes:
                continue
            size = self.classes[base_name]["size"]
            if not size:
                continue
            items.append((base["offset"], size, f"decima::{base_name}", f"base_{base_name}", True))

        for member in entry["members"]:
            if member.get("property"):
                self.stats["properties"] += 1
                continue
            resolved = self.resolve(member["type"])
            if resolved is None:
                size = self.inferred.get(member["type"], 0)
                items.append((member["offset"], size, None, member["name"], False))
                self.stats["opaque_member"] += 1
            else:
                items.append((member["offset"], resolved[1], resolved[0], member["name"], False))

        items.sort(key=lambda item: (item[0], -item[1]))

        fields = []
        used = set()
        cursor = 0
        for index, (offset, size, cpp, field, is_base) in enumerate(items):
            if offset < cursor:
                self.stats["overlap_dropped"] += 1
                continue
            if offset > cursor:
                fields.append((cursor, None, f"padding_{cursor:x}", offset - cursor))
                cursor = offset

            following = total
            for later in items[index + 1:]:
                if later[0] > offset:
                    following = later[0]
                    break
            available = (following - offset) if following else size

            if size == 0:
                self.stats["opaque_field"] += 1
                continue
            if cpp is None or (available and size > available):
                width = min(size, available) if available else size
                if width <= 0:
                    self.stats["opaque_field"] += 1
                    continue
                if cpp is not None:
                    self.stats["truncated_to_bytes"] += 1
                fields.append((offset, None, sanitize(field, used), width))
                cursor = offset + width
                continue

            fields.append((offset, cpp, sanitize(field, used), size))
            cursor = offset + size

        if total and cursor < total:
            fields.append((cursor, None, f"padding_{cursor:x}", total - cursor))

        return fields, total

    def emit_enums(self):
        lines = ["#pragma once", "", '#include "decima/prelude.h"', "", "namespace decima {", ""]
        for name in sorted(self.enums):
            entry = self.enums[name]
            lines.append(f"enum class {name} : {self.enum_underlying(entry)} {{")
            used = set()
            for value in entry["values"]:
                label = sanitize(value["name"], used)
                lines.append(f"    {label} = {value['value']},")
            lines.append("};")
            lines.append("")
            self.stats["enums"] += 1
        lines.append("}")
        lines.append("")
        return "\n".join(lines)

    def emit_forward(self):
        lines = ["#pragma once", "", '#include "decima/prelude.h"', "", "namespace decima {", ""]
        for name in sorted(self.classes):
            lines.append(f"struct {name};")
        lines.append("")
        lines.append("}")
        lines.append("")
        return "\n".join(lines)

    def emit_classes(self, names, index, total_chunks):
        lines = ["#pragma once", "", '#include "decima/enums.h"', '#include "decima/forward.h"',
                 '#include "decima/prelude.h"', ""]
        lines.append("namespace decima {")
        lines.append("")
        lines.append("#pragma pack(push, 1)")
        lines.append("")
        for name in names:
            fields, size = self.layout(name)
            lines.append(f"struct {name} {{")
            if not fields:
                lines.append("    std::uint8_t reserved[1];")
            for offset, cpp, field, width in fields:
                if cpp is None:
                    lines.append(f"    std::uint8_t {field}[{width}];")
                else:
                    lines.append(f"    {cpp} {field};")
            lines.append("};")
            if size:
                lines.append(f"static_assert(sizeof({name}) == {size});")
                for offset, cpp, field, width in fields:
                    if cpp is not None:
                        lines.append(f"static_assert(offsetof({name}, {field}) == {offset});")
            lines.append("")
            self.stats["classes"] += 1
        lines.append("#pragma pack(pop)")
        lines.append("")
        lines.append("}")
        lines.append("")
        return "\n".join(lines)

    def emit_properties(self):
        lines = ["#pragma once", "", '#include "decima/types.h"', "",
                 "namespace decima::properties {", ""]
        used = set()
        for name in sorted(self.classes):
            for member in self.classes[name]["members"]:
                if not member.get("property"):
                    continue
                label = sanitize(re.sub(r"[^A-Za-z0-9_]", "_", f"{name}_{member['name']}"), used)
                value = self.resolve_name(member["type"])
                lines.append(f"using {label}_type = {value};")
                lines.append(f"inline constexpr std::uintptr_t {label}_getter = {member['getter']:#x};")
                if member.get("setter"):
                    lines.append(f"inline constexpr std::uintptr_t {label}_setter = {member['setter']:#x};")
                lines.append("")
                self.stats["property_bindings"] += 1
        lines.append("}")
        lines.append("")
        return "\n".join(lines)

    def token_type(self, token):
        base = token["type"]
        modifiers = token["modifiers"].strip()
        if not base:
            return None
        if base == "void" and not modifiers:
            return "void"
        mapped = self.resolve_name(base)
        if mapped == "void" and base != "void":
            return None
        if modifiers in ("", None):
            return mapped
        if modifiers == "const *":
            return f"const {mapped}*"
        if modifiers == "*":
            return f"{mapped}*"
        if modifiers == "const &":
            return f"const {mapped}&"
        if modifiers == "&":
            return f"{mapped}&"
        if modifiers == "const":
            return f"const {mapped}"
        return None

    def emit_symbols(self):
        lines = ["#pragma once", "", '#include "decima/types.h"', "", "namespace decima::symbols {", ""]
        used_names = set()
        for group_name in sorted(self.symbols):
            group = self.symbols[group_name]
            entries = []
            for symbol in group["symbols"]:
                if symbol["kind"] not in ("function", "variable"):
                    continue
                definition = symbol["exported"]
                if not definition.get("rva"):
                    definition = symbol["internal"]
                if not definition.get("rva"):
                    continue

                tokens = symbol["exported"]["tokens"] or symbol["internal"]["tokens"]
                signature = None
                if tokens and symbol["kind"] == "function":
                    mapped = [self.token_type(t) for t in tokens]
                    if all(m is not None for m in mapped):
                        returns = mapped[0]
                        params = ", ".join(mapped[1:]) if len(mapped) > 1 else ""
                        signature = f"{returns} (*)({params})"

                entries.append((symbol, definition, signature))

            if not entries:
                continue

            for symbol, definition, signature in entries:
                label = re.sub(r"[^A-Za-z0-9_]", "_", f"{group_name}_{symbol['name']}")
                label = sanitize(label, used_names)
                lines.append(f"inline constexpr std::uintptr_t {label}_rva = {definition['rva']:#x};")
                if signature is not None:
                    lines.append(f"using {label}_t = {signature};")
                    self.stats["typed_functions"] += 1
                else:
                    self.stats["untyped_functions"] += 1
                lines.append("")
        lines.append("}")
        lines.append("")
        return "\n".join(lines)


def emit_prelude():
    guards = "\n".join(f"#ifdef {name}\n#undef {name}\n#endif" for name in MACRO_GUARDS)
    return f"""#pragma once

#include <cstddef>
#include <cstdint>

{guards}

namespace decima {{

struct HalfFloat {{
    std::uint16_t bits;
}};

struct String {{
    const char* text;
}};

struct WString {{
    const wchar_t* text;
}};

template <typename T>
struct Ref {{
    T* pointer;
}};

template <typename T>
struct cptr {{
    T* pointer;
}};

template <typename T>
struct StreamingRef {{
    T* pointer;
}};

template <typename T>
struct UUIDRef {{
    std::uint8_t uuid[16];
}};

template <typename T>
struct WeakPtr {{
    void* control;
    T* pointer;
    std::uint64_t reserved;
}};

template <typename T>
struct Array {{
    std::uint32_t count;
    std::uint32_t capacity;
    T* data;

    [[nodiscard]] T* begin() const {{ return data; }}
    [[nodiscard]] T* end() const {{ return data + count; }}
    [[nodiscard]] T& operator[](std::uint32_t index) const {{ return data[index]; }}
}};

template <>
struct Array<void> {{
    std::uint32_t count;
    std::uint32_t capacity;
    void* data;
}};

template <typename T>
struct HashMap {{
    std::uint8_t storage[16];
}};

template <typename T>
struct HashSet {{
    std::uint8_t storage[16];
}};

static_assert(sizeof(String) == 8);
static_assert(sizeof(Ref<int>) == 8);
static_assert(sizeof(UUIDRef<int>) == 16);
static_assert(sizeof(WeakPtr<int>) == 24);
static_assert(sizeof(Array<int>) == 16);
static_assert(sizeof(HashMap<int>) == 16);

inline std::uintptr_t image_base = 0;

template <typename T>
[[nodiscard]] T bind(std::uintptr_t rva) {{
    return reinterpret_cast<T>(image_base + rva);
}}

}}
"""


def load(path):
    if not os.path.exists(path) and os.path.exists(path + ".gz"):
        path = path + ".gz"
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as fh:
        return json.load(fh)


def main():
    parser = argparse.ArgumentParser(description="Generate C++ headers from the Decima dumps.")
    parser.add_argument("--types", default=TYPES)
    parser.add_argument("--symbols", default=SYMBOLS)
    parser.add_argument("--out", default=OUT)
    parser.add_argument("--chunk", type=int, default=1200)
    args = parser.parse_args()

    types = load(args.types)["types"]
    symbols = load(args.symbols)["groups"]

    generator = Generator(types, symbols)
    os.makedirs(args.out, exist_ok=True)

    def write(name, text):
        with open(os.path.join(args.out, name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)

    write("prelude.h", emit_prelude())
    write("enums.h", generator.emit_enums())
    write("forward.h", generator.emit_forward())

    ordered = generator.order()
    chunks = [ordered[i:i + args.chunk] for i in range(0, len(ordered), args.chunk)]
    for index, chunk in enumerate(chunks):
        write(f"types_{index}.h", generator.emit_classes(chunk, index, len(chunks)))

    includes = "\n".join(f'#include "decima/types_{i}.h"' for i in range(len(chunks)))
    write("types.h", f'#pragma once\n\n#include "decima/enums.h"\n#include "decima/prelude.h"\n{includes}\n')
    write("symbols.h", generator.emit_symbols())
    write("properties.h", generator.emit_properties())
    write("decima.h", '#pragma once\n\n#include "decima/properties.h"\n'
                      '#include "decima/symbols.h"\n#include "decima/types.h"\n')

    print(f"wrote {len(chunks) + 5} headers to {args.out}")
    for key in sorted(generator.stats):
        print(f"  {key}: {generator.stats[key]}")


if __name__ == "__main__":
    main()
