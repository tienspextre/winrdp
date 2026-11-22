#pragma once

#include <cstdint>

namespace winrdp {

// Converts a Linux X11/Wayland hardware keycode (evdev + 8) to a FreeRDP RDP_SCANCODE_* value
// (low byte = scan code, bit 0x100 = KBDEXT). Returns 0 if unmapped.
uint32_t x11_keycode_to_rdp_scancode(uint16_t hw_keycode);

struct ViewportTransform {
    double offset_x = 0.0; // logical widget X of remote frame top-left
    double offset_y = 0.0; // logical widget Y of remote frame top-left
    double draw_w = 0.0;   // logical widget width of rendered remote frame
    double draw_h = 0.0;   // logical widget height of rendered remote frame
    uint32_t fb_w = 0;     // remote framebuffer width in pixels
    uint32_t fb_h = 0;     // remote framebuffer height in pixels
    bool exact_1_to_1 = false;
};

ViewportTransform compute_viewport_transform(double widget_w,
                                             double widget_h,
                                             int scale_factor,
                                             uint32_t fb_w,
                                             uint32_t fb_h,
                                             bool smart_sizing);

bool map_widget_to_remote(const ViewportTransform& vp,
                          double widget_x,
                          double widget_y,
                          int& out_rx,
                          int& out_ry);

} // namespace winrdp
