#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace winrdp {

struct DirtyRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0; }
};

class Framebuffer {
public:
    Framebuffer() = default;

    void resize(uint32_t width, uint32_t height);
    void update_region(const uint8_t* src, uint32_t src_stride, int x, int y, int w, int h);
    void clear(uint32_t bgra_color = 0xFF101014U);

    uint32_t width() const { return width_.load(std::memory_order_acquire); }
    uint32_t height() const { return height_.load(std::memory_order_acquire); }
    uint64_t frame_generation() const { return frame_gen_.load(std::memory_order_acquire); }
    uint64_t resize_generation() const { return resize_gen_.load(std::memory_order_acquire); }

    // Acquires the framebuffer lock, passes current dimensions, stride (width * 4),
    // BGRA32 data pointer, accumulated dirty rect, and resize/frame generations to fn,
    // and clears the pending dirty rect once fn finishes.
    template <typename Fn>
    void consume(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        DirtyRect dirty;
        if (has_dirty_) {
            dirty.x = dirty_x1_;
            dirty.y = dirty_y1_;
            dirty.width = dirty_x2_ - dirty_x1_;
            dirty.height = dirty_y2_ - dirty_y1_;
        }
        fn(width_val_, height_val_, width_val_ * 4U,
           pixels_.empty() ? nullptr : pixels_.data(),
           dirty, resize_gen_val_, frame_gen_val_);
        has_dirty_ = false;
        dirty_x1_ = dirty_y1_ = dirty_x2_ = dirty_y2_ = 0;
    }

private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> pixels_;
    uint32_t width_val_ = 0;
    uint32_t height_val_ = 0;
    uint64_t frame_gen_val_ = 0;
    uint64_t resize_gen_val_ = 0;

    bool has_dirty_ = false;
    int dirty_x1_ = 0;
    int dirty_y1_ = 0;
    int dirty_x2_ = 0;
    int dirty_y2_ = 0;

    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    std::atomic<uint64_t> frame_gen_{0};
    std::atomic<uint64_t> resize_gen_{0};
};

} // namespace winrdp
