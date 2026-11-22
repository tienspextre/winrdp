#include "input/monitor_math.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <queue>
#include <unordered_set>

namespace winrdp {

namespace {

constexpr size_t kMaxRdpMonitors = 16;

bool intervals_overlap(int32_t a1, int32_t a2, int32_t b1, int32_t b2) {
    return std::min(a2, b2) > std::max(a1, b1);
}

int32_t cross_offset(int32_t target_pos, int32_t ref_pos, int32_t ref_logical, int32_t ref_phys) {
    if (ref_logical <= 0) {
        return target_pos - ref_pos;
    }
    return static_cast<int32_t>(std::lround(
        static_cast<double>(target_pos - ref_pos) * static_cast<double>(ref_phys) /
        static_cast<double>(ref_logical)));
}

} // namespace

std::string format_monitor_label(const MonitorInfo& m) {
    std::string out = "Monitor " + std::to_string(m.index + 1);
    if (!m.connector.empty()) {
        out += " — " + m.connector;
    } else if (!m.description.empty()) {
        out += " — " + m.description;
    }
    out += " (" + std::to_string(m.width) + "x" + std::to_string(m.height);
    if (m.scale_factor > 1) {
        out += ", " + std::to_string(m.scale_factor) + "x";
    }
    if (m.is_primary) {
        out += ", Primary";
    }
    out += ")";
    return out;
}

std::vector<MonitorInfo> filter_selected_monitors(const std::vector<MonitorInfo>& all_monitors,
                                                  const std::vector<int32_t>& selected_indices) {
    if (all_monitors.empty()) {
        return {};
    }

    std::vector<MonitorInfo> chosen;
    if (selected_indices.empty()) {
        chosen = all_monitors;
    } else {
        std::unordered_set<size_t> used_pos;
        for (int32_t idx : selected_indices) {
            if (idx < 0) continue;
            size_t match_pos = all_monitors.size();
            for (size_t i = 0; i < all_monitors.size(); ++i) {
                if (all_monitors[i].index == idx) {
                    match_pos = i;
                    break;
                }
            }
            if (match_pos == all_monitors.size() && static_cast<size_t>(idx) < all_monitors.size()) {
                match_pos = static_cast<size_t>(idx);
            }
            if (match_pos < all_monitors.size() && used_pos.insert(match_pos).second) {
                chosen.push_back(all_monitors[match_pos]);
            }
        }
        if (chosen.empty()) {
            chosen = all_monitors;
        }
    }

    if (chosen.size() > kMaxRdpMonitors) {
        chosen.resize(kMaxRdpMonitors);
    }

    size_t primary_pos = chosen.size();
    for (size_t i = 0; i < chosen.size(); ++i) {
        if (chosen[i].is_primary) {
            primary_pos = i;
            break;
        }
    }
    if (primary_pos == chosen.size()) {
        for (size_t i = 0; i < chosen.size(); ++i) {
            if (chosen[i].x == 0 && chosen[i].y == 0) {
                primary_pos = i;
                break;
            }
        }
    }
    if (primary_pos == chosen.size()) {
        primary_pos = 0;
    }
    for (size_t i = 0; i < chosen.size(); ++i) {
        chosen[i].is_primary = (i == primary_pos);
    }
    return chosen;
}

std::vector<MonitorInfo> normalize_physical_layout(std::vector<MonitorInfo> monitors) {
    if (monitors.empty()) {
        return {};
    }

    size_t primary_idx = monitors.size();
    for (size_t i = 0; i < monitors.size(); ++i) {
        auto& m = monitors[i];
        m.width = std::clamp(m.width & ~1, 200, 8192);
        m.height = std::clamp(m.height & ~1, 200, 8192);
        if (m.logical_width <= 0 || m.logical_height <= 0) {
            m.logical_x = m.x;
            m.logical_y = m.y;
            m.logical_width = m.width;
            m.logical_height = m.height;
        }
        if (m.is_primary && primary_idx == monitors.size()) {
            primary_idx = i;
        }
    }

    if (primary_idx == monitors.size()) {
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (monitors[i].x == 0 && monitors[i].y == 0) {
                primary_idx = i;
                break;
            }
        }
    }
    if (primary_idx == monitors.size()) {
        primary_idx = 0;
    }
    for (size_t i = 0; i < monitors.size(); ++i) {
        monitors[i].is_primary = (i == primary_idx);
    }

    if (monitors.size() == 1) {
        return monitors;
    }

    constexpr int32_t kTol = 4;
    std::vector<bool> placed(monitors.size(), false);
    std::queue<size_t> q;
    placed[primary_idx] = true;
    q.push(primary_idx);

