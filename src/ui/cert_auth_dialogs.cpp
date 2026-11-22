#include "ui/cert_auth_dialogs.hpp"

#include <future>
#include <memory>
#include <sstream>

namespace winrdp {

uint32_t prompt_certificate_sync(GtkWindow* parent,
                                 const CertPromptInfo& info,
                                 const std::function<void(bool)>& suspend_grab) {
    auto promise = std::make_shared<std::promise<uint32_t>>();
    auto future = promise->get_future();

    struct CertDialogReq {
        GtkWindow* parent;
        CertPromptInfo info;
        std::function<void(bool)> suspend_grab;
        std::shared_ptr<std::promise<uint32_t>> promise;
    };

    auto* req = new CertDialogReq{parent, info, suspend_grab, promise};
    g_idle_add(
        +[](gpointer data) -> gboolean {
            std::unique_ptr<CertDialogReq> r(static_cast<CertDialogReq*>(data));
            if (r->suspend_grab) r->suspend_grab(true);

            std::string heading = r->info.changed
                                      ? "Certificate Has Changed!"
                                      : "Verify Remote Desktop Certificate";
            std::ostringstream body;
            body << (r->info.is_gateway ? "RD Gateway: " : "Server: ")
                 << r->info.host << ":" << r->info.port << "\n"
                 << "Common Name: " << r->info.common_name << "\n"
                 << "Subject: " << r->info.subject << "\n"
                 << "Issuer: " << r->info.issuer << "\n"
                 << "Fingerprint: " << r->info.fingerprint;
            if (!r->info.valid_from.empty() || !r->info.valid_to.empty()) {
                body << "\nValid: " << r->info.valid_from << " -> " << r->info.valid_to;
            }
            if (r->info.changed) {
                body << "\n\nWARNING: Previous fingerprint was:\n"
                     << r->info.old_fingerprint;
            }

            AdwDialog* dlg = adw_alert_dialog_new(heading.c_str(), body.str().c_str());
            adw_alert_dialog_add_responses(
                ADW_ALERT_DIALOG(dlg),
                "cancel", "Cancel",
                "once", "Connect Once",
                "trust", "Trust & Remember",
                nullptr);
            adw_alert_dialog_set_response_appearance(
                ADW_ALERT_DIALOG(dlg), "trust",
                r->info.changed ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
            adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "once");
            adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");

            struct RespCtx {
                std::shared_ptr<std::promise<uint32_t>> promise;
                std::function<void(bool)> suspend_grab;
            };
            auto* rctx = new RespCtx{r->promise, r->suspend_grab};
            g_signal_connect(
                dlg,
                "response",
                G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer user_data) {
                    std::unique_ptr<RespCtx> rc(static_cast<RespCtx*>(user_data));
                    uint32_t val = 0;
                    if (response) {
                        if (std::string(response) == "trust") val = 1;
                        else if (std::string(response) == "once") val = 2;
                    }
                    if (rc->suspend_grab) rc->suspend_grab(false);
                    rc->promise->set_value(val);
                }),
                rctx);

            adw_dialog_present(dlg, GTK_WIDGET(r->parent));
            return G_SOURCE_REMOVE;
        },
        req);

    return future.get();
}

