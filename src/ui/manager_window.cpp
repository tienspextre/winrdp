#include "ui/manager_window.hpp"

#include "config/keyring.hpp"
#include "config/rdp_file.hpp"
#include "ui/profile_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <memory>

namespace winrdp {

namespace {

std::string to_lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool matches_search(const ConnectionProfile& p, const std::string& q_lower) {
    if (q_lower.empty()) return true;
    return to_lower_copy(p.name).find(q_lower) != std::string::npos ||
           to_lower_copy(p.hostname).find(q_lower) != std::string::npos ||
           to_lower_copy(p.username).find(q_lower) != std::string::npos ||
           to_lower_copy(p.group).find(q_lower) != std::string::npos;
}

} // namespace

ManagerWindow::ManagerWindow(GtkApplication* app, ProfileStore* store, ManagerCallbacks callbacks)
    : store_(store),
      callbacks_(std::move(callbacks)) {
    window_ = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window_), "WinRDP — Remote Desktop Manager");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1040, 680);

    g_signal_connect(
        window_,
        "destroy",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            static_cast<ManagerWindow*>(data)->window_ = nullptr;
        }),
        this);

    setup_ui();
    refresh();
}

ManagerWindow::~ManagerWindow() {
    if (window_) {
        GtkWidget* w = GTK_WIDGET(window_);
        window_ = nullptr;
        gtk_window_destroy(GTK_WINDOW(w));
    }
}

void ManagerWindow::present() {
    gtk_window_present(GTK_WINDOW(window_));
}

void ManagerWindow::refresh() {
    rebuild_sidebar();
    rebuild_cards();
}

