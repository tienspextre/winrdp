#pragma once

#include "config/profile.hpp"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace winrdp {

struct ManagerCallbacks {
    std::function<void(const ConnectionProfile& profile)> on_connect;
};

class ManagerWindow {
public:
    ManagerWindow(GtkApplication* app, ProfileStore* store, ManagerCallbacks callbacks);
    ~ManagerWindow();

    ManagerWindow(const ManagerWindow&) = delete;
    ManagerWindow& operator=(const ManagerWindow&) = delete;

    void present();
    void refresh();
    GtkWindow* gtk_window() const { return GTK_WINDOW(window_); }

private:
    enum class FilterKind {
        All,
        Favorites,
        Recent,
        Group,
    };

    void setup_ui();
    void rebuild_sidebar();
    void rebuild_cards();
    GtkWidget* build_connection_card(const ConnectionProfile& p);

    void on_quick_connect_activated();
    void on_new_connection();
    void on_import_rdp();
    void on_export_rdp(const ConnectionProfile& p);
    void on_delete_profile(const ConnectionProfile& p);

    ProfileStore* store_ = nullptr;
    ManagerCallbacks callbacks_;

    AdwApplicationWindow* window_ = nullptr;
    AdwNavigationSplitView* split_ = nullptr;
    GtkListBox* sidebar_list_ = nullptr;
    GtkWidget* quick_entry_ = nullptr;
    GtkWidget* quick_multimon_btn_ = nullptr;
    GtkWidget* search_bar_ = nullptr;
    GtkWidget* search_entry_ = nullptr;
    GtkWidget* stack_ = nullptr;
    GtkWidget* flow_box_ = nullptr;

    FilterKind filter_kind_ = FilterKind::All;
    std::string filter_group_;
    std::string search_query_;
    std::vector<std::string> sidebar_row_keys_;
};

} // namespace winrdp
