#pragma once

#include <string>

namespace bridger::licence {

// Whether the process is a copy of Death Stranding the Steam account is licensed for.
// LICENSE section 4 ties every right Bridger grants to that, and this is the check behind it.
struct Verdict {
    bool ok = false;
    std::string reason;   // why it failed, for the log
    std::string detail;   // how it passed, for the log
};

// Asks the Steam client through the game's own steam_api64.dll, after checking that library
// carries Valve's signature. Runs under a fault guard; a crash inside the Steam API is a
// failed verdict. Call once the game has initialised Steam, which it has by the time the
// engine has registered its types.
Verdict verify();

}
