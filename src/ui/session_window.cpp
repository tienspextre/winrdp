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
    : app_(app),
      profile_(std::move(profile)),
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
            self->suspend_coordinated_grab(true);
            self->destroy_secondary_panes();
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
            for (const auto& pane : self->secondary_panes_) {
                if (pane && pane->input) {
                    pane->input->set_fullscreen(self->is_fullscreen_);
                }
            }
            self->sync_coordinated_grab();
        }),
        this);

    if (!smoke_test_mode_) {
        start_rdp_session();
    }
}

SessionWindow::~SessionWindow() {
    alive_->valid = false;
    detach_monitor_hotplug();
    if (toast_timer_id_ != 0) {
        g_source_remove(toast_timer_id_);
        toast_timer_id_ = 0;
    }
    if (resize_timer_id_ != 0) {
        g_source_remove(resize_timer_id_);
        resize_timer_id_ = 0;
    }
    if (focus_leave_timer_id_ != 0) {
        g_source_remove(focus_leave_timer_id_);
        focus_leave_timer_id_ = 0;
    }
    if (hotplug_timer_id_ != 0) {
        g_source_remove(hotplug_timer_id_);
        hotplug_timer_id_ = 0;
    }
    suspend_coordinated_grab(true);
    destroy_secondary_panes();
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
        toggle_coordinated_grab();
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
    };
    bar_cb.on_send_cad = [this]() {
        if (session_) {
            session_->send_ctrl_alt_del();
            show_toast("Sent Ctrl+Alt+Del");
            if (canvas_) gtk_widget_grab_focus(canvas_->widget());
        }
    };
    bar_cb.on_toggle_smart_sizing = [this]() {
        profile_.smart_sizing = !profile_.smart_sizing;
        canvas_->set_smart_sizing(profile_.smart_sizing);
        for (const auto& pane : secondary_panes_) {
            if (pane && pane->canvas) {
                pane->canvas->set_smart_sizing(profile_.smart_sizing);
            }
        }
        bar_->set_smart_sizing(profile_.smart_sizing);
        show_toast(profile_.smart_sizing ? "Smart Sizing enabled" : "1:1 Native Scaling");
        if (callbacks_.on_profile_changed) callbacks_.on_profile_changed(profile_);
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
    };
    bar_cb.on_minimize = [this]() {
        suspend_coordinated_grab(true);
        if (window_) gtk_window_minimize(GTK_WINDOW(window_));
        for (const auto& pane : secondary_panes_) {
            if (pane && pane->window) {
                gtk_window_minimize(pane->window);
            }
        }
    };
    bar_cb.on_toggle_multimon = [this]() {
        toggle_multimon();
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
    };
    bar_cb.on_toggle_fullscreen = [this]() {
        toggle_fullscreen();
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
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
            self->destroy_secondary_panes();
            if (self->session_) {
                self->session_->disconnect();
            }
            self->start_rdp_session();
            if (self->profile_.display_mode == DisplayMode::MultiMonitor) {
                self->apply_fullscreen_state(true);
            }
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
    auto all_monitors = query_local_monitors(gtk_widget_get_display(GTK_WIDGET(window_)));
    auto selected = filter_selected_monitors(all_monitors, profile_.selected_monitors);
    if (profile_.display_mode == DisplayMode::Fullscreen && !selected.empty()) {
        profile_.width = static_cast<uint32_t>(selected[0].width);
        profile_.height = static_cast<uint32_t>(selected[0].height);
    }

    SessionCallbacks scb;
    std::weak_ptr<Alive> weak_alive = alive_;

    scb.on_frame_ready = [this, weak_alive]() {
        g_idle_add(
            +[](gpointer data) -> gboolean {
                auto* pair = static_cast<std::pair<SessionWindow*, std::weak_ptr<Alive>>*>(data);
                if (auto a = pair->second.lock(); a && a->valid && !pair->first->closing_) {
                    SessionWindow* self = pair->first;
                    if (self->session_) self->session_->clear_frame_pending();
                    if (self->canvas_) self->canvas_->request_redraw();
                    for (const auto& pane : self->secondary_panes_) {
                        if (pane && pane->canvas) {
                            pane->canvas->request_redraw();
                        }
                    }
                }
                delete pair;
                return G_SOURCE_REMOVE;
            },
            new std::pair<SessionWindow*, std::weak_ptr<Alive>>(this, weak_alive));
    };

    scb.on_desktop_resized = [this, weak_alive](uint32_t new_w, uint32_t new_h) {
        struct ResizeMsg {
            SessionWindow* win;
            std::weak_ptr<Alive> alive;
            uint32_t w;
            uint32_t h;
        };
        g_idle_add(
            +[](gpointer data) -> gboolean {
                std::unique_ptr<ResizeMsg> rm(static_cast<ResizeMsg*>(data));
                if (auto a = rm->alive.lock(); a && a->valid && !rm->win->closing_) {
                    SessionWindow* self = rm->win;
                    if (self->is_multimon_active_ && self->session_) {
                        const auto layout = self->session_->active_layout();
                        if (layout.viewports.size() > 1 &&
                            rm->w == layout.viewports[0].fb_width &&
                            rm->h == layout.viewports[0].fb_height &&
                            (rm->w != layout.desktop_width || rm->h != layout.desktop_height)) {
                            // Server fell back to single-monitor desktop size
                            self->sync_monitor_windows({}, self->is_fullscreen_);
                            self->show_toast("Remote host disabled multi-monitor — using single display");
                        } else if (layout.viewports.size() == self->secondary_panes_.size() + 1) {
                            self->canvas_->set_source_rect(
                                layout.viewports[0].fb_x,
                                layout.viewports[0].fb_y,
                                static_cast<int>(layout.viewports[0].fb_width),
                                static_cast<int>(layout.viewports[0].fb_height));
                            for (size_t i = 1; i < layout.viewports.size(); ++i) {
                                const auto& vp = layout.viewports[i];
                                self->secondary_panes_[i - 1]->viewport = vp;
                                self->secondary_panes_[i - 1]->canvas->set_source_rect(
                                    vp.fb_x,
                                    vp.fb_y,
                                    static_cast<int>(vp.fb_width),
                                    static_cast<int>(vp.fb_height));
                            }
                        }
                    }
                    if (self->canvas_) self->canvas_->request_redraw();
                    for (const auto& pane : self->secondary_panes_) {
                        if (pane && pane->canvas) {
                            pane->canvas->request_redraw();
                        }
                    }
                }
                return G_SOURCE_REMOVE;
            },
            new ResizeMsg{this, weak_alive, new_w, new_h});
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
            if (auto a = weak_alive.lock(); a && a->valid) {
                suspend_coordinated_grab(suspend);
            }
        });
    };

    scb.on_authenticate = [this, weak_alive](AuthPromptInfo& info) -> bool {
        if (closing_) return false;
        const bool ok = prompt_authentication_sync(
            GTK_WINDOW(window_), info, [this, weak_alive](bool suspend) {
                if (auto a = weak_alive.lock(); a && a->valid) {
                    suspend_coordinated_grab(suspend);
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

    session_ = std::make_unique<RdpSession>(profile_, std::move(all_monitors), std::move(scb));
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
    icb.on_toggle_multimon = [this]() {
        toggle_multimon();
    };
    icb.on_focus_changed = [this](bool focused) {
        on_pane_focus_changed(GTK_WINDOW(window_), focused);
    };
    icb.on_toggle_grab_requested = [this]() {
        toggle_coordinated_grab();
    };

    input_ = std::make_unique<InputManager>(
        GTK_WINDOW(window_), canvas_.get(), session_.get(), std::move(icb));
    input_->set_external_grab_control(true);
    input_->set_grab_mode(profile_.keyboard_grab_mode, profile_.grab_release_key);

    attach_monitor_hotplug();
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

void SessionWindow::toggle_multimon() {
    if (!window_ || !session_) return;
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    auto all_monitors = query_local_monitors(display);

    if (is_multimon_active_) {
        std::vector<MonitorInfo> prim = filter_selected_monitors(all_monitors, {});
        MonitorInfo target = prim[0];
        for (const auto& m : prim) {
            if (m.is_primary) {
                target = m;
                break;
            }
        }
        profile_.display_mode = DisplayMode::Fullscreen;
        sync_monitor_windows({}, true);
        if (session_->is_connected()) {
            session_->request_resize(
                static_cast<uint32_t>(target.width),
                static_cast<uint32_t>(target.height),
                profile_.scale_factor);
        }
        show_toast("Switched to Single Monitor");
        return;
    }

    auto selected = filter_selected_monitors(all_monitors, profile_.selected_monitors);
    if (selected.size() < 2 && all_monitors.size() >= 2) {
        selected = filter_selected_monitors(all_monitors, {});
    }
    if (selected.size() < 2) {
        show_toast("Only 1 local monitor detected");
        return;
    }

    profile_.display_mode = DisplayMode::MultiMonitor;
    const auto layout = compute_virtual_desktop_layout(selected, profile_.scale_factor);
    sync_monitor_windows(layout, true);
    if (session_->is_connected()) {
        session_->request_monitor_layout(selected, profile_.scale_factor);
    }
    show_toast("Multi-Monitor enabled (" + std::to_string(layout.viewports.size()) + " displays)");
}

void SessionWindow::apply_fullscreen_state(bool enter) {
    if (!window_) return;
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));

    if (enter) {
        if (profile_.display_mode == DisplayMode::MultiMonitor) {
            auto all_monitors = query_local_monitors(display);
            auto selected = filter_selected_monitors(all_monitors, profile_.selected_monitors);
            if (selected.size() > 1) {
                const auto layout = compute_virtual_desktop_layout(selected, profile_.scale_factor);
                sync_monitor_windows(layout, true);
                if (session_ && session_->is_connected()) {
                    session_->request_monitor_layout(selected, profile_.scale_factor);
                }
                return;
            }
        }
        sync_monitor_windows({}, true);
    } else {
        sync_monitor_windows({}, false);
    }
}

void SessionWindow::destroy_secondary_panes() {
    for (auto& pane : secondary_panes_) {
        if (!pane) continue;
        if (pane->window && active_grab_window_ == pane->window) {
            if (pane->input) {
                pane->input->release_all_keys();
                pane->input->force_grab(false);
            }
            active_grab_window_ = nullptr;
        }
        if (pane->window && last_focused_window_ == pane->window) {
            last_focused_window_ = nullptr;
        }
        if (session_ && pane->canvas) {
            session_->pointer_bridge().detach_widget(pane->canvas->widget());
        }
        pane->input.reset();
        pane->canvas.reset();
        if (pane->window) {
            GtkWindow* w = pane->window;
            pane->window = nullptr;
            g_signal_handlers_disconnect_by_data(w, this);
            g_signal_handlers_disconnect_by_data(w, pane.get());
            gtk_window_destroy(w);
        }
    }
    secondary_panes_.clear();
}

void SessionWindow::sync_monitor_windows(const VirtualDesktopLayout& layout, bool enter_fullscreen) {
    if (!window_) return;
    destroy_secondary_panes();

    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));

    if (enter_fullscreen && layout.viewports.size() > 1) {
        if (resize_timer_id_ != 0) {
            g_source_remove(resize_timer_id_);
            resize_timer_id_ = 0;
        }
        is_multimon_active_ = true;

        const MonitorViewport& vp0 = layout.viewports[0];
        canvas_->set_source_rect(
            vp0.fb_x, vp0.fb_y, static_cast<int>(vp0.fb_width), static_cast<int>(vp0.fb_height));

        GdkMonitor* m0 = find_gdk_monitor(display, vp0.monitor);
        if (m0) {
            gtk_window_fullscreen_on_monitor(GTK_WINDOW(window_), m0);
            g_object_unref(m0);
        } else {
            gtk_window_fullscreen(GTK_WINDOW(window_));
        }

        for (size_t i = 1; i < layout.viewports.size(); ++i) {
            const MonitorViewport& vp = layout.viewports[i];
            auto pane = std::make_unique<SecondaryMonitorPane>();
            pane->viewport = vp;

            GtkWidget* sw = gtk_window_new();
            pane->window = GTK_WINDOW(sw);
            if (app_) {
                gtk_window_set_application(pane->window, app_);
            }
            gtk_window_set_title(pane->window, ("WinRDP — " + profile_.display_title()).c_str());
            gtk_window_set_default_size(pane->window,
                                        std::max(320, vp.monitor.logical_width),
                                        std::max(200, vp.monitor.logical_height));

            pane->canvas = std::make_unique<GlCanvas>();
            pane->canvas->set_smart_sizing(profile_.smart_sizing);
            if (session_) {
                pane->canvas->bind_framebuffer(&session_->framebuffer());
                session_->pointer_bridge().attach_widget(pane->canvas->widget());
            }
            pane->canvas->set_source_rect(
                vp.fb_x, vp.fb_y, static_cast<int>(vp.fb_width), static_cast<int>(vp.fb_height));
            gtk_window_set_child(pane->window, pane->canvas->widget());

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
            icb.on_toggle_multimon = [this]() {
                toggle_multimon();
            };
            icb.on_focus_changed = [this, wptr = pane->window](bool focused) {
                on_pane_focus_changed(wptr, focused);
            };
            icb.on_toggle_grab_requested = [this]() {
                toggle_coordinated_grab();
            };

            pane->input = std::make_unique<InputManager>(
                pane->window, pane->canvas.get(), session_.get(), std::move(icb));
            pane->input->set_external_grab_control(true);
            pane->input->set_fullscreen(true);
            pane->input->set_grab_mode(profile_.keyboard_grab_mode, profile_.grab_release_key);

            g_signal_connect(
                pane->window,
                "close-request",
                G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
                    auto* self = static_cast<SessionWindow*>(data);
                    if (!self->closing_) {
                        self->close();
                    }
                    return TRUE;
                }),
                this);

            g_signal_connect(
                pane->window,
                "destroy",
                G_CALLBACK(+[](GtkWidget*, gpointer data) {
                    auto* p = static_cast<SecondaryMonitorPane*>(data);
                    p->window = nullptr;
                }),
                pane.get());

            GdkMonitor* mi = find_gdk_monitor(display, vp.monitor);
            if (mi) {
                gtk_window_fullscreen_on_monitor(pane->window, mi);
                g_object_unref(mi);
            } else {
                gtk_window_fullscreen(pane->window);
            }
            gtk_window_present(pane->window);
            secondary_panes_.push_back(std::move(pane));
        }

        if (bar_) bar_->set_multimon(true, layout.viewports.size());
        gtk_window_present(GTK_WINDOW(window_));
        if (canvas_) gtk_widget_grab_focus(canvas_->widget());
        return;
    }

    is_multimon_active_ = false;
    canvas_->clear_source_rect();
    if (bar_) bar_->set_multimon(false, 1);

    if (enter_fullscreen) {
        GdkMonitor* target = nullptr;
        auto all_monitors = query_local_monitors(display);
        if (profile_.selected_monitors.size() == 1 && !all_monitors.empty()) {
            auto sel = filter_selected_monitors(all_monitors, profile_.selected_monitors);
            if (!sel.empty()) {
                target = find_gdk_monitor(display, sel[0]);
            }
        }
        if (target) {
            gtk_window_fullscreen_on_monitor(GTK_WINDOW(window_), target);
            g_object_unref(target);
        } else {
            gtk_window_fullscreen(GTK_WINDOW(window_));
        }
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(window_));
    }
}

