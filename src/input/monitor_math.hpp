#pragma once

#include <freerdp/settings.h>

#include <cstdint>
#include <string>
#include <vector>

namespace winrdp {

struct MonitorInfo {
    int32_t index = 0;
    std::string connector;
    std::string description;
    int32_t logical_x = 0;
    int32_t logical_y = 0;
    int32_t logical_width = 0;
    int32_t logical_height = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 1920;
    int32_t height = 1080;
    int32_t physical_width_mm = 0;
    int32_t physical_height_mm = 0;
    int32_t scale_factor = 1;
    bool is_primary = false;
};

struct MonitorViewport {
    MonitorInfo monitor;
    int32_t rdp_x = 0;  // Primary-relative RDP coordinate (may be negative)
    int32_t rdp_y = 0;
    int32_t fb_x = 0;   // Non-negative offset in shared Framebuffer
    int32_t fb_y = 0;
    uint32_t fb_width = 0;
    uint32_t fb_height = 0;
    bool is_primary = false;
};

struct VirtualDesktopLayout {
    uint32_t desktop_width = 0;
    uint32_t desktop_height = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    std::vector<MonitorViewport> viewports; // Primary monitor is always at index 0
    std::vector<rdpMonitor> rdp_monitors;
};

// Formats a concise human-readable monitor label (e.g., "Monitor 1 — DP-1 (1920x1080, Primary)").
std::string format_monitor_label(const MonitorInfo& m);

// Filters monitors by user selection (empty = all monitors, max 16), guaranteeing 1 primary.
std::vector<MonitorInfo> filter_selected_monitors(const std::vector<MonitorInfo>& all_monitors,
                                                  const std::vector<int32_t>& selected_indices);

// Clamps resolutions to even [200..8192] and snaps adjacent/disjoint monitors cleanly
// in physical pixel coordinates (including mixed-DPI setups and non-adjacent subsets).
std::vector<MonitorInfo> normalize_physical_layout(std::vector<MonitorInfo> monitors);

// Builds primary-ordered viewports, Framebuffer offsets, and FreeRDP rdpMonitor array.
VirtualDesktopLayout compute_virtual_desktop_layout(const std::vector<MonitorInfo>& selected_monitors,
                                                    uint32_t scale_percent_override = 100);

// Convenience wrapper returning only the FreeRDP rdpMonitor array.
std::vector<rdpMonitor> build_freerdp_monitors(const std::vector<MonitorInfo>& monitors,
                                               uint32_t scale_percent_override = 100);

} // namespace winrdp
