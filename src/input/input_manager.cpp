#include "input/input_manager.hpp"
#include "input/scancode_map.hpp"

#include <freerdp/input.h>
#include <freerdp/scancode.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <cmath>

namespace winrdp {

InputManager::InputManager(GtkWindow* window,
                           GlCanvas* canvas,
                           RdpSession* session,
                           InputCallbacks callbacks)
    : window_(window),
      canvas_(canvas),
      session_(session),
      callbacks_(std::move(callbacks)) {
    GtkWidget* target = canvas_->widget();

    // Note: intentionally NO GtkIMContext attached so fcitx5/ibus never steals raw keys!
    key_ctrl_ = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl_, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl_, "key-pressed", G_CALLBACK(&InputManager::on_key_pressed), this);
    g_signal_connect(key_ctrl_, "key-released", G_CALLBACK(&InputManager::on_key_released), this);
    gtk_widget_add_controller(target, key_ctrl_);

    motion_ctrl_ = gtk_event_controller_motion_new();
    g_signal_connect(motion_ctrl_, "motion", G_CALLBACK(&InputManager::on_motion), this);
    gtk_widget_add_controller(target, motion_ctrl_);

    click_gesture_ = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture_), 0);
    g_signal_connect(click_gesture_, "pressed", G_CALLBACK(&InputManager::on_button_pressed), this);
    g_signal_connect(click_gesture_, "released", G_CALLBACK(&InputManager::on_button_released), this);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(click_gesture_));

    scroll_ctrl_ = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl_, "scroll", G_CALLBACK(&InputManager::on_scroll), this);
    gtk_widget_add_controller(target, scroll_ctrl_);

    focus_ctrl_ = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl_, "enter", G_CALLBACK(&InputManager::on_focus_enter), this);
    g_signal_connect(focus_ctrl_, "leave", G_CALLBACK(&InputManager::on_focus_leave), this);
    gtk_widget_add_controller(target, focus_ctrl_);
}

InputManager::~InputManager() {
    apply_system_grab(false);
    release_all_keys();
}

void InputManager::set_grab_mode(KeyboardGrabMode mode, const std::string& release_key) {
    grab_mode_ = mode;
    release_key_ = release_key.empty() ? "RightCtrl" : release_key;
    refresh_grab();
}

void InputManager::set_fullscreen(bool is_fullscreen) {
    is_fullscreen_ = is_fullscreen;
    refresh_grab();
}

void InputManager::set_grab_suspended(bool suspended) {
    grab_suspended_ = suspended;
    if (suspended) {
        release_all_keys();
    }
    refresh_grab();
}

void InputManager::set_user_grab_enabled(bool enabled) {
    if (user_grab_enabled_ == enabled) return;
    user_grab_enabled_ = enabled;
    refresh_grab();
    if (callbacks_.on_grab_changed) {
        callbacks_.on_grab_changed(grabbed_);
    }
}

void InputManager::refresh_grab() {
    bool want = false;
    if (has_focus_ && !grab_suspended_ && user_grab_enabled_) {
        if (grab_mode_ == KeyboardGrabMode::AlwaysWhenFocused) {
            want = true;
        } else if (grab_mode_ == KeyboardGrabMode::FullscreenOnly) {
            want = is_fullscreen_;
        }
    }
    apply_system_grab(want);
}

void InputManager::apply_system_grab(bool want_grab) {
    if (grabbed_ == want_grab) return;

    if (!window_) {
        grabbed_ = false;
        return;
    }
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    if (!surface) {
        grabbed_ = false;
        return;
    }

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_SURFACE(surface)) {
        GdkDisplay* display = gdk_surface_get_display(surface);
        Display* xdisplay = gdk_x11_display_get_xdisplay(display);
        Window xwin = gdk_x11_surface_get_xid(surface);
        if (xdisplay && xwin != None) {
            if (want_grab) {
                const int rc = XGrabKeyboard(xdisplay, xwin, True,
                                             GrabModeAsync, GrabModeAsync, CurrentTime);
                XFlush(xdisplay);
                grabbed_ = (rc == GrabSuccess);
            } else {
                XUngrabKeyboard(xdisplay, CurrentTime);
                XFlush(xdisplay);
                grabbed_ = false;
            }
            return;
        }
    }
#endif

    if (GDK_IS_TOPLEVEL(surface)) {
        if (want_grab) {
            gdk_toplevel_inhibit_system_shortcuts(GDK_TOPLEVEL(surface), nullptr);
            grabbed_ = true;
        } else {
            gdk_toplevel_restore_system_shortcuts(GDK_TOPLEVEL(surface));
            grabbed_ = false;
        }
    } else {
        grabbed_ = false;
    }
}