    auto align_vertical_y = [](const MonitorInfo& a, const MonitorInfo& b) -> int32_t {
        if (std::abs(a.logical_y - b.logical_y) <= kTol) {
            return a.y;
        }
        if (std::abs((a.logical_y + a.logical_height) - (b.logical_y + b.logical_height)) <= kTol) {
            return a.y + a.height - b.height;
        }
        return a.y + cross_offset(b.logical_y, a.logical_y, a.logical_height, a.height);
    };

    auto align_horizontal_x = [](const MonitorInfo& a, const MonitorInfo& b) -> int32_t {
        if (std::abs(a.logical_x - b.logical_x) <= kTol) {
            return a.x;
        }
        if (std::abs((a.logical_x + a.logical_width) - (b.logical_x + b.logical_width)) <= kTol) {
            return a.x + a.width - b.width;
        }
        return a.x + cross_offset(b.logical_x, a.logical_x, a.logical_width, a.width);
    };

    auto run_bfs = [&]() {
        while (!q.empty()) {
            const size_t ai = q.front();
            q.pop();
            const MonitorInfo& a = monitors[ai];

            for (size_t bi = 0; bi < monitors.size(); ++bi) {
                if (placed[bi]) continue;
                MonitorInfo& b = monitors[bi];

                const bool y_overlap = intervals_overlap(
                    a.logical_y, a.logical_y + a.logical_height,
                    b.logical_y, b.logical_y + b.logical_height);
                const bool x_overlap = intervals_overlap(
                    a.logical_x, a.logical_x + a.logical_width,
                    b.logical_x, b.logical_x + b.logical_width);

                if (y_overlap && std::abs((a.logical_x + a.logical_width) - b.logical_x) <= kTol) {
                    b.x = a.x + a.width;
                    b.y = align_vertical_y(a, b);
                    placed[bi] = true;
                    q.push(bi);
                } else if (y_overlap && std::abs((b.logical_x + b.logical_width) - a.logical_x) <= kTol) {
                    b.x = a.x - b.width;
                    b.y = align_vertical_y(a, b);
                    placed[bi] = true;
                    q.push(bi);
                } else if (x_overlap && std::abs((a.logical_y + a.logical_height) - b.logical_y) <= kTol) {
                    b.y = a.y + a.height;
                    b.x = align_horizontal_x(a, b);
                    placed[bi] = true;
                    q.push(bi);
                } else if (x_overlap && std::abs((b.logical_y + b.logical_height) - a.logical_y) <= kTol) {
                    b.y = a.y - b.height;
                    b.x = align_horizontal_x(a, b);
                    placed[bi] = true;
                    q.push(bi);
                }
            }
        }
    };

    run_bfs();

    // Handle disconnected subsets (e.g. selecting monitors 0 and 2 skipping monitor 1)
    while (true) {
        int64_t best_dist = INT64_MAX;
        size_t best_u = monitors.size();

        int32_t min_lx = INT32_MAX, max_lx = INT32_MIN, min_ly = INT32_MAX, max_ly = INT32_MIN;
        int32_t min_px = INT32_MAX, max_px = INT32_MIN, min_py = INT32_MAX, max_py = INT32_MIN;
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (!placed[i]) continue;
            min_lx = std::min(min_lx, monitors[i].logical_x);
            max_lx = std::max(max_lx, monitors[i].logical_x + monitors[i].logical_width);
            min_ly = std::min(min_ly, monitors[i].logical_y);
            max_ly = std::max(max_ly, monitors[i].logical_y + monitors[i].logical_height);
            min_px = std::min(min_px, monitors[i].x);
            max_px = std::max(max_px, monitors[i].x + monitors[i].width);
            min_py = std::min(min_py, monitors[i].y);
            max_py = std::max(max_py, monitors[i].y + monitors[i].height);
        }

        const int64_t center_lx = (static_cast<int64_t>(min_lx) + max_lx) / 2;
        const int64_t center_ly = (static_cast<int64_t>(min_ly) + max_ly) / 2;

        for (size_t i = 0; i < monitors.size(); ++i) {
            if (placed[i]) continue;
            const int64_t ucx = monitors[i].logical_x + monitors[i].logical_width / 2;
            const int64_t ucy = monitors[i].logical_y + monitors[i].logical_height / 2;
            const int64_t dx = ucx - center_lx;
            const int64_t dy = ucy - center_ly;
            const int64_t d2 = dx * dx + dy * dy;
            if (d2 < best_dist) {
                best_dist = d2;
                best_u = i;
            }
        }

        if (best_u == monitors.size()) {
            break;
        }

