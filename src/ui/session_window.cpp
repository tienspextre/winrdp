#include "ui/session_window.hpp"

#include "config/keyring.hpp"
#include "input/monitor_layout.hpp"
#include "ui/cert_auth_dialogs.hpp"

#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
#include <gdk/x11/gdkx.h>
G_GNUC_END_IGNORE_DEPRECATIONS
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace winrdp {

namespace {

uint64_t steady_ms_now() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void sync_css_flag(GtkWidget* widget, const char* css_class, bool active) {
    if (!widget) return;
    if (active) {
        gtk_widget_add_css_class(widget, css_class);
    } else {
        gtk_widget_remove_css_class(widget, css_class);
    }
}

std::pair<int, int> primary_logical_bounds(GdkDisplay* display) {
    int mon_w = 1920;
    int mon_h = 1080;
    const auto mons = query_local_monitors(display);
    if (!mons.empty()) {
        MonitorInfo primary = mons[0];
        for (const auto& m : mons) {
            if (m.is_primary) {
                primary = m;
                break;
            }
        }
        if (primary.logical_width > 0 && primary.logical_height > 0) {
            mon_w = primary.logical_width;
            mon_h = primary.logical_height;
        } else if (primary.width > 0 && primary.height > 0) {
            mon_w = primary.width;
            mon_h = primary.height;
        }
    }
    return {mon_w, mon_h};
}

bool is_x11_display(GdkDisplay* display) {
#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    return display && GDK_IS_X11_DISPLAY(display);
    G_GNUC_END_IGNORE_DEPRECATIONS
#else
    (void)display;
    return false;
#endif
}

#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
bool query_monitor_workarea_x11(GdkDisplay* display,
                                GdkSurface* surface,
                                int& out_x,
                                int& out_y,
                                int& out_w,
                                int& out_h,
                                int& out_scale) {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    if (!display || !GDK_IS_X11_DISPLAY(display)) return false;
    Display* xdpy = gdk_x11_display_get_xdisplay(display);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (!xdpy) return false;

    GdkMonitor* mon = nullptr;
    if (surface) {
        mon = gdk_display_get_monitor_at_surface(display, surface);
        if (mon) g_object_ref(mon);
    }
    if (!mon) {
        GListModel* list = gdk_display_get_monitors(display);
        if (list && g_list_model_get_n_items(list) > 0) {
            mon = GDK_MONITOR(g_list_model_get_item(list, 0));
        }
    }

    int mon_x = 0;
    int mon_y = 0;
    int mon_w = 1920;
    int mon_h = 1080;
    int scale = 1;
    std::string conn_str;
    if (mon) {
        GdkRectangle geom{};
        gdk_monitor_get_geometry(mon, &geom);
        scale = std::max(1, gdk_monitor_get_scale_factor(mon));
        mon_x = geom.x * scale;
        mon_y = geom.y * scale;
        mon_w = std::max(320, geom.width * scale);
        mon_h = std::max(200, geom.height * scale);
        const char* conn = gdk_monitor_get_connector(mon);
        if (conn) conn_str = conn;
        g_object_unref(mon);
    }

    out_scale = scale;
    const Window root = DefaultRootWindow(xdpy);

    // Prefer exact XRandR monitor device-pixel bounds when available (matches X11 coordinates cleanly)
    const int approx_cx = mon_x + mon_w / 2;
    const int approx_cy = mon_y + mon_h / 2;
    int nrr = 0;
    XRRMonitorInfo* rrm = XRRGetMonitors(xdpy, root, True, &nrr);
    if (rrm && nrr > 0) {
        const XRRMonitorInfo* chosen = nullptr;
        if (!conn_str.empty()) {
            for (int k = 0; k < nrr; ++k) {
                char* aname = XGetAtomName(xdpy, rrm[k].name);
                const bool match = aname && (conn_str == aname);
                if (aname) XFree(aname);
                if (match) {
                    chosen = &rrm[k];
                    break;
                }
            }
        }
        if (!chosen) {
            for (int k = 0; k < nrr; ++k) {
                if (approx_cx >= rrm[k].x && approx_cx < rrm[k].x + rrm[k].width &&
                    approx_cy >= rrm[k].y && approx_cy < rrm[k].y + rrm[k].height) {
                    chosen = &rrm[k];
                    break;
                }
            }
        }
        if (!chosen) {
            for (int k = 0; k < nrr; ++k) {
                if (rrm[k].primary != 0) {
                    chosen = &rrm[k];
                    break;
                }
            }
        }
        if (!chosen) {
            chosen = &rrm[0];
        }
        if (chosen && chosen->width >= 320 && chosen->height >= 200) {
            mon_x = chosen->x;
            mon_y = chosen->y;
            mon_w = chosen->width;
            mon_h = chosen->height;
        }
    }
    if (rrm) {
        XRRFreeMonitors(rrm);
    }

    unsigned long desktop = 0;
    const Atom cur_desk_atom = XInternAtom(xdpy, "_NET_CURRENT_DESKTOP", True);
    if (cur_desk_atom != None) {
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char* prop = nullptr;
        if (XGetWindowProperty(xdpy, root, cur_desk_atom, 0, 1, False, XA_CARDINAL,
                               &type, &format, &nitems, &bytes_after, &prop) == Success &&
            prop) {
            if (type == XA_CARDINAL && format == 32 && nitems >= 1) {
                desktop = reinterpret_cast<const unsigned long*>(prop)[0];
            }
            XFree(prop);
        }
    }

    bool have_wa = false;
    int wa_x = mon_x;
    int wa_y = mon_y;
    int wa_w = mon_w;
    int wa_h = mon_h;
    const Atom wa_atom = XInternAtom(xdpy, "_NET_WORKAREA", True);
    if (wa_atom != None) {
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char* prop = nullptr;
        if (XGetWindowProperty(xdpy, root, wa_atom, 0, 256, False, XA_CARDINAL,
                               &type, &format, &nitems, &bytes_after, &prop) == Success &&
            prop) {
            if (type == XA_CARDINAL && format == 32 && nitems >= 4) {
                const unsigned long* vals = reinterpret_cast<const unsigned long*>(prop);
                const unsigned long base = (desktop * 4 + 3 < nitems) ? (desktop * 4) : 0;
                wa_x = static_cast<int>(vals[base + 0]);
                wa_y = static_cast<int>(vals[base + 1]);
                wa_w = static_cast<int>(vals[base + 2]);
                wa_h = static_cast<int>(vals[base + 3]);
                have_wa = (wa_w >= 320 && wa_h >= 200);
            }
            XFree(prop);
        }
    }

    int use_x = mon_x;
    int use_y = mon_y + 40 * scale;
    int use_w = mon_w;
    int use_h = std::max(300, mon_h - 80 * scale);
    if (have_wa) {
        const int ix0 = std::max(mon_x, wa_x);
        const int iy0 = std::max(mon_y, wa_y);
        const int ix1 = std::min(mon_x + mon_w, wa_x + wa_w);
        const int iy1 = std::min(mon_y + mon_h, wa_y + wa_h);
        if (ix1 - ix0 >= 320 && iy1 - iy0 >= 200) {
            use_x = ix0;
            use_y = iy0;
            use_w = ix1 - ix0;
            use_h = iy1 - iy0;
        }
    }

    // Dual-monitor _NET_WORKAREA on GNOME/Ubuntu often reports wa_y == 0 despite the top panel
    const int min_top_y = mon_y + 40 * scale;
    if (use_y < min_top_y) {
        const int delta = min_top_y - use_y;
        use_y = min_top_y;
        use_h = std::max(240, use_h - delta);
    }

    out_x = use_x;
    out_y = use_y;
    out_w = use_w;
    out_h = use_h;
    return true;
}

void send_net_moveresize_x11(Display* xdpy,
                             Window xwin,
                             int outer_x,
                             int outer_y,
                             int outer_w,
                             int outer_h) {
    const Atom mr_atom = XInternAtom(xdpy, "_NET_MOVERESIZE_WINDOW", False);
    if (mr_atom != None) {
        // StaticGravity (10) | X | Y | Width | Height | normal application source (1 << 12)
        const long flags = 10L | (1L << 8) | (1L << 9) | (1L << 10) | (1L << 11) | (1L << 12);
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.serial = 0;
        ev.xclient.send_event = True;
        ev.xclient.message_type = mr_atom;
        ev.xclient.window = xwin;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = flags;
        ev.xclient.data.l[1] = outer_x;
        ev.xclient.data.l[2] = outer_y;
        ev.xclient.data.l[3] = outer_w;
        ev.xclient.data.l[4] = outer_h;
        const Window root = DefaultRootWindow(xdpy);
        XSendEvent(xdpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    }
    XMoveWindow(xdpy, xwin, outer_x, outer_y);
    XFlush(xdpy);
}

void query_frame_extents_x11(Display* xdpy,
                             Window xwin,
                             int& ext_left,
                             int& ext_right,
                             int& ext_top,
                             int& ext_bottom) {
    ext_left = ext_right = ext_top = ext_bottom = 0;
    const Atom ext_atom = XInternAtom(xdpy, "_GTK_FRAME_EXTENTS", True);
    if (ext_atom == None) return;

    Atom type = None;
    int format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char* prop = nullptr;
    if (XGetWindowProperty(xdpy, xwin, ext_atom, 0, 4, False, XA_CARDINAL,
                           &type, &format, &nitems, &bytes_after, &prop) == Success &&
        prop) {
        if (type == XA_CARDINAL && format == 32 && nitems >= 4) {
            const unsigned long* vals = reinterpret_cast<const unsigned long*>(prop);
            ext_left = static_cast<int>(vals[0]);
            ext_right = static_cast<int>(vals[1]);
            ext_top = static_cast<int>(vals[2]);
            ext_bottom = static_cast<int>(vals[3]);
        }
        XFree(prop);
    }
}
#endif

} // namespace

SessionWindow::SessionWindow(GtkApplication* app,
                             ConnectionProfile profile,
                             SessionWindowCallbacks callbacks,
                             bool smoke_test_mode,
                             uint64_t session_id,
                             GtkWindow* placement_parent)
    : app_(app),
      profile_(std::move(profile)),
      callbacks_(std::move(callbacks)),
      smoke_test_mode_(smoke_test_mode),
      session_id_(session_id),
      placement_parent_(placement_parent) {
    if (placement_parent_) {
        g_object_add_weak_pointer(G_OBJECT(placement_parent_), reinterpret_cast<gpointer*>(&placement_parent_));
    }
    window_ = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window_), ("WinRDP — " + profile_.display_title()).c_str());
    gtk_window_set_icon_name(GTK_WINDOW(window_), "io.winrdp.Client");

    compute_initial_windowed_size();
    gtk_window_set_default_size(GTK_WINDOW(window_), windowed_w_, windowed_h_);

    setup_ui();

    g_signal_connect(
        window_,
        "close-request",
        G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->closing_ = true;
            self->closed_ = true;
            self->cancel_initial_bootstrap();
            self->detach_primary_state_observer();
            self->detach_surface_monitor_guard(self->primary_bound_surface_, self->primary_enter_handler_);
            self->suspend_coordinated_grab(true);
            self->destroy_secondary_panes();
            if (self->session_) {
                self->session_->disconnect();
            }
            if (self->callbacks_.on_closed) {
                auto cb = std::move(self->callbacks_.on_closed);
                self->callbacks_.on_closed = nullptr;
                cb();
            }
            return FALSE;
        }),
        this);

    g_signal_connect(
        window_,
        "destroy",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->cancel_initial_bootstrap();
            self->primary_state_surface_ = nullptr;
            self->primary_state_handler_ = 0;
            self->primary_bound_surface_ = nullptr;
            self->primary_enter_handler_ = 0;
            self->window_ = nullptr;
            self->toolbar_view_ = nullptr;
            self->header_bar_ = nullptr;
            self->header_title_ = nullptr;
            self->hdr_grab_btn_ = nullptr;
            self->hdr_cad_btn_ = nullptr;
            self->hdr_sizing_btn_ = nullptr;
            self->hdr_multimon_btn_ = nullptr;
            self->hdr_fs_btn_ = nullptr;
            self->overlay_ = nullptr;
            self->closed_ = true;
        }),
        this);

    g_signal_connect(
        window_,
        "realize",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->attach_surface_monitor_guard(
                GTK_WINDOW(self->window_), self->primary_bound_surface_, self->primary_enter_handler_);
            self->attach_primary_state_observer(gtk_native_get_surface(GTK_NATIVE(self->window_)));
        }),
        this);

    g_signal_connect(
        window_,
        "unrealize",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->detach_primary_state_observer();
            self->detach_surface_monitor_guard(self->primary_bound_surface_, self->primary_enter_handler_);
        }),
        this);

    g_signal_connect(
        window_,
        "notify::fullscreened",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            if (!self->window_ || self->closing_) return;
            const bool fs = gtk_window_is_fullscreen(GTK_WINDOW(self->window_)) == TRUE;
            if (self->input_) {
                self->input_->set_fullscreen(fs);
            }
            if (!fs && (self->is_fullscreen_ || self->pending_unfullscreen_reposition_)) {
                self->is_fullscreen_ = false;
                if (self->is_multimon_active_) {
                    self->destroy_secondary_panes();
                    self->is_multimon_active_ = false;
                    self->current_layout_ = VirtualDesktopLayout{};
                    if (self->canvas_) self->canvas_->clear_source_rect();
                    if (self->bar_) self->bar_->set_multimon(false, 1);
                }
                self->update_window_chrome();
                gtk_window_unmaximize(GTK_WINDOW(self->window_));
                gtk_window_set_default_size(
                    GTK_WINDOW(self->window_), self->windowed_w_, self->windowed_h_);

                GdkSurface* surf = gtk_native_get_surface(GTK_NATIVE(self->window_));
                bool is_x11 = false;
#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                is_x11 = surf && GDK_IS_X11_SURFACE(surf);
                G_GNUC_END_IGNORE_DEPRECATIONS
#else
                (void)surf;
#endif
                if (is_x11) {
                    self->pending_unfullscreen_reposition_ = true;
                    self->reposition_attempts_ = 0;
                    self->schedule_windowed_reposition(60);
                }
            } else if (!fs && gtk_window_is_maximized(GTK_WINDOW(self->window_)) == TRUE) {
                gtk_window_unmaximize(GTK_WINDOW(self->window_));
            } else if (fs && !self->is_fullscreen_) {
                self->is_fullscreen_ = true;
                self->pending_unfullscreen_reposition_ = false;
                if (self->reposition_timer_id_ != 0) {
                    g_source_remove(self->reposition_timer_id_);
                    self->reposition_timer_id_ = 0;
                }
                self->update_window_chrome();
            }
            if (self->canvas_) {
                self->canvas_->request_redraw();
            }
            self->sync_coordinated_grab();
        }),
        this);

    auto on_active_notify = +[](GObject*, GParamSpec*, gpointer data) {
        auto* self = static_cast<SessionWindow*>(data);
        if (!self->window_) return;
        if (gtk_window_is_active(GTK_WINDOW(self->window_)) == TRUE &&
            self->initial_windowed_frame_committed_ &&
            (self->pending_initial_fullscreen_ || self->temp_transient_attached_) &&
            self->initial_fs_idle_id_ == 0) {
            struct ActiveBootstrapCtx {
                SessionWindow* self;
                std::weak_ptr<Alive> alive;
                bool enter_fs;
            };
            auto* ctx = new ActiveBootstrapCtx{
                self, std::weak_ptr<Alive>(self->alive_), self->initial_want_fullscreen_};
            self->initial_fs_idle_id_ = g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer idata) -> gboolean {
                    auto* ic = static_cast<ActiveBootstrapCtx*>(idata);
                    if (auto ia = ic->alive.lock(); ia && ia->valid) {
                        ic->self->initial_fs_idle_id_ = 0;
                        ic->self->finish_initial_wayland_bootstrap(ic->enter_fs);
                    }
                    return G_SOURCE_REMOVE;
                },
                ctx,
                +[](gpointer idata) { delete static_cast<ActiveBootstrapCtx*>(idata); });
        }
        self->on_pane_active_changed(GTK_WINDOW(self->window_));
    };
    g_signal_connect(window_, "notify::is-active", G_CALLBACK(on_active_notify), this);

    if (!smoke_test_mode_) {
        start_rdp_session();
    }
}

