#pragma once

#include "config/profile.hpp"

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>

namespace winrdp {

// Opens a modern 5-tab Libadwaita connection settings dialog (General, Display,
// Local Resources, Experience, Advanced). Calls on_save when the user clicks Save.
void show_profile_dialog(GtkWindow* parent,
                         const ConnectionProfile& initial_profile,
                         bool is_new,
                         std::function<void(ConnectionProfile saved)> on_save);

} // namespace winrdp
