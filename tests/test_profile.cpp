#include "config/profile.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#define WINRDP_CHECK(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK FAILED: " #cond " (" << __FILE__ << ":"        \
                      << __LINE__ << ")\n";                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

namespace fs = std::filesystem;
using namespace winrdp;

int main() {
    const fs::path tmp_dir = fs::temp_directory_path() / "winrdp_test_profiles";
    fs::remove_all(tmp_dir);
    fs::create_directories(tmp_dir);
    const fs::path store_file = tmp_dir / "profiles.json";

    ConnectionProfile p1;
    p1.id = ProfileStore::generate_uuid();
    p1.name = "Office PC";
    p1.group = "Work";
    p1.favorite = true;
    p1.hostname = "office.example.local";
    p1.port = 3391;
    p1.username = "alice";
    p1.domain = "ACME";
    p1.save_password = false;
    p1.remember_username = true;
    p1.runtime_password = "TOP_SECRET_SHOULD_NOT_BE_ON_DISK";
    p1.display_mode = DisplayMode::MultiMonitor;
    p1.selected_monitors = {0, 2};
    p1.connection_bar_mode = ConnectionBarMode::Hidden;
    p1.connection_bar_hide_hover = true;
    p1.codec = CodecMode::AVC444;
    p1.security = SecurityMode::NLA;
    p1.shared_folders.push_back(SharedFolder{"Downloads", "/home/alice/Downloads", false});

    ProfileStore s1(store_file.string());
    s1.set_last_credentials("tiennd35", "VIETTELGROUP");
    s1.upsert(p1);
    WINRDP_CHECK(s1.save());

    // Ensure plaintext password is never written to JSON
    {
        std::ifstream in(store_file);
        std::ostringstream ss;
        ss << in.rdbuf();
        WINRDP_CHECK(ss.str().find("TOP_SECRET_SHOULD_NOT_BE_ON_DISK") == std::string::npos);
    }

    ProfileStore s2(store_file.string());
    WINRDP_CHECK(s2.load());
    WINRDP_CHECK(s2.profiles().size() == 1);
    WINRDP_CHECK(s2.last_username() == "tiennd35");
    WINRDP_CHECK(s2.last_domain() == "VIETTELGROUP");
    const auto loaded = s2.find_by_id(p1.id);
    WINRDP_CHECK(loaded.has_value());
    WINRDP_CHECK(loaded->name == "Office PC");
    WINRDP_CHECK(loaded->group == "Work");
    WINRDP_CHECK(loaded->favorite == true);
    WINRDP_CHECK(loaded->port == 3391);
    WINRDP_CHECK(loaded->save_password == false);
    WINRDP_CHECK(loaded->remember_username == true);
    WINRDP_CHECK(loaded->display_mode == DisplayMode::MultiMonitor);
    WINRDP_CHECK(loaded->selected_monitors.size() == 2 &&
                 loaded->selected_monitors[0] == 0 && loaded->selected_monitors[1] == 2);
    WINRDP_CHECK(loaded->connection_bar_mode == ConnectionBarMode::Hidden);
    WINRDP_CHECK(loaded->connection_bar_hide_hover == true);
    WINRDP_CHECK(loaded->codec == CodecMode::AVC444);
    WINRDP_CHECK(loaded->shared_folders.size() == 1);
    WINRDP_CHECK(loaded->runtime_password.empty());
    WINRDP_CHECK(s2.groups().size() == 1 && s2.groups()[0] == "Work");

    // Credential normalization (DOMAIN\user, trailing slash, local .\, UPN @)
    {
        auto c1 = normalize_rdp_credentials("tiennd35", "VIETTELGROUP");
        WINRDP_CHECK(c1.username == "tiennd35" && c1.domain == "VIETTELGROUP");

        auto c2 = normalize_rdp_credentials("VIETTELGROUP\\tiennd35", "");
        WINRDP_CHECK(c2.username == "tiennd35" && c2.domain == "VIETTELGROUP");

        auto c3 = normalize_rdp_credentials("VIETTELGROUP\\tiennd35", "VIETTELGROUP");
        WINRDP_CHECK(c3.username == "tiennd35" && c3.domain == "VIETTELGROUP");

        auto c4 = normalize_rdp_credentials("  tiennd35 ", " VIETTELGROUP\\ ");
        WINRDP_CHECK(c4.username == "tiennd35" && c4.domain == "VIETTELGROUP");

        auto c5 = normalize_rdp_credentials(".\\localuser", "VIETTELGROUP");
        WINRDP_CHECK(c5.username == "localuser" && c5.domain.empty());

        auto c6 = normalize_rdp_credentials("tiennd35@viettelgroup.vn", "VIETTELGROUP");
        WINRDP_CHECK(c6.username == "tiennd35@viettelgroup.vn" && c6.domain.empty());
    }

    // IP literal helper
    WINRDP_CHECK(is_ip_literal("10.0.0.5") == true);
    WINRDP_CHECK(is_ip_literal("::1") == true);
    WINRDP_CHECK(is_ip_literal("[::1]") == true);
    WINRDP_CHECK(is_ip_literal("win11.corp.local") == false);

    // Quick connect URI parsing
    ConnectionProfile qc1;
    WINRDP_CHECK(parse_quick_connect_target("rdp://CORP\\alice@10.0.0.5:3390", qc1));
    WINRDP_CHECK(qc1.domain == "CORP" && qc1.username == "alice" && qc1.hostname == "10.0.0.5" && qc1.port == 3390);

    ConnectionProfile qc2;
    WINRDP_CHECK(parse_quick_connect_target("[::1]:3395", qc2));
    WINRDP_CHECK(qc2.hostname == "::1" && qc2.port == 3395);

    ConnectionProfile qc3;
    WINRDP_CHECK(parse_quick_connect_target("192.168.1.50", qc3));
    WINRDP_CHECK(qc3.hostname == "192.168.1.50" && qc3.port == 3389);

    WINRDP_CHECK(s2.remove(p1.id));
    WINRDP_CHECK(s2.profiles().empty());

    fs::remove_all(tmp_dir);
    std::cout << "test_profile: PASS\n";
    return 0;
}
