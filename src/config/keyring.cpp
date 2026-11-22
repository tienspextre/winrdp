#include "config/keyring.hpp"

#include <libsecret/secret.h>

namespace winrdp {

namespace {

const SecretSchema* winrdp_secret_schema() {
    static const SecretSchema schema = {
        "io.winrdp.Client",
        SECRET_SCHEMA_NONE,
        {
            {"profile_id", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"kind", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, static_cast<SecretSchemaAttributeType>(0)},
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    return &schema;
}

} // namespace

bool Keyring::store_secret(const std::string& profile_id,
                           const std::string& kind,
                           const std::string& label,
                           const std::string& secret) {
    if (profile_id.empty()) {
        return false;
    }
    if (secret.empty()) {
        clear_secret(profile_id, kind);
        return true;
    }
    GError* error = nullptr;
    gboolean ok = secret_password_store_sync(
        winrdp_secret_schema(),
        SECRET_COLLECTION_DEFAULT,
        label.empty() ? "WinRDP Password" : label.c_str(),
        secret.c_str(),
        nullptr,
        &error,
        "profile_id", profile_id.c_str(),
        "kind", kind.c_str(),
        nullptr);
    if (error) {
        g_warning("Keyring store failed: %s", error->message);
        g_error_free(error);
        return false;
    }
    return ok == TRUE;
}

std::optional<std::string> Keyring::lookup_secret(const std::string& profile_id,
                                                  const std::string& kind) {
    if (profile_id.empty()) {
        return std::nullopt;
    }
    GError* error = nullptr;
    gchar* pw = secret_password_lookup_sync(
        winrdp_secret_schema(),
        nullptr,
        &error,
        "profile_id", profile_id.c_str(),
        "kind", kind.c_str(),
        nullptr);
    if (error) {
        g_warning("Keyring lookup failed: %s", error->message);
        g_error_free(error);
        return std::nullopt;
    }
    if (!pw) {
        return std::nullopt;
    }
    std::string res(pw);
    secret_password_free(pw);
    return res;
}

void Keyring::clear_secret(const std::string& profile_id, const std::string& kind) {
    if (profile_id.empty()) {
        return;
    }
    GError* error = nullptr;
    secret_password_clear_sync(
        winrdp_secret_schema(),
        nullptr,
        &error,
        "profile_id", profile_id.c_str(),
        "kind", kind.c_str(),
        nullptr);
    if (error) {
        g_error_free(error);
    }
}

void Keyring::clear_all_for_profile(const std::string& profile_id) {
    clear_secret(profile_id, "password");
    clear_secret(profile_id, "gateway_password");
}

} // namespace winrdp