void ManagerWindow::setup_ui() {
    split_ = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
    adw_application_window_set_content(window_, GTK_WIDGET(split_));

    // ---- Sidebar ----
    GtkWidget* side_toolbar = adw_toolbar_view_new();
    GtkWidget* side_header = adw_header_bar_new();
    GtkWidget* app_title = gtk_label_new("WinRDP");
    gtk_widget_add_css_class(app_title, "heading");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(side_header), app_title);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(side_toolbar), side_header);

    GtkWidget* side_scroll = gtk_scrolled_window_new();
    sidebar_list_ = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(sidebar_list_), "navigation-sidebar");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side_scroll), GTK_WIDGET(sidebar_list_));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(side_toolbar), side_scroll);

    g_signal_connect(
        sidebar_list_,
        "row-selected",
        G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
            auto* self = static_cast<ManagerWindow*>(data);
            if (!row) return;
            const int idx = gtk_list_box_row_get_index(row);
            if (idx < 0 || static_cast<size_t>(idx) >= self->sidebar_row_keys_.size()) return;
            const std::string& key = self->sidebar_row_keys_[static_cast<size_t>(idx)];
            if (key == "__all__") {
                self->filter_kind_ = FilterKind::All;
                self->filter_group_.clear();
            } else if (key == "__fav__") {
                self->filter_kind_ = FilterKind::Favorites;
                self->filter_group_.clear();
            } else if (key == "__recent__") {
                self->filter_kind_ = FilterKind::Recent;
                self->filter_group_.clear();
            } else {
                self->filter_kind_ = FilterKind::Group;
                self->filter_group_ = key;
            }
            self->rebuild_cards();
        }),
        this);

    AdwNavigationPage* side_page = adw_navigation_page_new(side_toolbar, "Library");
    adw_navigation_split_view_set_sidebar(split_, side_page);

    // ---- Main Content ----
    GtkWidget* content_toolbar = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();

    // Quick connect box in header
    GtkWidget* quick_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    quick_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(quick_entry_), "Quick Connect: user@host:3389");
    gtk_widget_set_size_request(quick_entry_, 280, -1);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(quick_entry_), GTK_ENTRY_ICON_PRIMARY, "network-server-symbolic");

    GtkWidget* quick_btn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(quick_btn, "suggested-action");

    gtk_box_append(GTK_BOX(quick_box), quick_entry_);
    gtk_box_append(GTK_BOX(quick_box), quick_btn);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), quick_box);

    GtkWidget* new_btn = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(new_btn, "New Connection");

    GtkWidget* import_btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(import_btn, "Import .rdp File");

    GtkWidget* search_btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(search_btn), "system-search-symbolic");
    gtk_widget_set_tooltip_text(search_btn, "Search Connections");

    adw_header_bar_pack_start(ADW_HEADER_BAR(header), new_btn);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), import_btn);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), search_btn);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_toolbar), header);

    // Search bar
    search_bar_ = gtk_search_bar_new();
    search_entry_ = gtk_search_entry_new();
    gtk_widget_set_size_request(search_entry_, 340, -1);
    gtk_search_bar_set_child(GTK_SEARCH_BAR(search_bar_), search_entry_);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(search_bar_), GTK_EDITABLE(search_entry_));
    g_object_bind_property(search_btn, "active", search_bar_, "search-mode-enabled",
                           G_BINDING_BIDIRECTIONAL);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(content_toolbar), search_bar_);

    g_signal_connect(
        search_entry_,
        "search-changed",
        G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
            auto* self = static_cast<ManagerWindow*>(data);
            self->search_query_ = to_lower_copy(gtk_editable_get_text(GTK_EDITABLE(entry)));
            self->rebuild_cards();
        }),
        this);

    // Content stack (Empty vs Grid)
    stack_ = gtk_stack_new();
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content_toolbar), stack_);

    AdwStatusPage* empty_page = ADW_STATUS_PAGE(adw_status_page_new());
    adw_status_page_set_icon_name(empty_page, "computer-symbolic");
    adw_status_page_set_title(empty_page, "No Remote Desktops");
    adw_status_page_set_description(
        empty_page,
        "Create a connection profile, import a Windows .rdp file, or use Quick Connect above.");

    GtkWidget* empty_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(empty_actions, GTK_ALIGN_CENTER);
    GtkWidget* empty_new = gtk_button_new_with_label("New Connection");
    gtk_widget_add_css_class(empty_new, "pill");
    gtk_widget_add_css_class(empty_new, "suggested-action");
    GtkWidget* empty_imp = gtk_button_new_with_label("Import .rdp");
    gtk_widget_add_css_class(empty_imp, "pill");
    gtk_box_append(GTK_BOX(empty_actions), empty_new);
    gtk_box_append(GTK_BOX(empty_actions), empty_imp);
    adw_status_page_set_child(empty_page, empty_actions);

    gtk_stack_add_named(GTK_STACK(stack_), GTK_WIDGET(empty_page), "empty");

    GtkWidget* scroll = gtk_scrolled_window_new();
    flow_box_ = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow_box_), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow_box_), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow_box_), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow_box_), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow_box_), 12);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow_box_), 12);
    gtk_widget_set_valign(flow_box_, GTK_ALIGN_START);
    gtk_widget_set_margin_top(flow_box_, 18);
    gtk_widget_set_margin_bottom(flow_box_, 18);
    gtk_widget_set_margin_start(flow_box_, 18);
    gtk_widget_set_margin_end(flow_box_, 18);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), flow_box_);
    gtk_stack_add_named(GTK_STACK(stack_), scroll, "grid");

    AdwNavigationPage* content_page = adw_navigation_page_new(content_toolbar, "Connections");
    adw_navigation_split_view_set_content(split_, content_page);

    // Connect header / empty actions
    g_signal_connect(
        quick_entry_,
        "activate",
        G_CALLBACK(+[](GtkEntry*, gpointer data) {
            static_cast<ManagerWindow*>(data)->on_quick_connect_activated();
        }),
        this);
    g_signal_connect(
        quick_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<ManagerWindow*>(data)->on_quick_connect_activated();
        }),
        this);

    auto new_cb = G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<ManagerWindow*>(data)->on_new_connection();
    });
    auto imp_cb = G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<ManagerWindow*>(data)->on_import_rdp();
    });
    g_signal_connect(new_btn, "clicked", new_cb, this);
    g_signal_connect(empty_new, "clicked", new_cb, this);
    g_signal_connect(import_btn, "clicked", imp_cb, this);
    g_signal_connect(empty_imp, "clicked", imp_cb, this);
}