SessionWindow::~SessionWindow() {
    alive_->valid = false;
    cancel_initial_bootstrap();
    if (placement_parent_) {
        g_object_remove_weak_pointer(G_OBJECT(placement_parent_), reinterpret_cast<gpointer*>(&placement_parent_));
        placement_parent_ = nullptr;
    }
    detach_monitor_hotplug();
    detach_primary_state_observer();
    detach_surface_monitor_guard(primary_bound_surface_, primary_enter_handler_);
    if (group_raise_guard_timer_id_ != 0) {
        g_source_remove(group_raise_guard_timer_id_);
        group_raise_guard_timer_id_ = 0;
    }
    if (toast_timer_id_ != 0) {
        g_source_remove(toast_timer_id_);
        toast_timer_id_ = 0;
    }
    if (resize_timer_id_ != 0) {
        g_source_remove(resize_timer_id_);
        resize_timer_id_ = 0;
    }
    if (focus_leave_timer_id_ != 0) {
        g_source_remove(focus_leave_timer_id_);
        focus_leave_timer_id_ = 0;
    }
    if (hotplug_timer_id_ != 0) {
        g_source_remove(hotplug_timer_id_);
        hotplug_timer_id_ = 0;
    }
    if (repin_timer_id_ != 0) {
        g_source_remove(repin_timer_id_);
        repin_timer_id_ = 0;
    }
    if (reposition_timer_id_ != 0) {
        g_source_remove(reposition_timer_id_);
        reposition_timer_id_ = 0;
    }
    suspend_coordinated_grab(true);
    destroy_secondary_panes();
    if (session_) {
        session_->disconnect();
    }
    input_.reset();
    bar_.reset();
    canvas_.reset();
    if (window_) {
        g_signal_handlers_disconnect_by_data(window_, this);
        GtkWidget* w = GTK_WIDGET(window_);
        window_ = nullptr;
        gtk_window_destroy(GTK_WINDOW(w));
    }
}

void SessionWindow::compute_initial_windowed_size() {
    GdkDisplay* display = window_ ? gtk_widget_get_display(GTK_WIDGET(window_)) : nullptr;
    const auto [mon_w, mon_h] = primary_logical_bounds(display);

    if (smoke_test_mode_) {
        windowed_w_ = std::clamp<int>(static_cast<int>(profile_.width), 320, 1920);
        windowed_h_ = std::clamp<int>(static_cast<int>(profile_.height), 240, 1080);
        return;
    }

    if (profile_.display_mode == DisplayMode::Windowed) {
        const int max_w = std::max(640, mon_w - 120);
        const int max_h = std::max(480, mon_h - 160);
        windowed_w_ = std::clamp<int>(static_cast<int>(profile_.width), 640, max_w);
        windowed_h_ = std::clamp<int>(static_cast<int>(profile_.height), 480, max_h);
        return;
    }

    // Leave generous margin around all screen edges and below Mutter's 80% auto-maximize threshold
    const int cap_w = std::max(800, std::min(1360, mon_w - 160));
    const int cap_h = std::max(500, std::min(760, mon_h - 180));
    windowed_w_ = std::clamp<int>((mon_w * 72) / 100, 640, cap_w);
    windowed_h_ = std::clamp<int>((mon_h * 70) / 100, 440, cap_h);
}

bool SessionWindow::is_header_bar_visible() const {
    return header_bar_ && gtk_widget_get_visible(header_bar_) == TRUE;
}

int SessionWindow::header_bar_height() const {
    return header_bar_ ? gtk_widget_get_height(header_bar_) : 0;
}

void SessionWindow::update_window_chrome() {
    const bool show_header = !is_fullscreen_;
    if (header_bar_) {
        gtk_widget_set_visible(header_bar_, show_header ? TRUE : FALSE);
    }
    if (toolbar_view_) {
        adw_toolbar_view_set_reveal_top_bars(toolbar_view_, show_header ? TRUE : FALSE);
    }
    if (bar_) {
        bar_->set_fullscreen(is_fullscreen_);
        if (!smoke_test_mode_) {
            bar_->set_enabled(is_fullscreen_);
        }
    }
}

void SessionWindow::update_header_telemetry(const SessionTelemetry& t) {
    if (!header_title_) return;
    char mon_str[32] = "";
    if (t.monitor_count > 1) {
        std::snprintf(mon_str, sizeof(mon_str), " (%u monitors)", t.monitor_count);
    }
    std::string codec = t.codec;
    if (t.hw_accel && (t.codec == "AVC444" || t.codec == "AVC420")) {
        codec += " VA";
    }
    char buf[160];
    if (t.fps >= 1.0) {
        std::snprintf(buf, sizeof(buf), "%u×%u%s • %s • %.0ffps",
                      t.width, t.height, mon_str, codec.c_str(), t.fps);
    } else {
        std::snprintf(buf, sizeof(buf), "%u×%u%s • %s",
                      t.width, t.height, mon_str, codec.c_str());
    }
    adw_window_title_set_subtitle(header_title_, buf);
}

void SessionWindow::toggle_smart_sizing() {
    profile_.smart_sizing = !profile_.smart_sizing;
    if (canvas_) canvas_->set_smart_sizing(profile_.smart_sizing);
    for (const auto& pane : secondary_panes_) {
        if (pane && pane->canvas) {
            pane->canvas->set_smart_sizing(profile_.smart_sizing);
        }
    }
    if (bar_) bar_->set_smart_sizing(profile_.smart_sizing);
    sync_css_flag(hdr_sizing_btn_, "winrdp-hdr-active", profile_.smart_sizing);
    show_toast(profile_.smart_sizing ? "Smart Sizing enabled" : "1:1 Native Scaling");
    if (callbacks_.on_profile_changed) callbacks_.on_profile_changed(profile_);
    if (canvas_) gtk_widget_grab_focus(canvas_->widget());
}

void SessionWindow::send_ctrl_alt_del() {
    if (session_) {
        session_->send_ctrl_alt_del();
        show_toast("Sent Ctrl+Alt+Del");
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
    }
}

