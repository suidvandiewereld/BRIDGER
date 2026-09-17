#include "fx/dxbc.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>

namespace bridger::fx::dxbc {
namespace {

struct Md5 {
    std::uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu, d = 0x10325476u;

    static constexpr std::uint32_t rotl(std::uint32_t x, unsigned s) { return (x << s) | (x >> (32u - s)); }

    void block(const std::uint8_t* p) {
        static constexpr std::uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static constexpr unsigned r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                           5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                           4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                           6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            std::memcpy(&m[i], p + i * 4, 4);
        }
        std::uint32_t aa = a, bb = b, cc = c, dd = d;
        for (unsigned i = 0; i < 64; ++i) {
            std::uint32_t f;
            unsigned g;
            if (i < 16) { f = (bb & cc) | (~bb & dd); g = i; }
            else if (i < 32) { f = (dd & bb) | (~dd & cc); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = bb ^ cc ^ dd; g = (3 * i + 5) & 15; }
            else { f = cc ^ (bb | ~dd); g = (7 * i) & 15; }
            const std::uint32_t t = dd;
            dd = cc;
            cc = bb;
            bb = bb + rotl(aa + f + k[i] + m[g], r[i]);
            aa = t;
        }
        a += aa; b += bb; c += cc; d += dd;
    }
};

constexpr std::uint32_t kOpcodeMask = 0x7ff;
constexpr unsigned kOpMov = 54;
constexpr unsigned kOpRet = 62;
constexpr unsigned kOpRetc = 63;
constexpr unsigned kOpCustomData = 53;
constexpr unsigned kOpDclResource = 88;
constexpr unsigned kOpDclConstantBuffer = 89;
constexpr unsigned kOpDclSampler = 90;
constexpr unsigned kOpDclInputPs = 98;
constexpr unsigned kOpDclInputPsSgv = 99;
constexpr unsigned kOpDclInputPsSiv = 100;
constexpr unsigned kOpDclOutput = 101;
constexpr unsigned kOpDclOutputSgv = 102;
constexpr unsigned kOpDclOutputSiv = 103;
constexpr unsigned kOpDclTemps = 104;
constexpr unsigned kOpDclIndexableTemp = 105;
constexpr unsigned kOpDclGlobalFlags = 106;
constexpr unsigned kOpDclFirst = 88;
constexpr unsigned kOpDclLast = 106;
constexpr unsigned kOpDclStream = 143;
constexpr unsigned kOpDclSm5Last = 162;

constexpr unsigned kOperandTemp = 0;
constexpr unsigned kOperandInput = 1;
constexpr unsigned kOperandOutput = 2;
constexpr unsigned kOperandIndexableTemp = 3;
constexpr unsigned kOperandImmediate32 = 4;
constexpr unsigned kOperandImmediate64 = 5;
constexpr unsigned kOperandSampler = 6;
constexpr unsigned kOperandResource = 7;
constexpr unsigned kOperandConstantBuffer = 8;

constexpr std::uint32_t kHookSpace = 60;

constexpr unsigned kNamePosition = 1;
constexpr unsigned kInterpolationLinearNoPerspective = 4;

constexpr unsigned opcode(std::uint32_t token) { return token & kOpcodeMask; }
constexpr unsigned encoded_length(std::uint32_t token) { return (token >> 24) & 0x7f; }
constexpr bool extended(std::uint32_t token) { return (token >> 31) != 0; }

bool is_declaration(unsigned op) {
    return (op >= kOpDclFirst && op <= kOpDclLast) || (op >= kOpDclStream && op <= kOpDclSm5Last)
        || op == kOpCustomData;
}

bool is_custom_data(std::uint32_t token) {
    return opcode(token) == kOpCustomData;
}

struct Operand {
    std::size_t token = 0;
    std::size_t end = 0;
    unsigned type = 0;
    unsigned dims = 0;
    bool simple = false;
    std::size_t index = 0;
    std::size_t first = 0;
};

bool parse_operand(const std::vector<std::uint32_t>& w, std::size_t pos, std::size_t end, Operand& out) {
    if (pos >= end) return false;
    out.token = pos;
    const std::uint32_t first = w[pos++];
    std::uint32_t ext = first;
    while (extended(ext)) {
        if (pos >= end) return false;
        ext = w[pos++];
    }
    out.type = (first >> 12) & 0xff;
    const unsigned components = first & 3;
    if (out.type == kOperandImmediate32) {
        pos += components == 1 ? 1 : components == 2 ? 4 : 0;
        out.dims = 0;
        out.end = pos;
        return pos <= end;
    }
    if (out.type == kOperandImmediate64) {
        pos += components == 1 ? 2 : components == 2 ? 8 : 0;
        out.dims = 0;
        out.end = pos;
        return pos <= end;
    }
    out.dims = (first >> 20) & 3;
    out.simple = false;
    out.first = 0;
    for (unsigned d = 0; d < out.dims; ++d) {
        const unsigned rep = (first >> (22 + 3 * d)) & 7;
        if (d == 0 && rep == 0) {
            out.first = pos;
            if (out.dims == 1) {
                out.simple = true;
                out.index = pos;
            }
        }
        switch (rep) {
            case 0: pos += 1; break;
            case 1: pos += 2; break;
            case 2: {
                Operand inner;
                if (!parse_operand(w, pos, end, inner)) return false;
                pos = inner.end;
                break;
            }
            case 3: {
                pos += 1;
                Operand inner;
                if (!parse_operand(w, pos, end, inner)) return false;
                pos = inner.end;
                break;
            }
            case 4: {
                pos += 2;
                Operand inner;
                if (!parse_operand(w, pos, end, inner)) return false;
                pos = inner.end;
                break;
            }
            default: return false;
        }
        if (pos > end) return false;
    }
    out.end = pos;
    return true;
}

struct Instruction {
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t body = 0;
    unsigned op = 0;
    bool custom = false;
};

bool next_instruction(const std::vector<std::uint32_t>& w, std::size_t pos, Instruction& out) {
    if (pos >= w.size()) return false;
    const std::uint32_t token = w[pos];
    out.start = pos;
    out.op = opcode(token);
    out.custom = is_custom_data(token);
    std::size_t length;
    if (out.custom) {
        if (pos + 1 >= w.size()) return false;
        length = w[pos + 1];
        out.body = pos + 2;
    } else {
        length = encoded_length(token);
        std::size_t body = pos + 1;
        std::uint32_t ext = token;
        while (extended(ext)) {
            if (body >= w.size()) return false;
            ext = w[body++];
        }
        out.body = body;
    }
    if (length == 0 || pos + length > w.size()) return false;
    out.end = pos + length;
    return true;
}

bool walk_operands(const std::vector<std::uint32_t>& w, const Instruction& in, std::vector<Operand>& out) {
    out.clear();
    std::size_t pos = in.body;
    while (pos < in.end) {
        Operand operand;
        if (!parse_operand(w, pos, in.end, operand)) return false;
        out.push_back(operand);
        pos = operand.end;
    }
    return pos == in.end;
}

struct Program {
    std::vector<std::uint32_t> words;
    Stage stage = Stage::Unknown;
    unsigned major = 0, minor = 0;

