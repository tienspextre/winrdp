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

    // Legacy event controller preserves out-of-bounds motion + button release during
    // cross-monitor drags instead of cancelling at widget boundaries like GtkGestureClick.
    legacy_ctrl_ = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(legacy_ctrl_, GTK_PHASE_CAPTURE);
    g_signal_connect(legacy_ctrl_, "event", G_CALLBACK(&InputManager::on_legacy_event), this);
    gtk_widget_add_controller(target, legacy_ctrl_);

    scroll_ctrl_ = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_ctrl_, "scroll", G_CALLBACK(&InputManager::on_scroll), this);
    gtk_widget_add_controller(target, scroll_ctrl_);

    focus_ctrl_ = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl_, "enter", G_CALLBACK(&InputManager::on_focus_enter), this);
    g_signal_connect(focus_ctrl_, "leave", G_CALLBACK(&InputManager::on_focus_leave), this);
    gtk_widget_add_controller(target, focus_ctrl_);
}

InputManager::~InputManager() {
    cancel_grab_retry();
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
    if (!external_grab_control_ && callbacks_.on_grab_changed) {
        callbacks_.on_grab_changed(grabbed_);
    }
}

void InputManager::set_external_grab_control(bool external) {
    external_grab_control_ = external;
}

void InputManager::force_grab(bool want_grab) {
    if (want_grab) {
        grab_retry_count_ = 0;
    }
    apply_system_grab(want_grab);
}

bool InputManager::is_x11_surface() const {
#ifdef GDK_WINDOWING_X11
    if (!window_) return false;
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window_));
    return surface && GDK_IS_X11_SURFACE(surface);
#else
    return false;
#endif
}

void InputManager::cancel_grab_retry() {
    if (grab_retry_timer_id_ != 0) {
        g_source_remove(grab_retry_timer_id_);
        grab_retry_timer_id_ = 0;
    }
    grab_retry_count_ = 0;
}

void InputManager::refresh_grab() {
    if (external_grab_control_) return;
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
    want_grab_ = want_grab;
    if (!want_grab) {
        cancel_grab_retry();
    }
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
                if (grabbed_) {
                    cancel_grab_retry();
                } else if (grab_retry_timer_id_ == 0 && grab_retry_count_ < 8) {
                    grab_retry_timer_id_ = g_timeout_add(
                        100,
                        +[](gpointer data) -> gboolean {
                            auto* self = static_cast<InputManager*>(data);
                            self->grab_retry_timer_id_ = 0;
                            if (!self->want_grab_ || self->grabbed_) {
                                return G_SOURCE_REMOVE;
                            }
                            ++self->grab_retry_count_;
                            self->apply_system_grab(true);
                            if (self->grabbed_ && self->callbacks_.on_grab_changed) {
                                self->callbacks_.on_grab_changed(true);
                            }
                            return G_SOURCE_REMOVE;
                        },
                        this);
                }
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
    if (keyval == GDK_KEY_m || keyval == GDK_KEY_M) {
        if (callbacks_.on_toggle_multimon) callbacks_.on_toggle_multimon();
        return true;
    }
    if (keyval == GDK_KEY_g || keyval == GDK_KEY_G) {
        if (external_grab_control_ && callbacks_.on_toggle_grab_requested) {
            callbacks_.on_toggle_grab_requested();
        } else {
            set_user_grab_enabled(!user_grab_enabled_);
        }
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
            if (self->external_grab_control_ && self->callbacks_.on_toggle_grab_requested) {
                self->callbacks_.on_toggle_grab_requested();
            } else {
                self->set_user_grab_enabled(!self->user_grab_enabled_);
            }
        }
    } else {
        self->release_key_armed_ = false;
    }
}

bool InputManager::map_coords(double wx, double wy, int& rx, int& ry) const {
    if (callbacks_.map_pointer) {
        return callbacks_.map_pointer(wx, wy, rx, ry);
    }
    return canvas_ ? canvas_->map_to_remote(wx, wy, rx, ry) : false;
}