void SessionWindow::apply_skip_taskbar_hint(GtkWindow* win) {
#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
    if (!win) return;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(win));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    if (!surface || !GDK_IS_X11_SURFACE(surface)) return;

    GdkDisplay* display = gdk_surface_get_display(surface);
    Display* xdisplay = display ? gdk_x11_display_get_xdisplay(display) : nullptr;
    const Window xwin = gdk_x11_surface_get_xid(surface);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (!xdisplay || xwin == None) return;

    const Atom wm_state = XInternAtom(xdisplay, "_NET_WM_STATE", False);
    const Atom skip_tb = XInternAtom(xdisplay, "_NET_WM_STATE_SKIP_TASKBAR", False);
    const Atom skip_pg = XInternAtom(xdisplay, "_NET_WM_STATE_SKIP_PAGER", False);
    if (wm_state == None || skip_tb == None || skip_pg == None) return;

    if (gtk_widget_get_mapped(GTK_WIDGET(win)) == FALSE) {
        bool has_tb = false;
        bool has_pg = false;
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char* prop = nullptr;
        if (XGetWindowProperty(xdisplay, xwin, wm_state, 0, 64, False, XA_ATOM,
                               &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success &&
            prop) {
            if (actual_type == XA_ATOM && actual_format == 32) {
                const Atom* atoms = reinterpret_cast<const Atom*>(prop);
                for (unsigned long k = 0; k < nitems; ++k) {
                    if (atoms[k] == skip_tb) has_tb = true;
                    if (atoms[k] == skip_pg) has_pg = true;
                }
            }
            XFree(prop);
        }
        Atom add[2];
        int n_add = 0;
        if (!has_tb) add[n_add++] = skip_tb;
        if (!has_pg) add[n_add++] = skip_pg;
        if (n_add > 0) {
            XChangeProperty(xdisplay, xwin, wm_state, XA_ATOM, 32, PropModeAppend,
                            reinterpret_cast<const unsigned char*>(add), n_add);
        }
    }

    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.serial = 0;
    ev.xclient.send_event = True;
    ev.xclient.message_type = wm_state;
    ev.xclient.window = xwin;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
    ev.xclient.data.l[1] = static_cast<long>(skip_tb);
    ev.xclient.data.l[2] = static_cast<long>(skip_pg);
    ev.xclient.data.l[3] = 1; // source indication: normal app
    ev.xclient.data.l[4] = 0;

    const Window root = DefaultRootWindow(xdisplay);
    XSendEvent(xdisplay, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(xdisplay);
#else
    (void)win;
#endif
}

void SessionWindow::release_all_grabs_immediately() {
    release_all_pane_keys();
    if (input_) {
        input_->force_grab(false);
    }
    for (const auto& pane : secondary_panes_) {
        if (pane && pane->input) {
            pane->input->force_grab(false);
        }
    }
    active_grab_window_ = nullptr;
    if (bar_) {
        bar_->set_grabbed(false);
    }
    sync_css_flag(hdr_grab_btn_, "winrdp-hdr-active", false);
}

void SessionWindow::setup_ui() {
    toolbar_view_ = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_set_top_bar_style(toolbar_view_, ADW_TOOLBAR_RAISED);

    header_bar_ = adw_header_bar_new();
    header_title_ = ADW_WINDOW_TITLE(
        adw_window_title_new(profile_.display_title().c_str(), profile_.hostname.c_str()));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header_bar_), GTK_WIDGET(header_title_));

    auto make_hdr_btn = [](const char* icon_name, const char* tooltip) {
        GtkWidget* btn = gtk_button_new_from_icon_name(icon_name);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_set_focus_on_click(btn, FALSE);
        if (tooltip) gtk_widget_set_tooltip_text(btn, tooltip);
        return btn;
    };

    hdr_grab_btn_ = make_hdr_btn("input-keyboard-symbolic", "Toggle System Key Capture (Right Ctrl)");
    hdr_cad_btn_ = make_hdr_btn("system-lock-screen-symbolic", "Send Ctrl+Alt+Del");
    hdr_sizing_btn_ = make_hdr_btn("zoom-fit-best-symbolic", "Toggle Smart Sizing");
    hdr_fs_btn_ = make_hdr_btn("view-fullscreen-symbolic", "Enter Fullscreen (Ctrl+Alt+Enter)");
    hdr_multimon_btn_ = make_hdr_btn("video-joined-displays-symbolic", "Toggle Multi-Monitor (Ctrl+Alt+M)");

    sync_css_flag(hdr_sizing_btn_, "winrdp-hdr-active", profile_.smart_sizing);

    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), hdr_grab_btn_);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), hdr_cad_btn_);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), hdr_sizing_btn_);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), hdr_fs_btn_);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), hdr_multimon_btn_);

    g_signal_connect(
        hdr_grab_btn_, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->toggle_coordinated_grab();
            if (self->canvas_) gtk_widget_grab_focus(self->canvas_->widget());
        }),
        this);
    g_signal_connect(
        hdr_cad_btn_, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<SessionWindow*>(data)->send_ctrl_alt_del();
        }),
        this);
    g_signal_connect(
        hdr_sizing_btn_, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<SessionWindow*>(data)->toggle_smart_sizing();
        }),
        this);
    g_signal_connect(
        hdr_fs_btn_, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->toggle_fullscreen();
            if (self->canvas_) gtk_widget_grab_focus(self->canvas_->widget());
        }),
        this);
    g_signal_connect(
        hdr_multimon_btn_, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->toggle_multimon();
            if (self->canvas_) gtk_widget_grab_focus(self->canvas_->widget());
        }),
        this);

    adw_toolbar_view_add_top_bar(toolbar_view_, header_bar_);

    overlay_ = GTK_OVERLAY(gtk_overlay_new());
    adw_toolbar_view_set_content(toolbar_view_, GTK_WIDGET(overlay_));
    adw_application_window_set_content(window_, GTK_WIDGET(toolbar_view_));

    canvas_ = std::make_unique<GlCanvas>();
    canvas_->set_smart_sizing(profile_.smart_sizing);
    gtk_overlay_set_child(overlay_, canvas_->widget());

    // Debounced resize observer directly on GtkGLArea pixel size
    g_signal_connect(
        canvas_->widget(),
        "resize",
        G_CALLBACK(+[](GtkGLArea*, gint width, gint height, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->schedule_dynamic_resize(width, height);
        }),
        this);

    // Windows mstsc.exe-style Connection Bar inside same GtkOverlay (zero X11 popup flicker!)
    ConnectionBarCallbacks bar_cb;
    bar_cb.on_mode_changed = [this](ConnectionBarMode mode, bool hide_hover) {
        profile_.connection_bar_mode = mode;
        profile_.connection_bar_hide_hover = hide_hover;
        if (mode == ConnectionBarMode::Hidden) {
            show_toast("Connection bar hidden — press Ctrl+Alt+Home to show");
        } else if (mode == ConnectionBarMode::Pinned && hide_hover) {
            show_toast("Pinned — unpin to use Ctrl+Alt+Home hide mode");
        }
        if (callbacks_.on_profile_changed) {
            callbacks_.on_profile_changed(profile_);
        }
    };
    bar_cb.on_toggle_grab = [this]() {
        toggle_coordinated_grab();
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
    };
    bar_cb.on_send_cad = [this]() {
        send_ctrl_alt_del();
    };
    bar_cb.on_toggle_smart_sizing = [this]() {
        toggle_smart_sizing();
    };
    bar_cb.on_minimize = [this]() {
        release_all_grabs_immediately();
        if (window_) gtk_window_minimize(GTK_WINDOW(window_));
        for (const auto& pane : secondary_panes_) {
            if (pane && pane->window) {
                gtk_window_minimize(pane->window);
            }
        }
    };
    bar_cb.on_toggle_multimon = [this]() {
        toggle_multimon();
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
    };
    bar_cb.on_toggle_fullscreen = [this]() {
        toggle_fullscreen();
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
    };
    bar_cb.on_disconnect = [this]() {
        close();
    };

    bar_ = std::make_unique<ConnectionBar>(
        profile_.display_title(),
        profile_.connection_bar_mode,
        profile_.connection_bar_hide_hover,
        std::move(bar_cb));
    bar_->set_smart_sizing(profile_.smart_sizing);
    bar_->attach_to_overlay(overlay_);

    // Translucent centered status banner (Connecting / Reconnecting / Error)
    status_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(status_box_, "winrdp-status-banner");
    gtk_widget_set_halign(status_box_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(status_box_, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(status_box_, FALSE);

    status_spinner_ = gtk_spinner_new();
    gtk_widget_set_size_request(status_spinner_, 22, 22);

    GtkWidget* text_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    status_title_ = gtk_label_new("Connecting...");
    gtk_widget_set_halign(status_title_, GTK_ALIGN_START);
    gtk_widget_add_css_class(status_title_, "heading");
    status_detail_ = gtk_label_new("");
    gtk_widget_set_halign(status_detail_, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(status_detail_), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(status_detail_), 50);
    gtk_box_append(GTK_BOX(text_col), status_title_);
    gtk_box_append(GTK_BOX(text_col), status_detail_);

    status_retry_btn_ = gtk_button_new_with_label("Reconnect");
    gtk_widget_add_css_class(status_retry_btn_, "suggested-action");
    gtk_widget_set_visible(status_retry_btn_, FALSE);

    status_cancel_btn_ = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(status_cancel_btn_, "destructive-action");

    gtk_box_append(GTK_BOX(status_box_), status_spinner_);
    gtk_box_append(GTK_BOX(status_box_), text_col);
    gtk_box_append(GTK_BOX(status_box_), status_retry_btn_);
    gtk_box_append(GTK_BOX(status_box_), status_cancel_btn_);

    g_signal_connect(
        status_retry_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->destroy_secondary_panes();
            if (self->session_) {
                self->session_->disconnect();
            }
            self->start_rdp_session();
            if (self->profile_.display_mode == DisplayMode::MultiMonitor) {
                self->apply_fullscreen_state(true);
            }
        }),
        this);

    g_signal_connect(
        status_cancel_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<SessionWindow*>(data)->close();
        }),
        this);

    gtk_overlay_add_overlay(overlay_, status_box_);

    // Translucent HUD Toast (bottom-center, non-interactive)
    toast_revealer_ = GTK_REVEALER(gtk_revealer_new());
    gtk_revealer_set_transition_type(toast_revealer_, GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(toast_revealer_, 180);
    gtk_widget_set_halign(GTK_WIDGET(toast_revealer_), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(toast_revealer_), GTK_ALIGN_END);
    gtk_widget_set_can_target(GTK_WIDGET(toast_revealer_), FALSE);

    toast_label_ = gtk_label_new("");
    gtk_widget_add_css_class(toast_label_, "winrdp-hud-toast");
    gtk_revealer_set_child(toast_revealer_, toast_label_);
    gtk_overlay_add_overlay(overlay_, GTK_WIDGET(toast_revealer_));

    // Window-level local shortcuts in CAPTURE phase so Ctrl+Alt+Enter / Home / M work
    // even when focus is on status banner, ConnectionBar, or HeaderBar (not only canvas_).
    GtkEventController* win_key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(win_key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(
        win_key_ctrl,
        "key-pressed",
        G_CALLBACK(+[](GtkEventControllerKey*,
                       guint keyval,
                       guint,
                       GdkModifierType state,
                       gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            const bool ctrl = (state & GDK_CONTROL_MASK) != 0;
            const bool alt = (state & GDK_ALT_MASK) != 0;
            if (!ctrl || !alt) {
                return FALSE;
            }
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
                self->toggle_fullscreen();
                return TRUE;
            }
            if (keyval == GDK_KEY_Home || keyval == GDK_KEY_Pause || keyval == GDK_KEY_Break) {
                if (self->bar_) {
                    self->bar_->toggle_visibility_hotkey();
                    if (!self->bar_->is_pinned() && self->bar_->is_hide_hover() &&
                        self->bar_->is_revealed()) {
                        self->show_toast("Connection bar shown — auto-hides or press Ctrl+Alt+Home");
                    }
                }
                return TRUE;
            }
            if (keyval == GDK_KEY_m || keyval == GDK_KEY_M) {
                self->toggle_multimon();
                return TRUE;
            }
            return FALSE;
        }),
        this);
    gtk_widget_add_controller(GTK_WIDGET(window_), win_key_ctrl);

    update_window_chrome();
}

bool SessionWindow::map_pane_pointer(size_t vp_idx,
                                     GlCanvas* canvas,
                                     double wx,
                                     double wy,
                                     int& rx,
                                     int& ry) const {
    if (!canvas || !canvas->widget()) return false;
    const int ww = gtk_widget_get_width(canvas->widget());
    const int wh = gtk_widget_get_height(canvas->widget());
    if (is_multimon_active_ &&
        vp_idx < current_layout_.viewports.size() &&
        current_layout_.viewports.size() > 1 &&
        ww > 0 && wh > 0 &&
        (wx < 0.0 || wy < 0.0 || wx >= static_cast<double>(ww) || wy >= static_cast<double>(wh))) {
        return map_multimon_pointer_to_remote(
            current_layout_,
            vp_idx,
            wx,
            wy,
            static_cast<double>(ww),
            static_cast<double>(wh),
            rx,
            ry);
    }
    return canvas->map_to_remote(wx, wy, rx, ry);
}