    bool load(const Part& part) {
        if (part.data.size() < 8 || part.data.size() % 4 != 0) return false;
        words.resize(part.data.size() / 4);
        std::memcpy(words.data(), part.data.data(), part.data.size());
        const std::uint32_t version = words[0];
        const unsigned type = version >> 16;
        stage = type <= 5 ? static_cast<Stage>(type) : Stage::Unknown;
        major = (version >> 4) & 0xf;
        minor = version & 0xf;
        return words[1] == words.size();
    }

    [[nodiscard]] Part store(std::uint32_t tag) const {
        Part part;
        part.tag = tag;
        part.data.resize(words.size() * 4);
        std::memcpy(part.data.data(), words.data(), part.data.size());
        std::uint32_t length = static_cast<std::uint32_t>(words.size());
        std::memcpy(part.data.data() + 4, &length, 4);
        return part;
    }
};

struct SignatureEntry {
    std::string name;
    std::uint32_t semantic_index = 0;
    std::uint32_t system_value = 0;
    std::uint32_t component_type = 0;
    std::uint32_t reg = 0;
    std::uint8_t mask = 0;
    std::uint8_t used = 0;
    std::uint32_t stream = 0;
    std::uint32_t min_precision = 0;
};

struct Signature {
    bool extended = false;
    bool streams = false;
    std::vector<SignatureEntry> entries;

    [[nodiscard]] std::size_t stride() const { return extended ? 32 : streams ? 28 : 24; }

