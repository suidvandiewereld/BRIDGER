#include <cstddef>
#include <string>

#include "script/vm.h"

namespace bridger::script {

extern const unsigned char kPrelude[];
extern const std::size_t kPreludeSize;

bool run_prelude(lua_State* L, std::string& error) {
    if (luaL_loadbufferx(L, reinterpret_cast<const char*>(kPrelude), kPreludeSize, "=prelude", "t")
        != LUA_OK) {
        error = std::string("prelude: ") + lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        error = std::string("prelude: ") + lua_tostring(L, -1);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

}