void SessionWindow::start_rdp_session() {
    auto all_monitors = query_local_monitors(gtk_widget_get_display(GTK_WIDGET(window_)));
    auto selected = filter_selected_monitors(all_monitors, profile_.selected_monitors);
    ConnectionProfile session_profile = profile_;
    if (session_profile.display_mode == DisplayMode::Fullscreen && !selected.empty()) {
        session_profile.width = static_cast<uint32_t>(selected[0].width);
        session_profile.height = static_cast<uint32_t>(selected[0].height);
    }

    SessionCallbacks scb;
    std::weak_ptr<Alive> weak_alive = alive_;

    scb.on_frame_ready = [this, weak_alive]() {
        g_idle_add(
            +[](gpointer data) -> gboolean {
                auto* pair = static_cast<std::pair<SessionWindow*, std::weak_ptr<Alive>>*>(data);
                if (auto a = pair->second.lock(); a && a->valid && !pair->first->closing_) {
                    SessionWindow* self = pair->first;
                    if (self->session_) self->session_->clear_frame_pending();
                    if (self->canvas_) self->canvas_->request_redraw();
                    for (const auto& pane : self->secondary_panes_) {
                        if (pane && pane->canvas) {
                            pane->canvas->request_redraw();
                        }
                    }
                }
                delete pair;
                return G_SOURCE_REMOVE;
            },
            new std::pair<SessionWindow*, std::weak_ptr<Alive>>(this, weak_alive));
    };

    scb.on_desktop_resized = [this, weak_alive](uint32_t, uint32_t) {
        g_idle_add(
            +[](gpointer data) -> gboolean {
                auto* pair = static_cast<std::pair<SessionWindow*, std::weak_ptr<Alive>>*>(data);
                if (auto a = pair->second.lock(); a && a->valid && !pair->first->closing_) {
                    SessionWindow* self = pair->first;
                    if (self->is_multimon_active_ && self->session_) {
                        const auto layout = self->session_->active_layout();
                        if (layout.viewports.size() > 1 &&
                            layout.viewports.size() == self->secondary_panes_.size() + 1) {
                            self->current_layout_ = layout;
                            self->canvas_->set_source_rect(
                                layout.viewports[0].fb_x,
                                layout.viewports[0].fb_y,
                                static_cast<int>(layout.viewports[0].fb_width),
                                static_cast<int>(layout.viewports[0].fb_height));
                            for (size_t i = 1; i < layout.viewports.size(); ++i) {
                                const auto& vp = layout.viewports[i];
                                self->secondary_panes_[i - 1]->viewport = vp;
                                self->secondary_panes_[i - 1]->canvas->set_source_rect(
                                    vp.fb_x,
                                    vp.fb_y,
                                    static_cast<int>(vp.fb_width),
                                    static_cast<int>(vp.fb_height));
                            }
                        }
                    }
                    if (self->canvas_) self->canvas_->request_redraw();
                    for (const auto& pane : self->secondary_panes_) {
                        if (pane && pane->canvas) {
                            pane->canvas->request_redraw();
                        }
                    }
                }
                delete pair;
                return G_SOURCE_REMOVE;
            },
            new std::pair<SessionWindow*, std::weak_ptr<Alive>>(this, weak_alive));
    };

    scb.on_disp_ready = [this, weak_alive](uint32_t /*max_monitors*/) {
        g_timeout_add(
            150,
            +[](gpointer data) -> gboolean {
                auto* pair = static_cast<std::pair<SessionWindow*, std::weak_ptr<Alive>>*>(data);
                if (auto a = pair->second.lock(); a && a->valid && !pair->first->closing_) {
                    SessionWindow* self = pair->first;
                    if (self->is_multimon_active_ &&
                        self->session_ &&
                        self->session_->is_connected() &&
                        self->current_layout_.viewports.size() > 1) {
                        const auto& fb = self->session_->framebuffer();
                        if (fb.width() != self->current_layout_.desktop_width ||
                            fb.height() != self->current_layout_.desktop_height) {
                            std::vector<MonitorInfo> mons;
                            mons.reserve(self->current_layout_.viewports.size());
                            for (const auto& vp : self->current_layout_.viewports) {
                                mons.push_back(vp.monitor);
                            }
                            self->session_->request_monitor_layout(mons, self->profile_.scale_factor);
                        }
                    }
                }
                delete pair;
                return G_SOURCE_REMOVE;
            },
            new std::pair<SessionWindow*, std::weak_ptr<Alive>>(this, weak_alive));
    };

    scb.on_state_changed = [this, weak_alive](SessionState state, const std::string& msg, uint32_t attempt, uint32_t max_attempts) {
        struct StateMsg {
            SessionWindow* win;
            std::weak_ptr<Alive> alive;
            SessionState state;
            std::string msg;
            uint32_t attempt;
            uint32_t max_attempts;
        };
        auto* sm = new StateMsg{this, weak_alive, state, msg, attempt, max_attempts};
        g_idle_add(
            +[](gpointer data) -> gboolean {
                std::unique_ptr<StateMsg> m(static_cast<StateMsg*>(data));
                if (auto a = m->alive.lock(); a && a->valid && !m->win->closing_) {
                    m->win->update_status_banner(m->state, m->msg, m->attempt, m->max_attempts);
                }
                return G_SOURCE_REMOVE;
            },
            sm);
    };

    scb.on_telemetry = [this, weak_alive](const SessionTelemetry& t) {
        struct TelMsg {
            SessionWindow* win;
            std::weak_ptr<Alive> alive;
            SessionTelemetry t;
        };
        auto* tm = new TelMsg{this, weak_alive, t};
        g_idle_add(
            +[](gpointer data) -> gboolean {
                std::unique_ptr<TelMsg> m(static_cast<TelMsg*>(data));
                if (auto a = m->alive.lock(); a && a->valid && !m->win->closing_) {
                    if (m->win->bar_) m->win->bar_->update_telemetry(m->t);
                    m->win->update_header_telemetry(m->t);
                }
                return G_SOURCE_REMOVE;
            },
            tm);
    };

    scb.on_verify_certificate = [this, weak_alive](const CertPromptInfo& info) -> uint32_t {
        if (closing_) return 0;
        return prompt_certificate_sync(GTK_WINDOW(window_), info, [this, weak_alive](bool suspend) {
            if (auto a = weak_alive.lock(); a && a->valid) {
                suspend_coordinated_grab(suspend);
            }
        });
    };

    scb.on_authenticate = [this, weak_alive](AuthPromptInfo& info) -> bool {
        if (closing_) return false;
        const bool ok = prompt_authentication_sync(
            GTK_WINDOW(window_), info, [this, weak_alive](bool suspend) {
                if (auto a = weak_alive.lock(); a && a->valid) {
                    suspend_coordinated_grab(suspend);
                }
            });
        if (ok && !info.is_gateway) {
            const auto norm = normalize_rdp_credentials(info.username, info.domain);
            info.username = norm.username;
            info.domain = norm.domain;

            if (!profile_.id.empty()) {
                if (info.save_password) {
                    Keyring::store_secret(profile_.id, "password",
                                          "WinRDP: " + profile_.display_title(), info.password);
                } else {
                    Keyring::clear_secret(profile_.id, "password");
                }
            }

            struct AuthPersistMsg {
                SessionWindow* win;
                std::weak_ptr<Alive> alive;
                std::string username;
                std::string domain;
                std::string password;
                bool save_password;
                bool remember_username;
            };
            auto* msg = new AuthPersistMsg{
                this,
                weak_alive,
                norm.username,
                norm.domain,
                info.password,
                info.save_password,
                info.remember_username
            };
            g_idle_add(
                +[](gpointer data) -> gboolean {
                    std::unique_ptr<AuthPersistMsg> m(static_cast<AuthPersistMsg*>(data));
                    if (auto a = m->alive.lock(); a && a->valid && !m->win->closing_) {
                        SessionWindow* self = m->win;
                        self->profile_.save_password = m->save_password;
                        self->profile_.remember_username = m->remember_username;
                        if (m->remember_username || m->save_password) {
                            self->profile_.username = m->username;
                            self->profile_.domain = m->domain;
                        }
                        if (m->save_password) {
                            self->profile_.runtime_password = m->password;
                        } else {
                            self->profile_.runtime_password.clear();
                        }
                        if (self->callbacks_.on_profile_changed) {
                            self->callbacks_.on_profile_changed(self->profile_);
                        }
                    }
                    return G_SOURCE_REMOVE;
                },
                msg);
        }
        return ok;
    };

    session_ = std::make_unique<RdpSession>(std::move(session_profile), std::move(all_monitors), std::move(scb));
    canvas_->bind_framebuffer(&session_->framebuffer());
    session_->pointer_bridge().attach_widget(canvas_->widget());

    if (profile_.clipboard_redirection) {
        GdkClipboard* clip = gtk_widget_get_clipboard(GTK_WIDGET(window_));
        session_->clipboard_bridge().attach_gtk_clipboard(clip, profile_.clipboard_files);
    }

    InputCallbacks icb;
    icb.on_toggle_bar = [this]() {
        if (!bar_) return;
        bar_->toggle_visibility_hotkey();
        if (!bar_->is_pinned() && bar_->is_hide_hover() && bar_->is_revealed()) {
            show_toast("Connection bar shown — auto-hides or press Ctrl+Alt+Home");
        }
    };
    icb.on_toggle_fullscreen = [this]() {
        toggle_fullscreen();
    };
    icb.on_toggle_multimon = [this]() {
        toggle_multimon();
    };
    icb.on_focus_changed = [this](bool focused) {
        on_pane_focus_changed(GTK_WINDOW(window_), focused);
    };
    icb.on_pointer_pressed = [this]() {
        on_pane_pointer_pressed(GTK_WINDOW(window_));
    };
    icb.on_toggle_grab_requested = [this]() {
        toggle_coordinated_grab();
    };
    icb.on_grab_changed = [this, wptr = GTK_WINDOW(window_)](bool grabbed) {
        if (grabbed) {
            active_grab_window_ = wptr;
        } else if (active_grab_window_ == wptr) {
            active_grab_window_ = nullptr;
        }
        if (bar_) {
            bar_->set_grabbed(active_grab_window_ != nullptr || grabbed);
        }
        sync_css_flag(hdr_grab_btn_, "winrdp-hdr-active", active_grab_window_ != nullptr || grabbed);
    };
    icb.map_pointer = [this](double wx, double wy, int& rx, int& ry) {
        return map_pane_pointer(0, canvas_.get(), wx, wy, rx, ry);
    };

    input_ = std::make_unique<InputManager>(
        GTK_WINDOW(window_), canvas_.get(), session_.get(), std::move(icb));
    input_->set_external_grab_control(true);
    input_->set_grab_mode(profile_.keyboard_grab_mode, profile_.grab_release_key);

    attach_monitor_hotplug();
    session_->start();
}

void SessionWindow::clear_temp_transient() {
    if (temp_transient_attached_ && window_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), nullptr);
    }
    temp_transient_attached_ = false;
}

void SessionWindow::cancel_initial_bootstrap() {
    if (initial_fs_tick_id_ != 0 && window_) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(window_), initial_fs_tick_id_);
    }
    initial_fs_tick_id_ = 0;
    if (initial_fs_idle_id_ != 0) {
        g_source_remove(initial_fs_idle_id_);
        initial_fs_idle_id_ = 0;
    }
    if (initial_fs_fallback_timer_id_ != 0) {
        g_source_remove(initial_fs_fallback_timer_id_);
        initial_fs_fallback_timer_id_ = 0;
    }
    pending_initial_fullscreen_ = false;
    initial_windowed_frame_committed_ = false;
    initial_want_fullscreen_ = false;
    clear_temp_transient();
}