void InputManager::release_all_keys() {
    if (!session_) {
        pressed_scancodes_.clear();
        return;
    }
    for (uint32_t sc : pressed_scancodes_) {
        session_->send_key(sc, false, false);
    }
    pressed_scancodes_.clear();
    release_key_armed_ = false;
}

uint32_t InputManager::query_lock_sync_flags() const {
    uint32_t flags = 0;
#ifdef GDK_WINDOWING_X11
    if (window_) {
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
        if (surface && GDK_IS_X11_SURFACE(surface)) {
            Display* xdisplay = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
            if (xdisplay) {
                unsigned int state = 0;
                if (XkbGetIndicatorState(xdisplay, XkbUseCoreKbd, &state) == Success) {
                    if (state & 0x01U) flags |= KBD_SYNC_CAPS_LOCK;
                    if (state & 0x02U) flags |= KBD_SYNC_NUM_LOCK;
                    if (state & 0x04U) flags |= KBD_SYNC_SCROLL_LOCK;
                    return flags;
                }
            }
        }
    }
#endif
    if (window_) {
        GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
        GdkSeat* seat = display ? gdk_display_get_default_seat(display) : nullptr;
        GdkDevice* kbd = seat ? gdk_seat_get_keyboard(seat) : nullptr;
        if (kbd) {
            if (gdk_device_get_caps_lock_state(kbd)) flags |= KBD_SYNC_CAPS_LOCK;
            if (gdk_device_get_num_lock_state(kbd)) flags |= KBD_SYNC_NUM_LOCK;
            if (gdk_device_get_scroll_lock_state(kbd)) flags |= KBD_SYNC_SCROLL_LOCK;
        }
    }
    return flags;
}

bool InputManager::is_release_key(guint keyval, guint keycode) const {
    if (release_key_ == "RightAlt") {
        return keyval == GDK_KEY_Alt_R || keyval == GDK_KEY_ISO_Level3_Shift || keycode == 108;
    }
    if (release_key_ == "ScrollLock") {
        return keyval == GDK_KEY_Scroll_Lock || keycode == 78;
    }
    // Default: RightCtrl
    return keyval == GDK_KEY_Control_R || keycode == 105;
}

bool InputManager::handle_local_shortcut(guint keyval, GdkModifierType state) {
    const bool ctrl = (state & GDK_CONTROL_MASK) != 0;
    const bool alt = (state & GDK_ALT_MASK) != 0;
    if (!ctrl || !alt) {
        return false;
    }

    if (keyval == GDK_KEY_Home || keyval == GDK_KEY_Pause || keyval == GDK_KEY_Break) {
        if (callbacks_.on_toggle_bar) callbacks_.on_toggle_bar();
        return true;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (callbacks_.on_toggle_fullscreen) callbacks_.on_toggle_fullscreen();
        return true;
    }
    if (keyval == GDK_KEY_g || keyval == GDK_KEY_G) {
        set_user_grab_enabled(!user_grab_enabled_);
        return true;
    }
    return false;
}

gboolean InputManager::on_key_pressed(GtkEventControllerKey* /*ctrl*/,
                                      guint keyval,
                                      guint keycode,
                                      GdkModifierType state,
                                      gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);

    if (self->handle_local_shortcut(keyval, state)) {
        self->release_key_armed_ = false;
        return TRUE;
    }

    if (self->is_release_key(keyval, keycode)) {
        self->release_key_armed_ = true;
    } else {
        self->release_key_armed_ = false;
    }

    const uint32_t sc = x11_keycode_to_rdp_scancode(static_cast<uint16_t>(keycode));
    if (sc != 0 && self->session_) {
        const bool is_repeat = self->pressed_scancodes_.count(sc) > 0;
        self->pressed_scancodes_.insert(sc);
        self->session_->send_key(sc, true, is_repeat);
    }
    return TRUE;
}

void InputManager::on_key_released(GtkEventControllerKey* /*ctrl*/,
                                   guint keyval,
                                   guint keycode,
                                   GdkModifierType /*state*/,
                                   gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);

    const uint32_t sc = x11_keycode_to_rdp_scancode(static_cast<uint16_t>(keycode));
    if (sc != 0 && self->session_) {
        self->pressed_scancodes_.erase(sc);
        self->session_->send_key(sc, false, false);
    }

    if (self->is_release_key(keyval, keycode) && self->release_key_armed_) {
        self->release_key_armed_ = false;
        if (self->grab_mode_ != KeyboardGrabMode::Never) {
            self->set_user_grab_enabled(!self->user_grab_enabled_);
        }
    } else {
        self->release_key_armed_ = false;
    }
}

