#pragma once

#include "config/profile.hpp"
#include "input/input_manager.hpp"
#include "input/monitor_math.hpp"
#include "rdp/rdp_session.hpp"
#include "render/gl_canvas.hpp"
#include "ui/connection_bar.hpp"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace winrdp {

struct SessionWindowCallbacks {
    std::function<void(const ConnectionProfile& updated)> on_profile_changed;
    std::function<void()> on_closed;
};

class SessionWindow {
public:
    SessionWindow(GtkApplication* app,
                  ConnectionProfile profile,
                  SessionWindowCallbacks callbacks = {},
                  bool smoke_test_mode = false,
                  uint64_t session_id = 0);
    ~SessionWindow();

    SessionWindow(const SessionWindow&) = delete;
    SessionWindow& operator=(const SessionWindow&) = delete;

    void present();
    void present_and_focus();
    void close();
    bool is_closed() const { return closed_; }

    uint64_t session_id() const { return session_id_; }
    std::string display_title() const { return profile_.display_title(); }
    std::string hostname() const { return profile_.hostname; }

    GtkWindow* gtk_window() const { return GTK_WINDOW(window_); }
    GlCanvas& canvas() { return *canvas_; }
    ConnectionBar& connection_bar() { return *bar_; }
    bool is_multimon_active() const { return is_multimon_active_; }
    bool is_header_bar_visible() const;
    int header_bar_height() const;

    void show_toast(const std::string& text, guint duration_ms = 1800);

private:
    struct Alive {
        bool valid = true;
    };

    struct SecondaryMonitorPane {
        GtkWindow* window = nullptr;
        std::unique_ptr<GlCanvas> canvas;
        std::unique_ptr<InputManager> input;
        MonitorViewport viewport;
        GdkSurface* bound_surface = nullptr;
        gulong enter_handler = 0;
    };

    void setup_ui();
    void compute_initial_windowed_size();
    void update_window_chrome();
    void update_header_telemetry(const SessionTelemetry& t);
    void toggle_smart_sizing();
    void send_ctrl_alt_del();
    void release_all_grabs_immediately();
    void start_rdp_session();
    void toggle_fullscreen();
    void toggle_multimon();
    void apply_fullscreen_state(bool enter);
    void sync_monitor_windows(const VirtualDesktopLayout& layout, bool enter_fullscreen);
    void destroy_secondary_panes();
    void schedule_dynamic_resize(int pixel_w, int pixel_h);
    void update_status_banner(SessionState state, const std::string& msg, uint32_t attempt, uint32_t max_attempts);

    bool map_pane_pointer(size_t vp_idx, GlCanvas* canvas, double wx, double wy, int& rx, int& ry) const;

    void on_pane_focus_changed(GtkWindow* win, bool focused);
    void on_pane_active_changed(GtkWindow* win);
    void toggle_coordinated_grab();
    void suspend_coordinated_grab(bool suspend);
    void sync_coordinated_grab();
    void release_all_pane_keys();
    InputManager* find_input_for_window(GtkWindow* win) const;

    void attach_surface_monitor_guard(GtkWindow* win, GdkSurface*& slot_surface, gulong& slot_handler);
    void detach_surface_monitor_guard(GdkSurface*& slot_surface, gulong& slot_handler);
    void schedule_monitor_repin();
    void repin_misplaced_monitors();

    void attach_monitor_hotplug();
    void detach_monitor_hotplug();
    void handle_monitors_changed();

    GtkApplication* app_ = nullptr;
    ConnectionProfile profile_;
    SessionWindowCallbacks callbacks_;
    bool smoke_test_mode_ = false;
    uint64_t session_id_ = 0;
    std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();

    AdwApplicationWindow* window_ = nullptr;
    AdwToolbarView* toolbar_view_ = nullptr;
    GtkWidget* header_bar_ = nullptr;
    AdwWindowTitle* header_title_ = nullptr;
    GtkWidget* hdr_grab_btn_ = nullptr;
    GtkWidget* hdr_cad_btn_ = nullptr;
    GtkWidget* hdr_sizing_btn_ = nullptr;
    GtkWidget* hdr_multimon_btn_ = nullptr;
    GtkWidget* hdr_fs_btn_ = nullptr;
    GtkOverlay* overlay_ = nullptr;

    int windowed_w_ = 1280;
    int windowed_h_ = 800;

    std::unique_ptr<GlCanvas> canvas_;
    std::unique_ptr<ConnectionBar> bar_;
    std::unique_ptr<RdpSession> session_;
    std::unique_ptr<InputManager> input_;
    std::vector<std::unique_ptr<SecondaryMonitorPane>> secondary_panes_;
    VirtualDesktopLayout current_layout_{};

    // Status banner overlay (Connecting / Reconnecting / Error)
    GtkWidget* status_box_ = nullptr;
    GtkWidget* status_spinner_ = nullptr;
    GtkWidget* status_title_ = nullptr;
    GtkWidget* status_detail_ = nullptr;
    GtkWidget* status_retry_btn_ = nullptr;
    GtkWidget* status_cancel_btn_ = nullptr;

    // Translucent HUD toast
    GtkRevealer* toast_revealer_ = nullptr;
    GtkWidget* toast_label_ = nullptr;
    guint toast_timer_id_ = 0;

    // Debounced dynamic resolution
    guint resize_timer_id_ = 0;
    uint32_t pending_resize_w_ = 0;
    uint32_t pending_resize_h_ = 0;
    bool is_fullscreen_ = false;
    bool is_multimon_active_ = false;
    bool closing_ = false;
    bool closed_ = false;

    // Coordinated grab across primary + secondary monitor windows
    bool user_grab_enabled_ = true;
    bool grab_suspended_ = false;
    GtkWindow* last_focused_window_ = nullptr;
    GtkWindow* active_grab_window_ = nullptr;
    guint focus_leave_timer_id_ = 0;

    // Monitor pinning guard (prevents WM from moving one monitor pane onto another)
    GdkSurface* primary_bound_surface_ = nullptr;
    gulong primary_enter_handler_ = 0;
    guint repin_timer_id_ = 0;
    uint64_t repin_window_start_ms_ = 0;
    int repin_count_ = 0;

    // Monitor hotplug observer
    GListModel* monitor_list_model_ = nullptr;
    gulong monitors_changed_handler_ = 0;
    guint hotplug_timer_id_ = 0;
};

} // namespace winrdp