void SessionWindow::finish_initial_wayland_bootstrap(bool enter_fullscreen) {
    cancel_initial_bootstrap();
    if (closing_ || !window_) return;
    if (enter_fullscreen) {
        apply_fullscreen_state(true);
    }
    if (canvas_) {
        canvas_->request_redraw();
        gtk_widget_grab_focus(canvas_->widget());
    }
}

void SessionWindow::present() {
    if (!window_) return;
    const bool want_fs =
        !smoke_test_mode_ &&
        (profile_.display_mode == DisplayMode::Fullscreen ||
         profile_.display_mode == DisplayMode::MultiMonitor);
    const bool is_x11 = is_x11_display(gtk_widget_get_display(GTK_WIDGET(window_)));

    if (is_x11 || smoke_test_mode_) {
        if (want_fs) {
            apply_fullscreen_state(true);
        } else {
            is_fullscreen_ = false;
            update_window_chrome();
            gtk_window_present(GTK_WINDOW(window_));
        }
    } else {
        is_fullscreen_ = false;
        update_window_chrome();
        gtk_window_set_default_size(GTK_WINDOW(window_), windowed_w_, windowed_h_);

        bool can_attach_parent = false;
        if (placement_parent_ &&
            gtk_widget_get_visible(GTK_WIDGET(placement_parent_)) == TRUE &&
            gtk_widget_get_mapped(GTK_WIDGET(placement_parent_)) == TRUE) {
            GdkSurface* psurf = gtk_native_get_surface(GTK_NATIVE(placement_parent_));
            const bool minimized =
                psurf && GDK_IS_TOPLEVEL(psurf) &&
                ((gdk_toplevel_get_state(GDK_TOPLEVEL(psurf)) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0);
            can_attach_parent = !minimized;
        }
        if (can_attach_parent) {
            gtk_window_set_transient_for(GTK_WINDOW(window_), placement_parent_);
            temp_transient_attached_ = true;
        }

        gtk_window_present(GTK_WINDOW(window_));

        if (want_fs || temp_transient_attached_) {
            initial_windowed_frame_committed_ = false;
            initial_want_fullscreen_ = want_fs;
            pending_initial_fullscreen_ = want_fs;
            struct BootstrapCtx {
                SessionWindow* self;
                std::weak_ptr<Alive> alive;
                bool enter_fs;
            };
            auto* tick_ctx = new BootstrapCtx{this, std::weak_ptr<Alive>(alive_), want_fs};
            initial_fs_tick_id_ = gtk_widget_add_tick_callback(
                GTK_WIDGET(window_),
                +[](GtkWidget* w, GdkFrameClock*, gpointer data) -> gboolean {
                    auto* c = static_cast<BootstrapCtx*>(data);
                    auto a = c->alive.lock();
                    if (!a || !a->valid || c->self->closing_) {
                        if (a && a->valid) {
                            c->self->initial_fs_tick_id_ = 0;
                        }
                        return G_SOURCE_REMOVE;
                    }
                    SessionWindow* self = c->self;
                    if (gtk_widget_get_mapped(w) == TRUE &&
                        gtk_widget_get_width(w) > 0 &&
                        gtk_widget_get_height(w) > 0) {
                        self->initial_fs_tick_id_ = 0;
                        self->initial_windowed_frame_committed_ = true;
                        if (self->initial_fs_fallback_timer_id_ == 0) {
                            auto* timer_ctx = new BootstrapCtx{self, c->alive, c->enter_fs};
                            self->initial_fs_fallback_timer_id_ = g_timeout_add_full(
                                G_PRIORITY_DEFAULT,
                                80,
                                +[](gpointer tdata) -> gboolean {
                                    auto* tc = static_cast<BootstrapCtx*>(tdata);
                                    if (auto ta = tc->alive.lock(); ta && ta->valid) {
                                        tc->self->initial_fs_fallback_timer_id_ = 0;
                                        tc->self->finish_initial_wayland_bootstrap(tc->enter_fs);
                                    }
                                    return G_SOURCE_REMOVE;
                                },
                                timer_ctx,
                                +[](gpointer tdata) { delete static_cast<BootstrapCtx*>(tdata); });
                        }
                        return G_SOURCE_REMOVE;
                    }
                    return G_SOURCE_CONTINUE;
                },
                tick_ctx,
                +[](gpointer data) { delete static_cast<BootstrapCtx*>(data); });
        }
    }
    if (canvas_) gtk_widget_grab_focus(canvas_->widget());
}

void SessionWindow::present_and_focus() {
    if (!window_ || closing_) return;
    if (is_multimon_active_) {
        for (const auto& pane : secondary_panes_) {
            if (pane && pane->window) {
                gtk_window_present(pane->window);
            }
        }
    }
    gtk_window_present(GTK_WINDOW(window_));
    if (canvas_) {
        gtk_widget_grab_focus(canvas_->widget());
    }
}

void SessionWindow::close() {
    if (window_) {
        gtk_window_close(GTK_WINDOW(window_));
    }
}

void SessionWindow::toggle_fullscreen() {
    apply_fullscreen_state(!is_fullscreen_);
}

void SessionWindow::toggle_multimon() {
    cancel_initial_bootstrap();
    if (!window_ || !session_) return;
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    auto all_monitors = query_local_monitors(display);

    if (is_multimon_active_) {
        std::vector<MonitorInfo> prim = filter_selected_monitors(all_monitors, {});
        MonitorInfo target = prim[0];
        for (const auto& m : prim) {
            if (m.is_primary) {
                target = m;
                break;
            }
        }
        profile_.display_mode = DisplayMode::Fullscreen;
        if (callbacks_.on_profile_changed) {
            callbacks_.on_profile_changed(profile_);
        }
        sync_monitor_windows({}, true);
        if (session_->is_connected()) {
            session_->request_resize(
                static_cast<uint32_t>(target.width),
                static_cast<uint32_t>(target.height),
                profile_.scale_factor);
        }
        show_toast("Switched to Single Monitor");
        return;
    }

    auto selected = filter_selected_monitors(all_monitors, profile_.selected_monitors);
    if (selected.size() < 2 && all_monitors.size() >= 2) {
        selected = filter_selected_monitors(all_monitors, {});
    }
    if (selected.size() < 2) {
        show_toast("Only 1 local monitor detected");
        return;
    }

    profile_.display_mode = DisplayMode::MultiMonitor;
    if (callbacks_.on_profile_changed) {
        callbacks_.on_profile_changed(profile_);
    }
    const auto layout = compute_virtual_desktop_layout(selected, profile_.scale_factor);
    sync_monitor_windows(layout, true);
    if (session_->is_connected()) {
        session_->request_monitor_layout(selected, profile_.scale_factor);
    }
    show_toast("Multi-Monitor enabled (" + std::to_string(layout.viewports.size()) + " displays)");
}

void SessionWindow::apply_fullscreen_state(bool enter) {
    cancel_initial_bootstrap();
    if (!window_) return;
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));

    if (enter) {
        if (profile_.display_mode == DisplayMode::MultiMonitor) {
            auto all_monitors = query_local_monitors(display);
            auto selected = filter_selected_monitors(all_monitors, profile_.selected_monitors);
            if (selected.size() > 1) {
                const auto layout = compute_virtual_desktop_layout(selected, profile_.scale_factor);
                sync_monitor_windows(layout, true);
                if (session_ && session_->is_connected()) {
                    session_->request_monitor_layout(selected, profile_.scale_factor);
                }
                return;
            }
        }
        sync_monitor_windows({}, true);
    } else {
        sync_monitor_windows({}, false);
    }
}

void SessionWindow::destroy_secondary_panes() {
    if (group_raise_guard_timer_id_ != 0) {
        g_source_remove(group_raise_guard_timer_id_);
        group_raise_guard_timer_id_ = 0;
    }
    raising_group_ = false;
    for (auto& pane : secondary_panes_) {
        if (!pane) continue;
        detach_surface_monitor_guard(pane->bound_surface, pane->enter_handler);
        if (pane->window && active_grab_window_ == pane->window) {
            if (pane->input) {
                pane->input->release_all_keys();
                pane->input->force_grab(false);
            }
            active_grab_window_ = nullptr;
        }
        if (pane->window && last_focused_window_ == pane->window) {
            last_focused_window_ = nullptr;
        }
        if (session_ && pane->canvas) {
            session_->pointer_bridge().detach_widget(pane->canvas->widget());
        }
        pane->input.reset();
        pane->canvas.reset();
        if (pane->window) {
            GtkWindow* w = pane->window;
            pane->window = nullptr;
            g_signal_handlers_disconnect_by_data(w, this);
            g_signal_handlers_disconnect_by_data(w, pane.get());
            gtk_window_destroy(w);
        }
    }
    secondary_panes_.clear();
}