void InputManager::on_motion(GtkEventControllerMotion* /*ctrl*/,
                             double x,
                             double y,
                             gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);
    if (!self->session_ || !self->canvas_) return;
    int rx = 0, ry = 0;
    if (self->canvas_->map_to_remote(x, y, rx, ry)) {
        self->session_->send_mouse_move(rx, ry);
    }
}

void InputManager::on_button_pressed(GtkGestureClick* gesture,
                                     gint /*n_press*/,
                                     double x,
                                     double y,
                                     gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);
    if (!self->canvas_) return;
    gtk_widget_grab_focus(self->canvas_->widget());

    if (!self->session_) return;
    int rx = 0, ry = 0;
    if (!self->canvas_->map_to_remote(x, y, rx, ry)) return;

    const guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    switch (btn) {
        case 1: self->session_->send_mouse_button(PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON1, rx, ry, false); break;
        case 2: self->session_->send_mouse_button(PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON3, rx, ry, false); break;
        case 3: self->session_->send_mouse_button(PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON2, rx, ry, false); break;
        case 8: self->session_->send_mouse_button(PTR_XFLAGS_DOWN | PTR_XFLAGS_BUTTON1, rx, ry, true); break;
        case 9: self->session_->send_mouse_button(PTR_XFLAGS_DOWN | PTR_XFLAGS_BUTTON2, rx, ry, true); break;
        default: break;
    }
}

void InputManager::on_button_released(GtkGestureClick* gesture,
                                      gint /*n_press*/,
                                      double x,
                                      double y,
                                      gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);
    if (!self->session_ || !self->canvas_) return;
    int rx = 0, ry = 0;
    if (!self->canvas_->map_to_remote(x, y, rx, ry)) return;

    const guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    switch (btn) {
        case 1: self->session_->send_mouse_button(PTR_FLAGS_BUTTON1, rx, ry, false); break;
        case 2: self->session_->send_mouse_button(PTR_FLAGS_BUTTON3, rx, ry, false); break;
        case 3: self->session_->send_mouse_button(PTR_FLAGS_BUTTON2, rx, ry, false); break;
        case 8: self->session_->send_mouse_button(PTR_XFLAGS_BUTTON1, rx, ry, true); break;
        case 9: self->session_->send_mouse_button(PTR_XFLAGS_BUTTON2, rx, ry, true); break;
        default: break;
    }
}

gboolean InputManager::on_scroll(GtkEventControllerScroll* /*ctrl*/,
                                 double dx,
                                 double dy,
                                 gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);
    if (!self->session_) return FALSE;

    if (std::abs(dy) > 0.001) {
        // GTK dy < 0 means scroll UP (forward); RDP positive delta = forward
        const int steps = std::clamp(static_cast<int>(std::lround(-dy * 120.0)), -512, 512);
        if (steps != 0) {
            uint16_t flags = PTR_FLAGS_WHEEL;
            if (steps < 0) {
                flags |= PTR_FLAGS_WHEEL_NEGATIVE | static_cast<uint16_t>((-steps) & WheelRotationMask);
            } else {
                flags |= static_cast<uint16_t>(steps & WheelRotationMask);
            }
            self->session_->send_mouse_wheel(flags);
        }
    }
    if (std::abs(dx) > 0.001) {
        const int steps = std::clamp(static_cast<int>(std::lround(dx * 120.0)), -512, 512);
        if (steps != 0) {
            uint16_t flags = PTR_FLAGS_HWHEEL;
            if (steps < 0) {
                flags |= PTR_FLAGS_WHEEL_NEGATIVE | static_cast<uint16_t>((-steps) & WheelRotationMask);
            } else {
                flags |= static_cast<uint16_t>(steps & WheelRotationMask);
            }
            self->session_->send_mouse_wheel(flags);
        }
    }
    return TRUE;
}

void InputManager::on_focus_enter(GtkEventControllerFocus* /*ctrl*/, gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);
    self->has_focus_ = true;
    self->refresh_grab();
    if (self->session_) {
        const uint32_t locks = self->query_lock_sync_flags();
        self->session_->send_focus_in(static_cast<uint16_t>(locks));
        self->session_->send_sync_locks(locks);
    }
}

void InputManager::on_focus_leave(GtkEventControllerFocus* /*ctrl*/, gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);
    self->has_focus_ = false;
    self->release_all_keys();
    self->apply_system_grab(false);
}

} // namespace winrdp
