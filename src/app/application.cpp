#include "app/application.hpp"

#include "config/keyring.hpp"
#include "config/rdp_file.hpp"
#include "input/monitor_layout.hpp"
#include "rdp/framebuffer.hpp"
#include "rdp/rdp_session.hpp"

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace winrdp {

namespace {

#ifndef WINRDP_VERSION
#define WINRDP_VERSION "1.1.3"
#endif

constexpr const char* kFallbackCss = R"CSS(
.winrdp-conn-bar {
    background-color: rgba(18, 22, 30, 0.90);
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-top: none;
    border-radius: 0 0 10px 10px;
    padding: 3px 10px;
    color: #f3f5f8;
}
.winrdp-hud-toast {
    background-color: rgba(16, 20, 28, 0.90);
    border-radius: 999px;
    padding: 8px 18px;
    color: #ffffff;
}
.winrdp-hdr-active {
    background-color: alpha(@accent_bg_color, 0.28);
    color: @accent_color;
}
.winrdp-active-sessions-bar {
    background-color: alpha(@accent_bg_color, 0.10);
    border-bottom: 1px solid alpha(@borders, 0.85);
    padding: 6px 14px;
}
)CSS";

std::string executable_dir() {
    char buf[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return fs::path(buf).parent_path().string();
    }
    return {};
}

bool has_intel_gpu() {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/drm", ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind("renderD", 0) != 0 && name.rfind("card", 0) != 0) {
            continue;
        }
        std::ifstream vendor_file(entry.path() / "device" / "vendor");
        std::string vendor;
        if (vendor_file >> vendor && vendor == "0x8086") {
            return true;
        }
    }
    return false;
}

std::string resolve_css_path() {
    const std::string exe_dir = executable_dir();
    std::error_code ec;
    std::vector<fs::path> candidates;
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        candidates.push_back(cwd / "data" / "style.css");
    }
    if (!exe_dir.empty()) {
        const fs::path exe_path(exe_dir);
        candidates.push_back(exe_path / "data" / "style.css");
        candidates.push_back(exe_path.parent_path() / "data" / "style.css");
        candidates.push_back(exe_path.parent_path() / "share" / "winrdp" / "style.css");
    }
    if (const char* xdg_data = std::getenv("XDG_DATA_HOME")) {
        candidates.push_back(fs::path(xdg_data) / "winrdp" / "style.css");
    } else if (const char* home = std::getenv("HOME")) {
        candidates.push_back(fs::path(home) / ".local" / "share" / "winrdp" / "style.css");
    }
    candidates.push_back(fs::path("/usr/share/winrdp/style.css"));
    candidates.push_back(fs::path("/usr/local/share/winrdp/style.css"));
    for (const auto& c : candidates) {
        if (fs::exists(c, ec) && !ec) return c.string();
    }
    return {};
}

} // namespace

Application::Application() = default;
Application::~Application() = default;

CliOptions Application::parse_cli(int argc, char** argv) {
    CliOptions opts;
    auto parse_monitor_csv = [](const std::string& csv, std::vector<int32_t>& out) {
        std::stringstream ss(csv);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            if (tok.empty()) continue;
            try {
                size_t pos = 0;
                long v = std::stol(tok, &pos, 10);
                if (pos == tok.size() && v >= 0 && v < 64) {
                    const int32_t idx = static_cast<int32_t>(v);
                    if (std::find(out.begin(), out.end(), idx) == out.end()) {
                        out.push_back(idx);
                    }
                }
            } catch (...) {
            }
        }
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--version" || arg == "-v") {
            opts.show_version = true;
        } else if (arg == "--smoke-test") {
            opts.smoke_test = true;
        } else if (arg == "--list-monitors") {
            opts.list_monitors = true;
        } else if (arg == "--multimon" || arg == "-m") {
            opts.force_multimon = true;
        } else if (arg == "--monitors" && i + 1 < argc && argv[i + 1]) {
            parse_monitor_csv(argv[++i], opts.selected_monitors);
        } else if (arg.rfind("--monitors=", 0) == 0) {
            parse_monitor_csv(arg.substr(11), opts.selected_monitors);
        } else if (arg == "--fullscreen" || arg == "-f") {
            opts.force_fullscreen = true;
        } else if (arg == "--windowed" || arg == "-w") {
            opts.force_windowed = true;
        } else if (arg == "--no-bar") {
            opts.force_no_bar = true;
        } else if (arg == "--pinned-bar") {
            opts.force_pinned_bar = true;
        } else if (!arg.empty() && arg[0] != '-' && opts.target.empty()) {
            opts.target = arg;
        }
    }
    return opts;
}

