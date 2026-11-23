#include "input/monitor_layout.hpp"

#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
#include <gdk/x11/gdkx.h>
G_GNUC_END_IGNORE_DEPRECATIONS
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
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
        MonitorInfo fallback;
        fallback.index = 0;
        fallback.logical_width = 1920;
        fallback.logical_height = 1080;
        fallback.width = 1920;
        fallback.height = 1080;
        fallback.physical_width_mm = 520;
        fallback.physical_height_mm = 290;
        fallback.scale_factor = 1;
        fallback.is_primary = true;
        result.push_back(std::move(fallback));
        return result;
    }

    GListModel* list = gdk_display_get_monitors(display);
    if (!list) {
        MonitorInfo fallback;
        fallback.index = 0;
        fallback.logical_width = 1920;
        fallback.logical_height = 1080;
        fallback.width = 1920;
        fallback.height = 1080;
        fallback.physical_width_mm = 520;
        fallback.physical_height_mm = 290;
        fallback.scale_factor = 1;
        fallback.is_primary = true;
        result.push_back(std::move(fallback));
        return result;
    }

#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
    struct XrrMon {
        std::string name;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int mwidth = 0;
        int mheight = 0;
        bool primary = false;
    };
    std::vector<XrrMon> xrr_monitors;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    const bool is_x11_dpy = GDK_IS_X11_DISPLAY(display);
    Display* xdisplay = is_x11_dpy ? gdk_x11_display_get_xdisplay(display) : nullptr;
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (xdisplay) {
            int nrr = 0;
            XRRMonitorInfo* rrm = XRRGetMonitors(xdisplay, DefaultRootWindow(xdisplay), True, &nrr);
            if (rrm) {
                for (int k = 0; k < nrr; ++k) {
                    XrrMon xm;
                    char* aname = XGetAtomName(xdisplay, rrm[k].name);
                    if (aname) {
                        xm.name = aname;
                        XFree(aname);
                    }
                    xm.x = rrm[k].x;
                    xm.y = rrm[k].y;
                    xm.width = rrm[k].width;
                    xm.height = rrm[k].height;
                    xm.mwidth = rrm[k].mwidth;
                    xm.mheight = rrm[k].mheight;
                    xm.primary = (rrm[k].primary != 0);
                    xrr_monitors.push_back(std::move(xm));
                }
                XRRFreeMonitors(rrm);
            }
        }
#endif

    const guint n = g_list_model_get_n_items(list);
    for (guint i = 0; i < n; ++i) {
        GdkMonitor* mon = GDK_MONITOR(g_list_model_get_item(list, i));
        if (!mon) continue;

        GdkRectangle geom{};
        gdk_monitor_get_geometry(mon, &geom);
        const int sf = std::max(1, gdk_monitor_get_scale_factor(mon));
        const char* conn = gdk_monitor_get_connector(mon);
        const char* desc = gdk_monitor_get_description(mon);
        const char* model = gdk_monitor_get_model(mon);

        MonitorInfo info;
        info.index = static_cast<int32_t>(i);
        info.connector = conn ? conn : "";
        if (desc && desc[0] != '\0') {
            info.description = desc;
        } else if (model && model[0] != '\0') {
            info.description = model;
        }
        info.logical_x = geom.x;
        info.logical_y = geom.y;
        info.logical_width = std::max(1, geom.width);
        info.logical_height = std::max(1, geom.height);
        info.x = geom.x * sf;
        info.y = geom.y * sf;
        info.width = std::max(320, geom.width * sf);
        info.height = std::max(200, geom.height * sf);
        info.physical_width_mm = std::max(0, gdk_monitor_get_width_mm(mon));
        info.physical_height_mm = std::max(0, gdk_monitor_get_height_mm(mon));
        info.scale_factor = sf;
        info.is_primary = false;

#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
        if (!xrr_monitors.empty()) {
            const XrrMon* matched = nullptr;
            if (!info.connector.empty()) {
                for (const auto& xm : xrr_monitors) {
                    if (xm.name == info.connector) {
                        matched = &xm;
                        break;
                    }
                }
            }
            if (!matched && i < xrr_monitors.size()) {
                matched = &xrr_monitors[i];
            }
            if (matched) {
                info.x = matched->x;
                info.y = matched->y;
                if (matched->width >= 320) info.width = matched->width;
                if (matched->height >= 200) info.height = matched->height;
                if (info.physical_width_mm == 0 && matched->mwidth > 0) {
                    info.physical_width_mm = matched->mwidth;
                }
                if (info.physical_height_mm == 0 && matched->mheight > 0) {
                    info.physical_height_mm = matched->mheight;
                }
                info.is_primary = matched->primary;
            }
        }
#endif

        result.push_back(std::move(info));
        g_object_unref(mon);
    }

    if (result.empty()) {
        MonitorInfo fallback;
        fallback.index = 0;
        fallback.logical_width = 1920;
        fallback.logical_height = 1080;
        fallback.width = 1920;
        fallback.height = 1080;
        fallback.physical_width_mm = 520;
        fallback.physical_height_mm = 290;
        fallback.scale_factor = 1;
        fallback.is_primary = true;
        result.push_back(std::move(fallback));
    } else {
        size_t prim = result.size();
        for (size_t i = 0; i < result.size(); ++i) {
            if (result[i].is_primary) {
                prim = i;
                break;
            }
        }
        if (prim == result.size()) {
            for (size_t i = 0; i < result.size(); ++i) {
                if (result[i].logical_x == 0 && result[i].logical_y == 0) {
                    prim = i;
                    break;
                }
            }
        }
        if (prim == result.size()) {
            prim = 0;
        }
        for (size_t i = 0; i < result.size(); ++i) {
            result[i].is_primary = (i == prim);
        }
    }

    return result;
}

GdkMonitor* find_gdk_monitor(GdkDisplay* display, const MonitorInfo& info) {
    if (!display) {
        display = gdk_display_get_default();
    }
    if (!display) return nullptr;

    GListModel* list = gdk_display_get_monitors(display);
    if (!list) return nullptr;
    const guint n = g_list_model_get_n_items(list);
    if (n == 0) return nullptr;

    if (!info.connector.empty()) {
        for (guint i = 0; i < n; ++i) {
            GdkMonitor* mon = GDK_MONITOR(g_list_model_get_item(list, i));
            if (!mon) continue;
            const char* conn = gdk_monitor_get_connector(mon);
            if (conn && info.connector == conn) {
                return mon;
            }
            g_object_unref(mon);
        }
    }

    if (info.index >= 0 && static_cast<guint>(info.index) < n) {
        GdkMonitor* mon = GDK_MONITOR(g_list_model_get_item(list, static_cast<guint>(info.index)));
        if (mon) {
            return mon;
        }
    }

    for (guint i = 0; i < n; ++i) {
        GdkMonitor* mon = GDK_MONITOR(g_list_model_get_item(list, i));
        if (!mon) continue;
        GdkRectangle geom{};
        gdk_monitor_get_geometry(mon, &geom);
        if (geom.x == info.logical_x && geom.y == info.logical_y) {
            return mon;
        }
        g_object_unref(mon);
    }

    return GDK_MONITOR(g_list_model_get_item(list, 0));
}

} // namespace winrdp