bool prompt_authentication_sync(GtkWindow* parent,
                                AuthPromptInfo& info,
                                const std::function<void(bool)>& suspend_grab) {
    struct AuthOutcome {
        bool accepted = false;
        AuthPromptInfo updated;
    };
    auto promise = std::make_shared<std::promise<AuthOutcome>>();
    auto future = promise->get_future();

    struct AuthDialogReq {
        GtkWindow* parent;
        AuthPromptInfo info;
        std::function<void(bool)> suspend_grab;
        std::shared_ptr<std::promise<AuthOutcome>> promise;
    };

    auto* req = new AuthDialogReq{parent, info, suspend_grab, promise};
    g_idle_add(
        +[](gpointer data) -> gboolean {
            std::unique_ptr<AuthDialogReq> r(static_cast<AuthDialogReq*>(data));
            if (r->suspend_grab) r->suspend_grab(true);

            std::string title = (r->info.is_gateway ? "Gateway Credentials: " : "Sign in to ") +
                                r->info.target;
            AdwDialog* dlg = adw_alert_dialog_new(title.c_str(), "Enter your Windows credentials");

            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
            gtk_widget_set_size_request(box, 380, -1);

            GtkWidget* list = gtk_list_box_new();
            gtk_widget_add_css_class(list, "boxed-list");

            GtkWidget* user_row = adw_entry_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(user_row), "Username");
            gtk_widget_set_tooltip_text(user_row, "Enter username or DOMAIN\\username");
            gtk_editable_set_text(GTK_EDITABLE(user_row), r->info.username.c_str());

            GtkWidget* dom_row = adw_entry_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dom_row), "Domain (optional)");
            gtk_editable_set_text(GTK_EDITABLE(dom_row), r->info.domain.c_str());

            GtkWidget* pass_row = adw_password_entry_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(pass_row), "Password");
            gtk_editable_set_text(GTK_EDITABLE(pass_row), r->info.password.c_str());

            GtkWidget* remember_user_row = adw_switch_row_new();
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(remember_user_row), "Remember Last Username");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(remember_user_row),
                "Save username & domain on connect without saving password");
            adw_switch_row_set_active(
                ADW_SWITCH_ROW(remember_user_row), r->info.remember_username ? TRUE : FALSE);

            GtkWidget* save_row = adw_switch_row_new();
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(save_row), "Save Password in Keyring");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(save_row), "Store password encrypted in GNOME Keyring");
            adw_switch_row_set_active(
                ADW_SWITCH_ROW(save_row), r->info.save_password ? TRUE : FALSE);

            g_signal_connect_object(
                save_row,
                "notify::active",
                G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer user_data) {
                    if (adw_switch_row_get_active(ADW_SWITCH_ROW(obj)) == TRUE) {
                        adw_switch_row_set_active(ADW_SWITCH_ROW(user_data), TRUE);
                    }
                }),
                remember_user_row,
                G_CONNECT_DEFAULT);

            gtk_list_box_append(GTK_LIST_BOX(list), user_row);
            gtk_list_box_append(GTK_LIST_BOX(list), dom_row);
            gtk_list_box_append(GTK_LIST_BOX(list), pass_row);
            gtk_list_box_append(GTK_LIST_BOX(list), remember_user_row);
            gtk_list_box_append(GTK_LIST_BOX(list), save_row);
            gtk_box_append(GTK_BOX(box), list);

            adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dlg), box);
            adw_alert_dialog_add_responses(
                ADW_ALERT_DIALOG(dlg),
                "cancel", "Cancel",
                "connect", "Connect",
                nullptr);
            adw_alert_dialog_set_response_appearance(
                ADW_ALERT_DIALOG(dlg), "connect", ADW_RESPONSE_SUGGESTED);
            adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "connect");
            adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");

            struct RespCtx {
                AuthPromptInfo base;
                GtkWidget* user_row;
                GtkWidget* dom_row;
                GtkWidget* pass_row;
                GtkWidget* remember_user_row;
                GtkWidget* save_row;
                std::shared_ptr<std::promise<AuthOutcome>> promise;
                std::function<void(bool)> suspend_grab;
            };
            auto* rctx = new RespCtx{
                r->info,
                user_row,
                dom_row,
                pass_row,
                remember_user_row,
                save_row,
                r->promise,
                r->suspend_grab
            };

            g_signal_connect(
                dlg,
                "response",
                G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer user_data) {
                    std::unique_ptr<RespCtx> rc(static_cast<RespCtx*>(user_data));
                    AuthOutcome out;
                    out.updated = rc->base;
                    if (response && std::string(response) == "connect") {
                        out.accepted = true;
                        out.updated.username = gtk_editable_get_text(GTK_EDITABLE(rc->user_row));
                        out.updated.domain = gtk_editable_get_text(GTK_EDITABLE(rc->dom_row));
                        out.updated.password = gtk_editable_get_text(GTK_EDITABLE(rc->pass_row));
                        out.updated.remember_username =
                            adw_switch_row_get_active(ADW_SWITCH_ROW(rc->remember_user_row)) == TRUE;
                        out.updated.save_password =
                            adw_switch_row_get_active(ADW_SWITCH_ROW(rc->save_row)) == TRUE;
                    }
                    if (rc->suspend_grab) rc->suspend_grab(false);
                    rc->promise->set_value(std::move(out));
                }),
                rctx);

            adw_dialog_present(dlg, GTK_WIDGET(r->parent));
            return G_SOURCE_REMOVE;
        },
        req);

    AuthOutcome outcome = future.get();
    if (outcome.accepted) {
        info = std::move(outcome.updated);
        return true;
    }
    return false;
}

} // namespace winrdp
