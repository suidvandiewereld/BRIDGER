#pragma once

#include <cstddef>
#include <cstdint>

namespace bridger::decima {

enum class Kind : std::uint8_t {
    Atom = 0,
    Pointer = 1,
    Container = 2,
    Enum = 3,
    Class = 4,
    EnumFlags = 5,
};

inline constexpr std::uint32_t kUnregistered = 0xFFFFFFFF;

struct RTTI {
    std::uint32_t id;
    Kind kind;
    std::uint8_t reserved[3];
};

struct RTTIBaseEntry {
    const struct RTTIClass* type;
    std::uint32_t offset;
    std::uint32_t unknown_0c;
};

struct RTTIMemberEntry {
    const RTTI* type;
    std::uint16_t offset;
    std::uint16_t flags;
    std::uint32_t unknown_0c;
    const char* name;
    const void* getter;
    const void* setter;
    std::uint8_t unknown_28[16];
};

struct RTTIClass {
    std::uint32_t id;
    Kind kind;
    std::uint8_t base_count;
    std::uint8_t member_count;
    std::uint8_t unknown_07;
    std::uint8_t message_handler_count;
    std::uint8_t unknown_09;
    std::uint8_t unknown_0a;
    std::uint8_t runtime_flag;
    std::uint32_t unknown_0c;
    std::uint32_t size;
    std::uint16_t alignment;
    std::uint16_t flags;
    void* constructor;
    void* destructor;
    void* unknown_28;
    void* unknown_30;
    const char* name;
    std::uint32_t index;
    std::uint32_t unknown_44;
    void* unknown_48;
    void* unknown_50;
    const RTTIBaseEntry* bases;
    const RTTIMemberEntry* members;
};

struct RTTIEnumValue {
    std::int32_t value;
    std::uint32_t unknown_04;
    const char* name;
    std::uint8_t unknown_10[24];
};

struct RTTIEnum {
    std::uint32_t id;
    Kind kind;
    std::uint8_t size;
    std::uint8_t value_count;
    std::uint8_t unknown_07;
    std::uint32_t size_again;
    std::uint32_t unknown_0c;
    const char* name;
    const RTTIEnumValue* values;
};

struct RTTIPointerData {
    const char* name;
    std::uint32_t size;
    std::uint32_t alignment;
    void* constructor;
    void* destructor;
    void* unknown_20;
    void* unknown_28;
};

struct RTTIPointer {
    std::uint32_t id;
    Kind kind;
    std::uint8_t reserved[3];
    const RTTI* item_type;
    const RTTIPointerData* data;
};

struct RTTIAtom {
    std::uint32_t id;
    Kind kind;
    std::uint8_t reserved[3];
    std::uint64_t unknown_08;
    const char* name;
};

static_assert(sizeof(RTTIBaseEntry) == 16);
static_assert(sizeof(RTTIMemberEntry) == 56);
static_assert(sizeof(RTTIEnumValue) == 40);
static_assert(offsetof(RTTIClass, size) == 16);
static_assert(offsetof(RTTIClass, alignment) == 20);
static_assert(offsetof(RTTIClass, constructor) == 24);
static_assert(offsetof(RTTIClass, destructor) == 32);
static_assert(offsetof(RTTIClass, name) == 56);
static_assert(offsetof(RTTIClass, bases) == 88);
static_assert(offsetof(RTTIClass, members) == 96);
static_assert(offsetof(RTTIEnum, name) == 16);
static_assert(offsetof(RTTIEnum, values) == 24);
static_assert(offsetof(RTTIPointer, item_type) == 8);
static_assert(offsetof(RTTIPointer, data) == 16);
static_assert(offsetof(RTTIAtom, name) == 16);

}