void ManagerWindow::rebuild_sidebar() {
    while (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(sidebar_list_))) {
        gtk_list_box_remove(sidebar_list_, child);
    }
    sidebar_row_keys_.clear();

    auto add_row = [&](const char* icon_name, const std::string& label, const std::string& key, bool select) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);
        gtk_widget_set_margin_start(box, 8);
        gtk_widget_set_margin_end(box, 8);
        GtkWidget* ico = gtk_image_new_from_icon_name(icon_name);
        GtkWidget* lbl = gtk_label_new(label.c_str());
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), ico);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_list_box_append(sidebar_list_, box);
        sidebar_row_keys_.push_back(key);
        if (select) {
            GtkListBoxRow* r = gtk_list_box_get_row_at_index(
                sidebar_list_, static_cast<int>(sidebar_row_keys_.size()) - 1);
            if (r) gtk_list_box_select_row(sidebar_list_, r);
        }
    };

    add_row("view-grid-symbolic", "All Connections", "__all__", filter_kind_ == FilterKind::All);
    add_row("starred-symbolic", "Favorites", "__fav__", filter_kind_ == FilterKind::Favorites);
    add_row("document-open-recent-symbolic", "Recent", "__recent__", filter_kind_ == FilterKind::Recent);

    for (const auto& grp : store_->groups()) {
        add_row("folder-symbolic", grp, grp,
                filter_kind_ == FilterKind::Group && filter_group_ == grp);
    }
}

void ManagerWindow::rebuild_cards() {
    while (GtkWidget* child = gtk_widget_get_first_child(flow_box_)) {
        gtk_flow_box_remove(GTK_FLOW_BOX(flow_box_), child);
    }

    std::vector<ConnectionProfile> filtered;
    for (const auto& p : store_->profiles()) {
        if (filter_kind_ == FilterKind::Favorites && !p.favorite) continue;
        if (filter_kind_ == FilterKind::Recent && p.last_connected_epoch == 0) continue;
        if (filter_kind_ == FilterKind::Group && p.group != filter_group_) continue;
        if (!matches_search(p, search_query_)) continue;
        filtered.push_back(p);
    }

    if (filter_kind_ == FilterKind::Recent) {
        std::sort(filtered.begin(), filtered.end(),
                  [](const ConnectionProfile& a, const ConnectionProfile& b) {
                      return a.last_connected_epoch > b.last_connected_epoch;
                  });
    } else {
        std::sort(filtered.begin(), filtered.end(),
                  [](const ConnectionProfile& a, const ConnectionProfile& b) {
                      if (a.favorite != b.favorite) return a.favorite > b.favorite;
                      return to_lower_copy(a.display_title()) < to_lower_copy(b.display_title());
                  });
    }

    if (filtered.empty()) {
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "empty");
        return;
    }

    for (const auto& p : filtered) {
        gtk_flow_box_append(GTK_FLOW_BOX(flow_box_), build_connection_card(p));
    }
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "grid");
}

