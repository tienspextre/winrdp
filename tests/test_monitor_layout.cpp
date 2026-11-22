#include "input/monitor_math.hpp"

#include <iostream>
#include <vector>

#define WINRDP_CHECK(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK FAILED: " #cond " (" << __FILE__ << ":"        \
                      << __LINE__ << ")\n";                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

using namespace winrdp;

static MonitorInfo make_mon(int32_t idx, const char* conn,
                            int32_t lx, int32_t ly, int32_t lw, int32_t lh,
                            int32_t sf, bool primary) {
    MonitorInfo m;
    m.index = idx;
    m.connector = conn;
    m.logical_x = lx;
    m.logical_y = ly;
    m.logical_width = lw;
    m.logical_height = lh;
    m.x = lx * sf;
    m.y = ly * sf;
    m.width = lw * sf;
    m.height = lh * sf;
    m.physical_width_mm = 527;
    m.physical_height_mm = 296;
    m.scale_factor = sf;
    m.is_primary = primary;
    return m;
}

int main() {
    // 1. Standard dual side-by-side 1920x1080 monitors
    {
        std::vector<MonitorInfo> mons = {
            make_mon(0, "DP-1", 0, 0, 1920, 1080, 1, true),
            make_mon(1, "HDMI-1", 1920, 0, 1920, 1080, 1, false),
        };
        auto layout = compute_virtual_desktop_layout(mons);
        WINRDP_CHECK(layout.desktop_width == 3840);
        WINRDP_CHECK(layout.desktop_height == 1080);
        WINRDP_CHECK(layout.viewports.size() == 2);
        WINRDP_CHECK(layout.viewports[0].is_primary == true);
        WINRDP_CHECK(layout.viewports[0].rdp_x == 0 && layout.viewports[0].rdp_y == 0);
        WINRDP_CHECK(layout.viewports[0].fb_x == 0 && layout.viewports[0].fb_y == 0);
        WINRDP_CHECK(layout.viewports[1].rdp_x == 1920 && layout.viewports[1].rdp_y == 0);
        WINRDP_CHECK(layout.viewports[1].fb_x == 1920 && layout.viewports[1].fb_y == 0);
        WINRDP_CHECK(layout.rdp_monitors[0].is_primary == 1);
        WINRDP_CHECK(layout.rdp_monitors[1].is_primary == 0);
        WINRDP_CHECK(layout.rdp_monitors[0].attributes.orientation == ORIENTATION_LANDSCAPE);
    }

    // 2. Left-of-primary (negative RDP coordinates) -> shifted to non-negative Framebuffer offsets
    {
        std::vector<MonitorInfo> mons = {
            make_mon(0, "HDMI-1", 0, 0, 1920, 1080, 1, false),
            make_mon(1, "DP-1", 1920, 0, 2560, 1440, 1, true),
        };
        auto layout = compute_virtual_desktop_layout(mons);
        WINRDP_CHECK(layout.desktop_width == 4480);
        WINRDP_CHECK(layout.desktop_height == 1440);
        WINRDP_CHECK(layout.origin_x == -1920);
        // Primary always comes first at viewports[0]
        WINRDP_CHECK(layout.viewports[0].monitor.index == 1);
        WINRDP_CHECK(layout.viewports[0].rdp_x == 0 && layout.viewports[0].rdp_y == 0);
        WINRDP_CHECK(layout.viewports[0].fb_x == 1920 && layout.viewports[0].fb_y == 0);
        // Secondary sits on the left in RDP (-1920) and at x=0 in shared Framebuffer
        WINRDP_CHECK(layout.viewports[1].monitor.index == 0);
        WINRDP_CHECK(layout.viewports[1].rdp_x == -1920 && layout.viewports[1].rdp_y == 0);
        WINRDP_CHECK(layout.viewports[1].fb_x == 0 && layout.viewports[1].fb_y == 0);
    }

    // 3. Mixed DPI (4K @ 2x -> logical 1920x1080 next to 1080p @ 1x) + Portrait secondary
    {
        auto m0 = make_mon(0, "eDP-1", 0, 0, 1920, 1080, 2, true); // 3840x2160 physical
        MonitorInfo m1;
        m1.index = 1;
        m1.connector = "DP-2";
        m1.logical_x = 1920;
        m1.logical_y = 0;
        m1.logical_width = 1440;
        m1.logical_height = 2560;
        m1.x = 3840;
        m1.y = 0;
        m1.width = 1440;
        m1.height = 2560;
        m1.scale_factor = 1;
        m1.is_primary = false;

        auto layout = compute_virtual_desktop_layout({m0, m1});
        WINRDP_CHECK(layout.viewports[0].fb_width == 3840);
        WINRDP_CHECK(layout.viewports[1].rdp_x == 3840); // No overlap with 3840px wide primary!
        WINRDP_CHECK(layout.viewports[1].fb_width == 1440);
        WINRDP_CHECK(layout.viewports[1].fb_height == 2560);
        WINRDP_CHECK(layout.desktop_width == 3840 + 1440);
        WINRDP_CHECK(layout.desktop_height == 2560);
        WINRDP_CHECK(layout.rdp_monitors[0].attributes.desktopScaleFactor == 200);
        WINRDP_CHECK(layout.rdp_monitors[1].attributes.orientation == ORIENTATION_PORTRAIT);
    }

    // 4. Subset selection [1, 2] excluding primary 0 -> promotes index 1 to primary
    {
        std::vector<MonitorInfo> all = {
            make_mon(0, "DP-1", 0, 0, 1920, 1080, 1, true),
            make_mon(1, "DP-2", 1920, 0, 1920, 1080, 1, false),
            make_mon(2, "HDMI-1", 3840, 0, 1920, 1080, 1, false),
        };
        auto sel = filter_selected_monitors(all, {1, 2});
        WINRDP_CHECK(sel.size() == 2);
        WINRDP_CHECK(sel[0].index == 1 && sel[0].is_primary == true);
        WINRDP_CHECK(sel[1].index == 2 && sel[1].is_primary == false);

        // Non-adjacent subset [0, 2] skipping 1 -> snapped contiguously to 3840x1080 (no 1920px gap)
        auto sel_gap = filter_selected_monitors(all, {0, 2});
        auto gap_layout = compute_virtual_desktop_layout(sel_gap);
        WINRDP_CHECK(gap_layout.desktop_width == 3840);
        WINRDP_CHECK(gap_layout.viewports[1].rdp_x == 1920);
        WINRDP_CHECK(!format_monitor_label(sel_gap[0]).empty());
    }

    std::cout << "test_monitor_layout: PASS\n";
    return 0;
}
