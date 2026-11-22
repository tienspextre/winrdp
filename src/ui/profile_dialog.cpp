#include "ui/profile_dialog.hpp"

#include "config/keyring.hpp"
#include "input/monitor_layout.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace winrdp {

namespace {

struct ProfileDialogState {
    AdwWindow* win = nullptr;
    ConnectionProfile profile;
    bool is_new = false;
    std::function<void(ConnectionProfile)> on_save;

    // General
    GtkWidget* name_row = nullptr;
    GtkWidget* group_row = nullptr;
    GtkWidget* fav_row = nullptr;
    GtkWidget* host_row = nullptr;
    GtkWidget* port_row = nullptr;
    GtkWidget* user_row = nullptr;
    GtkWidget* domain_row = nullptr;
    GtkWidget* pass_row = nullptr;
    GtkWidget* remember_user_row = nullptr;
    GtkWidget* save_pass_row = nullptr;

    // Display
    GtkWidget* disp_mode_row = nullptr;
    GtkWidget* res_preset_row = nullptr;
    GtkWidget* width_row = nullptr;
    GtkWidget* height_row = nullptr;
    GtkWidget* dyn_res_row = nullptr;
    GtkWidget* smart_size_row = nullptr;
    GtkWidget* scale_row = nullptr;
    GtkWidget* bar_pin_row = nullptr;
    GtkWidget* bar_hide_row = nullptr;
    std::vector<MonitorInfo> detected_monitors;
    GtkWidget* mon_group = nullptr;
    GtkWidget* use_all_mons_row = nullptr;
    GtkWidget* target_mon_row = nullptr;
    std::vector<std::pair<int32_t, GtkWidget*>> mon_switch_rows;

    // Resources
    GtkWidget* audio_row = nullptr;
    GtkWidget* mic_row = nullptr;
    GtkWidget* clip_row = nullptr;
    GtkWidget* clip_files_row = nullptr;
    GtkWidget* printer_row = nullptr;
    GtkWidget* grab_mode_row = nullptr;
    GtkWidget* release_key_row = nullptr;
    GtkWidget* folders_group = nullptr;
    std::vector<GtkWidget*> folder_rows;

    // Experience
    GtkWidget* codec_row = nullptr;
    GtkWidget* hw_decode_row = nullptr;
    GtkWidget* font_smooth_row = nullptr;
    GtkWidget* comp_row = nullptr;
    GtkWidget* drag_row = nullptr;
    GtkWidget* anim_row = nullptr;
    GtkWidget* wall_row = nullptr;
    GtkWidget* autoreconn_row = nullptr;
    GtkWidget* max_retries_row = nullptr;
    GtkWidget* net_detect_row = nullptr;

    // Advanced
    GtkWidget* sec_row = nullptr;
    GtkWidget* ignore_cert_row = nullptr;
    GtkWidget* gw_host_row = nullptr;
    GtkWidget* gw_port_row = nullptr;
    GtkWidget* gw_same_row = nullptr;
    GtkWidget* gw_user_row = nullptr;
    GtkWidget* gw_dom_row = nullptr;
    GtkWidget* gw_pass_row = nullptr;
};

GtkWidget* make_combo_row(const char* title, const char* subtitle, const char* const* items, guint selected) {
    GtkWidget* row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    GtkStringList* model = gtk_string_list_new(items);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
    return row;
}

GtkWidget* make_switch_row(const char* title, const char* subtitle, bool active) {
    GtkWidget* row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active ? TRUE : FALSE);
    return row;
}

void rebuild_folders_list(ProfileDialogState* st) {
    for (GtkWidget* r : st->folder_rows) {
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(st->folders_group), r);
    }
    st->folder_rows.clear();

    for (size_t i = 0; i < st->profile.shared_folders.size(); ++i) {
        const auto& sf = st->profile.shared_folders[i];
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), sf.name.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sf.path.c_str());

        GtkWidget* rm_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_valign(rm_btn, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(rm_btn, "flat");
        gtk_widget_set_tooltip_text(rm_btn, "Remove shared folder");

        struct RmCtx {
            ProfileDialogState* st;
            size_t idx;
        };
        auto* rc = new RmCtx{st, i};
        g_signal_connect_data(
            rm_btn,
            "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                auto* c = static_cast<RmCtx*>(data);
                if (c->idx < c->st->profile.shared_folders.size()) {
                    c->st->profile.shared_folders.erase(
                        c->st->profile.shared_folders.begin() + static_cast<ptrdiff_t>(c->idx));
                    rebuild_folders_list(c->st);
                }
            }),
            rc,
            [](gpointer p, GClosure*) { delete static_cast<RmCtx*>(p); },
            static_cast<GConnectFlags>(0));

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), rm_btn);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(st->folders_group), row);
        st->folder_rows.push_back(row);
    }
}

