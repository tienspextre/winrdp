#include "ui/connection_bar.hpp"

#include <cstdio>

namespace winrdp {

ConnectionBar::ConnectionBar(std::string title,
                             ConnectionBarMode initial_mode,
                             ConnectionBarCallbacks callbacks)
    : title_(std::move(title)),
      mode_(initial_mode),
      callbacks_(std::move(callbacks)) {
    // Transparent 380x4px top-center trigger strip (active only in AutoHide mode)
    trigger_strip_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(trigger_strip_, 380, 4);
    gtk_widget_set_halign(trigger_strip_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(trigger_strip_, GTK_ALIGN_START);
    gtk_widget_add_css_class(trigger_strip_, "winrdp-trigger-strip");

    revealer_ = GTK_REVEALER(gtk_revealer_new());
    gtk_revealer_set_transition_type(revealer_, GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(revealer_, 180);
    gtk_widget_set_halign(GTK_WIDGET(revealer_), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(revealer_), GTK_ALIGN_START);

    bar_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(bar_box_, "winrdp-conn-bar");
    gtk_revealer_set_child(revealer_, bar_box_);

    auto make_icon_btn = [&](const char* icon_name, const char* tooltip) {
        GtkWidget* btn = gtk_button_new_from_icon_name(icon_name);
        gtk_widget_add_css_class(btn, "winrdp-bar-btn");
        gtk_widget_set_focus_on_click(btn, FALSE);
        if (tooltip) gtk_widget_set_tooltip_text(btn, tooltip);
        return btn;
    };

    pin_btn_ = make_icon_btn("view-pin-symbolic", "Pin/Unpin Connection Bar");
    signal_icon_ = gtk_image_new_from_icon_name("network-wireless-signal-excellent-symbolic");
    gtk_widget_add_css_class(signal_icon_, "winrdp-signal-good");
    gtk_widget_set_margin_start(signal_icon_, 4);
    gtk_widget_set_tooltip_text(signal_icon_, "Connection quality");

    codec_label_ = gtk_label_new("GFX");
    gtk_widget_add_css_class(codec_label_, "winrdp-bar-badge");

    title_label_ = gtk_label_new(title_.c_str());
    gtk_widget_add_css_class(title_label_, "winrdp-bar-title");
    gtk_label_set_ellipsize(GTK_LABEL(title_label_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title_label_), 28);

    grab_btn_ = make_icon_btn("input-keyboard-symbolic", "Toggle System Key Capture (Right Ctrl)");
    cad_btn_ = make_icon_btn("system-lock-screen-symbolic", "Send Ctrl+Alt+Del");
    sizing_btn_ = make_icon_btn("zoom-fit-best-symbolic", "Toggle Smart Sizing");
    hide_btn_ = make_icon_btn("go-up-symbolic", "Hide Connection Bar (Ctrl+Alt+Home)");
    min_btn_ = make_icon_btn("window-minimize-symbolic", "Minimize");
    multimon_btn_ = make_icon_btn("video-joined-displays-symbolic", "Toggle Multi-Monitor (Ctrl+Alt+M)");
    fs_btn_ = make_icon_btn("view-restore-symbolic", "Toggle Fullscreen / Windowed (Ctrl+Alt+Enter)");
    close_btn_ = make_icon_btn("window-close-symbolic", "Disconnect");
    gtk_widget_add_css_class(close_btn_, "danger");

    gtk_box_append(GTK_BOX(bar_box_), pin_btn_);
    gtk_box_append(GTK_BOX(bar_box_), signal_icon_);
    gtk_box_append(GTK_BOX(bar_box_), codec_label_);
    gtk_box_append(GTK_BOX(bar_box_), title_label_);
    gtk_box_append(GTK_BOX(bar_box_), grab_btn_);
    gtk_box_append(GTK_BOX(bar_box_), cad_btn_);
    gtk_box_append(GTK_BOX(bar_box_), sizing_btn_);
    gtk_box_append(GTK_BOX(bar_box_), hide_btn_);
    gtk_box_append(GTK_BOX(bar_box_), min_btn_);
    gtk_box_append(GTK_BOX(bar_box_), multimon_btn_);
    gtk_box_append(GTK_BOX(bar_box_), fs_btn_);
    gtk_box_append(GTK_BOX(bar_box_), close_btn_);

    // Hover motion controllers on trigger strip & bar itself
    GtkEventController* trig_motion = gtk_event_controller_motion_new();
    g_signal_connect(
        trig_motion,
        "enter",
        G_CALLBACK(+[](GtkEventControllerMotion*, double, double, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            self->pointer_in_trigger_ = true;
            if (self->mode_ == ConnectionBarMode::AutoHide) {
                self->schedule_hover_reveal();
            }
        }),
        this);
    g_signal_connect(
        trig_motion,
        "leave",
        G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            self->pointer_in_trigger_ = false;
            if (self->mode_ == ConnectionBarMode::AutoHide && !self->pointer_in_bar_) {
                self->schedule_hide();
            }
        }),
        this);
    gtk_widget_add_controller(trigger_strip_, trig_motion);

    GtkEventController* bar_motion = gtk_event_controller_motion_new();
    g_signal_connect(
        bar_motion,
        "enter",
        G_CALLBACK(+[](GtkEventControllerMotion*, double, double, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            self->pointer_in_bar_ = true;
            if (self->hide_timer_id_ != 0) {
                g_source_remove(self->hide_timer_id_);
                self->hide_timer_id_ = 0;
            }
        }),
        this);
    g_signal_connect(
        bar_motion,
        "leave",
        G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            self->pointer_in_bar_ = false;
            if (self->mode_ == ConnectionBarMode::AutoHide || self->temp_revealed_) {
                self->schedule_hide();
            }
        }),
        this);
    gtk_widget_add_controller(bar_box_, bar_motion);

    // Button actions
    g_signal_connect(
        pin_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            ConnectionBarMode next = (self->mode_ == ConnectionBarMode::Pinned)
                                         ? ConnectionBarMode::AutoHide
                                         : ConnectionBarMode::Pinned;
            self->set_mode(next);
            if (self->callbacks_.on_mode_changed) self->callbacks_.on_mode_changed(next);
        }),
        this);

    g_signal_connect(
        grab_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            if (self->callbacks_.on_toggle_grab) self->callbacks_.on_toggle_grab();
        }),
        this);

    g_signal_connect(
        cad_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            if (self->callbacks_.on_send_cad) self->callbacks_.on_send_cad();
        }),
        this);

    g_signal_connect(
        sizing_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            if (self->callbacks_.on_toggle_smart_sizing) self->callbacks_.on_toggle_smart_sizing();
        }),
        this);

    g_signal_connect(
        hide_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            self->set_mode(ConnectionBarMode::Hidden);
            if (self->callbacks_.on_mode_changed) {
                self->callbacks_.on_mode_changed(ConnectionBarMode::Hidden);
            }
        }),
        this);

    g_signal_connect(
        min_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            if (self->callbacks_.on_minimize) self->callbacks_.on_minimize();
        }),
        this);

    g_signal_connect(
        multimon_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            if (self->callbacks_.on_toggle_multimon) self->callbacks_.on_toggle_multimon();
        }),
        this);

    g_signal_connect(
        fs_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            if (self->callbacks_.on_toggle_fullscreen) self->callbacks_.on_toggle_fullscreen();
        }),
        this);

    g_signal_connect(
        close_btn_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ConnectionBar*>(data);
            if (self->callbacks_.on_disconnect) self->callbacks_.on_disconnect();
        }),
        this);

    apply_mode_state();
}

