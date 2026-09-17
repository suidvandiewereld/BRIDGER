#include "decima/decima.h"

#include <cstdint>

namespace {

using namespace decima;

static_assert(sizeof(EntityComponent) == 80);
static_assert(offsetof(EntityComponent, Resource) == 48);
static_assert(offsetof(EntityComponent, Entity) == 72);
static_assert(offsetof(EntityComponent, base_RTTIRefObject) == 0);
static_assert(offsetof(EntityComponent, base_WeakPtrRTTITarget) == 32);

static_assert(sizeof(RTTIRefObject) == 32);
static_assert(offsetof(RTTIRefObject, ObjectUUID) == 8);
static_assert(sizeof(GGUUID) == 16);

using SetFaction = symbols::AIBehaviorGroupSymbols_AIBehaviorGroup_ExportedSetFaction_t;
static_assert(symbols::AIBehaviorGroupSymbols_AIBehaviorGroup_ExportedSetFaction_rva != 0);
static_assert(properties::AIAtmosphereBox_VisibilityDistance_getter != 0);

using VisibilityDistance = properties::AIAtmosphereBox_VisibilityDistance_type;
static_assert(sizeof(VisibilityDistance) == 4);

bool exercise(AIBehaviorGroup* group, AIFaction* faction) {
    EntityComponent component{};
    if (component.Resource.pointer != nullptr) {
        return false;
    }
    const auto set_faction = bind<SetFaction>(
        symbols::AIBehaviorGroupSymbols_AIBehaviorGroup_ExportedSetFaction_rva);
    if (image_base != 0 && group != nullptr) {
        set_faction(group, faction);
    }
    return true;
}

}

extern "C" bool bridger_headers_compile_check(void* group, void* faction) {
    return exercise(static_cast<AIBehaviorGroup*>(group), static_cast<AIFaction*>(faction));
}
