#include "ui/session_window.hpp"

#include "config/keyring.hpp"
#include "input/monitor_layout.hpp"
#include "ui/cert_auth_dialogs.hpp"

#include <algorithm>
#include <cstdio>

namespace winrdp {

SessionWindow::SessionWindow(GtkApplication* app,
                             ConnectionProfile profile,
                             SessionWindowCallbacks callbacks,
                             bool smoke_test_mode)
    : profile_(std::move(profile)),
      callbacks_(std::move(callbacks)),
      smoke_test_mode_(smoke_test_mode) {
    window_ = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window_), ("WinRDP — " + profile_.display_title()).c_str());

    const int init_w = std::clamp<int>(static_cast<int>(profile_.width), 640, 1920);
    const int init_h = std::clamp<int>(static_cast<int>(profile_.height), 480, 1080);
    gtk_window_set_default_size(GTK_WINDOW(window_), init_w, init_h);

    setup_ui();

    g_signal_connect(
        window_,
        "close-request",
        G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->closing_ = true;
            self->closed_ = true;
            if (self->input_) {
                self->input_->set_grab_suspended(true);
            }
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
            self->window_ = nullptr;
            self->closed_ = true;
        }),
        this);

    g_signal_connect(
        window_,
        "notify::fullscreened",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            self->is_fullscreen_ = gtk_window_is_fullscreen(GTK_WINDOW(obj)) == TRUE;
            if (self->bar_) self->bar_->set_fullscreen(self->is_fullscreen_);
            if (self->input_) self->input_->set_fullscreen(self->is_fullscreen_);
        }),
        this);

    if (!smoke_test_mode_) {
        start_rdp_session();
    }
}

SessionWindow::~SessionWindow() {
    alive_->valid = false;
    if (toast_timer_id_ != 0) {
        g_source_remove(toast_timer_id_);
        toast_timer_id_ = 0;
    }
    if (resize_timer_id_ != 0) {
        g_source_remove(resize_timer_id_);
        resize_timer_id_ = 0;
    }
    if (session_) {
        session_->disconnect();
    }
    input_.reset();
    bar_.reset();
    canvas_.reset();
    if (window_) {
        GtkWidget* w = GTK_WIDGET(window_);
        window_ = nullptr;
        gtk_window_destroy(GTK_WINDOW(w));
    }
}

void SessionWindow::setup_ui() {
    overlay_ = GTK_OVERLAY(gtk_overlay_new());
    adw_application_window_set_content(window_, GTK_WIDGET(overlay_));

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
    bar_cb.on_mode_changed = [this](ConnectionBarMode mode) {
        profile_.connection_bar_mode = mode;
        if (mode == ConnectionBarMode::Hidden) {
            show_toast("Connection bar hidden — press Ctrl+Alt+Home to show");
        }
        if (callbacks_.on_profile_changed) {
            callbacks_.on_profile_changed(profile_);
        }
    };
    bar_cb.on_toggle_grab = [this]() {
        if (input_) {
            input_->set_user_grab_enabled(!input_->is_grabbed());
            gtk_widget_grab_focus(canvas_->widget());
        }
    };
    bar_cb.on_send_cad = [this]() {
        if (session_) {
            session_->send_ctrl_alt_del();
            show_toast("Sent Ctrl+Alt+Del");
            gtk_widget_grab_focus(canvas_->widget());
        }
    };
    bar_cb.on_toggle_smart_sizing = [this]() {
        profile_.smart_sizing = !profile_.smart_sizing;
        canvas_->set_smart_sizing(profile_.smart_sizing);
        bar_->set_smart_sizing(profile_.smart_sizing);
        show_toast(profile_.smart_sizing ? "Smart Sizing enabled" : "1:1 Native Scaling");
        if (callbacks_.on_profile_changed) callbacks_.on_profile_changed(profile_);
        gtk_widget_grab_focus(canvas_->widget());
    };
    bar_cb.on_minimize = [this]() {
        if (input_) input_->set_grab_suspended(true);
        gtk_window_minimize(GTK_WINDOW(window_));
    };
    bar_cb.on_toggle_fullscreen = [this]() {
        toggle_fullscreen();
        gtk_widget_grab_focus(canvas_->widget());
    };
    bar_cb.on_disconnect = [this]() {
        close();
    };

    bar_ = std::make_unique<ConnectionBar>(
        profile_.display_title(), profile_.connection_bar_mode, std::move(bar_cb));
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
            if (self->session_) {
                self->session_->disconnect();
            }
            self->start_rdp_session();
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
}

