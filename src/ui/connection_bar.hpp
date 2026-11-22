#pragma once

#include "config/profile.hpp"
#include "rdp/rdp_session.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <string>

namespace winrdp {

struct ConnectionBarCallbacks {
    std::function<void(ConnectionBarMode mode, bool hide_hover)> on_mode_changed;
    std::function<void()> on_toggle_grab;
    std::function<void()> on_send_cad;
    std::function<void()> on_toggle_smart_sizing;
    std::function<void()> on_minimize;
    std::function<void()> on_toggle_multimon;
    std::function<void()> on_toggle_fullscreen;
    std::function<void()> on_disconnect;
};

class ConnectionBar {
public:
    ConnectionBar(std::string title,
                  ConnectionBarMode initial_mode,
                  bool initial_hide_hover,
                  ConnectionBarCallbacks callbacks);
    ~ConnectionBar();

    ConnectionBar(const ConnectionBar&) = delete;
    ConnectionBar& operator=(const ConnectionBar&) = delete;

    // Adds the trigger strip and top revealer directly into the session GtkOverlay
    // (single window, zero X11 popup flicker!)
    void attach_to_overlay(GtkOverlay* overlay);

    void set_enabled(bool enabled);
    bool is_enabled() const { return enabled_; }

    void set_mode(ConnectionBarMode mode);
    void set_state(bool pinned, bool hide_hover, bool preserve_reveal_if_hovered = false);
    ConnectionBarMode mode() const {
        if (pinned_) return ConnectionBarMode::Pinned;
        return hide_hover_ ? ConnectionBarMode::Hidden : ConnectionBarMode::AutoHide;
    }
    bool is_pinned() const { return pinned_; }
    bool is_hide_hover() const { return hide_hover_; }

    // Used by Ctrl+Alt+Home / Ctrl+Alt+Pause hotkey
    void toggle_visibility_hotkey();

    void set_title(const std::string& title);
    void set_grabbed(bool grabbed);
    void set_fullscreen(bool is_fullscreen);
    void set_multimon(bool active, size_t monitor_count = 1);
    void set_smart_sizing(bool enabled);
    void update_telemetry(const SessionTelemetry& t);

    bool is_revealed() const;

private:
    void apply_mode_state(bool force_reveal = false);
    void schedule_hover_reveal();
    void schedule_hide();
    void schedule_hotkey_auto_hide();
    void cancel_timers();

    std::string title_;
    ConnectionBarCallbacks callbacks_;

    GtkWidget* trigger_strip_ = nullptr;
    GtkRevealer* revealer_ = nullptr;
    GtkWidget* bar_box_ = nullptr;

    GtkWidget* pin_btn_ = nullptr;
    GtkWidget* signal_icon_ = nullptr;
    GtkWidget* codec_label_ = nullptr;
    GtkWidget* title_label_ = nullptr;
    GtkWidget* grab_btn_ = nullptr;
    GtkWidget* cad_btn_ = nullptr;
    GtkWidget* sizing_btn_ = nullptr;
    GtkWidget* hide_btn_ = nullptr;
    GtkWidget* min_btn_ = nullptr;
    GtkWidget* multimon_btn_ = nullptr;
    GtkWidget* fs_btn_ = nullptr;
    GtkWidget* close_btn_ = nullptr;

    bool enabled_ = true;
    bool pinned_ = false;
    bool hide_hover_ = false;
    bool pointer_in_trigger_ = false;
    bool pointer_in_bar_ = false;
    bool hotkey_revealed_ = false;
    guint reveal_timer_id_ = 0;
    guint hide_timer_id_ = 0;
    guint hotkey_idle_timer_id_ = 0;
};

} // namespace winrdp