void Application::load_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    const std::string css_path = resolve_css_path();
    if (!css_path.empty()) {
        gtk_css_provider_load_from_path(provider, css_path.c_str());
    } else {
        gtk_css_provider_load_from_string(provider, kFallbackCss);
    }
    GdkDisplay* display = gdk_display_get_default();
    if (display) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

void Application::launch_session(ConnectionProfile profile) {
    if (!profile.id.empty() && profile.runtime_password.empty()) {
        if (auto pw = Keyring::lookup_secret(profile.id, "password")) {
            profile.runtime_password = *pw;
        }
        if (auto gpw = Keyring::lookup_secret(profile.id, "gateway_password")) {
            profile.runtime_gateway_password = *gpw;
        }
    }

    if (cli_.force_fullscreen) profile.display_mode = DisplayMode::Fullscreen;
    if (cli_.force_windowed) profile.display_mode = DisplayMode::Windowed;
    if (cli_.force_multimon) profile.display_mode = DisplayMode::MultiMonitor;
    if (!cli_.selected_monitors.empty()) {
        profile.selected_monitors = cli_.selected_monitors;
        if (cli_.selected_monitors.size() >= 2 && !cli_.force_windowed) {
            profile.display_mode = DisplayMode::MultiMonitor;
        }
    }
    if (cli_.force_no_bar) profile.connection_bar_mode = ConnectionBarMode::Hidden;
    if (cli_.force_pinned_bar) profile.connection_bar_mode = ConnectionBarMode::Pinned;

    if (!profile.id.empty() && store_.find_by_id(profile.id)) {
        const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
        store_.touch_last_connected(profile.id, now_epoch);
        store_.save();
        if (manager_) manager_->refresh();
    }

    auto holder = std::make_unique<SessionWindow>(
        GTK_APPLICATION(app_),
        std::move(profile),
        SessionWindowCallbacks{
            .on_profile_changed = [this](const ConnectionProfile& updated) {
                if (store_.find_by_id(updated.id)) {
                    store_.upsert(updated);
                    store_.save();
                    if (manager_) manager_->refresh();
                }
            },
            .on_closed = [this]() {
                // Defer erase of closed sessions safely via g_idle_add
                g_idle_add(
                    +[](gpointer data) -> gboolean {
                        auto* self = static_cast<Application*>(data);
                        self->sessions_.erase(
                            std::remove_if(self->sessions_.begin(), self->sessions_.end(),
                                           [](const std::unique_ptr<SessionWindow>& s) {
                                               return s->is_closed();
                                           }),
                            self->sessions_.end());
                        self->sync_active_sessions_to_manager();
                        return G_SOURCE_REMOVE;
                    },
                    this);
            },
        },
        false,
        next_session_id_++);

    holder->present();
    sessions_.push_back(std::move(holder));
    sync_active_sessions_to_manager();
}

void Application::sync_active_sessions_to_manager() {
    if (!manager_) return;
    std::vector<ActiveSessionSummary> active;
    active.reserve(sessions_.size());
    for (const auto& s : sessions_) {
        if (!s || s->is_closed()) continue;
        active.push_back(ActiveSessionSummary{
            .id = s->session_id(),
            .title = s->display_title(),
            .host = s->hostname(),
            .multimon = s->is_multimon_active(),
        });
    }
    manager_->set_active_sessions(active);
}

