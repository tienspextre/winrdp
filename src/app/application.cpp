#include "app/application.hpp"

#include "config/keyring.hpp"
#include "config/rdp_file.hpp"
#include "rdp/framebuffer.hpp"
#include "rdp/rdp_session.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace winrdp {

namespace {

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

std::string resolve_css_path() {
    const std::string exe_dir = executable_dir();
    std::vector<fs::path> candidates = {
        fs::current_path() / "data" / "style.css",
    };
    if (!exe_dir.empty()) {
        candidates.push_back(fs::path(exe_dir) / "data" / "style.css");
        candidates.push_back(fs::path(exe_dir).parent_path() / "data" / "style.css");
    }
    if (const char* xdg_data = std::getenv("XDG_DATA_HOME")) {
        candidates.push_back(fs::path(xdg_data) / "winrdp" / "style.css");
    } else if (const char* home = std::getenv("HOME")) {
        candidates.push_back(fs::path(home) / ".local" / "share" / "winrdp" / "style.css");
    }
    for (const auto& c : candidates) {
        if (fs::exists(c)) return c.string();
    }
    return {};
}

} // namespace

Application::Application() = default;
Application::~Application() = default;

CliOptions Application::parse_cli(int argc, char** argv) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--version" || arg == "-v") {
            opts.show_version = true;
        } else if (arg == "--smoke-test") {
            opts.smoke_test = true;
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
                        return G_SOURCE_REMOVE;
                    },
                    this);
            },
        },
        false);

    holder->present();
    sessions_.push_back(std::move(holder));
}

int Application::run_smoke_test() {
    adw_init();
    if (!gtk_init_check()) {
        std::cerr << "SMOKE_TEST_FAIL: gtk_init_check failed (check DISPLAY)\n";
        return 2;
    }
    load_css();

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
    std::vector<uint8_t> patch(128 * 128 * 4, 0);
    for (size_t i = 0; i < patch.size(); i += 4) {
        patch[i + 0] = 0xD4; // B
        patch[i + 1] = 0x78; // G
        patch[i + 2] = 0x00; // R
        patch[i + 3] = 0xFF; // A
    }
    test_fb.update_region(patch.data(), 128 * 4, 0, 0, 128, 128);
    sess_win->canvas().bind_framebuffer(&test_fb);

    // Cycle all 3 ConnectionBar modes inside the single GtkOverlay
    sess_win->connection_bar().set_mode(ConnectionBarMode::Pinned);
    const bool pinned_ok = sess_win->connection_bar().is_revealed();
    sess_win->connection_bar().set_mode(ConnectionBarMode::Hidden);
    const bool hidden_ok = !sess_win->connection_bar().is_revealed();
    sess_win->connection_bar().set_mode(ConnectionBarMode::AutoHide);

    sess_win->present();
    sess_win->canvas().request_redraw();

    // Pump GLib main context until GtkGLArea finishes at least 1 render pass (max 2s)
    GMainContext* ctx = g_main_context_default();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(ctx)) {
            g_main_context_iteration(ctx, FALSE);
        }
        if (sess_win->canvas().gl_initialized() &&
            sess_win->canvas().dirty_upload_count() >= 1) {
            break;
        }
        g_usleep(10000);
    }

    const bool gl_ok = sess_win->canvas().gl_initialized() &&
                       sess_win->canvas().last_gl_error() == 0 &&
                       sess_win->canvas().dirty_upload_count() >= 1;
    const bool mgr_ok = gtk_widget_get_realized(GTK_WIDGET(manager_->gtk_window())) == TRUE;

    std::cout << "=== WinRDP Smoke Test Report ===\n";
    std::cout << "FreeRDP version: " << RdpSession::freerdp_version_string() << "\n";
    std::cout << "H.264/FFmpeg GFX support: " << (RdpSession::has_h264_hwaccel_support() ? "YES" : "NO") << "\n";
    std::cout << "LIBVA_DRIVER_NAME: " << (std::getenv("LIBVA_DRIVER_NAME") ? std::getenv("LIBVA_DRIVER_NAME") : "(unset)") << "\n";
    std::cout << "OpenGL initialized: " << (sess_win->canvas().gl_initialized() ? "YES" : "NO") << "\n";
    std::cout << "OpenGL Vendor: " << sess_win->canvas().gl_vendor() << "\n";
    std::cout << "OpenGL Renderer: " << sess_win->canvas().gl_renderer() << "\n";
    std::cout << "OpenGL Version: " << sess_win->canvas().gl_version() << "\n";
    std::cout << "Dirty-rect GL uploads: " << sess_win->canvas().dirty_upload_count()
              << " (glGetError=" << sess_win->canvas().last_gl_error() << ")\n";
    std::cout << "ConnectionBar overlay modes (Pinned=" << pinned_ok
              << ", Hidden=" << hidden_ok << "): " << ((pinned_ok && hidden_ok) ? "OK" : "FAIL") << "\n";
    std::cout << "ManagerWindow realized: " << (mgr_ok ? "OK" : "FAIL") << "\n";

    sess_win.reset();
    manager_.reset();
    g_object_unref(app_);
    app_ = nullptr;

    if (!gl_ok || !pinned_ok || !hidden_ok || !mgr_ok || !RdpSession::has_h264_hwaccel_support()) {
        std::cerr << "SMOKE_TEST_FAIL\n";
        return 1;
    }
    std::cout << "SMOKE_TEST_PASS\n";
    return 0;
}

int Application::run(int argc, char** argv) {
    // Enable Intel iHD VA-API hardware decode by default when available
    if (!std::getenv("LIBVA_DRIVER_NAME") &&
        fs::exists("/usr/lib/x86_64-linux-gnu/dri/iHD_drv_video.so")) {
        setenv("LIBVA_DRIVER_NAME", "iHD", 0);
    }

    cli_ = parse_cli(argc, argv);

    if (cli_.show_version) {
        std::cout << "WinRDP 1.0.0 (FreeRDP " << RdpSession::freerdp_version_string()
                  << ", H264/HWAccel=" << (RdpSession::has_h264_hwaccel_support() ? "ON" : "OFF")
                  << ")\n";
        return 0;
    }

    if (cli_.smoke_test) {
        return run_smoke_test();
    }

    store_.load();

    app_ = adw_application_new("io.winrdp.Client", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(
        app_,
        "activate",
        G_CALLBACK(+[](GtkApplication*, gpointer data) {
            auto* self = static_cast<Application*>(data);
            self->load_css();

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
                    });
            }
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
