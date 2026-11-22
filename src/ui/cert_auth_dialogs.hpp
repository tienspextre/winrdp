#pragma once

#include "rdp/rdp_session.hpp"

#include <adwaita.h>
#include <gtk/gtk.h>

namespace winrdp {

// Must be called from a background thread (marshals to GTK main loop via g_idle_add and blocks).
// Returns 0 = Cancel, 1 = Trust & Store, 2 = Connect Once.
uint32_t prompt_certificate_sync(GtkWindow* parent,
                                 const CertPromptInfo& info,
                                 const std::function<void(bool)>& suspend_grab);

// Must be called from a background thread (marshals to GTK main loop via g_idle_add and blocks).
bool prompt_authentication_sync(GtkWindow* parent,
                                AuthPromptInfo& info,
                                const std::function<void(bool)>& suspend_grab);

} // namespace winrdp