void SessionWindow::start_rdp_session() {
    auto monitors = query_local_monitors(gtk_widget_get_display(GTK_WIDGET(window_)));
    if (profile_.display_mode == DisplayMode::Fullscreen && !monitors.empty()) {
        // Prefer primary monitor resolution on initial fullscreen connect
        for (const auto& m : monitors) {
            if (m.is_primary) {
                profile_.width = static_cast<uint32_t>(m.width);
                profile_.height = static_cast<uint32_t>(m.height);
                break;
            }
        }
    }

    SessionCallbacks scb;
    std::weak_ptr<Alive> weak_alive = alive_;

    scb.on_frame_ready = [this, weak_alive]() {
        g_idle_add(
            +[](gpointer data) -> gboolean {
                auto* pair = static_cast<std::pair<SessionWindow*, std::weak_ptr<Alive>>*>(data);
                if (auto a = pair->second.lock(); a && a->valid && !pair->first->closing_) {
                    if (pair->first->session_) pair->first->session_->clear_frame_pending();
                    if (pair->first->canvas_) pair->first->canvas_->request_redraw();
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
                    if (pair->first->canvas_) pair->first->canvas_->request_redraw();
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
                }
                return G_SOURCE_REMOVE;
            },
            tm);
    };

    scb.on_verify_certificate = [this, weak_alive](const CertPromptInfo& info) -> uint32_t {
        if (closing_) return 0;
        return prompt_certificate_sync(GTK_WINDOW(window_), info, [this, weak_alive](bool suspend) {
            if (auto a = weak_alive.lock(); a && a->valid && input_) {
                input_->set_grab_suspended(suspend);
            }
        });
    };

    scb.on_authenticate = [this, weak_alive](AuthPromptInfo& info) -> bool {
        if (closing_) return false;
        const bool ok = prompt_authentication_sync(
            GTK_WINDOW(window_), info, [this, weak_alive](bool suspend) {
                if (auto a = weak_alive.lock(); a && a->valid && input_) {
                    input_->set_grab_suspended(suspend);
                }
            });
        if (ok && info.save_password && !info.is_gateway && !profile_.id.empty()) {
            profile_.username = info.username;
            profile_.domain = info.domain;
            profile_.save_password = true;
            profile_.runtime_password = info.password;
            Keyring::store_secret(profile_.id, "password",
                                  "WinRDP: " + profile_.display_title(), info.password);
            if (callbacks_.on_profile_changed) {
                auto copy = profile_;
                g_idle_add(
                    +[](gpointer data) -> gboolean {
                        auto* p = static_cast<std::pair<SessionWindow*, ConnectionProfile>*>(data);
                        if (p->first->callbacks_.on_profile_changed) {
                            p->first->callbacks_.on_profile_changed(p->second);
                        }
                        delete p;
                        return G_SOURCE_REMOVE;
                    },
                    new std::pair<SessionWindow*, ConnectionProfile>(this, std::move(copy)));
            }
        }
        return ok;
    };

    session_ = std::make_unique<RdpSession>(profile_, std::move(monitors), std::move(scb));
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
        if (bar_->mode() == ConnectionBarMode::Hidden && bar_->is_revealed()) {
            show_toast("Connection bar shown — Ctrl+Alt+Home to hide");
        }
    };
    icb.on_toggle_fullscreen = [this]() {
        toggle_fullscreen();
    };
    icb.on_grab_changed = [this](bool grabbed) {
        if (bar_) bar_->set_grabbed(grabbed);
        show_toast(grabbed ? "System keys captured (Right Ctrl to release)"
                           : "Keyboard released to Ubuntu (Right Ctrl to capture)");
    };

    input_ = std::make_unique<InputManager>(
        GTK_WINDOW(window_), canvas_.get(), session_.get(), std::move(icb));
    input_->set_grab_mode(profile_.keyboard_grab_mode, profile_.grab_release_key);

    session_->start();
}