void SessionWindow::sync_monitor_windows(const VirtualDesktopLayout& layout, bool enter_fullscreen) {
    if (!window_) return;
    destroy_secondary_panes();

    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));

    if (enter_fullscreen && !is_fullscreen_ &&
        gtk_widget_get_mapped(GTK_WIDGET(window_)) == TRUE &&
        gtk_window_is_maximized(GTK_WINDOW(window_)) == FALSE) {
        const auto [mon_w, mon_h] = primary_logical_bounds(display);
        const int max_w = std::max(640, mon_w - 160);
        const int max_h = std::max(440, mon_h - 180);
        const int cur_w = gtk_widget_get_width(GTK_WIDGET(window_));
        const int cur_h = gtk_widget_get_height(GTK_WIDGET(window_));
        if (cur_w >= 640 && cur_h >= 440) {
            windowed_w_ = std::clamp(cur_w, 640, max_w);
            windowed_h_ = std::clamp(cur_h, 440, max_h);
        }
    }

    if (enter_fullscreen && layout.viewports.size() > 1) {
        pending_unfullscreen_reposition_ = false;
        if (reposition_timer_id_ != 0) {
            g_source_remove(reposition_timer_id_);
            reposition_timer_id_ = 0;
        }
        if (resize_timer_id_ != 0) {
            g_source_remove(resize_timer_id_);
            resize_timer_id_ = 0;
        }
        is_multimon_active_ = true;
        current_layout_ = layout;
        is_fullscreen_ = true;
        update_window_chrome();

        const MonitorViewport& vp0 = layout.viewports[0];
        canvas_->set_source_rect(
            vp0.fb_x, vp0.fb_y, static_cast<int>(vp0.fb_width), static_cast<int>(vp0.fb_height));

        GdkMonitor* m0 = find_gdk_monitor(display, vp0.monitor);
        if (m0) {
            gtk_window_fullscreen_on_monitor(GTK_WINDOW(window_), m0);
            g_object_unref(m0);
        } else {
            gtk_window_fullscreen(GTK_WINDOW(window_));
        }
        gtk_window_present(GTK_WINDOW(window_));

        for (size_t i = 1; i < layout.viewports.size(); ++i) {
            const MonitorViewport& vp = layout.viewports[i];
            auto pane = std::make_unique<SecondaryMonitorPane>();
            pane->viewport = vp;

            AdwApplicationWindow* sw = ADW_APPLICATION_WINDOW(adw_application_window_new(app_));
            pane->window = GTK_WINDOW(sw);
            gtk_window_set_icon_name(pane->window, "io.winrdp.Client");
            gtk_window_set_title(pane->window,
                                 ("WinRDP — " + profile_.display_title() +
                                  " (Display " + std::to_string(i + 1) + ")")
                                     .c_str());
            gtk_window_set_default_size(pane->window,
                                        std::max(320, vp.monitor.logical_width),
                                        std::max(200, vp.monitor.logical_height));

            pane->canvas = std::make_unique<GlCanvas>();
            pane->canvas->set_smart_sizing(profile_.smart_sizing);
            if (session_) {
                pane->canvas->bind_framebuffer(&session_->framebuffer());
                session_->pointer_bridge().attach_widget(pane->canvas->widget());
            }
            pane->canvas->set_source_rect(
                vp.fb_x, vp.fb_y, static_cast<int>(vp.fb_width), static_cast<int>(vp.fb_height));
            adw_application_window_set_content(sw, pane->canvas->widget());

            InputCallbacks icb;
            icb.on_toggle_bar = [this]() {
                if (!bar_) return;
                bar_->toggle_visibility_hotkey();
                if (!bar_->is_pinned() && bar_->is_hide_hover() && bar_->is_revealed()) {
                    show_toast("Connection bar shown — auto-hides or press Ctrl+Alt+Home");
                }
            };
            icb.on_toggle_fullscreen = [this]() {
                toggle_fullscreen();
            };
            icb.on_toggle_multimon = [this]() {
                toggle_multimon();
            };
            icb.on_focus_changed = [this, wptr = pane->window](bool focused) {
                on_pane_focus_changed(wptr, focused);
            };
            icb.on_pointer_pressed = [this, wptr = pane->window]() {
                on_pane_pointer_pressed(wptr);
            };
            icb.on_toggle_grab_requested = [this]() {
                toggle_coordinated_grab();
            };
            icb.on_grab_changed = [this, wptr = pane->window](bool grabbed) {
                if (grabbed) {
                    active_grab_window_ = wptr;
                } else if (active_grab_window_ == wptr) {
                    active_grab_window_ = nullptr;
                }
                if (bar_) {
                    bar_->set_grabbed(active_grab_window_ != nullptr || grabbed);
                }
                sync_css_flag(hdr_grab_btn_, "winrdp-hdr-active", active_grab_window_ != nullptr || grabbed);
            };
            icb.map_pointer = [this, i, c = pane->canvas.get()](double wx, double wy, int& rx, int& ry) {
                return map_pane_pointer(i, c, wx, wy, rx, ry);
            };

            pane->input = std::make_unique<InputManager>(
                pane->window, pane->canvas.get(), session_.get(), std::move(icb));
            pane->input->set_external_grab_control(true);
            pane->input->set_fullscreen(true);
            pane->input->set_grab_mode(profile_.keyboard_grab_mode, profile_.grab_release_key);

            g_signal_connect(
                pane->window,
                "close-request",
                G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
                    auto* self = static_cast<SessionWindow*>(data);
                    if (!self->closing_) {
                        self->close();
                    }
                    return TRUE;
                }),
                this);

            g_signal_connect(
                pane->window,
                "realize",
                G_CALLBACK(+[](GtkWidget* w, gpointer) {
                    SessionWindow::apply_skip_taskbar_hint(GTK_WINDOW(w));
                }),
                nullptr);

            g_signal_connect(
                pane->window,
                "map",
                G_CALLBACK(+[](GtkWidget* w, gpointer) {
                    SessionWindow::apply_skip_taskbar_hint(GTK_WINDOW(w));
                }),
                nullptr);

            // Connected by `this` or `pane.get()`; destroy callback clears pointers safely
            g_signal_connect(
                pane->window,
                "notify::fullscreened",
                G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
                    auto* self = static_cast<SessionWindow*>(data);
                    SessionWindow::apply_skip_taskbar_hint(GTK_WINDOW(obj));
                    self->sync_coordinated_grab();
                }),
                this);

            g_signal_connect(
                pane->window,
                "notify::is-active",
                G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
                    auto* self = static_cast<SessionWindow*>(data);
                    self->on_pane_active_changed(GTK_WINDOW(obj));
                }),
                this);

            g_signal_connect(
                pane->window,
                "destroy",
                G_CALLBACK(+[](GtkWidget*, gpointer data) {
                    auto* p = static_cast<SecondaryMonitorPane*>(data);
                    p->bound_surface = nullptr;
                    p->enter_handler = 0;
                    p->window = nullptr;
                }),
                pane.get());

            g_signal_connect(
                pane->window,
                "unrealize",
                G_CALLBACK(+[](GtkWidget*, gpointer data) {
                    auto* p = static_cast<SecondaryMonitorPane*>(data);
                    if (p->bound_surface && p->enter_handler != 0) {
                        g_signal_handler_disconnect(p->bound_surface, p->enter_handler);
                    }
                    p->bound_surface = nullptr;
                    p->enter_handler = 0;
                }),
                pane.get());

            GdkMonitor* mi = find_gdk_monitor(display, vp.monitor);
            if (mi) {
                gtk_window_fullscreen_on_monitor(pane->window, mi);
                g_object_unref(mi);
            } else {
                gtk_window_fullscreen(pane->window);
            }
            gtk_window_present(pane->window);
            apply_skip_taskbar_hint(pane->window);
            attach_surface_monitor_guard(pane->window, pane->bound_surface, pane->enter_handler);
            secondary_panes_.push_back(std::move(pane));
        }

        if (bar_) bar_->set_multimon(true, layout.viewports.size());
        sync_css_flag(hdr_multimon_btn_, "winrdp-hdr-active", true);
        gtk_window_present(GTK_WINDOW(window_));
        last_focused_window_ = GTK_WINDOW(window_);
        session_had_focus_ = true;
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
        sync_coordinated_grab();
        return;
    }

    is_multimon_active_ = false;
    current_layout_ = VirtualDesktopLayout{};
    canvas_->clear_source_rect();
    if (bar_) bar_->set_multimon(false, 1);
    sync_css_flag(hdr_multimon_btn_, "winrdp-hdr-active", false);

    if (enter_fullscreen) {
        pending_unfullscreen_reposition_ = false;
        if (reposition_timer_id_ != 0) {
            g_source_remove(reposition_timer_id_);
            reposition_timer_id_ = 0;
        }
        is_fullscreen_ = true;
        update_window_chrome();

        GdkMonitor* target = nullptr;
        auto all_monitors = query_local_monitors(display);
        if (profile_.selected_monitors.size() == 1 && !all_monitors.empty()) {
            auto sel = filter_selected_monitors(all_monitors, profile_.selected_monitors);
            if (!sel.empty()) {
                target = find_gdk_monitor(display, sel[0]);
            }
        }
        if (target) {
            gtk_window_fullscreen_on_monitor(GTK_WINDOW(window_), target);
            g_object_unref(target);
        } else {
            gtk_window_fullscreen(GTK_WINDOW(window_));
        }
        gtk_window_present(GTK_WINDOW(window_));
    } else {
        release_all_grabs_immediately();
        if (input_) {
            input_->set_fullscreen(false);
        }
        is_fullscreen_ = false;
        update_window_chrome();

        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
        bool is_x11 = false;
#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        is_x11 = surface && GDK_IS_X11_SURFACE(surface);
        G_GNUC_END_IGNORE_DEPRECATIONS
#else
        (void)surface;
#endif

        gtk_window_unfullscreen(GTK_WINDOW(window_));
        gtk_window_unmaximize(GTK_WINDOW(window_));
        gtk_window_set_default_size(GTK_WINDOW(window_), windowed_w_, windowed_h_);
        gtk_window_present(GTK_WINDOW(window_));
        if (canvas_) {
            canvas_->request_redraw();
        }

        if (is_x11) {
            pending_unfullscreen_reposition_ = true;
            reposition_attempts_ = 0;
            schedule_windowed_reposition(60);
        } else {
            pending_unfullscreen_reposition_ = false;
            if (reposition_timer_id_ != 0) {
                g_source_remove(reposition_timer_id_);
                reposition_timer_id_ = 0;
            }
        }
    }
    sync_coordinated_grab();
}

void SessionWindow::schedule_windowed_reposition(guint delay_ms) {
    if (reposition_timer_id_ != 0) {
        g_source_remove(reposition_timer_id_);
        reposition_timer_id_ = 0;
    }
    if (closing_ || !window_) return;
    reposition_timer_id_ = g_timeout_add(
        delay_ms,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->reposition_timer_id_ = 0;
            self->reposition_windowed_in_workarea();
            return G_SOURCE_REMOVE;
        },
        this);
}

void SessionWindow::reposition_windowed_in_workarea() {
    if (closing_ || !window_ || is_fullscreen_) {
        pending_unfullscreen_reposition_ = false;
        return;
    }
    if (gtk_window_is_fullscreen(GTK_WINDOW(window_)) == TRUE) {
        if (reposition_attempts_++ < 8) {
            schedule_windowed_reposition(80);
        } else {
            pending_unfullscreen_reposition_ = false;
        }
        return;
    }
    if (gtk_window_is_maximized(GTK_WINDOW(window_)) == TRUE) {
        gtk_window_unmaximize(GTK_WINDOW(window_));
        if (reposition_attempts_++ < 8) {
            schedule_windowed_reposition(80);
        } else {
            pending_unfullscreen_reposition_ = false;
        }
        return;
    }

#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    const bool is_x11_surf = surface && GDK_IS_X11_SURFACE(surface);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (is_x11_surf) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        GdkDisplay* display = gdk_surface_get_display(surface);
        Display* xdpy = display ? gdk_x11_display_get_xdisplay(display) : nullptr;
        const Window xwin = gdk_x11_surface_get_xid(surface);
        G_GNUC_END_IGNORE_DEPRECATIONS
        int use_x = 0;
        int use_y = 0;
        int use_w = 1920;
        int use_h = 1000;
        int scale = 1;
        if (xdpy && xwin != None &&
            query_monitor_workarea_x11(display, surface, use_x, use_y, use_w, use_h, scale)) {
            int ext_left = 0;
            int ext_right = 0;
            int ext_top = 0;
            int ext_bottom = 0;
            query_frame_extents_x11(xdpy, xwin, ext_left, ext_right, ext_top, ext_bottom);

            const int min_vis_w = smoke_test_mode_ ? 320 : 640;
            const int min_vis_h = smoke_test_mode_ ? 240 : 440;
            const int max_vis_w = std::max(min_vis_w, use_w - 80 * scale);
            const int max_vis_h = std::max(min_vis_h, use_h - 80 * scale);
            const int target_vis_w = std::clamp(windowed_w_ * scale, min_vis_w, max_vis_w);
            const int target_vis_h = std::clamp(windowed_h_ * scale, min_vis_h, max_vis_h);

            const int target_vis_x = use_x + std::max(0, (use_w - target_vis_w) / 2);
            const int target_vis_y = use_y + std::max(0, (use_h - target_vis_h) / 2);
            const int target_outer_x = target_vis_x - ext_left;
            const int target_outer_y = target_vis_y - ext_top;
            const int target_outer_w = target_vis_w + ext_left + ext_right;
            const int target_outer_h = target_vis_h + ext_top + ext_bottom;

            XWindowAttributes attr{};
            int cur_rx = 0;
            int cur_ry = 0;
            Window child = None;
            const bool have_attr = XGetWindowAttributes(xdpy, xwin, &attr) != 0;
            const bool have_coords = XTranslateCoordinates(
                xdpy, xwin, DefaultRootWindow(xdpy), 0, 0, &cur_rx, &cur_ry, &child) != 0;

            const int cur_vis_x = cur_rx + ext_left;
            const int cur_vis_y = cur_ry + ext_top;
            const int cur_vis_w = have_attr ? (attr.width - ext_left - ext_right) : 0;
            const int cur_vis_h = have_attr ? (attr.height - ext_top - ext_bottom) : 0;

            const bool settled =
                have_attr && have_coords &&
                (ext_top > 0 || reposition_attempts_ >= 2) &&
                std::abs(cur_vis_x - target_vis_x) <= 8 &&
                std::abs(cur_vis_y - target_vis_y) <= 8 &&
                std::abs(cur_vis_w - target_vis_w) <= 24 &&
                std::abs(cur_vis_h - target_vis_h) <= 24 &&
                cur_vis_y >= use_y;

            if (!settled) {
                gtk_window_set_default_size(
                    GTK_WINDOW(window_), target_vis_w / scale, target_vis_h / scale);
                send_net_moveresize_x11(
                    xdpy, xwin, target_outer_x, target_outer_y, target_outer_w, target_outer_h);
                if (reposition_attempts_++ < 6) {
                    schedule_windowed_reposition(120);
                    return;
                }
            }
        }
    }