ConnectionBar::~ConnectionBar() {
    cancel_timers();
}

void ConnectionBar::attach_to_overlay(GtkOverlay* overlay) {
    gtk_overlay_add_overlay(overlay, trigger_strip_);
    gtk_overlay_add_overlay(overlay, GTK_WIDGET(revealer_));
}

void ConnectionBar::cancel_timers() {
    if (reveal_timer_id_ != 0) {
        g_source_remove(reveal_timer_id_);
        reveal_timer_id_ = 0;
    }
    if (hide_timer_id_ != 0) {
        g_source_remove(hide_timer_id_);
        hide_timer_id_ = 0;
    }
}

void ConnectionBar::set_mode(ConnectionBarMode mode) {
    mode_ = mode;
    temp_revealed_ = false;
    cancel_timers();
    apply_mode_state();
}

void ConnectionBar::toggle_visibility_hotkey() {
    cancel_timers();
    if (mode_ == ConnectionBarMode::Hidden) {
        temp_revealed_ = !temp_revealed_;
        gtk_revealer_set_reveal_child(revealer_, temp_revealed_ ? TRUE : FALSE);
        return;
    }
    if (mode_ == ConnectionBarMode::Pinned) {
        set_mode(ConnectionBarMode::Hidden);
        if (callbacks_.on_mode_changed) callbacks_.on_mode_changed(ConnectionBarMode::Hidden);
        return;
    }
    // AutoHide: toggle current reveal or hide completely
    if (is_revealed()) {
        gtk_revealer_set_reveal_child(revealer_, FALSE);
    } else {
        gtk_revealer_set_reveal_child(revealer_, TRUE);
    }
}