void SessionWindow::schedule_dynamic_resize(int pixel_w, int pixel_h) {
    if (!session_ || !profile_.dynamic_resolution || pixel_w < 320 || pixel_h < 200) {
        return;
    }
    if (is_multimon_active_) {
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
            if (self->session_ && self->session_->is_connected() && !self->is_multimon_active_) {
                self->session_->request_resize(
                    self->pending_resize_w_, self->pending_resize_h_, self->profile_.scale_factor);
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

InputManager* SessionWindow::find_input_for_window(GtkWindow* win) const {
    if (!win) return nullptr;
    if (window_ && GTK_WINDOW(window_) == win) {
        return input_.get();
    }
    for (const auto& pane : secondary_panes_) {
        if (pane && pane->window == win) {
            return pane->input.get();
        }
    }
    return nullptr;
}

void SessionWindow::release_all_pane_keys() {
    if (input_) input_->release_all_keys();
    for (const auto& pane : secondary_panes_) {
        if (pane && pane->input) {
            pane->input->release_all_keys();
        }
    }
}

void SessionWindow::on_pane_focus_changed(GtkWindow* win, bool focused) {
    if (focused) {
        if (focus_leave_timer_id_ != 0) {
            g_source_remove(focus_leave_timer_id_);
            focus_leave_timer_id_ = 0;
        }
        last_focused_window_ = win;
        sync_coordinated_grab();
        return;
    }

    if (focus_leave_timer_id_ != 0) {
        g_source_remove(focus_leave_timer_id_);
    }
    focus_leave_timer_id_ = g_timeout_add(
        40,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<SessionWindow*>(data);
            self->focus_leave_timer_id_ = 0;

            GtkWindow* focused_win = nullptr;
            if (self->input_ && self->input_->has_focus()) {
                focused_win = GTK_WINDOW(self->window_);
            } else {
                for (const auto& pane : self->secondary_panes_) {
                    if (pane && pane->input && pane->input->has_focus()) {
                        focused_win = pane->window;
                        break;
                    }
                }
            }

            if (focused_win) {
                self->last_focused_window_ = focused_win;
                self->sync_coordinated_grab();
            } else {
                self->last_focused_window_ = nullptr;
                self->release_all_pane_keys();
                self->sync_coordinated_grab();
            }
            return G_SOURCE_REMOVE;
        },
        this);
}

void SessionWindow::toggle_coordinated_grab() {
    user_grab_enabled_ = !user_grab_enabled_;
    sync_coordinated_grab();
    show_toast(active_grab_window_ != nullptr
                   ? "System keys captured (Right Ctrl to release)"
                   : "Keyboard released to Ubuntu (Right Ctrl to capture)");
}

void SessionWindow::suspend_coordinated_grab(bool suspend) {
    grab_suspended_ = suspend;
    if (suspend) {
        release_all_pane_keys();
    }
    sync_coordinated_grab();
}

void SessionWindow::sync_coordinated_grab() {
    bool want_mode = false;
    if (profile_.keyboard_grab_mode == KeyboardGrabMode::AlwaysWhenFocused) {
        want_mode = true;
    } else if (profile_.keyboard_grab_mode == KeyboardGrabMode::FullscreenOnly) {
        want_mode = is_fullscreen_;
    }

    GtkWindow* desired_win = nullptr;
    if (!grab_suspended_ && user_grab_enabled_ && want_mode && last_focused_window_) {
        InputManager* mgr = find_input_for_window(last_focused_window_);
        if (mgr && mgr->has_focus()) {
            desired_win = last_focused_window_;
        }
    }

    if (active_grab_window_ && active_grab_window_ != desired_win) {
        if (InputManager* old_mgr = find_input_for_window(active_grab_window_)) {
            old_mgr->force_grab(false);
        }
        active_grab_window_ = nullptr;
    }

    if (desired_win) {
        if (InputManager* new_mgr = find_input_for_window(desired_win)) {
            if (!new_mgr->is_grabbed()) {
                new_mgr->force_grab(true);
            }
            active_grab_window_ = new_mgr->is_grabbed() ? desired_win : nullptr;
        }
    }

    if (bar_) {
        bar_->set_grabbed(active_grab_window_ != nullptr);
    }
}

void SessionWindow::attach_monitor_hotplug() {
    detach_monitor_hotplug();
    if (!window_) return;
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    if (!display) return;
    monitor_list_model_ = gdk_display_get_monitors(display);
    if (!monitor_list_model_) return;
    monitors_changed_handler_ = g_signal_connect(
        monitor_list_model_,
        "items-changed",
        G_CALLBACK(+[](GListModel*, guint, guint, guint, gpointer data) {
            auto* self = static_cast<SessionWindow*>(data);
            if (self->closing_ || !self->is_multimon_active_) return;
            if (self->hotplug_timer_id_ != 0) {
                g_source_remove(self->hotplug_timer_id_);
            }
            self->hotplug_timer_id_ = g_timeout_add(
                600,
                +[](gpointer d) -> gboolean {
                    auto* s = static_cast<SessionWindow*>(d);
                    s->hotplug_timer_id_ = 0;
                    s->handle_monitors_changed();
                    return G_SOURCE_REMOVE;
                },
                self);
        }),
        this);
}

void SessionWindow::detach_monitor_hotplug() {
    if (hotplug_timer_id_ != 0) {
        g_source_remove(hotplug_timer_id_);
        hotplug_timer_id_ = 0;
    }
    if (monitor_list_model_ && monitors_changed_handler_ != 0) {
        g_signal_handler_disconnect(monitor_list_model_, monitors_changed_handler_);
        monitors_changed_handler_ = 0;
    }
    monitor_list_model_ = nullptr;
}

void SessionWindow::handle_monitors_changed() {
    if (closing_ || !is_multimon_active_ || !session_ || !session_->is_connected() || !window_) {
        return;
    }
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    auto all_monitors = query_local_monitors(display);
    auto selected = filter_selected_monitors(all_monitors, profile_.selected_monitors);
    if (selected.size() > 1) {
        const auto layout = compute_virtual_desktop_layout(selected, profile_.scale_factor);
        sync_monitor_windows(layout, true);
        session_->request_monitor_layout(selected, profile_.scale_factor);
        show_toast("Monitor layout updated (" + std::to_string(layout.viewports.size()) + " displays)");
    } else if (!selected.empty()) {
        sync_monitor_windows({}, true);
        session_->request_resize(
            static_cast<uint32_t>(selected[0].width),
            static_cast<uint32_t>(selected[0].height),
            profile_.scale_factor);
        show_toast("Switched to Single Monitor");
    }
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
            suspend_coordinated_grab(false);
            if (canvas_) gtk_widget_grab_focus(canvas_->widget());
            break;
        case SessionState::Reconnecting: {
            suspend_coordinated_grab(true);
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
            suspend_coordinated_grab(true);
            if (!closing_) close();
            break;
        case SessionState::Error:
            gtk_spinner_stop(GTK_SPINNER(status_spinner_));
            suspend_coordinated_grab(true);
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