#endif
    pending_unfullscreen_reposition_ = false;
}

bool SessionWindow::query_client_and_workarea_x11(int& client_x,
                                                  int& client_y,
                                                  int& client_w,
                                                  int& client_h,
                                                  int& work_x,
                                                  int& work_y,
                                                  int& work_w,
                                                  int& work_h) const {
#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
    if (!window_) return false;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    if (!surface || !GDK_IS_X11_SURFACE(surface)) return false;

    GdkDisplay* display = gdk_surface_get_display(surface);
    Display* xdpy = display ? gdk_x11_display_get_xdisplay(display) : nullptr;
    const Window xwin = gdk_x11_surface_get_xid(surface);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (!xdpy || xwin == None) return false;

    int scale = 1;
    if (!query_monitor_workarea_x11(display, surface, work_x, work_y, work_w, work_h, scale)) {
        return false;
    }
    int ext_left = 0;
    int ext_right = 0;
    int ext_top = 0;
    int ext_bottom = 0;
    query_frame_extents_x11(xdpy, xwin, ext_left, ext_right, ext_top, ext_bottom);

    int rx = 0;
    int ry = 0;
    Window child = None;
    if (!XTranslateCoordinates(xdpy, xwin, DefaultRootWindow(xdpy), 0, 0, &rx, &ry, &child)) {
        return false;
    }
    XWindowAttributes attr{};
    if (!XGetWindowAttributes(xdpy, xwin, &attr)) {
        return false;
    }
    client_x = rx + ext_left;
    client_y = ry + ext_top;
    client_w = std::max(0, attr.width - ext_left - ext_right);
    client_h = std::max(0, attr.height - ext_top - ext_bottom);
    return true;
#else
    (void)client_x;
    (void)client_y;
    (void)client_w;
    (void)client_h;
    (void)work_x;
    (void)work_y;
    (void)work_w;
    (void)work_h;
    return false;
#endif
}