ConnectionProfile collect_profile(ProfileDialogState* st) {
    ConnectionProfile p = st->profile;
    if (p.id.empty()) p.id = ProfileStore::generate_uuid();

    p.name = gtk_editable_get_text(GTK_EDITABLE(st->name_row));
    p.group = gtk_editable_get_text(GTK_EDITABLE(st->group_row));
    p.favorite = adw_switch_row_get_active(ADW_SWITCH_ROW(st->fav_row)) == TRUE;
    p.hostname = gtk_editable_get_text(GTK_EDITABLE(st->host_row));
    p.port = static_cast<uint16_t>(adw_spin_row_get_value(ADW_SPIN_ROW(st->port_row)));
    const auto norm = normalize_rdp_credentials(
        gtk_editable_get_text(GTK_EDITABLE(st->user_row)),
        gtk_editable_get_text(GTK_EDITABLE(st->domain_row)));
    p.username = norm.username;
    p.domain = norm.domain;
    p.remember_username = adw_switch_row_get_active(ADW_SWITCH_ROW(st->remember_user_row)) == TRUE;
    p.save_password = adw_switch_row_get_active(ADW_SWITCH_ROW(st->save_pass_row)) == TRUE;
    p.runtime_password = gtk_editable_get_text(GTK_EDITABLE(st->pass_row));

    p.display_mode = static_cast<DisplayMode>(adw_combo_row_get_selected(ADW_COMBO_ROW(st->disp_mode_row)));
    p.width = static_cast<uint32_t>(adw_spin_row_get_value(ADW_SPIN_ROW(st->width_row)));
    p.height = static_cast<uint32_t>(adw_spin_row_get_value(ADW_SPIN_ROW(st->height_row)));
    p.dynamic_resolution = adw_switch_row_get_active(ADW_SWITCH_ROW(st->dyn_res_row)) == TRUE;
    p.smart_sizing = adw_switch_row_get_active(ADW_SWITCH_ROW(st->smart_size_row)) == TRUE;

    static const uint32_t kScales[] = {100, 100, 125, 150, 200};
    guint sidx = adw_combo_row_get_selected(ADW_COMBO_ROW(st->scale_row));
    p.scale_factor = kScales[std::min<guint>(sidx, 4)];

    const bool bar_pin = adw_switch_row_get_active(ADW_SWITCH_ROW(st->bar_pin_row)) == TRUE;
    const bool bar_hide = adw_switch_row_get_active(ADW_SWITCH_ROW(st->bar_hide_row)) == TRUE;
    p.connection_bar_hide_hover = bar_hide;
    p.connection_bar_mode = bar_pin ? ConnectionBarMode::Pinned
                                    : (bar_hide ? ConnectionBarMode::Hidden : ConnectionBarMode::AutoHide);

    p.selected_monitors.clear();
    if (p.display_mode == DisplayMode::MultiMonitor) {
        const bool use_all = adw_switch_row_get_active(ADW_SWITCH_ROW(st->use_all_mons_row)) == TRUE;
        if (!use_all) {
            for (const auto& entry : st->mon_switch_rows) {
                if (adw_switch_row_get_active(ADW_SWITCH_ROW(entry.second)) == TRUE) {
                    p.selected_monitors.push_back(entry.first);
                }
            }
        }
    } else if (p.display_mode == DisplayMode::Fullscreen) {
        const guint t_idx = adw_combo_row_get_selected(ADW_COMBO_ROW(st->target_mon_row));
        if (t_idx > 0 && static_cast<size_t>(t_idx - 1) < st->detected_monitors.size()) {
            p.selected_monitors.push_back(st->detected_monitors[t_idx - 1].index);
        }
    }

    p.audio_mode = static_cast<AudioMode>(adw_combo_row_get_selected(ADW_COMBO_ROW(st->audio_row)));
    p.mic_redirection = adw_switch_row_get_active(ADW_SWITCH_ROW(st->mic_row)) == TRUE;
    p.clipboard_redirection = adw_switch_row_get_active(ADW_SWITCH_ROW(st->clip_row)) == TRUE;
    p.clipboard_files = adw_switch_row_get_active(ADW_SWITCH_ROW(st->clip_files_row)) == TRUE;
    p.printer_redirection = adw_switch_row_get_active(ADW_SWITCH_ROW(st->printer_row)) == TRUE;
    p.keyboard_grab_mode = static_cast<KeyboardGrabMode>(
        adw_combo_row_get_selected(ADW_COMBO_ROW(st->grab_mode_row)));

    static const char* kRelKeys[] = {"RightCtrl", "RightAlt", "ScrollLock"};
    guint rk_idx = adw_combo_row_get_selected(ADW_COMBO_ROW(st->release_key_row));
    p.grab_release_key = kRelKeys[std::min<guint>(rk_idx, 2)];

    p.codec = static_cast<CodecMode>(adw_combo_row_get_selected(ADW_COMBO_ROW(st->codec_row)));
    p.hardware_decode = adw_switch_row_get_active(ADW_SWITCH_ROW(st->hw_decode_row)) == TRUE;
    p.font_smoothing = adw_switch_row_get_active(ADW_SWITCH_ROW(st->font_smooth_row)) == TRUE;
    p.desktop_composition = adw_switch_row_get_active(ADW_SWITCH_ROW(st->comp_row)) == TRUE;
    p.full_window_drag = adw_switch_row_get_active(ADW_SWITCH_ROW(st->drag_row)) == TRUE;
    p.menu_animations = adw_switch_row_get_active(ADW_SWITCH_ROW(st->anim_row)) == TRUE;
    p.wallpaper = adw_switch_row_get_active(ADW_SWITCH_ROW(st->wall_row)) == TRUE;
    p.auto_reconnect = adw_switch_row_get_active(ADW_SWITCH_ROW(st->autoreconn_row)) == TRUE;
    p.max_reconnect_attempts = static_cast<uint32_t>(
        adw_spin_row_get_value(ADW_SPIN_ROW(st->max_retries_row)));
    p.network_auto_detect = adw_switch_row_get_active(ADW_SWITCH_ROW(st->net_detect_row)) == TRUE;

    p.security = static_cast<SecurityMode>(adw_combo_row_get_selected(ADW_COMBO_ROW(st->sec_row)));
    p.ignore_certificate = adw_switch_row_get_active(ADW_SWITCH_ROW(st->ignore_cert_row)) == TRUE;
    p.gateway_hostname = gtk_editable_get_text(GTK_EDITABLE(st->gw_host_row));
    p.gateway_port = static_cast<uint16_t>(adw_spin_row_get_value(ADW_SPIN_ROW(st->gw_port_row)));
    p.gateway_use_same_creds = adw_switch_row_get_active(ADW_SWITCH_ROW(st->gw_same_row)) == TRUE;
    p.gateway_username = gtk_editable_get_text(GTK_EDITABLE(st->gw_user_row));
    p.gateway_domain = gtk_editable_get_text(GTK_EDITABLE(st->gw_dom_row));
    p.runtime_gateway_password = gtk_editable_get_text(GTK_EDITABLE(st->gw_pass_row));

    if (p.name.empty() && !p.hostname.empty()) {
        p.name = p.display_title();
    }
    return p;
}

} // namespace