    bool load(const Part& part) {
        extended = part.tag == kISG1 || part.tag == fourcc('O', 'S', 'G', '1') || part.tag == fourcc('P', 'S', 'G', '1');
        streams = part.tag == fourcc('O', 'S', 'G', '5');
        const auto& d = part.data;
        if (d.size() < 8) return false;
        std::uint32_t count, offset;
        std::memcpy(&count, d.data(), 4);
        std::memcpy(&offset, d.data() + 4, 4);
        const std::size_t stride = this->stride();
        if (offset + count * stride > d.size()) return false;
        entries.clear();
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* e = d.data() + offset + i * stride;
            SignatureEntry entry;
            std::uint32_t name_offset;
            std::size_t at = 0;
            if (extended || streams) { std::memcpy(&entry.stream, e, 4); at = 4; }
            std::memcpy(&name_offset, e + at, 4);
            std::memcpy(&entry.semantic_index, e + at + 4, 4);
            std::memcpy(&entry.system_value, e + at + 8, 4);
            std::memcpy(&entry.component_type, e + at + 12, 4);
            std::memcpy(&entry.reg, e + at + 16, 4);
            entry.mask = e[at + 20];
            entry.used = e[at + 21];
            if (extended) std::memcpy(&entry.min_precision, e + at + 24, 4);
            if (name_offset >= d.size()) return false;
            const char* s = reinterpret_cast<const char*>(d.data() + name_offset);
            const std::size_t max = d.size() - name_offset;
            entry.name.assign(s, strnlen(s, max));
            entries.push_back(std::move(entry));
        }
        return true;
    }

    [[nodiscard]] Part store(std::uint32_t tag) const {
        const std::size_t stride = this->stride();
        const std::size_t table = 8 + entries.size() * stride;
        std::vector<std::uint8_t> strings;
        std::vector<std::uint32_t> offsets;
        for (const auto& entry : entries) {
            std::uint32_t found = ~0u;
            for (std::size_t i = 0; i < offsets.size(); ++i) {
                if (entries[i].name == entry.name) { found = offsets[i]; break; }
            }
            if (found == ~0u) {
                found = static_cast<std::uint32_t>(table + strings.size());
                strings.insert(strings.end(), entry.name.begin(), entry.name.end());
                strings.push_back(0);
            }
            offsets.push_back(found);
        }
        while (strings.size() % 4 != 0) strings.push_back(0xab);
        Part part;
        part.tag = tag;
        part.data.resize(table + strings.size());
        auto* d = part.data.data();
        const std::uint32_t count = static_cast<std::uint32_t>(entries.size());
        const std::uint32_t offset = 8;
        std::memcpy(d, &count, 4);
        std::memcpy(d + 4, &offset, 4);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            std::uint8_t* e = d + 8 + i * stride;
            std::size_t at = 0;
            if (extended || streams) { std::memcpy(e, &entry.stream, 4); at = 4; }
            std::memcpy(e + at, &offsets[i], 4);
            std::memcpy(e + at + 4, &entry.semantic_index, 4);
            std::memcpy(e + at + 8, &entry.system_value, 4);
            std::memcpy(e + at + 12, &entry.component_type, 4);
            std::memcpy(e + at + 16, &entry.reg, 4);
            e[at + 20] = entry.mask;
            e[at + 21] = entry.used;
            e[at + 22] = 0;
            e[at + 23] = 0;
            if (extended) std::memcpy(e + at + 24, &entry.min_precision, 4);
        }
        std::memcpy(d + table, strings.data(), strings.size());
        return part;
    }
};

std::uint32_t input_signature_tag(const Container& c) {
    if (c.find(kISGN) != nullptr) return kISGN;
    if (c.find(kISG1) != nullptr) return kISG1;
    return 0;
}

std::uint32_t code_tag(const Container& c) {
    if (c.find(kSHEX) != nullptr) return kSHEX;
    if (c.find(kSHDR) != nullptr) return kSHDR;
    return 0;
}

std::uint32_t register_token(unsigned type, unsigned mask) {
    return 2u | ((mask & 0xf) << 4) | (type << 12) | (1u << 20);
}

}

bool is_container(const void* bytes, std::size_t size) {
    if (bytes == nullptr || size < 32) return false;
    const auto* p = static_cast<const std::uint8_t*>(bytes);
    std::uint32_t magic, total;
    std::memcpy(&magic, p, 4);
    std::memcpy(&total, p + 24, 4);
    return magic == kDXBC && total <= size && total >= 32;
}

bool is_dxil(const void* bytes, std::size_t size) {
    Container c;
    return c.parse(bytes, size) && c.find(kDXIL) != nullptr;
}

bool Container::parse(const void* bytes, std::size_t size) {
    parts.clear();
    if (!is_container(bytes, size)) return false;
    const auto* p = static_cast<const std::uint8_t*>(bytes);
    std::uint32_t total, count;
    std::memcpy(&version, p + 20, 4);
    std::memcpy(&total, p + 24, 4);
    std::memcpy(&count, p + 28, 4);
    if (32 + static_cast<std::size_t>(count) * 4 > total) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t offset;
        std::memcpy(&offset, p + 32 + i * 4, 4);
        if (offset + 8 > total) return false;
        Part part;
        std::uint32_t length;
        std::memcpy(&part.tag, p + offset, 4);
        std::memcpy(&length, p + offset + 4, 4);
        if (offset + 8 + static_cast<std::size_t>(length) > total) return false;
        part.data.assign(p + offset + 8, p + offset + 8 + length);
        parts.push_back(std::move(part));
    }
    return true;
}