int Application::run_smoke_test() {
    g_set_prgname("io.winrdp.Client");
    g_set_application_name("WinRDP");
    adw_init();
    if (!gtk_init_check()) {
        std::cerr << "SMOKE_TEST_FAIL: gtk_init_check failed (check DISPLAY)\n";
        return 2;
    }
    load_css();
    gtk_window_set_default_icon_name("io.winrdp.Client");

    app_ = adw_application_new("io.winrdp.Client.SmokeTest", G_APPLICATION_NON_UNIQUE);
    g_application_register(G_APPLICATION(app_), nullptr, nullptr);
    store_.load();

    manager_ = std::make_unique<ManagerWindow>(
        GTK_APPLICATION(app_), &store_, ManagerCallbacks{});
    gtk_window_set_default_size(manager_->gtk_window(), 900, 560);
    manager_->present();

    ConnectionProfile test_prof;
    test_prof.id = "smoke-test";
    test_prof.name = "SmokeTest-Win11";
    test_prof.hostname = "127.0.0.1";
    test_prof.display_mode = DisplayMode::Windowed;
    test_prof.width = 640;
    test_prof.height = 480;
    test_prof.connection_bar_mode = ConnectionBarMode::AutoHide;

    auto sess_win = std::make_unique<SessionWindow>(
        GTK_APPLICATION(app_), test_prof, SessionWindowCallbacks{}, true);

    Framebuffer test_fb;
    test_fb.resize(640, 480);
    std::vector<uint8_t> patch(640 * 480 * 4, 0);
    for (size_t i = 0; i < patch.size(); i += 4) {
        patch[i + 0] = 0xD4; // B
        patch[i + 1] = 0x78; // G
        patch[i + 2] = 0x00; // R
        patch[i + 3] = 0xFF; // A
    }
    test_fb.update_region(patch.data(), 640 * 4, 0, 0, 128, 128);
    test_fb.update_region(patch.data(), 640 * 4, 350, 50, 128, 128);
    sess_win->canvas().set_source_rect(0, 0, 320, 480);
    sess_win->canvas().bind_framebuffer(&test_fb);

    // Secondary GlCanvas rendering right half (320..640) from the same shared Framebuffer
    AdwApplicationWindow* sec_win = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app_)));
    gtk_window_set_icon_name(GTK_WINDOW(sec_win), "io.winrdp.Client");
    gtk_window_set_title(GTK_WINDOW(sec_win), "WinRDP SmokeTest Secondary Pane");
    gtk_window_set_default_size(GTK_WINDOW(sec_win), 320, 480);
    auto right_canvas = std::make_unique<GlCanvas>();
    right_canvas->set_source_rect(320, 0, 320, 480);
    right_canvas->bind_framebuffer(&test_fb);
    adw_application_window_set_content(sec_win, right_canvas->widget());

    // Cycle all 3 ConnectionBar modes inside the single GtkOverlay
    sess_win->connection_bar().set_mode(ConnectionBarMode::Pinned);
    const bool pinned_ok = sess_win->connection_bar().is_revealed();
    sess_win->connection_bar().set_mode(ConnectionBarMode::Hidden);
    const bool hidden_ok = !sess_win->connection_bar().is_revealed();
    sess_win->connection_bar().set_mode(ConnectionBarMode::AutoHide);

    sess_win->present();
    gtk_widget_realize(GTK_WIDGET(sec_win));
    SessionWindow::apply_skip_taskbar_hint(GTK_WINDOW(sec_win));
    gtk_window_present(GTK_WINDOW(sec_win));
    SessionWindow::apply_skip_taskbar_hint(GTK_WINDOW(sec_win));
    sess_win->canvas().request_redraw();
    right_canvas->request_redraw();

    auto has_skip_taskbar = [](GtkWindow* win) -> bool {
#ifdef GDK_WINDOWING_X11
        GdkSurface* surface = win ? gtk_native_get_surface(GTK_NATIVE(win)) : nullptr;
        if (!surface || !GDK_IS_X11_SURFACE(surface)) {
            return true; // Non-X11 (e.g., Wayland): EWMH check not applicable
        }
        GdkDisplay* display = gdk_surface_get_display(surface);
        Display* xdisplay = display ? gdk_x11_display_get_xdisplay(display) : nullptr;
        const Window xwin = gdk_x11_surface_get_xid(surface);
        if (!xdisplay || xwin == None) return false;
        const Atom wm_state = XInternAtom(xdisplay, "_NET_WM_STATE", False);
        const Atom skip_tb = XInternAtom(xdisplay, "_NET_WM_STATE_SKIP_TASKBAR", False);
        if (wm_state == None || skip_tb == None) return false;
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char* prop = nullptr;
        bool found = false;
        if (XGetWindowProperty(xdisplay, xwin, wm_state, 0, 64, False, XA_ATOM,
                               &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success &&
            prop) {
            if (actual_type == XA_ATOM && actual_format == 32) {
                const Atom* atoms = reinterpret_cast<const Atom*>(prop);
                for (unsigned long i = 0; i < nitems; ++i) {
                    if (atoms[i] == skip_tb) {
                        found = true;
                        break;
                    }
                }
            }
            XFree(prop);
        }
        return found;
#else
        (void)win;
        return true;
#endif
    };

    // Pump GLib main context until both primary & secondary GtkGLAreas render >= 1 pass and WM sets state (max 2s)
    GMainContext* ctx = g_main_context_default();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(ctx)) {
            g_main_context_iteration(ctx, FALSE);
        }
        if (sess_win->canvas().gl_initialized() &&
            sess_win->canvas().dirty_upload_count() >= 1 &&
            right_canvas->gl_initialized() &&
            right_canvas->dirty_upload_count() >= 1 &&
            has_skip_taskbar(GTK_WINDOW(sec_win))) {
            break;
        }
        g_usleep(10000);
    }

    const bool gl_ok = sess_win->canvas().gl_initialized() &&
                       sess_win->canvas().last_gl_error() == 0 &&
                       sess_win->canvas().dirty_upload_count() >= 1;
    int rx = 0, ry = 0;
    const bool map_ok = right_canvas->map_to_remote(160.0, 240.0, rx, ry) && rx >= 320;
    const bool multimon_gl_ok = right_canvas->gl_initialized() &&
                                right_canvas->last_gl_error() == 0 &&
                                right_canvas->dirty_upload_count() >= 1 &&
                                map_ok;
    const bool skip_taskbar_ok = has_skip_taskbar(GTK_WINDOW(sec_win));
    const bool mgr_ok = gtk_widget_get_realized(GTK_WIDGET(manager_->gtk_window())) == TRUE;
    const bool header_ok = sess_win->is_header_bar_visible() && sess_win->header_bar_height() > 0;
    const char* prgname = g_get_prgname();
    const char* win_icon = gtk_window_get_icon_name(sess_win->gtk_window());
    const bool identity_ok = (prgname && std::string(prgname) == "io.winrdp.Client") &&
                             (win_icon && std::string(win_icon) == "io.winrdp.Client");

    const std::string css_path = resolve_css_path();

    std::cout << "=== WinRDP Smoke Test Report ===\n";
    std::cout << "CSS path: " << (css_path.empty() ? "(fallback)" : css_path) << "\n";
    std::cout << "FreeRDP version: " << RdpSession::freerdp_version_string() << "\n";
    std::cout << "H.264/FFmpeg GFX support: " << (RdpSession::has_h264_hwaccel_support() ? "YES" : "NO") << "\n";
    std::cout << "LIBVA_DRIVER_NAME: " << (std::getenv("LIBVA_DRIVER_NAME") ? std::getenv("LIBVA_DRIVER_NAME") : "(unset)") << "\n";
    std::cout << "OpenGL initialized: " << (sess_win->canvas().gl_initialized() ? "YES" : "NO") << "\n";
    std::cout << "OpenGL Vendor: " << sess_win->canvas().gl_vendor() << "\n";
    std::cout << "OpenGL Renderer: " << sess_win->canvas().gl_renderer() << "\n";
    std::cout << "OpenGL Version: " << sess_win->canvas().gl_version() << "\n";
    std::cout << "Dirty-rect GL uploads (primary): " << sess_win->canvas().dirty_upload_count()
              << " (glGetError=" << sess_win->canvas().last_gl_error() << ")\n";
    std::cout << "Multi-monitor dual-canvas GL (uploads=" << right_canvas->dirty_upload_count()
              << ", map_rx=" << rx << "): " << (multimon_gl_ok ? "OK" : "FAIL") << "\n";
    std::cout << "Secondary pane skip-taskbar hint: " << (skip_taskbar_ok ? "OK" : "FAIL") << "\n";
    std::cout << "ConnectionBar overlay modes (Pinned=" << pinned_ok
              << ", Hidden=" << hidden_ok << "): " << ((pinned_ok && hidden_ok) ? "OK" : "FAIL") << "\n";
    std::cout << "Windowed HeaderBar (visible=" << sess_win->is_header_bar_visible()
              << ", height=" << sess_win->header_bar_height() << "px): "
              << (header_ok ? "OK" : "FAIL") << "\n";
    std::cout << "App identity (prgname=" << (prgname ? prgname : "(null)")
              << ", icon=" << (win_icon ? win_icon : "(null)") << "): "
              << (identity_ok ? "OK" : "FAIL") << "\n";
    std::cout << "ManagerWindow realized: " << (mgr_ok ? "OK" : "FAIL") << "\n";

    right_canvas.reset();
    gtk_window_destroy(GTK_WINDOW(sec_win));
    sess_win.reset();
    manager_.reset();
    g_object_unref(app_);
    app_ = nullptr;

    if (!gl_ok || !multimon_gl_ok || !skip_taskbar_ok || !pinned_ok || !hidden_ok || !header_ok ||
        !identity_ok || !mgr_ok || !RdpSession::has_h264_hwaccel_support()) {
        std::cerr << "SMOKE_TEST_FAIL\n";
        return 1;
    }
    std::cout << "SMOKE_TEST_PASS\n";
    return 0;
}