void show_profile_dialog(GtkWindow* parent,
                         const ConnectionProfile& initial_profile,
                         bool is_new,
                         std::function<void(ConnectionProfile saved)> on_save) {
    auto* st = new ProfileDialogState();
    st->profile = initial_profile;
    st->is_new = is_new;
    st->on_save = std::move(on_save);

    if (!st->profile.id.empty() && st->profile.runtime_password.empty()) {
        if (auto pw = Keyring::lookup_secret(st->profile.id, "password")) {
            st->profile.runtime_password = *pw;
        }
        if (auto gpw = Keyring::lookup_secret(st->profile.id, "gateway_password")) {
            st->profile.runtime_gateway_password = *gpw;
        }
    }

    st->win = ADW_WINDOW(adw_window_new());
    gtk_window_set_title(GTK_WINDOW(st->win),
                         is_new ? "New Connection" : ("Edit: " + st->profile.display_title()).c_str());
    gtk_window_set_default_size(GTK_WINDOW(st->win), 780, 660);
    gtk_widget_set_size_request(GTK_WIDGET(st->win), 720, 560);
    gtk_window_set_modal(GTK_WINDOW(st->win), TRUE);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(st->win), parent);

    GtkWidget* toolbar = adw_toolbar_view_new();
    adw_window_set_content(st->win, toolbar);

    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);

    GtkWidget* cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget* save_btn = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save_btn, "suggested-action");

    adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel_btn);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), save_btn);

    AdwWindowTitle* win_title = ADW_WINDOW_TITLE(adw_window_title_new(
        is_new ? "New Connection" : "Edit Connection",
        is_new ? "Configure remote desktop profile" : st->profile.display_title().c_str()));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(win_title));

    GtkWidget* stack = adw_view_stack_new();

    GtkWidget* switcher_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(switcher_bar, "winrdp-profile-tabs-bar");
    GtkWidget* top_switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(top_switcher), ADW_VIEW_STACK(stack));
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(top_switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    gtk_widget_set_halign(top_switcher, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(top_switcher, TRUE);
    gtk_box_append(GTK_BOX(switcher_bar), top_switcher);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), switcher_bar);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack);

    // ---- Tab 1: General ----
    GtkWidget* p1 = adw_preferences_page_new();
    GtkWidget* g1 = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g1), "Connection Identity");

    st->name_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->name_row), "Display Name");
    gtk_editable_set_text(GTK_EDITABLE(st->name_row), st->profile.name.c_str());

    st->group_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->group_row), "Group (optional)");
    gtk_editable_set_text(GTK_EDITABLE(st->group_row), st->profile.group.c_str());

    st->fav_row = make_switch_row("Favorite", "Pin to Favorites filter", st->profile.favorite);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), st->name_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), st->group_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), st->fav_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p1), ADW_PREFERENCES_GROUP(g1));

    GtkWidget* g1b = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g1b), "Remote Computer & Credentials");

    st->host_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->host_row), "PC Name / Host / IP");
    gtk_editable_set_text(GTK_EDITABLE(st->host_row), st->profile.hostname.c_str());

    st->port_row = adw_spin_row_new_with_range(1, 65535, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->port_row), "RDP Port");
    adw_spin_row_set_value(ADW_SPIN_ROW(st->port_row), st->profile.port);

    st->user_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->user_row), "Username");
    gtk_editable_set_text(GTK_EDITABLE(st->user_row), st->profile.username.c_str());

    st->domain_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->domain_row), "Domain (optional)");
    gtk_editable_set_text(GTK_EDITABLE(st->domain_row), st->profile.domain.c_str());

    st->pass_row = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->pass_row), "Password");
    gtk_editable_set_text(GTK_EDITABLE(st->pass_row), st->profile.runtime_password.c_str());

    st->remember_user_row = make_switch_row(
        "Remember Last Username",
        "Save last username & domain used on connect (without saving password)",
        st->profile.remember_username);

    st->save_pass_row = make_switch_row(
        "Save Password in GNOME Keyring",
        "Encrypted via libsecret — never stored in plaintext JSON",
        st->profile.save_password);

    g_signal_connect_object(
        st->save_pass_row,
        "notify::active",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer user_data) {
            if (adw_switch_row_get_active(ADW_SWITCH_ROW(obj)) == TRUE) {
                adw_switch_row_set_active(ADW_SWITCH_ROW(user_data), TRUE);
            }
        }),
        st->remember_user_row,
        G_CONNECT_DEFAULT);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1b), st->host_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1b), st->port_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1b), st->user_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1b), st->domain_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1b), st->pass_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1b), st->remember_user_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1b), st->save_pass_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p1), ADW_PREFERENCES_GROUP(g1b));

    AdwViewStackPage* vp1 = adw_view_stack_add_titled(ADW_VIEW_STACK(stack), p1, "general", "General");
    adw_view_stack_page_set_icon_name(vp1, "computer-symbolic");

    // ---- Tab 2: Display ----
    GtkWidget* p2 = adw_preferences_page_new();
    GtkWidget* g2 = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g2), "Display & Resolution");

    const char* disp_modes[] = {
        "Fullscreen (Single Monitor)",
        "Windowed",
        "Multi-Monitor (Fullscreen)",
        nullptr
    };
    st->disp_mode_row = make_combo_row(
        "Display Mode", nullptr, disp_modes, static_cast<guint>(st->profile.display_mode));

    const char* res_presets[] = {"Custom / Native", "1920 x 1080", "2560 x 1440", "3840 x 2160", "1600 x 900", "1280 x 720", nullptr};
    st->res_preset_row = make_combo_row("Resolution Preset", nullptr, res_presets, 0);

    st->width_row = adw_spin_row_new_with_range(320, 8192, 2);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->width_row), "Desktop Width");
    adw_spin_row_set_value(ADW_SPIN_ROW(st->width_row), st->profile.width);

    st->height_row = adw_spin_row_new_with_range(200, 8192, 2);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->height_row), "Desktop Height");
    adw_spin_row_set_value(ADW_SPIN_ROW(st->height_row), st->profile.height);

    g_signal_connect(
        st->res_preset_row,
        "notify::selected",
        G_CALLBACK((+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* s = static_cast<ProfileDialogState*>(data);
            guint idx = adw_combo_row_get_selected(ADW_COMBO_ROW(obj));
            static const int pw[] = {0, 1920, 2560, 3840, 1600, 1280};
            static const int ph[] = {0, 1080, 1440, 2160, 900, 720};
            if (idx > 0 && idx < 6) {
                adw_spin_row_set_value(ADW_SPIN_ROW(s->width_row), pw[idx]);
                adw_spin_row_set_value(ADW_SPIN_ROW(s->height_row), ph[idx]);
            }
        })),
        st);

    st->dyn_res_row = make_switch_row(
        "Dynamic Resolution (MS-RDPEDISP)",
        "Automatically adjust Windows resolution when resizing or toggling fullscreen",
        st->profile.dynamic_resolution);
    st->smart_size_row = make_switch_row(
        "Smart Sizing",
        "Scale the remote desktop smoothly to fit the window",
        st->profile.smart_sizing);

    const char* scales[] = {"Auto", "100%", "125%", "150%", "200%", nullptr};
    guint scale_idx = 0;
    if (st->profile.scale_factor == 125) scale_idx = 2;
    else if (st->profile.scale_factor == 150) scale_idx = 3;
    else if (st->profile.scale_factor >= 200) scale_idx = 4;
    st->scale_row = make_combo_row("Remote Windows DPI Scaling", nullptr, scales, scale_idx);

    const bool bar_pinned = (st->profile.connection_bar_mode == ConnectionBarMode::Pinned);
    const bool bar_hide = st->profile.connection_bar_hide_hover ||
                          (st->profile.connection_bar_mode == ConnectionBarMode::Hidden);
    st->bar_pin_row = make_switch_row(
        "Pin Connection Bar",
        "Always keep the Connection Bar visible in fullscreen",
        bar_pinned);
    st->bar_hide_row = make_switch_row(
        "Hide Connection Bar (Hotkey-Only)",
        "Disable top-edge hover reveal when unpinned; show via Ctrl+Alt+Home",
        bar_hide);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->disp_mode_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->res_preset_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->width_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->height_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->dyn_res_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->smart_size_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->scale_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->bar_pin_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2), st->bar_hide_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p2), ADW_PREFERENCES_GROUP(g2));

    // Monitors sub-group
    st->detected_monitors = query_local_monitors(gtk_widget_get_display(GTK_WIDGET(st->win)));
    st->mon_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(st->mon_group), "Monitors");
    {
        std::string desc = std::to_string(st->detected_monitors.size()) +
                           (st->detected_monitors.size() == 1
                                ? " display detected — connect another monitor to span across screens"
                                : " displays detected");
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(st->mon_group), desc.c_str());
    }

    st->use_all_mons_row = make_switch_row(
        "Use All Connected Monitors",
        "Automatically span Windows across every local display",
        st->profile.selected_monitors.empty());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(st->mon_group), st->use_all_mons_row);

    std::vector<std::string> target_labels;
    target_labels.emplace_back("Auto (Current / Primary)");
    guint target_sel = 0;

    for (size_t i = 0; i < st->detected_monitors.size(); ++i) {
        const auto& m = st->detected_monitors[i];
        const std::string label = format_monitor_label(m);
        target_labels.push_back(label);
        if (st->profile.selected_monitors.size() == 1 &&
            st->profile.selected_monitors[0] == m.index) {
            target_sel = static_cast<guint>(i + 1);
        }

        const bool checked = st->profile.selected_monitors.empty() ||
                             (std::find(st->profile.selected_monitors.begin(),
                                        st->profile.selected_monitors.end(),
                                        m.index) != st->profile.selected_monitors.end());
        std::string sub = std::to_string(m.width) + "x" + std::to_string(m.height) +
                          " at (" + std::to_string(m.x) + ", " + std::to_string(m.y) + ")" +
                          " • Scale " + std::to_string(m.scale_factor) + "x";
        if (m.is_primary) {
            sub += " • Primary";
        }
        GtkWidget* mrow = make_switch_row(label.c_str(), sub.c_str(), checked);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(st->mon_group), mrow);
        st->mon_switch_rows.emplace_back(m.index, mrow);
    }

    std::vector<const char*> target_ptrs;
    target_ptrs.reserve(target_labels.size() + 1);
    for (const auto& s : target_labels) {
        target_ptrs.push_back(s.c_str());
    }
    target_ptrs.push_back(nullptr);

    st->target_mon_row = make_combo_row(
        "Target Fullscreen Monitor",
        "Display used when entering single-monitor fullscreen",
        target_ptrs.data(),
        target_sel);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(st->mon_group), st->target_mon_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p2), ADW_PREFERENCES_GROUP(st->mon_group));

    auto update_monitor_visibility = +[](ProfileDialogState* s) {
        const auto mode = static_cast<DisplayMode>(
            adw_combo_row_get_selected(ADW_COMBO_ROW(s->disp_mode_row)));
        const bool is_multimon = (mode == DisplayMode::MultiMonitor);
        const bool is_fs = (mode == DisplayMode::Fullscreen);
        const bool use_all = adw_switch_row_get_active(ADW_SWITCH_ROW(s->use_all_mons_row)) == TRUE;

        gtk_widget_set_visible(s->mon_group, (is_multimon || is_fs) ? TRUE : FALSE);
        gtk_widget_set_visible(s->use_all_mons_row, is_multimon ? TRUE : FALSE);
        for (const auto& entry : s->mon_switch_rows) {
            gtk_widget_set_visible(entry.second, (is_multimon && !use_all) ? TRUE : FALSE);
        }
        gtk_widget_set_visible(s->target_mon_row, is_fs ? TRUE : FALSE);
    };

    g_signal_connect(
        st->disp_mode_row,
        "notify::selected",
        G_CALLBACK((+[](GObject*, GParamSpec*, gpointer data) {
            auto* s = static_cast<ProfileDialogState*>(data);
            const auto mode = static_cast<DisplayMode>(
                adw_combo_row_get_selected(ADW_COMBO_ROW(s->disp_mode_row)));
            const bool is_multimon = (mode == DisplayMode::MultiMonitor);
            const bool is_fs = (mode == DisplayMode::Fullscreen);
            const bool use_all = adw_switch_row_get_active(ADW_SWITCH_ROW(s->use_all_mons_row)) == TRUE;
            gtk_widget_set_visible(s->mon_group, (is_multimon || is_fs) ? TRUE : FALSE);
            gtk_widget_set_visible(s->use_all_mons_row, is_multimon ? TRUE : FALSE);
            for (const auto& entry : s->mon_switch_rows) {
                gtk_widget_set_visible(entry.second, (is_multimon && !use_all) ? TRUE : FALSE);
            }
            gtk_widget_set_visible(s->target_mon_row, is_fs ? TRUE : FALSE);
        })),
        st);

    g_signal_connect(
        st->use_all_mons_row,
        "notify::active",
        G_CALLBACK((+[](GObject*, GParamSpec*, gpointer data) {
            auto* s = static_cast<ProfileDialogState*>(data);
            const auto mode = static_cast<DisplayMode>(
                adw_combo_row_get_selected(ADW_COMBO_ROW(s->disp_mode_row)));
            const bool is_multimon = (mode == DisplayMode::MultiMonitor);
            const bool use_all = adw_switch_row_get_active(ADW_SWITCH_ROW(s->use_all_mons_row)) == TRUE;
            for (const auto& entry : s->mon_switch_rows) {
                gtk_widget_set_visible(entry.second, (is_multimon && !use_all) ? TRUE : FALSE);
            }
        })),
        st);

    update_monitor_visibility(st);

    AdwViewStackPage* vp2 = adw_view_stack_add_titled(ADW_VIEW_STACK(stack), p2, "display", "Display");
    adw_view_stack_page_set_icon_name(vp2, "video-display-symbolic");

    // ---- Tab 3: Local Resources ----
    GtkWidget* p3 = adw_preferences_page_new();
    GtkWidget* g3 = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g3), "Audio, Clipboard & Keyboard");

    const char* audio_modes[] = {"Play on this computer (Ubuntu)", "Play on remote Windows", "Do not play", nullptr};
    st->audio_row = make_combo_row(
        "Remote Audio Playback", nullptr, audio_modes, static_cast<guint>(st->profile.audio_mode));
    st->mic_row = make_switch_row("Redirect Microphone", "Pass Ubuntu mic to Windows", st->profile.mic_redirection);
    st->clip_row = make_switch_row("Clipboard (Text & Images)", "2-way copy & paste with Ubuntu", st->profile.clipboard_redirection);
    st->clip_files_row = make_switch_row("Clipboard File Copy/Paste", "Copy & paste files via FUSE3", st->profile.clipboard_files);
    st->printer_row = make_switch_row("Redirect Local Printers", nullptr, st->profile.printer_redirection);

    const char* grab_modes[] = {"Only in Fullscreen", "Always when focused", "Never", nullptr};
    st->grab_mode_row = make_combo_row(
        "Capture System Shortcuts (Win, Alt+Tab, PrtSc)",
        "Sends Windows keys directly to remote Windows",
        grab_modes,
        static_cast<guint>(st->profile.keyboard_grab_mode));

    const char* rel_keys[] = {"Right Ctrl", "Right Alt", "Scroll Lock", nullptr};
    guint rk_idx = 0;
    if (st->profile.grab_release_key == "RightAlt") rk_idx = 1;
    else if (st->profile.grab_release_key == "ScrollLock") rk_idx = 2;
    st->release_key_row = make_combo_row(
        "Release Keyboard Grab Key", "Tap this key to toggle keyboard capture", rel_keys, rk_idx);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), st->audio_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), st->mic_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), st->clip_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), st->clip_files_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), st->printer_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), st->grab_mode_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), st->release_key_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p3), ADW_PREFERENCES_GROUP(g3));

    st->folders_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(st->folders_group), "Shared Folders (\\\\tsclient)");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(st->folders_group),
        "Accessible inside Windows Explorer under \\\\tsclient");

    GtkWidget* add_folder_btn = gtk_button_new_with_label("Add Folder");
    gtk_widget_add_css_class(add_folder_btn, "flat");
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(st->folders_group), add_folder_btn);

    g_signal_connect(
        add_folder_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* s = static_cast<ProfileDialogState*>(data);
            GtkFileDialog* fd = gtk_file_dialog_new();
            gtk_file_dialog_set_title(fd, "Choose Folder to Share with Windows");
            gtk_file_dialog_select_folder(
                fd,
                GTK_WINDOW(s->win),
                nullptr,
                +[](GObject* src, GAsyncResult* res, gpointer user_data) {
                    auto* dlg_st = static_cast<ProfileDialogState*>(user_data);
                    GError* err = nullptr;
                    GFile* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
                    if (err) g_error_free(err);
                    if (folder) {
                        char* cpath = g_file_get_path(folder);
                        if (cpath) {
                            SharedFolder sf;
                            sf.path = cpath;
                            sf.name = fs::path(cpath).filename().string();
                            if (sf.name.empty()) sf.name = "UbuntuShare";
                            dlg_st->profile.shared_folders.push_back(std::move(sf));
                            rebuild_folders_list(dlg_st);
                            g_free(cpath);
                        }
                        g_object_unref(folder);
                    }
                },
                s);
            g_object_unref(fd);
        }),
        st);

    rebuild_folders_list(st);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p3), ADW_PREFERENCES_GROUP(st->folders_group));

    AdwViewStackPage* vp3 = adw_view_stack_add_titled(ADW_VIEW_STACK(stack), p3, "resources", "Resources");
    adw_view_stack_page_set_icon_name(vp3, "folder-remote-symbolic");

    // ---- Tab 4: Experience ----
    GtkWidget* p4 = adw_preferences_page_new();
    GtkWidget* g4 = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g4), "Graphics & Acceleration");

    const char* codecs[] = {
        "Auto (AVC444 -> AVC420 -> RemoteFX)",
        "H.264 / AVC444 (Crisp text + 60fps)",
        "H.264 / AVC420 (Low bandwidth)",
        "RemoteFX (Classic GFX)",
        "Standard GDI (Legacy)",
        nullptr
    };
    st->codec_row = make_combo_row(
        "Graphics Codec", nullptr, codecs, static_cast<guint>(st->profile.codec));
    st->hw_decode_row = make_switch_row(
        "Hardware Video Decode (Intel VA-API)",
        "Offloads H.264/AVC444 decoding to Intel UHD GPU",
        st->profile.hardware_decode);
    st->font_smooth_row = make_switch_row("Font Smoothing (ClearType)", nullptr, st->profile.font_smoothing);
    st->comp_row = make_switch_row("Desktop Composition", nullptr, st->profile.desktop_composition);
    st->drag_row = make_switch_row("Show Window Contents While Dragging", nullptr, st->profile.full_window_drag);
    st->anim_row = make_switch_row("Menu & Window Animations", nullptr, st->profile.menu_animations);
    st->wall_row = make_switch_row("Desktop Background Wallpaper", nullptr, st->profile.wallpaper);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), st->codec_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), st->hw_decode_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), st->font_smooth_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), st->comp_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), st->drag_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), st->anim_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), st->wall_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p4), ADW_PREFERENCES_GROUP(g4));

    GtkWidget* g4b = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g4b), "Resilience");
    st->autoreconn_row = make_switch_row("Auto-Reconnect on Drop", "Reconnect seamlessly without closing fullscreen", st->profile.auto_reconnect);
    st->max_retries_row = adw_spin_row_new_with_range(1, 50, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->max_retries_row), "Max Reconnect Attempts");
    adw_spin_row_set_value(ADW_SPIN_ROW(st->max_retries_row), st->profile.max_reconnect_attempts);
    st->net_detect_row = make_switch_row("Network Auto-Detect (Live Latency/RTT)", nullptr, st->profile.network_auto_detect);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4b), st->autoreconn_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4b), st->max_retries_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4b), st->net_detect_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p4), ADW_PREFERENCES_GROUP(g4b));

    AdwViewStackPage* vp4 = adw_view_stack_add_titled(ADW_VIEW_STACK(stack), p4, "experience", "Experience");
    adw_view_stack_page_set_icon_name(vp4, "preferences-desktop-display-symbolic");

    // ---- Tab 5: Advanced ----
    GtkWidget* p5 = adw_preferences_page_new();
    GtkWidget* g5 = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g5), "Security & RD Gateway");

    const char* sec_modes[] = {"Auto Negotiate", "NLA (CredSSP — Recommended)", "TLS", "Classic RDP", nullptr};
    st->sec_row = make_combo_row("Security Layer", nullptr, sec_modes, static_cast<guint>(st->profile.security));
    st->ignore_cert_row = make_switch_row(
        "Ignore Certificate Warnings",
        "Skip TLS certificate confirmation prompt",
        st->profile.ignore_certificate);

    st->gw_host_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->gw_host_row), "RD Gateway Server (optional)");
    gtk_editable_set_text(GTK_EDITABLE(st->gw_host_row), st->profile.gateway_hostname.c_str());

    st->gw_port_row = adw_spin_row_new_with_range(1, 65535, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->gw_port_row), "Gateway Port");
    adw_spin_row_set_value(ADW_SPIN_ROW(st->gw_port_row), st->profile.gateway_port);

    st->gw_same_row = make_switch_row("Use Same Credentials for Gateway", nullptr, st->profile.gateway_use_same_creds);

    st->gw_user_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->gw_user_row), "Gateway Username");
    gtk_editable_set_text(GTK_EDITABLE(st->gw_user_row), st->profile.gateway_username.c_str());

    st->gw_dom_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->gw_dom_row), "Gateway Domain");
    gtk_editable_set_text(GTK_EDITABLE(st->gw_dom_row), st->profile.gateway_domain.c_str());

    st->gw_pass_row = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->gw_pass_row), "Gateway Password");
    gtk_editable_set_text(GTK_EDITABLE(st->gw_pass_row), st->profile.runtime_gateway_password.c_str());

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), st->sec_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), st->ignore_cert_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), st->gw_host_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), st->gw_port_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), st->gw_same_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), st->gw_user_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), st->gw_dom_row);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), st->gw_pass_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p5), ADW_PREFERENCES_GROUP(g5));

    AdwViewStackPage* vp5 = adw_view_stack_add_titled(ADW_VIEW_STACK(stack), p5, "advanced", "Advanced");
    adw_view_stack_page_set_icon_name(vp5, "security-high-symbolic");

    g_signal_connect(
        cancel_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* s = static_cast<ProfileDialogState*>(data);
            gtk_window_close(GTK_WINDOW(s->win));
        }),
        st);

    g_signal_connect(
        save_btn,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* s = static_cast<ProfileDialogState*>(data);
            ConnectionProfile updated = collect_profile(s);
            if (updated.hostname.empty()) {
                gtk_widget_grab_focus(s->host_row);
                return;
            }
            if (updated.save_password) {
                Keyring::store_secret(updated.id, "password",
                                      "WinRDP: " + updated.display_title(), updated.runtime_password);
                if (!updated.gateway_hostname.empty() && !updated.gateway_use_same_creds) {
                    Keyring::store_secret(updated.id, "gateway_password",
                                          "WinRDP Gateway: " + updated.display_title(),
                                          updated.runtime_gateway_password);
                }
            } else {
                Keyring::clear_all_for_profile(updated.id);
            }
            if (s->on_save) s->on_save(std::move(updated));
            gtk_window_close(GTK_WINDOW(s->win));
        }),
        st);

    g_signal_connect(
        st->win,
        "destroy",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            delete static_cast<ProfileDialogState*>(data);
        }),
        st);

    gtk_window_present(GTK_WINDOW(st->win));
}

} // namespace winrdp