std::vector<std::uint8_t> Container::build() const {
    std::vector<std::uint8_t> out;
    const std::size_t header = 32 + parts.size() * 4;
    std::size_t total = header;
    for (const auto& part : parts) total += 8 + part.data.size();
    out.resize(total, 0);
    std::memcpy(out.data(), &kDXBC, 4);
    std::memcpy(out.data() + 20, &version, 4);
    const std::uint32_t total32 = static_cast<std::uint32_t>(total);
    const std::uint32_t count = static_cast<std::uint32_t>(parts.size());
    std::memcpy(out.data() + 24, &total32, 4);
    std::memcpy(out.data() + 28, &count, 4);
    std::size_t at = header;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::uint32_t offset = static_cast<std::uint32_t>(at);
        std::memcpy(out.data() + 32 + i * 4, &offset, 4);
        const std::uint32_t length = static_cast<std::uint32_t>(parts[i].data.size());
        std::memcpy(out.data() + at, &parts[i].tag, 4);
        std::memcpy(out.data() + at + 4, &length, 4);
        std::memcpy(out.data() + at + 8, parts[i].data.data(), parts[i].data.size());
        at += 8 + parts[i].data.size();
    }
    std::uint8_t hash[16];
    checksum(out.data(), out.size(), hash);
    std::memcpy(out.data() + 4, hash, 16);
    return out;
}

Part* Container::find(std::uint32_t tag) {
    for (auto& part : parts) if (part.tag == tag) return &part;
    return nullptr;
}

const Part* Container::find(std::uint32_t tag) const {
    for (const auto& part : parts) if (part.tag == tag) return &part;
    return nullptr;
}

void checksum(const void* bytes, std::size_t size, std::uint8_t out[16]) {
    Md5 md5;
    const auto* data = static_cast<const std::uint8_t*>(bytes) + 20;
    const std::size_t length = size > 20 ? size - 20 : 0;
    const std::uint32_t bits = static_cast<std::uint32_t>(length * 8);
    const std::uint32_t bits_again = (bits >> 2) | 1u;
    const std::size_t remainder = length % 64;
    const std::size_t whole = length - remainder;
    for (std::size_t at = 0; at < whole; at += 64) md5.block(data + at);
    std::uint8_t tail[128]{};
    if (remainder >= 56) {
        std::memcpy(tail, data + whole, remainder);
        tail[remainder] = 0x80;
        md5.block(tail);
        std::uint8_t last[64]{};
        std::memcpy(last, &bits, 4);
        std::memcpy(last + 60, &bits_again, 4);
        md5.block(last);
    } else {
        std::memcpy(tail, &bits, 4);
        std::memcpy(tail + 4, data + whole, remainder);
        tail[4 + remainder] = 0x80;
        std::memcpy(tail + 60, &bits_again, 4);
        md5.block(tail);
    }
    std::memcpy(out, &md5.a, 4);
    std::memcpy(out + 4, &md5.b, 4);
    std::memcpy(out + 8, &md5.c, 4);
    std::memcpy(out + 12, &md5.d, 4);
}

bool verify(const void* bytes, std::size_t size) {
    if (!is_container(bytes, size)) return false;
    std::uint32_t total;
    std::memcpy(&total, static_cast<const std::uint8_t*>(bytes) + 24, 4);
    std::uint8_t hash[16];
    checksum(bytes, total, hash);
    return std::memcmp(hash, static_cast<const std::uint8_t*>(bytes) + 4, 16) == 0;
}

std::uint64_t identity(const void* bytes, std::size_t size) {
    if (!is_container(bytes, size)) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        const auto* p = static_cast<const std::uint8_t*>(bytes);
        for (std::size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
        return h;
    }
    std::uint64_t lo, hi;
    std::memcpy(&lo, static_cast<const std::uint8_t*>(bytes) + 4, 8);
    std::memcpy(&hi, static_cast<const std::uint8_t*>(bytes) + 12, 8);
    return lo ^ (hi * 0x9e3779b97f4a7c15ull);
}

