#include "input/monitor_layout.hpp"

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <cstring>

namespace winrdp {

std::vector<MonitorInfo> query_local_monitors(GdkDisplay* display) {
    std::vector<MonitorInfo> result;
    if (!display) {
        display = gdk_display_get_default();
    }
    if (!display) {
        result.push_back(MonitorInfo{0, 0, 1920, 1080, 520, 290, 1, true});
        return result;
    }

    GListModel* list = gdk_display_get_monitors(display);
    if (!list) {
        result.push_back(MonitorInfo{0, 0, 1920, 1080, 520, 290, 1, true});
        return result;
    }

    const guint n = g_list_model_get_n_items(list);
    for (guint i = 0; i < n; ++i) {
        GdkMonitor* mon = GDK_MONITOR(g_list_model_get_item(list, i));
        if (!mon) continue;

        GdkRectangle geom{};
        gdk_monitor_get_geometry(mon, &geom);
        const int sf = std::max(1, gdk_monitor_get_scale_factor(mon));

        MonitorInfo info;
        info.x = geom.x * sf;
        info.y = geom.y * sf;
        info.width = std::max(320, geom.width * sf);
        info.height = std::max(200, geom.height * sf);
        info.physical_width_mm = std::max(0, gdk_monitor_get_width_mm(mon));
        info.physical_height_mm = std::max(0, gdk_monitor_get_height_mm(mon));
        info.scale_factor = sf;
        info.is_primary = (geom.x == 0 && geom.y == 0) || (i == 0);

        result.push_back(info);
        g_object_unref(mon);
    }

    if (result.empty()) {
        result.push_back(MonitorInfo{0, 0, 1920, 1080, 520, 290, 1, true});
    } else {
        // Ensure exactly one monitor is marked primary (prefer one at 0,0)
        bool found_origin = false;
        for (auto& m : result) {
            if (m.x == 0 && m.y == 0) {
                m.is_primary = true;
                found_origin = true;
                break;
            }
            m.is_primary = false;
        }
        if (!found_origin) {
            result[0].is_primary = true;
        } else {
            bool kept_one = false;
            for (auto& m : result) {
                if (m.is_primary && !kept_one) {
                    kept_one = true;
                } else {
                    m.is_primary = false;
                }
            }
        }
    }

    return result;
}

std::vector<rdpMonitor> build_freerdp_monitors(const std::vector<MonitorInfo>& monitors,
                                               uint32_t scale_percent_override) {
    std::vector<rdpMonitor> out;
    if (monitors.empty()) return out;

    int32_t origin_x = monitors[0].x;
    int32_t origin_y = monitors[0].y;
    for (const auto& m : monitors) {
        if (m.is_primary) {
            origin_x = m.x;
            origin_y = m.y;
            break;
        }
    }

    out.reserve(monitors.size());
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        rdpMonitor rm{};
        rm.x = m.x - origin_x;
        rm.y = m.y - origin_y;
        rm.width = m.width;
        rm.height = m.height;
        rm.is_primary = m.is_primary ? 1 : 0;
        rm.orig_screen = static_cast<UINT32>(i);
        rm.attributes.physicalWidth = static_cast<UINT32>(std::max(0, m.physical_width_mm));
        rm.attributes.physicalHeight = static_cast<UINT32>(std::max(0, m.physical_height_mm));
        rm.attributes.orientation = ORIENTATION_LANDSCAPE;
        uint32_t dscale = scale_percent_override > 100
                              ? scale_percent_override
                              : static_cast<uint32_t>(std::max(1, m.scale_factor) * 100);
        rm.attributes.desktopScaleFactor = dscale;
        rm.attributes.deviceScaleFactor = 100;
        out.push_back(rm);
    }
    return out;
}

void apply_x11_fullscreen_monitors(GtkWindow* window, bool enable_multimon) {
#ifdef GDK_WINDOWING_X11
    if (!window) return;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (!surface || !GDK_IS_X11_SURFACE(surface)) return;

    GdkDisplay* display = gdk_surface_get_display(surface);
    if (!display || !GDK_IS_X11_DISPLAY(display)) return;

    Display* xdisplay = gdk_x11_display_get_xdisplay(display);
    Window xwin = gdk_x11_surface_get_xid(surface);
    if (!xdisplay || xwin == None) return;

    Atom wm_fs_monitors = XInternAtom(xdisplay, "_NET_WM_FULLSCREEN_MONITORS", False);
    Window root = DefaultRootWindow(xdisplay);

    if (!enable_multimon) {
        XDeleteProperty(xdisplay, xwin, wm_fs_monitors);
        XFlush(xdisplay);
        return;
    }

    GListModel* list = gdk_display_get_monitors(display);
    if (!list) return;
    const guint count = g_list_model_get_n_items(list);
    if (count <= 1) return;

    long top_idx = 0, bottom_idx = 0, left_idx = 0, right_idx = 0;
    int min_x = INT32_MAX, max_x = INT32_MIN, min_y = INT32_MAX, max_y = INT32_MIN;

    for (guint i = 0; i < count; ++i) {
        GdkMonitor* mon = GDK_MONITOR(g_list_model_get_item(list, i));
        if (!mon) continue;
        GdkRectangle g{};
        gdk_monitor_get_geometry(mon, &g);
        if (g.x < min_x) { min_x = g.x; left_idx = i; }
        if (g.x + g.width > max_x) { max_x = g.x + g.width; right_idx = i; }
        if (g.y < min_y) { min_y = g.y; top_idx = i; }
        if (g.y + g.height > max_y) { max_y = g.y + g.height; bottom_idx = i; }
        g_object_unref(mon);
    }

    XEvent xev{};
    xev.type = ClientMessage;
    xev.xclient.window = xwin;
    xev.xclient.message_type = wm_fs_monitors;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = top_idx;
    xev.xclient.data.l[1] = bottom_idx;
    xev.xclient.data.l[2] = left_idx;
    xev.xclient.data.l[3] = right_idx;
    xev.xclient.data.l[4] = 1; // source indication: application
    XSendEvent(xdisplay, root, False, SubstructureNotifyMask | SubstructureRedirectMask, &xev);
    XFlush(xdisplay);
#else
    (void)window;
    (void)enable_multimon;
#endif
}

} // namespace winrdp
