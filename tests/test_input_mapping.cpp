#include "input/scancode_map.hpp"

#include <freerdp/scancode.h>

#include <iostream>

#define WINRDP_CHECK(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK FAILED: " #cond " (" << __FILE__ << ":"        \
                      << __LINE__ << ")\n";                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

using namespace winrdp;

int main() {
    // 1. Keycode mapping (evdev + 8)
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(9) == RDP_SCANCODE_ESCAPE);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(36) == RDP_SCANCODE_RETURN);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(38) == RDP_SCANCODE_KEY_A);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(64) == RDP_SCANCODE_LMENU);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(105) == RDP_SCANCODE_RCONTROL);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(107) == RDP_SCANCODE_PRINTSCREEN);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(127) == RDP_SCANCODE_PAUSE);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(133) == RDP_SCANCODE_LWIN);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(134) == RDP_SCANCODE_RWIN);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(111) == RDP_SCANCODE_UP);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(116) == RDP_SCANCODE_DOWN);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(67) == RDP_SCANCODE_F1);
    WINRDP_CHECK(x11_keycode_to_rdp_scancode(96) == RDP_SCANCODE_F12);

    // 2. Exact 1:1 viewport transform (1920x1080 widget, scale=1, 1920x1080 framebuffer)
    const auto vp1 = compute_viewport_transform(1920.0, 1080.0, 1, 1920, 1080, false);
    WINRDP_CHECK(vp1.exact_1_to_1);
    WINRDP_CHECK(vp1.offset_x == 0.0 && vp1.offset_y == 0.0);
    int rx = -1, ry = -1;
    WINRDP_CHECK(map_widget_to_remote(vp1, 960.0, 540.0, rx, ry));
    WINRDP_CHECK(rx == 960 && ry == 540);

    // 3. Smart sizing letterboxing (1920x1200 widget -> 1920x1080 frame -> 60px top/bottom bars)
    const auto vp2 = compute_viewport_transform(1920.0, 1200.0, 1, 1920, 1080, true);
    WINRDP_CHECK(vp2.offset_x == 0.0 && vp2.offset_y == 60.0);
    WINRDP_CHECK(vp2.draw_w == 1920.0 && vp2.draw_h == 1080.0);
    WINRDP_CHECK(map_widget_to_remote(vp2, 0.0, 60.0, rx, ry));
    WINRDP_CHECK(rx == 0 && ry == 0);
    WINRDP_CHECK(map_widget_to_remote(vp2, 1920.0, 1140.0, rx, ry));
    WINRDP_CHECK(rx == 1919 && ry == 1079);

    // Clicking inside top letterbox clamps safely to y=0
    WINRDP_CHECK(map_widget_to_remote(vp2, 100.0, 10.0, rx, ry));
    WINRDP_CHECK(rx == 100 && ry == 0);

    std::cout << "test_input_mapping: PASS\n";
    return 0;
}
