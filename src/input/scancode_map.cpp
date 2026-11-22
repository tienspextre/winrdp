#include "input/scancode_map.hpp"

#include <freerdp/scancode.h>

#include <algorithm>
#include <cmath>

namespace winrdp {

uint32_t x11_keycode_to_rdp_scancode(uint16_t hw_keycode) {
    if (hw_keycode < 9) {
        return 0;
    }
    const uint16_t evdev = static_cast<uint16_t>(hw_keycode - 8U);

    // PC AT Set 1 scan codes match Linux evdev directly for 0x01..0x58 (except 0x54..0x56 tweaks)
    switch (evdev) {
        case 84: return RDP_SCANCODE_SYSREQ;
        case 85: return 0; // KEY_ZENKAKUHANKAKU
        case 86: return RDP_SCANCODE_OEM_102;
        case 87: return RDP_SCANCODE_F11;
        case 88: return RDP_SCANCODE_F12;
        case 89: return RDP_SCANCODE_ABNT_C1;
        case 92: return RDP_SCANCODE_CONVERT_JP;
        case 93: return RDP_SCANCODE_HIRAGANA;
        case 94: return RDP_SCANCODE_NONCONVERT_JP;
        case 96: return RDP_SCANCODE_RETURN_KP;
        case 97: return RDP_SCANCODE_RCONTROL;
        case 98: return RDP_SCANCODE_DIVIDE;
        case 99: return RDP_SCANCODE_PRINTSCREEN;
        case 100: return RDP_SCANCODE_RMENU;
        case 102: return RDP_SCANCODE_HOME;
        case 103: return RDP_SCANCODE_UP;
        case 104: return RDP_SCANCODE_PRIOR;
        case 105: return RDP_SCANCODE_LEFT;
        case 106: return RDP_SCANCODE_RIGHT;
        case 107: return RDP_SCANCODE_END;
        case 108: return RDP_SCANCODE_DOWN;
        case 109: return RDP_SCANCODE_NEXT;
        case 110: return RDP_SCANCODE_INSERT;
        case 111: return RDP_SCANCODE_DELETE;
        case 113: return RDP_SCANCODE_VOLUME_MUTE;
        case 114: return RDP_SCANCODE_VOLUME_DOWN;
        case 115: return RDP_SCANCODE_VOLUME_UP;
        case 119: return RDP_SCANCODE_PAUSE;
        case 121: return RDP_SCANCODE_ABNT_C2;
        case 122: return RDP_SCANCODE_KANA_HANGUL;
        case 123: return RDP_SCANCODE_HANJA_KANJI;
        case 124: return RDP_SCANCODE_BACKSLASH_JP;
        case 125: return RDP_SCANCODE_LWIN;
        case 126: return RDP_SCANCODE_RWIN;
        case 127: return RDP_SCANCODE_APPS;
        case 142: return RDP_SCANCODE_SLEEP;
        case 158: return RDP_SCANCODE_BROWSER_BACK;
        case 159: return RDP_SCANCODE_BROWSER_FORWARD;
        case 163: return RDP_SCANCODE_MEDIA_NEXT_TRACK;
        case 164: return RDP_SCANCODE_MEDIA_PLAY_PAUSE;
        case 165: return RDP_SCANCODE_MEDIA_PREV_TRACK;
        case 166: return RDP_SCANCODE_MEDIA_STOP;
        case 172: return RDP_SCANCODE_BROWSER_HOME;
        case 173: return RDP_SCANCODE_BROWSER_REFRESH;
        case 183: return RDP_SCANCODE_F13;
        case 184: return RDP_SCANCODE_F14;
        case 185: return RDP_SCANCODE_F15;
        case 186: return RDP_SCANCODE_F16;
        case 187: return RDP_SCANCODE_F17;
        case 188: return RDP_SCANCODE_F18;
        case 189: return RDP_SCANCODE_F19;
        case 190: return RDP_SCANCODE_F20;
        case 191: return RDP_SCANCODE_F21;
        case 192: return RDP_SCANCODE_F22;
        case 193: return RDP_SCANCODE_F23;
        case 194: return RDP_SCANCODE_F24;
        case 217: return RDP_SCANCODE_BROWSER_SEARCH;
        default:
            if (evdev >= 1 && evdev <= 83) {
                return MAKE_RDP_SCANCODE(static_cast<BYTE>(evdev), FALSE);
            }
            return 0;
    }
}

ViewportTransform compute_viewport_transform(double widget_w,
                                             double widget_h,
                                             int scale_factor,
                                             uint32_t fb_w,
                                             uint32_t fb_h,
                                             bool smart_sizing) {
    ViewportTransform vp;
    vp.fb_w = fb_w;
    vp.fb_h = fb_h;
    if (widget_w <= 0.0 || widget_h <= 0.0 || fb_w == 0 || fb_h == 0) {
        return vp;
    }

    const double sf = std::max(1, scale_factor);
    const double pixel_w = widget_w * sf;
    const double pixel_h = widget_h * sf;

    if (smart_sizing) {
        // Aspect-preserving letterbox fit across the widget
        const double sx = widget_w / static_cast<double>(fb_w);
        const double sy = widget_h / static_cast<double>(fb_h);
        const double s = std::min(sx, sy);
        vp.draw_w = static_cast<double>(fb_w) * s;
        vp.draw_h = static_cast<double>(fb_h) * s;
        vp.offset_x = (widget_w - vp.draw_w) * 0.5;
        vp.offset_y = (widget_h - vp.draw_h) * 0.5;
        vp.exact_1_to_1 = (std::abs(vp.draw_w * sf - static_cast<double>(fb_w)) < 0.5 &&
                           std::abs(vp.draw_h * sf - static_cast<double>(fb_h)) < 0.5);
        return vp;
    }

    // 1:1 native pixel or aspect-fit when remote frame exceeds widget size
    if (static_cast<double>(fb_w) <= pixel_w + 0.5 &&
        static_cast<double>(fb_h) <= pixel_h + 0.5) {
        vp.draw_w = static_cast<double>(fb_w) / sf;
        vp.draw_h = static_cast<double>(fb_h) / sf;
        vp.offset_x = (widget_w - vp.draw_w) * 0.5;
        vp.offset_y = (widget_h - vp.draw_h) * 0.5;
        vp.exact_1_to_1 = true;
    } else {
        const double sx = widget_w / static_cast<double>(fb_w);
        const double sy = widget_h / static_cast<double>(fb_h);
        const double s = std::min(sx, sy);
        vp.draw_w = static_cast<double>(fb_w) * s;
        vp.draw_h = static_cast<double>(fb_h) * s;
        vp.offset_x = (widget_w - vp.draw_w) * 0.5;
        vp.offset_y = (widget_h - vp.draw_h) * 0.5;
        vp.exact_1_to_1 = false;
    }
    return vp;
}

bool map_widget_to_remote(const ViewportTransform& vp,
                          double widget_x,
                          double widget_y,
                          int& out_rx,
                          int& out_ry) {
    if (vp.draw_w <= 0.0 || vp.draw_h <= 0.0 || vp.fb_w == 0 || vp.fb_h == 0) {
        return false;
    }
    const double rel_x = (widget_x - vp.offset_x) / vp.draw_w;
    const double rel_y = (widget_y - vp.offset_y) / vp.draw_h;

    const double clamped_x = std::clamp(rel_x, 0.0, 1.0);
    const double clamped_y = std::clamp(rel_y, 0.0, 1.0);

    const int max_x = static_cast<int>(vp.fb_w) - 1;
    const int max_y = static_cast<int>(vp.fb_h) - 1;

    out_rx = std::clamp(static_cast<int>(std::lround(clamped_x * max_x)), 0, max_x);
    out_ry = std::clamp(static_cast<int>(std::lround(clamped_y * max_y)), 0, max_y);
    return true;
}

} // namespace winrdp
