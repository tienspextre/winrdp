#include "rdp/framebuffer.hpp"

#include <algorithm>
#include <cstring>

namespace winrdp {

void Framebuffer::resize(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_history_.clear();
    if (width == 0 || height == 0) {
        pixels_.clear();
        width_val_ = 0;
        height_val_ = 0;
        has_dirty_ = false;
        dirty_x1_ = dirty_y1_ = dirty_x2_ = dirty_y2_ = 0;
    } else {
        width_val_ = width;
        height_val_ = height;
        pixels_.assign(static_cast<size_t>(width) * height * 4U, 0);
        // Initialize dark background #101014 (BGRA: 14, 10, 10, FF)
        for (size_t i = 0; i < pixels_.size(); i += 4U) {
            pixels_[i + 0] = 0x14;
            pixels_[i + 1] = 0x10;
            pixels_[i + 2] = 0x10;
            pixels_[i + 3] = 0xFF;
        }
        has_dirty_ = true;
        dirty_x1_ = 0;
        dirty_y1_ = 0;
        dirty_x2_ = static_cast<int>(width);
        dirty_y2_ = static_cast<int>(height);
    }

    ++resize_gen_val_;
    ++frame_gen_val_;
    if (width_val_ > 0 && height_val_ > 0) {
        dirty_history_.emplace_back(frame_gen_val_,
                                    DirtyRect{0, 0, static_cast<int>(width_val_), static_cast<int>(height_val_)});
    }
    width_.store(width_val_, std::memory_order_release);
    height_.store(height_val_, std::memory_order_release);
    resize_gen_.store(resize_gen_val_, std::memory_order_release);
    frame_gen_.store(frame_gen_val_, std::memory_order_release);
}

void Framebuffer::clear(uint32_t bgra_color) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pixels_.empty()) return;
    const uint8_t b = static_cast<uint8_t>(bgra_color & 0xFF);
    const uint8_t g = static_cast<uint8_t>((bgra_color >> 8) & 0xFF);
    const uint8_t r = static_cast<uint8_t>((bgra_color >> 16) & 0xFF);
    const uint8_t a = static_cast<uint8_t>((bgra_color >> 24) & 0xFF);
    for (size_t i = 0; i < pixels_.size(); i += 4U) {
        pixels_[i + 0] = b;
        pixels_[i + 1] = g;
        pixels_[i + 2] = r;
        pixels_[i + 3] = a;
    }
    has_dirty_ = true;
    dirty_x1_ = 0;
    dirty_y1_ = 0;
    dirty_x2_ = static_cast<int>(width_val_);
    dirty_y2_ = static_cast<int>(height_val_);
    ++frame_gen_val_;
    dirty_history_.clear();
    dirty_history_.emplace_back(frame_gen_val_,
                                DirtyRect{0, 0, static_cast<int>(width_val_), static_cast<int>(height_val_)});
    frame_gen_.store(frame_gen_val_, std::memory_order_release);
}

void Framebuffer::update_region(const uint8_t* src, uint32_t src_stride, int x, int y, int w, int h) {
    if (!src || w <= 0 || h <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (width_val_ == 0 || height_val_ == 0 || pixels_.empty()) {
        return;
    }

    const int fw = static_cast<int>(width_val_);
    const int fh = static_cast<int>(height_val_);

    int x1 = std::max(0, x);
    int y1 = std::max(0, y);
    int x2 = std::min(fw, x + w);
    int y2 = std::min(fh, y + h);
    if (x1 >= x2 || y1 >= y2) {
        return;
    }

    const size_t dst_stride = static_cast<size_t>(width_val_) * 4U;
    const size_t copy_bytes = static_cast<size_t>(x2 - x1) * 4U;

    for (int row = y1; row < y2; ++row) {
        const uint8_t* src_row = src + static_cast<size_t>(row) * src_stride + static_cast<size_t>(x1) * 4U;
        uint8_t* dst_row = pixels_.data() + static_cast<size_t>(row) * dst_stride + static_cast<size_t>(x1) * 4U;
        std::memcpy(dst_row, src_row, copy_bytes);
    }

    if (!has_dirty_) {
        has_dirty_ = true;
        dirty_x1_ = x1;
        dirty_y1_ = y1;
        dirty_x2_ = x2;
        dirty_y2_ = y2;
    } else {
        dirty_x1_ = std::min(dirty_x1_, x1);
        dirty_y1_ = std::min(dirty_y1_, y1);
        dirty_x2_ = std::max(dirty_x2_, x2);
        dirty_y2_ = std::max(dirty_y2_, y2);
    }

    ++frame_gen_val_;
    dirty_history_.emplace_back(frame_gen_val_, DirtyRect{x1, y1, x2 - x1, y2 - y1});
    while (dirty_history_.size() > kMaxDirtyHistory) {
        dirty_history_.pop_front();
    }
    frame_gen_.store(frame_gen_val_, std::memory_order_release);
}

} // namespace winrdp
