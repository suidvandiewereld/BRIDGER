#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain3;

namespace bridger::ui {
class DrawList;
}

namespace bridger::overlay {

class Renderer {
public:
    ~Renderer();

    bool initialise(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue);
    void release_targets();
    void shutdown();

    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }

    void render(const ui::DrawList* list, IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    unsigned width_ = 0;
    unsigned height_ = 0;
};

}