void ConnectionBar::apply_mode_state(bool force_reveal) {
    if (mode_ == ConnectionBarMode::Pinned) {
        gtk_widget_add_css_class(bar_box_, "pinned");
        gtk_widget_add_css_class(pin_btn_, "active");
        gtk_widget_set_visible(trigger_strip_, FALSE);
        gtk_widget_set_can_target(trigger_strip_, FALSE);
        gtk_revealer_set_reveal_child(revealer_, TRUE);
    } else if (mode_ == ConnectionBarMode::AutoHide) {
        gtk_widget_remove_css_class(bar_box_, "pinned");
        gtk_widget_remove_css_class(pin_btn_, "active");
        gtk_widget_set_visible(trigger_strip_, TRUE);
        gtk_widget_set_can_target(trigger_strip_, TRUE);
        gtk_revealer_set_reveal_child(revealer_, force_reveal ? TRUE : FALSE);
    } else { // Hidden (displayconnectionbar:i:0)
        gtk_widget_remove_css_class(bar_box_, "pinned");
        gtk_widget_remove_css_class(pin_btn_, "active");
        gtk_widget_set_visible(trigger_strip_, FALSE);
        gtk_widget_set_can_target(trigger_strip_, FALSE);
        gtk_revealer_set_reveal_child(revealer_, temp_revealed_ ? TRUE : FALSE);
    }
}

