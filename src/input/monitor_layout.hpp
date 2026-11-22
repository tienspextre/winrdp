#pragma once

#include <freerdp/settings.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <vector>

namespace winrdp {

struct MonitorInfo {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 1920;
    int32_t height = 1080;
    int32_t physical_width_mm = 0;
    int32_t physical_height_mm = 0;
    int32_t scale_factor = 1;
    bool is_primary = false;
};

// Enumerates local GdkMonitors from the default display (device-pixel coordinates).
std::vector<MonitorInfo> query_local_monitors(GdkDisplay* display = nullptr);

// Normalizes monitors so the primary monitor is at (0,0) and builds FreeRDP rdpMonitor array.
std::vector<rdpMonitor> build_freerdp_monitors(const std::vector<MonitorInfo>& monitors,
                                               uint32_t scale_percent_override = 100);

// Applies _NET_WM_FULLSCREEN_MONITORS on X11 when multi-monitor fullscreen is requested.
void apply_x11_fullscreen_monitors(GtkWindow* window, bool enable_multimon);

} // namespace winrdp
