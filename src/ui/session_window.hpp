#pragma once

#include "config/profile.hpp"
#include "input/input_manager.hpp"
#include "rdp/rdp_session.hpp"
#include "render/gl_canvas.hpp"
#include "ui/connection_bar.hpp"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

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
                  bool smoke_test_mode = false);
    ~SessionWindow();

    SessionWindow(const SessionWindow&) = delete;
    SessionWindow& operator=(const SessionWindow&) = delete;

    void present();
    void close();
    bool is_closed() const { return closed_; }

    GtkWindow* gtk_window() const { return GTK_WINDOW(window_); }
    GlCanvas& canvas() { return *canvas_; }
    ConnectionBar& connection_bar() { return *bar_; }

    void show_toast(const std::string& text, guint duration_ms = 1800);

private:
    struct Alive {
        bool valid = true;
    };

    void setup_ui();
    void start_rdp_session();
    void toggle_fullscreen();
    void apply_fullscreen_state(bool enter);
    void schedule_dynamic_resize(int pixel_w, int pixel_h);
    void update_status_banner(SessionState state, const std::string& msg, uint32_t attempt, uint32_t max_attempts);

    ConnectionProfile profile_;
    SessionWindowCallbacks callbacks_;
    bool smoke_test_mode_ = false;
    std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();

    AdwApplicationWindow* window_ = nullptr;
    GtkOverlay* overlay_ = nullptr;

    std::unique_ptr<GlCanvas> canvas_;
    std::unique_ptr<ConnectionBar> bar_;
    std::unique_ptr<RdpSession> session_;
    std::unique_ptr<InputManager> input_;

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
    bool closing_ = false;
    bool closed_ = false;
};

} // namespace winrdp