Summary summarise(const void* bytes, std::size_t size) {
    Summary s;
    Container c;
    if (!c.parse(bytes, size)) { s.problem = "not a DXBC container"; return s; }
    if (c.find(kDXIL) != nullptr) { s.problem = "DXIL, not shader model 5"; return s; }
    const auto tag = code_tag(c);
    if (tag == 0) { s.problem = "no instruction stream"; return s; }
    Program program;
    if (!program.load(*c.find(tag))) { s.problem = "instruction stream length mismatch"; return s; }
    s.stage = program.stage;
    s.major = program.major;
    s.minor = program.minor;
    std::size_t pos = 2;
    Instruction in;
    std::vector<Operand> operands;
    while (pos < program.words.size()) {
        if (!next_instruction(program.words, pos, in)) { s.problem = std::format("bad instruction at dword {}", pos); return s; }
        if (in.custom) { pos = in.end; continue; }
        if (is_declaration(in.op)) {
            if (in.op == kOpDclTemps) {
                s.temps = program.words[in.body];
            } else if (in.op == kOpDclOutput) {
                Operand o;
                if (parse_operand(program.words, in.body, in.end, o) && o.type == kOperandOutput && o.simple) {
                    ++s.outputs;
                    if (program.words[o.index] == 0) s.writes_target0 = true;
                }
            } else if (in.op == kOpDclInputPsSiv) {
                Operand o;
                if (parse_operand(program.words, in.body, in.end, o) && o.end < in.end
                    && program.words[o.end] == kNamePosition) {
                    s.has_position = true;
                }
            }
        } else {
            ++s.instructions;
            if (!walk_operands(program.words, in, operands)) {
                s.problem = std::format("unrecognised operand encoding in opcode {} at dword {}", in.op, pos);
                return s;
            }
        }
        pos = in.end;
    }
    return s;
}

std::uint32_t output_register(const void* bytes, std::size_t size, std::uint32_t system_value) {
    Container c;
    if (!c.parse(bytes, size)) return ~0u;
    for (const auto tag : {kOSGN, fourcc('O', 'S', 'G', '1'), fourcc('O', 'S', 'G', '5')}) {
        const auto* part = c.find(tag);
        if (part == nullptr) continue;
        Signature outputs;
        if (!outputs.load(*part)) return ~0u;
        for (const auto& e : outputs.entries) if (e.system_value == system_value) return e.reg;
        return ~0u;
    }
    return ~0u;
}

