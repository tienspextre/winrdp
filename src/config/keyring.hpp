#pragma once

#include <optional>
#include <string>

namespace winrdp {

class Keyring {
public:
    static bool store_secret(const std::string& profile_id,
                             const std::string& kind,
                             const std::string& label,
                             const std::string& secret);

    static std::optional<std::string> lookup_secret(const std::string& profile_id,
                                                    const std::string& kind);

    static void clear_secret(const std::string& profile_id, const std::string& kind);
    static void clear_all_for_profile(const std::string& profile_id);
};

} // namespace winrdp