int Application::run(int argc, char** argv) {
    // Enable Intel iHD VA-API hardware decode by default when available on Intel GPUs
    std::error_code ec;
    if (!std::getenv("LIBVA_DRIVER_NAME") &&
        has_intel_gpu() &&
        fs::exists("/usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so", ec)) {
        setenv("LIBVA_DRIVER_NAME", "iHD", 0);
    }

    cli_ = parse_cli(argc, argv);

    if (cli_.show_version) {
        std::cout << "WinRDP " << WINRDP_VERSION << " (FreeRDP " << RdpSession::freerdp_version_string()
                  << ", H264/HWAccel=" << (RdpSession::has_h264_hwaccel_support() ? "ON" : "OFF")
                  << ")\n";
        return 0;
    }

    if (cli_.list_monitors) {
        adw_init();
        if (!gtk_init_check()) {
            std::cerr << "Failed to initialize GTK display for --list-monitors\n";
            return 2;
        }
        const auto mons = query_local_monitors();
        std::cout << "Detected " << mons.size() << " monitor(s):\n";
        for (const auto& m : mons) {
            std::cout << "  [" << m.index << "] "
                      << (m.connector.empty() ? "unknown" : m.connector)
                      << "  " << m.width << "x" << m.height
                      << "+" << m.x << "+" << m.y
                      << "  scale=" << m.scale_factor << "x"
                      << (m.is_primary ? "  (Primary)" : "");
            if (!m.description.empty()) {
                std::cout << "  - " << m.description;
            }
            std::cout << "\n";
        }
        return 0;
    }

    if (cli_.smoke_test) {
        return run_smoke_test();
    }

    store_.load();

    g_set_prgname("io.winrdp.Client");
    g_set_application_name("WinRDP");

    app_ = adw_application_new("io.winrdp.Client", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(
        app_,
        "activate",
        G_CALLBACK(+[](GtkApplication*, gpointer data) {
            auto* self = static_cast<Application*>(data);
            self->load_css();
            gtk_window_set_default_icon_name("io.winrdp.Client");

            if (!self->cli_.target.empty()) {
                ConnectionProfile direct;
                std::string err;
                if (fs::exists(self->cli_.target) && load_rdp_file(self->cli_.target, direct, &err)) {
                    self->launch_session(std::move(direct));
                    return;
                }
                if (parse_quick_connect_target(self->cli_.target, direct)) {
                    self->launch_session(std::move(direct));
                    return;
                }
                std::cerr << "Invalid target or .rdp file: " << self->cli_.target << "\n";
            }

            if (!self->manager_) {
                self->manager_ = std::make_unique<ManagerWindow>(
                    GTK_APPLICATION(self->app_),
                    &self->store_,
                    ManagerCallbacks{
                        .on_connect = [self](const ConnectionProfile& p) {
                            self->launch_session(p);
                        },
                        .on_focus_session = [self](uint64_t sid) {
                            for (const auto& s : self->sessions_) {
                                if (s && !s->is_closed() && s->session_id() == sid) {
                                    s->present_and_focus();
                                    return;
                                }
                            }
                        },
                        .on_disconnect_session = [self](uint64_t sid) {
                            for (const auto& s : self->sessions_) {
                                if (s && !s->is_closed() && s->session_id() == sid) {
                                    s->close();
                                    return;
                                }
                            }
                        },
                    });
            }
            self->sync_active_sessions_to_manager();
            self->manager_->present();
        }),
        this);

    const int status = g_application_run(G_APPLICATION(app_), 1, argv);
    sessions_.clear();
    manager_.reset();
    g_object_unref(app_);
    app_ = nullptr;
    return status;
}

} // namespace winrdp
