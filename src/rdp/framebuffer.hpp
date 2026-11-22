#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
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

    // Multi-consumer dirty-rect snapshot: each consumer maintains its own resize/frame cursor
    // so multiple monitor canvases read accumulated updates independently without clobbering.
    template <typename Fn>
    void read_snapshot(uint64_t& consumer_resize_gen, uint64_t& consumer_frame_gen, Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        DirtyRect dirty;
        if (width_val_ > 0 && height_val_ > 0 && !pixels_.empty()) {
            if (consumer_resize_gen != resize_gen_val_ ||
                consumer_frame_gen == 0 ||
                dirty_history_.empty() ||
                consumer_frame_gen + 1 < dirty_history_.front().first) {
                dirty.x = 0;
                dirty.y = 0;
                dirty.width = static_cast<int>(width_val_);
                dirty.height = static_cast<int>(height_val_);
            } else if (consumer_frame_gen < frame_gen_val_) {
                bool first = true;
                int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                for (const auto& entry : dirty_history_) {
                    if (entry.first <= consumer_frame_gen || entry.second.empty()) {
                        continue;
                    }
                    const int ex2 = entry.second.x + entry.second.width;
                    const int ey2 = entry.second.y + entry.second.height;
                    if (first) {
                        x1 = entry.second.x;
                        y1 = entry.second.y;
                        x2 = ex2;
                        y2 = ey2;
                        first = false;
                    } else {
                        x1 = std::min(x1, entry.second.x);
                        y1 = std::min(y1, entry.second.y);
                        x2 = std::max(x2, ex2);
                        y2 = std::max(y2, ey2);
                    }
                }
                if (!first) {
                    dirty.x = x1;
                    dirty.y = y1;
                    dirty.width = x2 - x1;
                    dirty.height = y2 - y1;
                }
            }
        }

        fn(width_val_, height_val_, width_val_ * 4U,
           pixels_.empty() ? nullptr : pixels_.data(),
           dirty, resize_gen_val_, frame_gen_val_);

        consumer_resize_gen = resize_gen_val_;
        consumer_frame_gen = frame_gen_val_;
    }

    // Legacy single-consumer dirty-rect consume helper.
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
    static constexpr size_t kMaxDirtyHistory = 128;

    mutable std::mutex mutex_;
    std::vector<uint8_t> pixels_;
    std::deque<std::pair<uint64_t, DirtyRect>> dirty_history_;
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
