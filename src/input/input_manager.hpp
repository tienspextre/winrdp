#pragma once

#include "config/profile.hpp"
#include "rdp/rdp_session.hpp"
#include "render/gl_canvas.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

namespace winrdp {

struct InputCallbacks {
    std::function<void()> on_toggle_bar;
    std::function<void()> on_toggle_fullscreen;
    std::function<void(bool grabbed)> on_grab_changed;
};

class InputManager {
public:
    InputManager(GtkWindow* window, GlCanvas* canvas, RdpSession* session, InputCallbacks callbacks);
    ~InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    void set_grab_mode(KeyboardGrabMode mode, const std::string& release_key);
    void set_fullscreen(bool is_fullscreen);
    void set_grab_suspended(bool suspended); // e.g., when cert/auth modal is open
    void set_user_grab_enabled(bool enabled);
    bool is_grabbed() const { return grabbed_; }

    void refresh_grab();
    void release_all_keys();

private:
    static gboolean on_key_pressed(GtkEventControllerKey* ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
    static void on_key_released(GtkEventControllerKey* ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
    static void on_motion(GtkEventControllerMotion* ctrl, double x, double y, gpointer user_data);
    static void on_button_pressed(GtkGestureClick* gesture, gint n_press, double x, double y, gpointer user_data);
    static void on_button_released(GtkGestureClick* gesture, gint n_press, double x, double y, gpointer user_data);
    static gboolean on_scroll(GtkEventControllerScroll* ctrl, double dx, double dy, gpointer user_data);
    static void on_focus_enter(GtkEventControllerFocus* ctrl, gpointer user_data);
    static void on_focus_leave(GtkEventControllerFocus* ctrl, gpointer user_data);

    void apply_system_grab(bool want_grab);
    uint32_t query_lock_sync_flags() const;
    bool is_release_key(guint keyval, guint keycode) const;
    bool handle_local_shortcut(guint keyval, GdkModifierType state);

    GtkWindow* window_ = nullptr;
    GlCanvas* canvas_ = nullptr;
    RdpSession* session_ = nullptr;
    InputCallbacks callbacks_;

    KeyboardGrabMode grab_mode_ = KeyboardGrabMode::FullscreenOnly;
    std::string release_key_ = "RightCtrl";
    bool is_fullscreen_ = false;
    bool has_focus_ = false;
    bool grab_suspended_ = false;
    bool user_grab_enabled_ = true;
    bool grabbed_ = false;

    // RightCtrl tap detection so Ctrl+C with RightCtrl still works in Windows,
    // while tapping RightCtrl alone releases/re-captures the keyboard grab!
    bool release_key_armed_ = false;

    std::unordered_set<uint32_t> pressed_scancodes_;

    GtkEventController* key_ctrl_ = nullptr;
    GtkEventController* motion_ctrl_ = nullptr;
    GtkGesture* click_gesture_ = nullptr;
    GtkEventController* scroll_ctrl_ = nullptr;
    GtkEventController* focus_ctrl_ = nullptr;
};

} // namespace winrdp