GtkWidget* ManagerWindow::build_connection_card(const ConnectionProfile& p) {
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(card, "winrdp-card");
    gtk_widget_set_size_request(card, 240, -1);

    // Top row: icon + titles + favorite star
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* ico = gtk_image_new_from_icon_name("computer-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ico), 32);

    GtkWidget* titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(titles, TRUE);
    GtkWidget* name_lbl = gtk_label_new(p.display_title().c_str());
    gtk_widget_add_css_class(name_lbl, "winrdp-card-title");
    gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);

    std::string sub = p.hostname + (p.port != 3389 ? (":" + std::to_string(p.port)) : "");
    if (!p.username.empty()) sub = p.username + "@" + sub;
    GtkWidget* sub_lbl = gtk_label_new(sub.c_str());
    gtk_widget_add_css_class(sub_lbl, "winrdp-card-subtitle");
    gtk_widget_set_halign(sub_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(sub_lbl), PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(titles), name_lbl);
    gtk_box_append(GTK_BOX(titles), sub_lbl);

    GtkWidget* fav_btn = gtk_button_new_from_icon_name(
        p.favorite ? "starred-symbolic" : "non-starred-symbolic");
    gtk_widget_add_css_class(fav_btn, "flat");
    gtk_widget_set_valign(fav_btn, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(top), ico);
    gtk_box_append(GTK_BOX(top), titles);
    gtk_box_append(GTK_BOX(top), fav_btn);
    gtk_box_append(GTK_BOX(card), top);

    // Tags row
    GtkWidget* tags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* mode_tag = gtk_label_new(to_string(p.display_mode));
    gtk_widget_add_css_class(mode_tag, "winrdp-pill-tag");
    GtkWidget* codec_tag = gtk_label_new(to_string(p.codec));
    gtk_widget_add_css_class(codec_tag, "winrdp-pill-tag");
    gtk_box_append(GTK_BOX(tags), mode_tag);
    gtk_box_append(GTK_BOX(tags), codec_tag);
    if (!p.group.empty()) {
        GtkWidget* grp_tag = gtk_label_new(p.group.c_str());
        gtk_widget_add_css_class(grp_tag, "winrdp-pill-tag");
        gtk_box_append(GTK_BOX(tags), grp_tag);
    }
    gtk_box_append(GTK_BOX(card), tags);

    // Bottom action row
    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* conn_btn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(conn_btn, "suggested-action");
    gtk_widget_set_hexpand(conn_btn, TRUE);

    GtkWidget* edit_btn = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_add_css_class(edit_btn, "flat");
    gtk_widget_set_tooltip_text(edit_btn, "Edit Connection");

    GtkWidget* exp_btn = gtk_button_new_from_icon_name("document-save-symbolic");
    gtk_widget_add_css_class(exp_btn, "flat");
    gtk_widget_set_tooltip_text(exp_btn, "Export .rdp");

    GtkWidget* del_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(del_btn, "flat");
    gtk_widget_set_tooltip_text(del_btn, "Delete");

    gtk_box_append(GTK_BOX(actions), conn_btn);
    gtk_box_append(GTK_BOX(actions), edit_btn);
    gtk_box_append(GTK_BOX(actions), exp_btn);
    gtk_box_append(GTK_BOX(actions), del_btn);
    gtk_box_append(GTK_BOX(card), actions);

    struct CardCtx {
        ManagerWindow* self;
        ConnectionProfile profile;
    };
    auto connect_action = [this](const ConnectionProfile& prof) {
        if (callbacks_.on_connect) callbacks_.on_connect(prof);
    };
    (void)connect_action;

    auto make_ctx = [&]() { return new CardCtx{this, p}; };
    auto destroy_ctx = [](gpointer d, GClosure*) { delete static_cast<CardCtx*>(d); };

    g_signal_connect_data(
        conn_btn, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* c = static_cast<CardCtx*>(d);
            if (c->self->callbacks_.on_connect) c->self->callbacks_.on_connect(c->profile);
        }),
        make_ctx(), destroy_ctx, static_cast<GConnectFlags>(0));

    // Double-click card to connect
    GtkGesture* dbl = gtk_gesture_click_new();
    g_signal_connect_data(
        dbl, "pressed",
        G_CALLBACK(+[](GtkGestureClick*, gint n_press, double, double, gpointer d) {
            if (n_press == 2) {
                auto* c = static_cast<CardCtx*>(d);
                if (c->self->callbacks_.on_connect) c->self->callbacks_.on_connect(c->profile);
            }
        }),
        make_ctx(), destroy_ctx, static_cast<GConnectFlags>(0));
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(dbl));

    g_signal_connect_data(
        fav_btn, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* c = static_cast<CardCtx*>(d);
            auto prof = c->profile;
            prof.favorite = !prof.favorite;
            c->self->store_->upsert(prof);
            c->self->store_->save();
            c->self->refresh();
        }),
        make_ctx(), destroy_ctx, static_cast<GConnectFlags>(0));

    g_signal_connect_data(
        edit_btn, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* c = static_cast<CardCtx*>(d);
            show_profile_dialog(GTK_WINDOW(c->self->window_), c->profile, false,
                                [win = c->self](ConnectionProfile saved) {
                                    win->store_->upsert(std::move(saved));
                                    win->store_->save();
                                    win->refresh();
                                });
        }),
        make_ctx(), destroy_ctx, static_cast<GConnectFlags>(0));

    g_signal_connect_data(
        exp_btn, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* c = static_cast<CardCtx*>(d);
            c->self->on_export_rdp(c->profile);
        }),
        make_ctx(), destroy_ctx, static_cast<GConnectFlags>(0));

    g_signal_connect_data(
        del_btn, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer d) {
            auto* c = static_cast<CardCtx*>(d);
            c->self->on_delete_profile(c->profile);
        }),
        make_ctx(), destroy_ctx, static_cast<GConnectFlags>(0));

    return card;
}

