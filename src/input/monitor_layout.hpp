#pragma once

#include "input/monitor_math.hpp"

#include <gtk/gtk.h>

#include <vector>

namespace winrdp {

// Enumerates local GdkMonitors (enriched with exact XRandR primary & physical geometry on X11).
std::vector<MonitorInfo> query_local_monitors(GdkDisplay* display = nullptr);

// Finds the matching GdkMonitor (caller owns returned reference via g_object_unref), or nullptr.
GdkMonitor* find_gdk_monitor(GdkDisplay* display, const MonitorInfo& info);

} // namespace winrdp