void SessionWindow::schedule_dynamic_resize(int pixel_w, int pixel_h) {
    if (pending_initial_fullscreen_) {
        return;
    }
    if (!session_ || !profile_.dynamic_resolution || pixel_w < 320 || pixel_h < 200) {
        return;
    }
    if (is_multimon_active_) {
        return;
    }
    pending_resize_w_ = static_cast<uint32_t>(pixel_w);
    pending_resize_h_ = static_cast<uint32_t>(pixel_h);

    if (resize_timer_id_ != 0) {
        g_source_remove(resize_timer_id_);
    }
    resize_timer_id_ = g_timeout_add(
        250,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->resize_timer_id_ = 0;
            if (self->session_ && self->session_->is_connected() && !self->is_multimon_active_) {
                self->session_->request_resize(
                    self->pending_resize_w_, self->pending_resize_h_, self->profile_.scale_factor);
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

InputManager* SessionWindow::find_input_for_window(GtkWindow* win) const {
    if (!win) return nullptr;
    if (window_ && GTK_WINDOW(window_) == win) {
        return input_.get();
    }
    for (const auto& pane : secondary_panes_) {
        if (pane && pane->window == win) {
            return pane->input.get();
        }
    }
    return nullptr;
}

void SessionWindow::release_all_pane_keys() {
    if (input_) input_->release_all_keys();
    for (const auto& pane : secondary_panes_) {
        if (pane && pane->input) {
            pane->input->release_all_keys();
        }
    }
}

GtkWindow* SessionWindow::find_focused_pane() const {
    if (window_ &&
        ((input_ && input_->has_focus()) || gtk_window_is_active(GTK_WINDOW(window_)) == TRUE)) {
        return GTK_WINDOW(window_);
    }
    for (const auto& pane : secondary_panes_) {
        if (pane && pane->window &&
            ((pane->input && pane->input->has_focus()) || gtk_window_is_active(pane->window) == TRUE)) {
            return pane->window;
        }
    }
    return nullptr;
}

void SessionWindow::on_pane_pointer_pressed(GtkWindow* win) {
    if (!win || closing_) return;
    if (is_multimon_active_ && !session_had_focus_ && !raising_group_) {
        session_had_focus_ = true;
        last_focused_window_ = win;
        raise_sibling_panes(win);
    }
}

void SessionWindow::raise_sibling_panes(GtkWindow* active_win) {
    if (closing_ || !is_multimon_active_ || !window_ || raising_group_) return;

    const uint64_t now = steady_ms_now();
    if (now - last_group_raise_ms_ < 200) {
        return;
    }
    last_group_raise_ms_ = now;
    raising_group_ = true;

    bool need_refocus_active = false;
    auto raise_one = [&](GtkWindow* other) {
        if (!other || other == active_win) return;
        GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(other));
        bool minimized = false;
        if (s && GDK_IS_TOPLEVEL(s)) {
            minimized = (gdk_toplevel_get_state(GDK_TOPLEVEL(s)) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
        }
        if (!minimized) {
#if defined(GDK_WINDOWING_X11) && defined(WINRDP_HAVE_X11)
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            const bool is_x11_other = s && GDK_IS_X11_SURFACE(s);
            G_GNUC_END_IGNORE_DEPRECATIONS
            if (is_x11_other) {
                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                GdkDisplay* dpy = gdk_surface_get_display(s);
                Display* xdpy = dpy ? gdk_x11_display_get_xdisplay(dpy) : nullptr;
                const Window xw = gdk_x11_surface_get_xid(s);
                G_GNUC_END_IGNORE_DEPRECATIONS
                if (xdpy && xw != None) {
                    XMapRaised(xdpy, xw);
                    XFlush(xdpy);
                    return;
                }
            }
#endif
        }
        gtk_window_present(other);
        need_refocus_active = true;
    };

    raise_one(GTK_WINDOW(window_));
    for (const auto& pane : secondary_panes_) {
        if (pane && pane->window) {
            raise_one(pane->window);
        }
    }
    if (need_refocus_active && active_win) {
        gtk_window_present(active_win);
    }

    if (group_raise_guard_timer_id_ != 0) {
        g_source_remove(group_raise_guard_timer_id_);
    }
    group_raise_guard_timer_id_ = g_timeout_add(
        200,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->group_raise_guard_timer_id_ = 0;
            self->raising_group_ = false;
            if (self->closing_) return G_SOURCE_REMOVE;

            GtkWindow* focused_win = self->find_focused_pane();
            if (focused_win) {
                self->session_had_focus_ = true;
                self->last_focused_window_ = focused_win;
            } else {
                self->session_had_focus_ = false;
                self->last_focused_window_ = nullptr;
                self->release_all_pane_keys();
            }
            self->sync_coordinated_grab();
            return G_SOURCE_REMOVE;
        },
        this);
}

void SessionWindow::attach_primary_state_observer(GdkSurface* surface) {
    detach_primary_state_observer();
    if (!surface || !GDK_IS_TOPLEVEL(surface)) return;
    primary_state_surface_ = surface;
    primary_was_minimized_ =
        (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    primary_state_handler_ = g_signal_connect(
        surface,
        "notify::state",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
            static_cast<SessionWindow*>(data)->on_primary_toplevel_state_changed();
        }),
        this);
}

void SessionWindow::detach_primary_state_observer() {
    if (primary_state_surface_ && primary_state_handler_ != 0) {
        g_signal_handler_disconnect(primary_state_surface_, primary_state_handler_);
    }
    primary_state_surface_ = nullptr;
    primary_state_handler_ = 0;
    primary_was_minimized_ = false;
}

void SessionWindow::on_primary_toplevel_state_changed() {
    if (!primary_state_surface_ || !GDK_IS_TOPLEVEL(primary_state_surface_)) return;
    const bool minimized =
        (gdk_toplevel_get_state(GDK_TOPLEVEL(primary_state_surface_)) & GDK_TOPLEVEL_STATE_MINIMIZED) != 0;
    if (minimized == primary_was_minimized_) return;
    primary_was_minimized_ = minimized;
    if (!is_multimon_active_ || closing_) return;

    if (minimized) {
        session_had_focus_ = false;
        release_all_grabs_immediately();
        for (const auto& pane : secondary_panes_) {
            if (pane && pane->window) {
                gtk_window_minimize(pane->window);
            }
        }
    } else {
        session_had_focus_ = true;
        if (window_) {
            raise_sibling_panes(GTK_WINDOW(window_));
        }
    }
}

void SessionWindow::on_pane_active_changed(GtkWindow* win) {
    if (!win || closing_) return;
    if (gtk_window_is_active(win) == TRUE) {
        if (focus_leave_timer_id_ != 0) {
            g_source_remove(focus_leave_timer_id_);
            focus_leave_timer_id_ = 0;
        }
        const bool returning_from_external = !session_had_focus_;
        session_had_focus_ = true;
        last_focused_window_ = win;
        if (win == GTK_WINDOW(window_) && canvas_) {
            gtk_widget_grab_focus(canvas_->widget());
        } else {
            for (const auto& pane : secondary_panes_) {
                if (pane && pane->window == win && pane->canvas) {
                    gtk_widget_grab_focus(pane->canvas->widget());
                    break;
                }
            }
        }
        if (is_multimon_active_ && returning_from_external && !raising_group_) {
            raise_sibling_panes(win);
        }
        sync_coordinated_grab();
    } else {
        on_pane_focus_changed(win, false);
    }
}

void SessionWindow::on_pane_focus_changed(GtkWindow* win, bool focused) {
    if (focused) {
        if (focus_leave_timer_id_ != 0) {
            g_source_remove(focus_leave_timer_id_);
            focus_leave_timer_id_ = 0;
        }
        const bool returning_from_external = !session_had_focus_;
        session_had_focus_ = true;
        last_focused_window_ = win;
        if (is_multimon_active_ && returning_from_external && !raising_group_) {
            raise_sibling_panes(win);
        }
        sync_coordinated_grab();
        return;
    }

    if (raising_group_) {
        return;
    }

    if (focus_leave_timer_id_ != 0) {
        g_source_remove(focus_leave_timer_id_);
        focus_leave_timer_id_ = 0;
    }

    if (!is_multimon_active_) {
        session_had_focus_ = false;
        last_focused_window_ = nullptr;
        release_all_pane_keys();
        sync_coordinated_grab();
        return;
    }

    focus_leave_timer_id_ = g_timeout_add(
        40,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->focus_leave_timer_id_ = 0;

            GtkWindow* focused_win = self->find_focused_pane();
            if (focused_win) {
                self->session_had_focus_ = true;
                self->last_focused_window_ = focused_win;
                self->sync_coordinated_grab();
            } else {
                self->session_had_focus_ = false;
                self->last_focused_window_ = nullptr;
                self->release_all_pane_keys();
                self->sync_coordinated_grab();
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

void SessionWindow::toggle_coordinated_grab() {
    user_grab_enabled_ = !user_grab_enabled_;
    sync_coordinated_grab();
    show_toast(active_grab_window_ != nullptr
                   ? "System keys captured (Right Ctrl to release)"
                   : "Keyboard released to Ubuntu (Right Ctrl to capture)");
}

void SessionWindow::suspend_coordinated_grab(bool suspend) {
    grab_suspended_ = suspend;
    if (suspend) {
        release_all_pane_keys();
    }
    sync_coordinated_grab();
}

void SessionWindow::sync_coordinated_grab() {
    if (bar_) {
        bar_->set_fullscreen(is_fullscreen_);
    }
    sync_css_flag(hdr_multimon_btn_, "winrdp-hdr-active", is_multimon_active_);

    bool want_mode = false;
    if (profile_.keyboard_grab_mode == KeyboardGrabMode::AlwaysWhenFocused) {
        want_mode = true;
    } else if (profile_.keyboard_grab_mode == KeyboardGrabMode::FullscreenOnly) {
        want_mode = is_fullscreen_;
    }

    const bool use_wayland_inhibit =
        window_ && input_ && gtk_widget_get_realized(GTK_WIDGET(window_)) && !input_->is_x11_surface();

    if (use_wayland_inhibit) {
        bool any_active = (window_ && gtk_window_is_active(GTK_WINDOW(window_)) == TRUE) ||
                          (input_ && input_->has_focus());
        if (!any_active) {
            for (const auto& pane : secondary_panes_) {
                if (pane && pane->window &&
                    (gtk_window_is_active(pane->window) == TRUE ||
                     (pane->input && pane->input->has_focus()))) {
                    any_active = true;
                    break;
                }
            }
        }
        const bool want_wl_grab = !grab_suspended_ && user_grab_enabled_ && want_mode && any_active;
        if (input_) input_->force_grab(want_wl_grab);
        for (const auto& pane : secondary_panes_) {
            if (pane && pane->input) {
                pane->input->force_grab(want_wl_grab);
            }
        }
        active_grab_window_ = want_wl_grab
                                  ? (last_focused_window_ ? last_focused_window_ : GTK_WINDOW(window_))
                                  : nullptr;
        if (bar_) {
            bar_->set_grabbed(want_wl_grab);
        }
        sync_css_flag(hdr_grab_btn_, "winrdp-hdr-active", want_wl_grab);
        return;
    }

    auto is_win_focused = [this](GtkWindow* w) -> bool {
        if (!w) return false;
        InputManager* mgr = find_input_for_window(w);
        return (mgr && mgr->has_focus()) || (gtk_window_is_active(w) == TRUE);
    };

    GtkWindow* candidate = last_focused_window_;
    if (!is_win_focused(candidate)) {
        candidate = nullptr;
        if (window_ && is_win_focused(GTK_WINDOW(window_))) {
            candidate = GTK_WINDOW(window_);
        } else {
            for (const auto& pane : secondary_panes_) {
                if (pane && is_win_focused(pane->window)) {
                    candidate = pane->window;
                    break;
                }
            }
        }
    }

    GtkWindow* desired_win = (!grab_suspended_ && user_grab_enabled_ && want_mode) ? candidate : nullptr;

    if (active_grab_window_ && active_grab_window_ != desired_win) {
        if (InputManager* old_mgr = find_input_for_window(active_grab_window_)) {
            old_mgr->force_grab(false);
        }
        active_grab_window_ = nullptr;
    }

    if (desired_win) {
        if (InputManager* new_mgr = find_input_for_window(desired_win)) {
            if (!new_mgr->is_grabbed()) {
                new_mgr->force_grab(true);
            }
            active_grab_window_ = new_mgr->is_grabbed() ? desired_win : nullptr;
        }
    }

    if (bar_) {
        bar_->set_grabbed(active_grab_window_ != nullptr);
    }
    sync_css_flag(hdr_grab_btn_, "winrdp-hdr-active", active_grab_window_ != nullptr);
}

void SessionWindow::attach_surface_monitor_guard(GtkWindow* win,
                                                 GdkSurface*& slot_surface,
                                                 gulong& slot_handler) {
    detach_surface_monitor_guard(slot_surface, slot_handler);
    if (!win) return;
    GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(win));
    if (!s) return;
    slot_surface = s;
    slot_handler = g_signal_connect(
        s,
        "enter-monitor",
        G_CALLBACK(+[](GdkSurface*, GdkMonitor*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            if (!self->closing_ && self->is_multimon_active_) {
                self->schedule_monitor_repin();
            }
        }),
        this);
}

void SessionWindow::detach_surface_monitor_guard(GdkSurface*& slot_surface, gulong& slot_handler) {
    if (slot_surface && slot_handler != 0) {
        g_signal_handler_disconnect(slot_surface, slot_handler);
    }
    slot_surface = nullptr;
    slot_handler = 0;
}

void SessionWindow::schedule_monitor_repin() {
    if (repin_timer_id_ != 0) {
        g_source_remove(repin_timer_id_);
    }
    repin_timer_id_ = g_timeout_add(
        150,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->repin_timer_id_ = 0;
            self->repin_misplaced_monitors();
            return G_SOURCE_REMOVE;
        },
        this);
}

void SessionWindow::repin_misplaced_monitors() {
    if (closing_ || !is_multimon_active_ || !window_ || current_layout_.viewports.size() < 2) {
        return;
    }

    const uint64_t now = steady_ms_now();
    if (now - repin_window_start_ms_ > 2000) {
        repin_window_start_ms_ = now;
        repin_count_ = 0;
    }
    if (repin_count_ >= 3) {
        return;
    }

    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    if (!display) return;

    bool did_repin = false;
    auto check_and_fix = [&](GtkWindow* w, const MonitorInfo& expected_info) {
        if (!w || gtk_window_is_fullscreen(w) != TRUE) return;
        GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(w));
        if (!s) return;
        GdkMonitor* actual = gdk_display_get_monitor_at_surface(display, s);
        GdkMonitor* expected = find_gdk_monitor(display, expected_info);
        if (expected) {
            if (actual && actual != expected) {
                gtk_window_fullscreen_on_monitor(w, expected);
                did_repin = true;
            }
            g_object_unref(expected);
        }
    };

    check_and_fix(GTK_WINDOW(window_), current_layout_.viewports[0].monitor);
    for (size_t i = 0; i < secondary_panes_.size() && (i + 1) < current_layout_.viewports.size(); ++i) {
        if (secondary_panes_[i]) {
            check_and_fix(secondary_panes_[i]->window, current_layout_.viewports[i + 1].monitor);
        }
    }

    if (did_repin) {
        ++repin_count_;
    }
}

void SessionWindow::attach_monitor_hotplug() {
    detach_monitor_hotplug();
    if (!window_) return;
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    if (!display) return;
    monitor_list_model_ = gdk_display_get_monitors(display);
    if (!monitor_list_model_) return;
    monitors_changed_handler_ = g_signal_connect(
        monitor_list_model_,
        "items-changed",
        G_CALLBACK(+[](GListModel*, guint, guint, guint, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            if (self->closing_ || !self->is_multimon_active_) return;
            if (self->hotplug_timer_id_ != 0) {
                g_source_remove(self->hotplug_timer_id_);
            }
            self->hotplug_timer_id_ = g_timeout_add(
                600,
                +[](gpointer d) -> gboolean {
                    auto* s = static_cast<SessionWindow*>(d);
                    s->hotplug_timer_id_ = 0;
                    s->handle_monitors_changed();
                    return G_SOURCE_REMOVE;
                },
                self);
        }),
        this);
}

void SessionWindow::detach_monitor_hotplug() {
    if (hotplug_timer_id_ != 0) {
        g_source_remove(hotplug_timer_id_);
        hotplug_timer_id_ = 0;
    }
    if (monitor_list_model_ && monitors_changed_handler_ != 0) {
        g_signal_handler_disconnect(monitor_list_model_, monitors_changed_handler_);
        monitors_changed_handler_ = 0;
    }
    monitor_list_model_ = nullptr;
}

void SessionWindow::handle_monitors_changed() {
    if (closing_ || !is_multimon_active_ || !session_ || !session_->is_connected() || !window_) {
        return;
    }
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    auto all_monitors = query_local_monitors(display);
    auto selected = filter_selected_monitors(all_monitors, profile_.selected_monitors);
    if (selected.size() > 1) {
        const auto layout = compute_virtual_desktop_layout(selected, profile_.scale_factor);
        sync_monitor_windows(layout, true);
        session_->request_monitor_layout(selected, profile_.scale_factor);
        show_toast("Monitor layout updated (" + std::to_string(layout.viewports.size()) + " displays)");
    } else if (!selected.empty()) {
        sync_monitor_windows({}, true);
        session_->request_resize(
            static_cast<uint32_t>(selected[0].width),
            static_cast<uint32_t>(selected[0].height),
            profile_.scale_factor);
        show_toast("Switched to Single Monitor");
    }
}

void SessionWindow::update_status_banner(SessionState state,
                                         const std::string& msg,
                                         uint32_t attempt,
                                         uint32_t max_attempts) {
    switch (state) {
        case SessionState::Idle:
            gtk_widget_set_visible(status_box_, FALSE);
            if (header_title_) adw_window_title_set_subtitle(header_title_, profile_.hostname.c_str());
            break;
        case SessionState::Connecting:
            gtk_widget_set_visible(status_box_, TRUE);
            gtk_widget_set_visible(status_spinner_, TRUE);
            gtk_spinner_start(GTK_SPINNER(status_spinner_));
            gtk_label_set_text(GTK_LABEL(status_title_), ("Connecting to " + profile_.display_title()).c_str());
            gtk_label_set_text(GTK_LABEL(status_detail_), msg.c_str());
            gtk_widget_set_visible(status_retry_btn_, FALSE);
            gtk_button_set_label(GTK_BUTTON(status_cancel_btn_), "Cancel");
            if (header_title_) adw_window_title_set_subtitle(header_title_, "Connecting...");
            break;
        case SessionState::Connected:
            gtk_spinner_stop(GTK_SPINNER(status_spinner_));
            gtk_widget_set_visible(status_box_, FALSE);
            if (header_title_) {
                adw_window_title_set_subtitle(header_title_, ("Connected — " + profile_.hostname).c_str());
            }
            suspend_coordinated_grab(false);
            if (canvas_) gtk_widget_grab_focus(canvas_->widget());
            break;
        case SessionState::Reconnecting: {
            suspend_coordinated_grab(true);
            gtk_widget_set_visible(status_box_, TRUE);
            gtk_widget_set_visible(status_spinner_, TRUE);
            gtk_spinner_start(GTK_SPINNER(status_spinner_));
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Reconnecting (attempt %u of %u)...", attempt, max_attempts);
            gtk_label_set_text(GTK_LABEL(status_title_), buf);
            gtk_label_set_text(GTK_LABEL(status_detail_), msg.c_str());
            gtk_widget_set_visible(status_retry_btn_, FALSE);
            gtk_button_set_label(GTK_BUTTON(status_cancel_btn_), "Cancel");
            if (header_title_) adw_window_title_set_subtitle(header_title_, "Reconnecting...");
            break;
        }
        case SessionState::Disconnected:
            gtk_spinner_stop(GTK_SPINNER(status_spinner_));
            suspend_coordinated_grab(true);
            if (!closing_) close();
            break;
        case SessionState::Error:
            gtk_spinner_stop(GTK_SPINNER(status_spinner_));
            suspend_coordinated_grab(true);
            gtk_widget_set_visible(status_box_, TRUE);
            gtk_widget_set_visible(status_spinner_, FALSE);
            gtk_label_set_text(GTK_LABEL(status_title_), "Connection Failed");
            gtk_label_set_text(GTK_LABEL(status_detail_), msg.c_str());
            gtk_widget_set_visible(status_retry_btn_, TRUE);
            gtk_button_set_label(GTK_BUTTON(status_cancel_btn_), "Close");
            if (header_title_) adw_window_title_set_subtitle(header_title_, "Connection Failed");
            break;
    }
}

void SessionWindow::show_toast(const std::string& text, guint duration_ms) {
    if (!toast_revealer_ || !toast_label_) return;
    gtk_label_set_text(GTK_LABEL(toast_label_), text.c_str());
    gtk_revealer_set_reveal_child(toast_revealer_, TRUE);
    if (toast_timer_id_ != 0) {
        g_source_remove(toast_timer_id_);
    }
    toast_timer_id_ = g_timeout_add(
        duration_ms,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->toast_timer_id_ = 0;
            if (self->toast_revealer_) {
                gtk_revealer_set_reveal_child(self->toast_revealer_, FALSE);
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

} // namespace winrdp