void ManagerWindow::on_quick_connect_activated() {
    const std::string text = gtk_editable_get_text(GTK_EDITABLE(quick_entry_));
    ConnectionProfile p;
    if (!parse_quick_connect_target(text, p)) {
        gtk_widget_grab_focus(quick_entry_);
        return;
    }
    if (callbacks_.on_connect) callbacks_.on_connect(p);
}

void ManagerWindow::on_new_connection() {
    ConnectionProfile p;
    p.id = ProfileStore::generate_uuid();
    show_profile_dialog(GTK_WINDOW(window_), p, true, [this](ConnectionProfile saved) {
        store_->upsert(std::move(saved));
        store_->save();
        refresh();
    });
}

void ManagerWindow::on_import_rdp() {
    GtkFileDialog* fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Import .rdp File");

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Remote Desktop Files (*.rdp)");
    gtk_file_filter_add_pattern(filter, "*.rdp");
    gtk_file_filter_add_pattern(filter, "*.RDP");
    GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(fd, G_LIST_MODEL(filters));
    g_object_unref(filter);
    g_object_unref(filters);

    gtk_file_dialog_open(
        fd,
        GTK_WINDOW(window_),
        nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer data) {
            auto* self = static_cast<ManagerWindow*>(data);
            GError* err = nullptr;
            GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &err);
            if (err) g_error_free(err);
            if (!file) return;
            char* path = g_file_get_path(file);
            g_object_unref(file);
            if (!path) return;

            ConnectionProfile imported;
            std::string perr;
            const bool ok = load_rdp_file(path, imported, &perr);
            g_free(path);
            if (ok) {
                self->store_->upsert(std::move(imported));
                self->store_->save();
                self->refresh();
            } else {
                AdwDialog* alert = adw_alert_dialog_new("Import Failed", perr.c_str());
                adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "ok", "OK");
                adw_dialog_present(alert, GTK_WIDGET(self->window_));
            }
        },
        this);
    g_object_unref(fd);
}

void ManagerWindow::on_export_rdp(const ConnectionProfile& p) {
    GtkFileDialog* fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Export .rdp File");
    gtk_file_dialog_set_initial_name(fd, (p.display_title() + ".rdp").c_str());

    struct ExportCtx {
        ManagerWindow* self;
        ConnectionProfile profile;
    };
    auto* ec = new ExportCtx{this, p};

    gtk_file_dialog_save(
        fd,
        GTK_WINDOW(window_),
        nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer data) {
            std::unique_ptr<ExportCtx> c(static_cast<ExportCtx*>(data));
            GError* err = nullptr;
            GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
            if (err) g_error_free(err);
            if (!file) return;
            char* path = g_file_get_path(file);
            g_object_unref(file);
            if (!path) return;
            std::string serr;
            if (!save_rdp_file(path, c->profile, &serr)) {
                AdwDialog* alert = adw_alert_dialog_new("Export Failed", serr.c_str());
                adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "ok", "OK");
                adw_dialog_present(alert, GTK_WIDGET(c->self->window_));
            }
            g_free(path);
        },
        ec);
    g_object_unref(fd);
}

void ManagerWindow::on_delete_profile(const ConnectionProfile& p) {
    AdwDialog* dlg = adw_alert_dialog_new(
        ("Delete \"" + p.display_title() + "\"?").c_str(),
        "This removes the connection profile and any saved password from GNOME Keyring.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", "Cancel", "delete", "Delete", nullptr);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "cancel");

    struct DelCtx {
        ManagerWindow* self;
        std::string id;
    };
    auto* dc = new DelCtx{this, p.id};
    g_signal_connect(
        dlg, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char* resp, gpointer data) {
            std::unique_ptr<DelCtx> c(static_cast<DelCtx*>(data));
            if (resp && std::string(resp) == "delete") {
                Keyring::clear_all_for_profile(c->id);
                c->self->store_->remove(c->id);
                c->self->store_->save();
                c->self->refresh();
            }
        }),
        dc);
    adw_dialog_present(dlg, GTK_WIDGET(window_));
}

} // namespace winrdp