        MonitorInfo& u = monitors[best_u];
        const int64_t ucx = u.logical_x + u.logical_width / 2;
        const int64_t ucy = u.logical_y + u.logical_height / 2;
        const int64_t dx = ucx - center_lx;
        const int64_t dy = ucy - center_ly;

        if (std::abs(dx) >= std::abs(dy)) {
            u.x = (dx >= 0) ? max_px : (min_px - u.width);
            u.y = std::clamp(min_py, min_py, std::max(min_py, max_py - u.height));
        } else {
            u.y = (dy >= 0) ? max_py : (min_py - u.height);
            u.x = std::clamp(min_px, min_px, std::max(min_px, max_px - u.width));
        }
        placed[best_u] = true;
        q.push(best_u);
        run_bfs();
    }

    return monitors;
}

VirtualDesktopLayout compute_virtual_desktop_layout(const std::vector<MonitorInfo>& selected_monitors,
                                                    uint32_t scale_percent_override) {
    VirtualDesktopLayout layout;
    if (selected_monitors.empty()) {
        return layout;
    }

    std::vector<MonitorInfo> normalized = normalize_physical_layout(selected_monitors);

    size_t primary_pos = 0;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i].is_primary) {
            primary_pos = i;
            break;
        }
    }

    const int32_t prim_x = normalized[primary_pos].x;
    const int32_t prim_y = normalized[primary_pos].y;

    // Order with primary at index 0
    std::vector<MonitorInfo> ordered;
    ordered.reserve(normalized.size());
    ordered.push_back(normalized[primary_pos]);
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i != primary_pos) {
            ordered.push_back(normalized[i]);
        }
    }

    int32_t min_x = INT32_MAX;
    int32_t min_y = INT32_MAX;
    int32_t max_x = INT32_MIN;
    int32_t max_y = INT32_MIN;

    layout.viewports.reserve(ordered.size());
    layout.rdp_monitors.reserve(ordered.size());

    for (const auto& m : ordered) {
        const int32_t rx = m.x - prim_x;
        const int32_t ry = m.y - prim_y;
        min_x = std::min(min_x, rx);
        min_y = std::min(min_y, ry);
        max_x = std::max(max_x, rx + m.width);
        max_y = std::max(max_y, ry + m.height);

        MonitorViewport vp;
        vp.monitor = m;
        vp.rdp_x = rx;
        vp.rdp_y = ry;
        vp.fb_width = static_cast<uint32_t>(m.width);
        vp.fb_height = static_cast<uint32_t>(m.height);
        vp.is_primary = m.is_primary;
        layout.viewports.push_back(vp);

        rdpMonitor rm{};
        rm.x = rx;
        rm.y = ry;
        rm.width = m.width;
        rm.height = m.height;
        rm.is_primary = m.is_primary ? 1 : 0;
        rm.orig_screen = static_cast<UINT32>(std::max(0, m.index));

        int32_t phys_w = m.physical_width_mm;
        int32_t phys_h = m.physical_height_mm;
        if (phys_w < 10 || phys_h < 10) {
            phys_w = 0;
            phys_h = 0;
        } else {
            phys_w = std::clamp(phys_w, 10, 10000);
            phys_h = std::clamp(phys_h, 10, 10000);
        }
        rm.attributes.physicalWidth = static_cast<UINT32>(phys_w);
        rm.attributes.physicalHeight = static_cast<UINT32>(phys_h);
        rm.attributes.orientation = (m.height > m.width) ? ORIENTATION_PORTRAIT : ORIENTATION_LANDSCAPE;

        uint32_t dscale = scale_percent_override > 100
                              ? scale_percent_override
                              : static_cast<uint32_t>(std::max(1, m.scale_factor) * 100);
        rm.attributes.desktopScaleFactor = std::clamp<uint32_t>(dscale, 100, 500);
        rm.attributes.deviceScaleFactor = 100;
        layout.rdp_monitors.push_back(rm);
    }

    layout.origin_x = min_x;
    layout.origin_y = min_y;
    layout.desktop_width = static_cast<uint32_t>(std::max(320, max_x - min_x));
    layout.desktop_height = static_cast<uint32_t>(std::max(200, max_y - min_y));

    for (auto& vp : layout.viewports) {
        vp.fb_x = vp.rdp_x - min_x;
        vp.fb_y = vp.rdp_y - min_y;
    }

    return layout;
}

std::vector<rdpMonitor> build_freerdp_monitors(const std::vector<MonitorInfo>& monitors,
                                               uint32_t scale_percent_override) {
    return compute_virtual_desktop_layout(monitors, scale_percent_override).rdp_monitors;
}

} // namespace winrdp
