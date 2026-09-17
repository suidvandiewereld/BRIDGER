
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "lauxlib.h"
#include "lua.h"

int main(int argc, char** argv) {
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream stream(argv[i], std::ios::binary);
        if (!stream.is_open()) {
            std::printf("%s: cannot open\n", argv[i]);
            ++failures;
            continue;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const auto source = buffer.str();
        lua_State* L = luaL_newstate();
        const std::string chunk = std::string("@") + argv[i];
        if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) {
            std::printf("%s\n", lua_tostring(L, -1));
            ++failures;
        } else {
            std::printf("ok %s\n", argv[i]);
        }
        lua_close(L);
    }
    return failures == 0 ? 0 : 1;
}