void ConnectionBar::schedule_hover_reveal() {
    if (hide_timer_id_ != 0) {
        g_source_remove(hide_timer_id_);
        hide_timer_id_ = 0;
    }
    if (reveal_timer_id_ != 0) return;
    reveal_timer_id_ = g_timeout_add(
        90,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<ConnectionBar*>(data);
            self->reveal_timer_id_ = 0;
            if (self->pointer_in_trigger_ && self->mode_ == ConnectionBarMode::AutoHide) {
                gtk_revealer_set_reveal_child(self->revealer_, TRUE);
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

void ConnectionBar::schedule_hide() {
    if (reveal_timer_id_ != 0) {
        g_source_remove(reveal_timer_id_);
        reveal_timer_id_ = 0;
    }
    if (hide_timer_id_ != 0) {
        g_source_remove(hide_timer_id_);
    }
    hide_timer_id_ = g_timeout_add(
        450,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<ConnectionBar*>(data);
            self->hide_timer_id_ = 0;
            if (!self->pointer_in_bar_ && !self->pointer_in_trigger_) {
                if (self->mode_ == ConnectionBarMode::AutoHide) {
                    gtk_revealer_set_reveal_child(self->revealer_, FALSE);
                } else if (self->mode_ == ConnectionBarMode::Hidden && self->temp_revealed_) {
                    self->temp_revealed_ = false;
                    gtk_revealer_set_reveal_child(self->revealer_, FALSE);
                }
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

void ConnectionBar::set_title(const std::string& title) {
    title_ = title;
    gtk_label_set_text(GTK_LABEL(title_label_), title_.c_str());
}

void ConnectionBar::set_grabbed(bool grabbed) {
    if (grabbed) {
        gtk_widget_add_css_class(grab_btn_, "active");
    } else {
        gtk_widget_remove_css_class(grab_btn_, "active");
    }
}

void ConnectionBar::set_fullscreen(bool is_fullscreen) {
    gtk_button_set_icon_name(GTK_BUTTON(fs_btn_),
                             is_fullscreen ? "view-restore-symbolic" : "view-fullscreen-symbolic");
}

void ConnectionBar::set_multimon(bool active, size_t monitor_count) {
    if (!multimon_btn_) return;
    if (active) {
        gtk_widget_add_css_class(multimon_btn_, "active");
        char tip[128];
        std::snprintf(tip, sizeof(tip),
                      "Multi-Monitor Active (%zu displays) — Ctrl+Alt+M for Single Monitor",
                      monitor_count);
        gtk_widget_set_tooltip_text(multimon_btn_, tip);
    } else {
        gtk_widget_remove_css_class(multimon_btn_, "active");
        gtk_widget_set_tooltip_text(multimon_btn_, "Toggle Multi-Monitor (Ctrl+Alt+M)");
    }
}

void ConnectionBar::set_smart_sizing(bool enabled) {
    if (enabled) {
        gtk_widget_add_css_class(sizing_btn_, "active");
    } else {
        gtk_widget_remove_css_class(sizing_btn_, "active");
    }
}

void ConnectionBar::update_telemetry(const SessionTelemetry& t) {
    std::string badge = t.codec;
    if (t.hw_accel && (t.codec == "AVC444" || t.codec == "AVC420")) {
        badge += " VA";
    }
    if (t.fps >= 1.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " %.0ffps", t.fps);
        badge += buf;
    }
    gtk_label_set_text(GTK_LABEL(codec_label_), badge.c_str());

    gtk_widget_remove_css_class(signal_icon_, "winrdp-signal-good");
    gtk_widget_remove_css_class(signal_icon_, "winrdp-signal-warn");
    gtk_widget_remove_css_class(signal_icon_, "winrdp-signal-poor");

    if (t.rtt_ms < 0 || t.rtt_ms <= 50) {
        gtk_image_set_from_icon_name(GTK_IMAGE(signal_icon_), "network-wireless-signal-excellent-symbolic");
        gtk_widget_add_css_class(signal_icon_, "winrdp-signal-good");
    } else if (t.rtt_ms <= 140) {
        gtk_image_set_from_icon_name(GTK_IMAGE(signal_icon_), "network-wireless-signal-good-symbolic");
        gtk_widget_add_css_class(signal_icon_, "winrdp-signal-warn");
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(signal_icon_), "network-wireless-signal-weak-symbolic");
        gtk_widget_add_css_class(signal_icon_, "winrdp-signal-poor");
    }

    char mon_str[32] = "";
    if (t.monitor_count > 1) {
        std::snprintf(mon_str, sizeof(mon_str), " (%u monitors)", t.monitor_count);
    }
    char tip[256];
    std::snprintf(tip, sizeof(tip), "%ux%u%s • Codec: %s%s • RTT: %s • %.1f fps",
                  t.width, t.height, mon_str, t.codec.c_str(),
                  t.hw_accel ? " (Intel VA-API)" : "",
                  t.rtt_ms >= 0 ? (std::to_string(t.rtt_ms) + " ms").c_str() : "n/a",
                  t.fps);
    gtk_widget_set_tooltip_text(signal_icon_, tip);
}

bool ConnectionBar::is_revealed() const {
    return gtk_revealer_get_reveal_child(revealer_) == TRUE;
}

} // namespace winrdp
