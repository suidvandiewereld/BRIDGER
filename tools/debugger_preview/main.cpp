
#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

#include "debugger/debugger.h"
#include "decima/dumper.h"
#include "ui/font.h"

namespace {

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}

int main(int argc, char** argv) {
    const std::string prefix = argc > 1 ? argv[1] : "debugger";
    const std::string script = argc > 2 ? argv[2] : "shot main";
    namespace ui = bridger::ui;

    ui::font::register_file(BRIDGER_SOURCE_DIR_W L"/assets/Bridges-Black.ttf");
    ui::font::build(L"Segoe UI");

    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    bridger::decima::load_static_index(BRIDGER_SOURCE_DIR "/data/rtti/ds/index.json", image);
    bridger::decima::load_static_symbols(BRIDGER_SOURCE_DIR "/data/rtti/ds/symbol_index.json", image);

    bridger::mem::Module game;
    game.base = image;
    game.size = 0x1000;
    bridger::debugger::start(".", game);

    HWND hwnd = nullptr;
    for (int i = 0; i < 100 && hwnd == nullptr; ++i) {
        sleep_ms(50);
        hwnd = FindWindowW(L"BridgerDebugger", nullptr);
    }
    if (hwnd == nullptr) {
        std::printf("the debugger window never appeared\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, nullptr, 0, 0, 1500, 900, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    sleep_ms(600);

    std::stringstream steps(script);
    std::string step;
    while (std::getline(steps, step, ';')) {
        std::stringstream words(step);
        std::string verb;
        words >> verb;
        int x = 0, y = 0;
        if (verb == "click" || verb == "double") {
            words >> x >> y;
            const LPARAM at = MAKELPARAM(x, y);
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, at);
            sleep_ms(80);
            PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
            sleep_ms(60);
            PostMessageW(hwnd, WM_LBUTTONUP, 0, at);
            if (verb == "double") {
                sleep_ms(60);
                PostMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, at);
                sleep_ms(60);
                PostMessageW(hwnd, WM_LBUTTONUP, 0, at);
            }
            sleep_ms(200);
        } else if (verb == "type") {
            std::string text;
            std::getline(words, text);
            for (const char c : text.substr(text.empty() ? 0 : 1)) {
                PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(c), 0);
                sleep_ms(25);
            }
            sleep_ms(250);
        } else if (verb == "key") {
            int vk = 0;
            words >> vk;
            PostMessageW(hwnd, WM_KEYDOWN, vk, 0);
            sleep_ms(40);
            PostMessageW(hwnd, WM_KEYUP, vk, 0);
            sleep_ms(200);
        } else if (verb == "wheel") {
            int notches = 0;
            words >> x >> y >> notches;
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            sleep_ms(50);
            PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<short>(-notches * WHEEL_DELTA)), 0);
            sleep_ms(250);
        } else if (verb == "wait") {
            int ms = 0;
            words >> ms;
            sleep_ms(ms);
        } else if (verb == "shot") {
            std::string name;
            words >> name;
            const auto path = std::filesystem::path(prefix + "_" + name + ".bmp");
            std::filesystem::remove(path);
            bridger::debugger::capture(path);
            for (int i = 0; i < 60 && !std::filesystem::exists(path); ++i) {
                sleep_ms(50);
            }
            sleep_ms(150);
            std::printf("%s %s\n", std::filesystem::exists(path) ? "wrote" : "missing",
                        path.string().c_str());
        }
    }

    bridger::debugger::stop();
    return 0;
}