void SessionWindow::present() {
    gtk_window_present(GTK_WINDOW(window_));
    if (!smoke_test_mode_ &&
        (profile_.display_mode == DisplayMode::Fullscreen ||
         profile_.display_mode == DisplayMode::MultiMonitor)) {
        apply_fullscreen_state(true);
    }
    gtk_widget_grab_focus(canvas_->widget());
}

void SessionWindow::close() {
    if (window_) {
        gtk_window_close(GTK_WINDOW(window_));
    }
}

void SessionWindow::toggle_fullscreen() {
    apply_fullscreen_state(!is_fullscreen_);
}

void SessionWindow::apply_fullscreen_state(bool enter) {
    if (!window_) return;
    if (enter) {
        if (profile_.display_mode == DisplayMode::MultiMonitor) {
            apply_x11_fullscreen_monitors(GTK_WINDOW(window_), true);
        } else {
            apply_x11_fullscreen_monitors(GTK_WINDOW(window_), false);
        }
        gtk_window_fullscreen(GTK_WINDOW(window_));
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(window_));
    }
}

void SessionWindow::schedule_dynamic_resize(int pixel_w, int pixel_h) {
    if (!session_ || !profile_.dynamic_resolution || pixel_w < 320 || pixel_h < 200) {
        return;
    }
    if (profile_.display_mode == DisplayMode::MultiMonitor && is_fullscreen_) {
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
            if (self->session_ && self->session_->is_connected()) {
                self->session_->request_resize(
                    self->pending_resize_w_, self->pending_resize_h_, self->profile_.scale_factor);
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

void SessionWindow::update_status_banner(SessionState state,
                                         const std::string& msg,
                                         uint32_t attempt,
                                         uint32_t max_attempts) {
    switch (state) {
        case SessionState::Idle:
            gtk_widget_set_visible(status_box_, FALSE);
            break;
        case SessionState::Connecting:
            gtk_widget_set_visible(status_box_, TRUE);
            gtk_widget_set_visible(status_spinner_, TRUE);
            gtk_spinner_start(GTK_SPINNER(status_spinner_));
            gtk_label_set_text(GTK_LABEL(status_title_), ("Connecting to " + profile_.display_title()).c_str());
            gtk_label_set_text(GTK_LABEL(status_detail_), msg.c_str());
            gtk_widget_set_visible(status_retry_btn_, FALSE);
            gtk_button_set_label(GTK_BUTTON(status_cancel_btn_), "Cancel");
            break;
        case SessionState::Connected:
            gtk_spinner_stop(GTK_SPINNER(status_spinner_));
            gtk_widget_set_visible(status_box_, FALSE);
            if (input_) {
                input_->set_grab_suspended(false);
                input_->refresh_grab();
            }
            if (bar_) bar_->set_grabbed(input_ && input_->is_grabbed());
            gtk_widget_grab_focus(canvas_->widget());
            break;
        case SessionState::Reconnecting: {
            if (input_) input_->set_grab_suspended(true);
            gtk_widget_set_visible(status_box_, TRUE);
            gtk_widget_set_visible(status_spinner_, TRUE);
            gtk_spinner_start(GTK_SPINNER(status_spinner_));
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Reconnecting (attempt %u of %u)...", attempt, max_attempts);
            gtk_label_set_text(GTK_LABEL(status_title_), buf);
            gtk_label_set_text(GTK_LABEL(status_detail_), msg.c_str());
            gtk_widget_set_visible(status_retry_btn_, FALSE);
            gtk_button_set_label(GTK_BUTTON(status_cancel_btn_), "Cancel");
            break;
        }
        case SessionState::Disconnected:
            gtk_spinner_stop(GTK_SPINNER(status_spinner_));
            if (input_) input_->set_grab_suspended(true);
            if (!closing_) close();
            break;
        case SessionState::Error:
            gtk_spinner_stop(GTK_SPINNER(status_spinner_));
            if (input_) input_->set_grab_suspended(true);
            gtk_widget_set_visible(status_box_, TRUE);
            gtk_widget_set_visible(status_spinner_, FALSE);
            gtk_label_set_text(GTK_LABEL(status_title_), "Connection Failed");
            gtk_label_set_text(GTK_LABEL(status_detail_), msg.c_str());
            gtk_widget_set_visible(status_retry_btn_, TRUE);
            gtk_button_set_label(GTK_BUTTON(status_cancel_btn_), "Close");
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