gboolean InputManager::on_legacy_event(GtkEventControllerLegacy* /*ctrl*/,
                                       GdkEvent* event,
                                       gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);
    if (!event || !self->canvas_ || !self->window_) return FALSE;

    const GdkEventType et = gdk_event_get_event_type(event);
    if (et != GDK_MOTION_NOTIFY && et != GDK_BUTTON_PRESS && et != GDK_BUTTON_RELEASE) {
        return FALSE;
    }

    double sx = 0.0, sy = 0.0;
    if (!gdk_event_get_position(event, &sx, &sy)) {
        return FALSE;
    }

    double nx = 0.0, ny = 0.0;
    gtk_native_get_surface_transform(GTK_NATIVE(self->window_), &nx, &ny);
    graphene_point_t in_pt = GRAPHENE_POINT_INIT(
        static_cast<float>(sx - nx), static_cast<float>(sy - ny));
    graphene_point_t out_pt{};
    double wx = static_cast<double>(in_pt.x);
    double wy = static_cast<double>(in_pt.y);
    if (gtk_widget_compute_point(GTK_WIDGET(self->window_), self->canvas_->widget(), &in_pt, &out_pt)) {
        wx = static_cast<double>(out_pt.x);
        wy = static_cast<double>(out_pt.y);
    }

    if (et == GDK_MOTION_NOTIFY) {
        if (self->session_) {
            int rx = 0, ry = 0;
            if (self->map_coords(wx, wy, rx, ry)) {
                self->session_->send_mouse_move(rx, ry);
            }
        }
        return FALSE;
    }

    const guint btn = gdk_button_event_get_button(event);
    if (et == GDK_BUTTON_PRESS) {
        gtk_widget_grab_focus(self->canvas_->widget());
        if (!self->session_) return TRUE;
        int rx = 0, ry = 0;
        if (!self->map_coords(wx, wy, rx, ry)) return TRUE;
        switch (btn) {
            case 1: self->session_->send_mouse_button(PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON1, rx, ry, false); break;
            case 2: self->session_->send_mouse_button(PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON3, rx, ry, false); break;
            case 3: self->session_->send_mouse_button(PTR_FLAGS_DOWN | PTR_FLAGS_BUTTON2, rx, ry, false); break;
            case 8: self->session_->send_mouse_button(PTR_XFLAGS_DOWN | PTR_XFLAGS_BUTTON1, rx, ry, true); break;
            case 9: self->session_->send_mouse_button(PTR_XFLAGS_DOWN | PTR_XFLAGS_BUTTON2, rx, ry, true); break;
            default: break;
        }
        return TRUE;
    }

    // GDK_BUTTON_RELEASE
    if (!self->session_) return TRUE;
    int rx = 0, ry = 0;
    if (!self->map_coords(wx, wy, rx, ry)) return TRUE;
    switch (btn) {
        case 1: self->session_->send_mouse_button(PTR_FLAGS_BUTTON1, rx, ry, false); break;
        case 2: self->session_->send_mouse_button(PTR_FLAGS_BUTTON3, rx, ry, false); break;
        case 3: self->session_->send_mouse_button(PTR_FLAGS_BUTTON2, rx, ry, false); break;
        case 8: self->session_->send_mouse_button(PTR_XFLAGS_BUTTON1, rx, ry, true); break;
        case 9: self->session_->send_mouse_button(PTR_XFLAGS_BUTTON2, rx, ry, true); break;
        default: break;
    }
    return TRUE;
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
    if (!self->external_grab_control_) {
        self->refresh_grab();
    }
    if (self->session_) {
        const uint32_t locks = self->query_lock_sync_flags();
        self->session_->send_focus_in(static_cast<uint16_t>(locks));
        self->session_->send_sync_locks(locks);
    }
    if (self->callbacks_.on_focus_changed) {
        self->callbacks_.on_focus_changed(true);
    }
}

void InputManager::on_focus_leave(GtkEventControllerFocus* /*ctrl*/, gpointer user_data) {
    auto* self = static_cast<InputManager*>(user_data);
    self->has_focus_ = false;
    if (!self->external_grab_control_) {
        self->release_all_keys();
        self->apply_system_grab(false);
    }
    if (self->callbacks_.on_focus_changed) {
        self->callbacks_.on_focus_changed(false);
    }
}

} // namespace winrdp
