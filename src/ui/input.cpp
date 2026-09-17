#include "ui/input.h"

#include <Windows.h>

#include <cstring>
#include <mutex>

namespace bridger::ui::input {

struct Context {
    std::mutex mutex;
    State pending;
    State frame;
    Vec2 previous_mouse;
    bool was_down = false;
    bool queued_down = false;
    bool queued_up = false;
    bool queued_double = false;
    bool queued_right = false;
    std::uint64_t last_click_ms = 0;
    Vec2 last_click_at;
};

namespace {

Context g_default_context;
thread_local Context* t_context = &g_default_context;

constexpr std::uint64_t kDoubleClickMs = 400;
constexpr float kDoubleClickSlop = 6.0f;


bool is_modifier(std::uint32_t key) {
    switch (key) {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

}

void on_mouse_move(float x, float y) {
    std::scoped_lock lock(t_context->mutex);
    t_context->pending.mouse = {x, y};
}

void on_mouse_button(bool down) {
    std::scoped_lock lock(t_context->mutex);
    if (down) {
        t_context->queued_down = true;
        const std::uint64_t now = GetTickCount64();
        const float dx = t_context->pending.mouse.x - t_context->last_click_at.x;
        const float dy = t_context->pending.mouse.y - t_context->last_click_at.y;
        if (now - t_context->last_click_ms <= kDoubleClickMs && dx * dx + dy * dy
                <= kDoubleClickSlop * kDoubleClickSlop) {
            t_context->queued_double = true;
            t_context->last_click_ms = 0;
        } else {
            t_context->last_click_ms = now;
        }
        t_context->last_click_at = t_context->pending.mouse;
    } else {
        t_context->queued_up = true;
    }
    t_context->pending.down = down;
}

void on_right_button(bool down) {
    if (!down) {
        return;
    }
    std::scoped_lock lock(t_context->mutex);
    t_context->queued_right = true;
}

void on_wheel(float delta) {
    std::scoped_lock lock(t_context->mutex);
    t_context->pending.wheel += delta;
}

void on_character(char value) {
    std::scoped_lock lock(t_context->mutex);
    if (value >= 32 && value < 127) {
        t_context->pending.characters.push_back(value);
    }
}

void on_key(std::uint32_t key, bool down) {
    if (key >= 256) {
        return;
    }
    std::scoped_lock lock(t_context->mutex);
    if (down) {
        t_context->pending.key_pressed[key] = true;
        t_context->pending.key_held[key] = true;
        if (t_context->pending.captured == 0 && !is_modifier(key)) {
            t_context->pending.captured = key;
        }
    } else {
        t_context->pending.key_held[key] = false;
    }
}

void begin_frame() {
    std::scoped_lock lock(t_context->mutex);
    t_context->frame = t_context->pending;
    t_context->frame.mouse_delta = {t_context->pending.mouse.x - t_context->previous_mouse.x,
                           t_context->pending.mouse.y - t_context->previous_mouse.y};
    t_context->previous_mouse = t_context->pending.mouse;

    t_context->frame.pressed = t_context->queued_down;
    t_context->frame.released = t_context->queued_up;
    t_context->frame.double_clicked = t_context->queued_double;
    t_context->frame.right_pressed = t_context->queued_right;
    if (t_context->queued_down) {
        t_context->was_down = true;
    }
    if (t_context->queued_up) {
        t_context->was_down = false;
    }
    t_context->frame.down = t_context->was_down || t_context->queued_down;

    t_context->frame.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    t_context->frame.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    t_context->frame.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

    t_context->queued_down = false;
    t_context->queued_up = false;
    t_context->queued_double = false;
    t_context->queued_right = false;
    t_context->pending.wheel = 0.0f;
    t_context->pending.characters.clear();
    t_context->pending.key_pressed.fill(false);
    t_context->pending.captured = 0;
}

void end_frame() {
}

Context* create_context() {
    return new Context();
}

void destroy_context(Context* context) {
    if (context != &g_default_context) {
        delete context;
    }
}

void set_context(Context* context) {
    t_context = context != nullptr ? context : &g_default_context;
}

const State& state() {
    return t_context->frame;
}

std::string clipboard() {
    if (OpenClipboard(nullptr) == 0) {
        return {};
    }
    std::string out;
    if (const HANDLE handle = GetClipboardData(CF_TEXT); handle != nullptr) {
        if (const auto* text = static_cast<const char*>(GlobalLock(handle)); text != nullptr) {
            for (const char* cursor = text; *cursor != 0; ++cursor) {
                if (*cursor >= 32 && *cursor < 127) {
                    out.push_back(*cursor);
                }
            }
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return out;
}

void set_clipboard(std::string_view value) {
    if (OpenClipboard(nullptr) == 0) {
        return;
    }
    EmptyClipboard();
    const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, value.size() + 1);
    if (handle != nullptr) {
        if (auto* text = static_cast<char*>(GlobalLock(handle)); text != nullptr) {
            std::memcpy(text, value.data(), value.size());
            text[value.size()] = 0;
            GlobalUnlock(handle);
            SetClipboardData(CF_TEXT, handle);
        } else {
            GlobalFree(handle);
        }
    }
    CloseClipboard();
}

std::string key_name(std::uint32_t key) {
    if (key == 0) {
        return "unbound";
    }
    if (key >= VK_F1 && key <= VK_F24) {
        return "F" + std::to_string(key - VK_F1 + 1);
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        return "Numpad " + std::to_string(key - VK_NUMPAD0);
    }
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) {
        return std::string(1, static_cast<char>(key));
    }
    switch (key) {
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Esc";
        case VK_TAB: return "Tab";
        case VK_BACK: return "Backspace";
        case VK_DELETE: return "Delete";
        case VK_INSERT: return "Insert";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "Page Up";
        case VK_NEXT: return "Page Down";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_ADD: return "Numpad +";
        case VK_SUBTRACT: return "Numpad -";
        case VK_MULTIPLY: return "Numpad *";
        case VK_DIVIDE: return "Numpad /";
        case VK_DECIMAL: return "Numpad .";
        case VK_OEM_COMMA: return ",";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_MINUS: return "-";
        case VK_OEM_PLUS: return "=";
        case VK_OEM_1: return ";";
        case VK_OEM_2: return "/";
        case VK_OEM_3: return "`";
        case VK_OEM_4: return "[";
        case VK_OEM_5: return "\\";
        case VK_OEM_6: return "]";
        case VK_OEM_7: return "'";
        default: break;
    }
    return "Key " + std::to_string(key);
}

}