SpliceResult splice_pixel_shader(const void* host_bytes, std::size_t host_size, const void* hook_bytes,
                                 std::size_t hook_size, const void* upstream, std::size_t upstream_size) {
    SpliceResult result;
    auto fail = [&](std::string message) { result.error = std::move(message); result.bytecode.clear(); return result; };

    Container host;
    if (!host.parse(host_bytes, host_size)) return fail("host is not a DXBC container");
    Container hook;
    if (!hook.parse(hook_bytes, hook_size)) return fail("hook is not a DXBC container");
    const auto host_tag = code_tag(host);
    const auto hook_tag = code_tag(hook);
    if (host_tag == 0 || hook_tag == 0) return fail("missing instruction stream");
    Program hp, kp;
    if (!hp.load(*host.find(host_tag))) return fail("host instruction stream is malformed");
    if (!kp.load(*hook.find(hook_tag))) return fail("hook instruction stream is malformed");
    if (hp.stage != Stage::Pixel) return fail("host is not a pixel shader");
    if (kp.stage != Stage::Pixel) return fail("hook is not a pixel shader");
    if (hp.major != 5) return fail(std::format("host is shader model {}.{}, only 5 is supported", hp.major, hp.minor));
    if (kp.major != 5 || kp.minor != hp.minor) {
        return fail(std::format("hook is ps_{}_{} but the host is ps_{}_{}; compile the hook with the host's profile",
                                kp.major, kp.minor, hp.major, hp.minor));
    }

    const auto hook_isgn_tag = input_signature_tag(hook);
    Signature hook_inputs;
    if (hook_isgn_tag == 0 || !hook_inputs.load(*hook.find(hook_isgn_tag))) return fail("hook has no input signature");
    constexpr unsigned kTargets = 8;
    std::optional<std::uint32_t> hook_position;
    std::optional<unsigned> color_of_input[32];
    bool touched[kTargets] = {};
    for (const auto& e : hook_inputs.entries) {
        if (e.system_value == kNamePosition) { hook_position = e.reg; continue; }
        if (_stricmp(e.name.c_str(), "COLOR") == 0 && e.semantic_index < kTargets && e.reg < 32) {
            color_of_input[e.reg] = e.semantic_index;
            touched[e.semantic_index] = true;
            continue;
        }
        return fail(std::format("hook input {}{} is not supported; use COLOR0..COLOR7 and SV_Position", e.name, e.semantic_index));
    }

    auto& hw = hp.words;
    std::size_t pos = 2;
    Instruction in;
    std::vector<Operand> operands;
    unsigned host_temps = 0;
    std::size_t temps_at = 0;
    std::size_t first_instruction = 0;
    bool declared[kTargets] = {};
    unsigned host_mask[kTargets] = {};
    std::optional<std::uint32_t> host_position;
    std::size_t position_decl_mask_at = 0;
    std::uint32_t max_input = 0;
    bool any_input = false;
    std::uint32_t next_id[3] = {0, 0, 0};
    auto binding_kind = [](unsigned type) -> int {
        return type == kOperandSampler ? 0 : type == kOperandResource ? 1 : type == kOperandConstantBuffer ? 2 : -1;
    };
    while (pos < hw.size()) {
        if (!next_instruction(hw, pos, in)) return fail(std::format("host: bad instruction at dword {}", pos));
        if (in.custom || is_declaration(in.op)) {
            if (in.op == kOpDclTemps) {
                host_temps = hw[in.body];
                temps_at = in.body;
            } else if (in.op == kOpDclOutput) {
                Operand o;
                if (parse_operand(hw, in.body, in.end, o) && o.type == kOperandOutput && o.simple && hw[o.index] < kTargets) {
                    declared[hw[o.index]] = true;
                    host_mask[hw[o.index]] = (hw[o.token] >> 4) & 0xf;
                }
            } else if (in.op == kOpDclResource || in.op == kOpDclConstantBuffer || in.op == kOpDclSampler
                       || (in.op >= kOpDclStream && in.op <= kOpDclSm5Last)) {
                Operand o;
                if (!in.custom && parse_operand(hw, in.body, in.end, o)) {
                    const int kind = binding_kind(o.type);
                    if (kind >= 0 && o.first != 0) next_id[kind] = std::max(next_id[kind], hw[o.first] + 1);
                }
            } else if (in.op == kOpDclInputPs || in.op == kOpDclInputPsSiv || in.op == kOpDclInputPsSgv) {
                Operand o;
                if (parse_operand(hw, in.body, in.end, o) && o.type == kOperandInput && o.simple) {
                    any_input = true;
                    max_input = std::max(max_input, hw[o.index]);
                    if (in.op == kOpDclInputPsSiv && o.end < in.end && hw[o.end] == kNamePosition) {
                        host_position = hw[o.index];
                        position_decl_mask_at = o.token;
                    }
                }
            }
            pos = in.end;
            first_instruction = pos;
            continue;
        }
        break;
    }
    if (first_instruction == 0) return fail("host has no instructions");

    auto& kw = kp.words;
    std::size_t hook_body_start = 0;
    unsigned hook_temps = 0;
    std::vector<std::uint32_t> hook_bindings;
    for (pos = 2; pos < kw.size(); pos = in.end) {
        if (!next_instruction(kw, pos, in)) return fail(std::format("hook: bad instruction at dword {}", pos));
        if (in.custom) return fail("hook uses an immediate constant buffer, which the host cannot carry; avoid static arrays");
        if (!is_declaration(in.op)) break;
        switch (in.op) {
            case kOpDclTemps: hook_temps = kw[in.body]; break;
            case kOpDclGlobalFlags: break;
            case kOpDclInputPs: case kOpDclInputPsSiv: break;
            case kOpDclOutput: {
                Operand o;
                if (!parse_operand(kw, in.body, in.end, o) || o.type != kOperandOutput || !o.simple || kw[o.index] >= kTargets) {
                    return fail("hook must write SV_Target0..7 only");
                }
                touched[kw[o.index]] = true;
                break;
            }
            case kOpDclConstantBuffer: case kOpDclResource: case kOpDclSampler: {
                if (kp.minor < 1) {
                    return fail("hook binds a constant buffer, texture or sampler; that needs shader model 5.1 "
                                "(the root signature slots live in register space 60)");
                }
                Operand o;
                if (!parse_operand(kw, in.body, in.end, o) || binding_kind(o.type) < 0 || o.first == 0 || o.dims != 3) {
                    return fail("hook: binding declaration not understood");
                }
                if (kw[in.end - 1] != kHookSpace) {
                    return fail(std::format("hook binds register space {}; hook bindings must use space{}",
                                            kw[in.end - 1], kHookSpace));
                }
                std::vector<std::uint32_t> copy(kw.begin() + in.start, kw.begin() + in.end);
                copy[o.first - in.start] += next_id[binding_kind(o.type)];
                hook_bindings.insert(hook_bindings.end(), copy.begin(), copy.end());
                break;
            }
            case kOpDclIndexableTemp: return fail("hook uses indexable temporaries");
            default: return fail(std::format("hook uses declaration opcode {}", in.op));
        }
        hook_body_start = in.end;
    }
    if (hook_body_start == 0) return fail("hook has no declarations");
    unsigned touched_count = 0;
    std::uint32_t temp_for[kTargets] = {};
    for (unsigned k = 0; k < kTargets; ++k) {
        if (!touched[k]) continue;
        if (!declared[k]) return fail(std::format("hook uses render target {} which the host does not write", k));
        temp_for[k] = host_temps + touched_count++;
    }
    if (touched_count == 0) return fail("hook touches no render target");
    const std::uint32_t hook_temp_base = host_temps + touched_count;

    std::vector<std::size_t> returns;
    std::vector<std::size_t> target_tokens;
    for (pos = first_instruction; pos < hw.size(); pos = in.end) {
        if (!next_instruction(hw, pos, in)) return fail(std::format("host: bad instruction at dword {}", pos));
        if (in.custom) return fail("host has custom data after its declarations");
        if (is_declaration(in.op)) return fail("host declares after its instructions");
        if (!walk_operands(hw, in, operands)) {
            return fail(std::format("host: unrecognised operand encoding in opcode {} at dword {}", in.op, pos));
        }
        if (in.op == kOpRet || in.op == kOpRetc) returns.push_back(pos);
        for (const auto& o : operands) {
            if (o.type == kOperandOutput) {
                if (!o.simple) return fail("host indexes its outputs dynamically");
                if (hw[o.index] < kTargets && touched[hw[o.index]]) target_tokens.push_back(o.token);
            }
        }
    }
    if (returns.empty()) return fail("host never returns");

    std::optional<std::uint32_t> position_register = host_position;
    bool add_position = false;
    std::optional<std::uint32_t> signature_position;
    if (hook_position && !host_position) {
        const auto tag = input_signature_tag(host);
        Signature inputs;
        if (tag != 0 && inputs.load(*host.find(tag))) {
            for (const auto& e : inputs.entries) {
                if (e.system_value == kNamePosition) signature_position = e.reg;
            }
        }
    }
    if (hook_position && !host_position) {
        add_position = true;
        const auto linked = upstream != nullptr ? output_register(upstream, upstream_size, kNamePosition) : ~0u;
        if (signature_position) {
            position_register = signature_position;
        } else if (linked != ~0u) {
            position_register = linked;
        } else {
            position_register = any_input ? max_input + 1 : 0;
        }
        if (*position_register >= 32) return fail("host has no free input register for SV_Position");
    }

    std::vector<std::uint32_t> body;
    for (unsigned k = 0; k < kTargets; ++k) {
        if (!touched[k]) continue;
        body.push_back(kOpMov | (5u << 24));
        body.push_back(register_token(kOperandOutput, host_mask[k]));
        body.push_back(k);
        body.push_back(2u | (1u << 2) | (0xe4u << 4) | (kOperandTemp << 12) | (1u << 20));
        body.push_back(temp_for[k]);
    }
    bool saw_final_ret = false;
    for (pos = hook_body_start; pos < kw.size(); pos = in.end) {
        if (!next_instruction(kw, pos, in)) return fail(std::format("hook: bad instruction at dword {}", pos));
        if (in.custom || is_declaration(in.op)) return fail("hook declares after its instructions");
        if (in.op == kOpRet) {
            if (in.end != kw.size()) return fail("hook returns early; write it with a single return");
            saw_final_ret = true;
            break;
        }
        if (in.op == kOpRetc) return fail("hook returns conditionally; write it with a single return");
        if (!walk_operands(kw, in, operands)) {
            return fail(std::format("hook: unrecognised operand encoding in opcode {} at dword {}", in.op, pos));
        }
        std::vector<std::uint32_t> copy(kw.begin() + in.start, kw.begin() + in.end);
        for (const auto& o : operands) {
            const std::size_t token = o.token - in.start;
            const std::size_t index = o.index - in.start;
            switch (o.type) {
                case kOperandTemp:
                    if (!o.simple) return fail("hook indexes temporaries dynamically");
                    copy[index] += hook_temp_base;
                    break;
                case kOperandInput: {
                    if (!o.simple) return fail("hook indexes inputs dynamically");
                    const auto reg = copy[index];
                    if (reg < 32 && color_of_input[reg]) {
                        copy[token] = (copy[token] & ~(0xffu << 12)) | (kOperandTemp << 12);
                        copy[index] = temp_for[*color_of_input[reg]];
                    } else if (hook_position && reg == *hook_position) {
                        copy[index] = *position_register;
                    } else {
                        return fail(std::format("hook reads input v{} which is not COLORk or SV_Position", reg));
                    }
                    break;
                }
                case kOperandOutput: {
                    if (!o.simple || copy[index] >= kTargets || !touched[copy[index]]) return fail("hook writes an undeclared render target");
                    const unsigned mask = (copy[token] >> 4) & 0xf & host_mask[copy[index]];
                    copy[token] = (copy[token] & ~(0xfu << 4)) | (mask << 4);
                    break;
                }
                case kOperandImmediate32: case kOperandImmediate64: break;
                case kOperandSampler: case kOperandResource: case kOperandConstantBuffer:
                    if (o.first == 0) return fail("hook indexes a binding dynamically");
                    copy[o.first - in.start] += next_id[binding_kind(o.type)];
                    break;
                default: return fail(std::format("hook uses operand type {} which has no binding in the host", o.type));
            }
        }
        body.insert(body.end(), copy.begin(), copy.end());
    }
    if (!saw_final_ret) return fail("hook does not end with a return");

    std::vector<std::uint32_t> out;
    out.reserve(hw.size() + body.size() * returns.size() + 8);
    out.push_back(hw[0]);
    out.push_back(0);
    for (pos = 2; pos < first_instruction; pos = in.end) {
        next_instruction(hw, pos, in);
        if (in.op == kOpDclTemps) {
            out.push_back(hw[in.start]);
            out.push_back(host_temps + touched_count + hook_temps);
            continue;
        }
        if (position_decl_mask_at != 0 && in.start < position_decl_mask_at && position_decl_mask_at < in.end) {
            std::vector<std::uint32_t> copy(hw.begin() + in.start, hw.begin() + in.end);
            copy[position_decl_mask_at - in.start] |= 0xfu << 4;
            out.insert(out.end(), copy.begin(), copy.end());
            continue;
        }
        out.insert(out.end(), hw.begin() + in.start, hw.begin() + in.end);
    }
    if (temps_at == 0) {
        out.push_back(kOpDclTemps | (2u << 24));
        out.push_back(host_temps + touched_count + hook_temps);
    }
    out.insert(out.end(), hook_bindings.begin(), hook_bindings.end());
    if (add_position) {
        out.push_back(kOpDclInputPsSiv | (kInterpolationLinearNoPerspective << 11) | (4u << 24));
        out.push_back(register_token(kOperandInput, 0xf));
        out.push_back(*position_register);
        out.push_back(kNamePosition);
    }
    std::size_t next_token = 0;
    std::size_t next_return = 0;
    for (pos = first_instruction; pos < hw.size(); pos = in.end) {
        next_instruction(hw, pos, in);
        if (next_return < returns.size() && returns[next_return] == in.start) {
            ++next_return;
            out.insert(out.end(), body.begin(), body.end());
            ++result.insertions;
        }
        std::vector<std::uint32_t> copy(hw.begin() + in.start, hw.begin() + in.end);
        while (next_token < target_tokens.size() && target_tokens[next_token] < in.end) {
            const std::size_t token = target_tokens[next_token] - in.start;
            Operand o;
            parse_operand(copy, token, copy.size(), o);
            const auto target = copy[o.index];
            copy[token] = (copy[token] & ~(0xffu << 12)) | (kOperandTemp << 12);
            copy[o.index] = temp_for[target];
            ++next_token;
        }
        out.insert(out.end(), copy.begin(), copy.end());
    }
    out[1] = static_cast<std::uint32_t>(out.size());

    Container product = host;
    Program np;
    np.words = std::move(out);
    *product.find(host_tag) = np.store(host_tag);
    if (add_position) {
        const auto tag = input_signature_tag(product);
        Signature inputs;
        if (tag == 0 || !inputs.load(*product.find(tag))) return fail("host has no input signature to extend");
        if (signature_position) {
            for (auto& e : inputs.entries) if (e.system_value == kNamePosition) { e.used = 0xf; e.mask = 0xf; }
            *product.find(tag) = inputs.store(tag);
            result.bindings = !hook_bindings.empty();
            result.bytecode = product.build();
            return result;
        }
        SignatureEntry entry;
        entry.name = "SV_Position";
        entry.system_value = kNamePosition;
        entry.component_type = 3;
        entry.reg = *position_register;
        entry.mask = 0xf;
        entry.used = 0xf;
        inputs.entries.push_back(entry);
        *product.find(tag) = inputs.store(tag);
    } else if (hook_position && host_position) {
        const auto tag = input_signature_tag(product);
        Signature inputs;
        if (tag != 0 && inputs.load(*product.find(tag))) {
            for (auto& e : inputs.entries) if (e.system_value == kNamePosition) { e.used = 0xf; e.mask = 0xf; }
            *product.find(tag) = inputs.store(tag);
        }
    }
    result.bindings = !hook_bindings.empty();
    result.bytecode = product.build();
    return result;
}

}
