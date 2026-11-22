#include "config/profile.hpp"

#include <cjson/cJSON.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace winrdp {

namespace {

std::string json_get_str(const cJSON* obj, const char* key, const std::string& def = "") {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return def;
}

bool json_get_bool(const cJSON* obj, const char* key, bool def) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return def;
}

int64_t json_get_i64(const cJSON* obj, const char* key, int64_t def) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return static_cast<int64_t>(item->valuedouble);
    }
    return def;
}

uint32_t json_get_u32(const cJSON* obj, const char* key, uint32_t def) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0) {
        return static_cast<uint32_t>(item->valuedouble);
    }
    return def;
}

std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

} // namespace

std::string ConnectionProfile::display_title() const {
    if (!name.empty()) {
        return name;
    }
    if (!hostname.empty()) {
        return port == 3389 ? hostname : (hostname + ":" + std::to_string(port));
    }
    return "Untitled Connection";
}

std::string ProfileStore::default_file_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base;
    if (xdg && xdg[0] != '\0') {
        base = fs::path(xdg);
    } else {
        const char* home = std::getenv("HOME");
        base = home ? (fs::path(home) / ".config") : fs::temp_directory_path();
    }
    return (base / "winrdp" / "profiles.json").string();
}

std::string ProfileStore::generate_uuid() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t a = dis(gen);
    uint64_t b = dis(gen);
    // Set UUID v4 version and variant bits
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>((a >> 32) & 0xFFFFFFFFU),
                  static_cast<unsigned>((a >> 16) & 0xFFFFU),
                  static_cast<unsigned>(a & 0xFFFFU),
                  static_cast<unsigned>((b >> 48) & 0xFFFFU),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return std::string(buf);
}

ProfileStore::ProfileStore(std::string file_path)
    : file_path_(std::move(file_path)) {}

bool ProfileStore::load() {
    profiles_.clear();
    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();
    if (content.empty()) {
        return true;
    }

    cJSON* root = cJSON_ParseWithLength(content.data(), content.size());
    if (!root) {
        return false;
    }

    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_IsArray(root) ? root : nullptr;
    }

    if (arr) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, arr) {
            if (!cJSON_IsObject(item)) {
                continue;
            }
            ConnectionProfile p;
            p.id = json_get_str(item, "id");
            if (p.id.empty()) {
                p.id = generate_uuid();
            }
            p.name = json_get_str(item, "name");
            p.group = json_get_str(item, "group");
            p.favorite = json_get_bool(item, "favorite", false);
            p.last_connected_epoch = json_get_i64(item, "last_connected_epoch", 0);

            p.hostname = json_get_str(item, "hostname");
            uint32_t port = json_get_u32(item, "port", 3389);
            p.port = static_cast<uint16_t>(std::clamp<uint32_t>(port, 1, 65535));
            p.username = json_get_str(item, "username");
            p.domain = json_get_str(item, "domain");
            p.save_password = json_get_bool(item, "save_password", true);

            p.display_mode = parse_display_mode(json_get_str(item, "display_mode", "Fullscreen"));
            p.width = std::clamp<uint32_t>(json_get_u32(item, "width", 1920), 320, 16384);
            p.height = std::clamp<uint32_t>(json_get_u32(item, "height", 1080), 200, 16384);
            p.dynamic_resolution = json_get_bool(item, "dynamic_resolution", true);
            p.smart_sizing = json_get_bool(item, "smart_sizing", false);
            p.scale_factor = json_get_u32(item, "scale_factor", 100);
            p.connection_bar_mode = parse_bar_mode(json_get_str(item, "connection_bar_mode", "AutoHide"));
            p.bar_toggle_shortcut = json_get_str(item, "bar_toggle_shortcut", "Ctrl+Alt+Home");
            p.grab_release_key = json_get_str(item, "grab_release_key", "RightCtrl");

            p.codec = parse_codec_mode(json_get_str(item, "codec", "Auto"));
            p.hardware_decode = json_get_bool(item, "hardware_decode", true);
            p.font_smoothing = json_get_bool(item, "font_smoothing", true);
            p.desktop_composition = json_get_bool(item, "desktop_composition", true);
            p.full_window_drag = json_get_bool(item, "full_window_drag", true);
            p.menu_animations = json_get_bool(item, "menu_animations", true);
            p.wallpaper = json_get_bool(item, "wallpaper", true);
            p.auto_reconnect = json_get_bool(item, "auto_reconnect", true);
            p.max_reconnect_attempts = json_get_u32(item, "max_reconnect_attempts", 10);
            p.network_auto_detect = json_get_bool(item, "network_auto_detect", true);

            p.audio_mode = parse_audio_mode(json_get_str(item, "audio_mode", "Local"));
            p.mic_redirection = json_get_bool(item, "mic_redirection", false);
            p.clipboard_redirection = json_get_bool(item, "clipboard_redirection", true);
            p.clipboard_files = json_get_bool(item, "clipboard_files", true);
            p.printer_redirection = json_get_bool(item, "printer_redirection", false);
            p.keyboard_grab_mode = parse_grab_mode(json_get_str(item, "keyboard_grab_mode", "FullscreenOnly"));

            const cJSON* folders = cJSON_GetObjectItemCaseSensitive(item, "shared_folders");
            if (cJSON_IsArray(folders)) {
                const cJSON* f = nullptr;
                cJSON_ArrayForEach(f, folders) {
                    if (!cJSON_IsObject(f)) {
                        continue;
                    }
                    SharedFolder sf;
                    sf.name = json_get_str(f, "name");
                    sf.path = json_get_str(f, "path");
                    sf.read_only = json_get_bool(f, "read_only", false);
                    if (!sf.path.empty()) {
                        if (sf.name.empty()) {
                            sf.name = fs::path(sf.path).filename().string();
                            if (sf.name.empty()) {
                                sf.name = "Share";
                            }
                        }
                        p.shared_folders.push_back(std::move(sf));
                    }
                }
            }

            p.security = parse_security_mode(json_get_str(item, "security", "Auto"));
            p.ignore_certificate = json_get_bool(item, "ignore_certificate", false);
            p.gateway_hostname = json_get_str(item, "gateway_hostname");
            uint32_t gw_port = json_get_u32(item, "gateway_port", 443);
            p.gateway_port = static_cast<uint16_t>(std::clamp<uint32_t>(gw_port, 1, 65535));
            p.gateway_username = json_get_str(item, "gateway_username");
            p.gateway_domain = json_get_str(item, "gateway_domain");
            p.gateway_use_same_creds = json_get_bool(item, "gateway_use_same_creds", true);
            p.remote_app = json_get_str(item, "remote_app");
            p.extra_notes = json_get_str(item, "extra_notes");

            profiles_.push_back(std::move(p));
        }
    }

    cJSON_Delete(root);
    return true;
}

bool ProfileStore::save() const {
    std::error_code ec;
    fs::path p(file_path_);
    if (!p.parent_path().empty()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON* arr = cJSON_AddArrayToObject(root, "profiles");

    for (const auto& prof : profiles_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", prof.id.c_str());
        cJSON_AddStringToObject(o, "name", prof.name.c_str());
        cJSON_AddStringToObject(o, "group", prof.group.c_str());
        cJSON_AddBoolToObject(o, "favorite", prof.favorite);
        cJSON_AddNumberToObject(o, "last_connected_epoch", static_cast<double>(prof.last_connected_epoch));

        cJSON_AddStringToObject(o, "hostname", prof.hostname.c_str());
        cJSON_AddNumberToObject(o, "port", prof.port);
        cJSON_AddStringToObject(o, "username", prof.username.c_str());
        cJSON_AddStringToObject(o, "domain", prof.domain.c_str());
        cJSON_AddBoolToObject(o, "save_password", prof.save_password);

        cJSON_AddStringToObject(o, "display_mode", to_string(prof.display_mode));
        cJSON_AddNumberToObject(o, "width", prof.width);
        cJSON_AddNumberToObject(o, "height", prof.height);
        cJSON_AddBoolToObject(o, "dynamic_resolution", prof.dynamic_resolution);
        cJSON_AddBoolToObject(o, "smart_sizing", prof.smart_sizing);
        cJSON_AddNumberToObject(o, "scale_factor", prof.scale_factor);
        cJSON_AddStringToObject(o, "connection_bar_mode", to_string(prof.connection_bar_mode));
        cJSON_AddStringToObject(o, "bar_toggle_shortcut", prof.bar_toggle_shortcut.c_str());
        cJSON_AddStringToObject(o, "grab_release_key", prof.grab_release_key.c_str());

        cJSON_AddStringToObject(o, "codec", to_string(prof.codec));
        cJSON_AddBoolToObject(o, "hardware_decode", prof.hardware_decode);
        cJSON_AddBoolToObject(o, "font_smoothing", prof.font_smoothing);
        cJSON_AddBoolToObject(o, "desktop_composition", prof.desktop_composition);
        cJSON_AddBoolToObject(o, "full_window_drag", prof.full_window_drag);
        cJSON_AddBoolToObject(o, "menu_animations", prof.menu_animations);
        cJSON_AddBoolToObject(o, "wallpaper", prof.wallpaper);
        cJSON_AddBoolToObject(o, "auto_reconnect", prof.auto_reconnect);
        cJSON_AddNumberToObject(o, "max_reconnect_attempts", prof.max_reconnect_attempts);
        cJSON_AddBoolToObject(o, "network_auto_detect", prof.network_auto_detect);

        cJSON_AddStringToObject(o, "audio_mode", to_string(prof.audio_mode));
        cJSON_AddBoolToObject(o, "mic_redirection", prof.mic_redirection);
        cJSON_AddBoolToObject(o, "clipboard_redirection", prof.clipboard_redirection);
        cJSON_AddBoolToObject(o, "clipboard_files", prof.clipboard_files);
        cJSON_AddBoolToObject(o, "printer_redirection", prof.printer_redirection);
        cJSON_AddStringToObject(o, "keyboard_grab_mode", to_string(prof.keyboard_grab_mode));

        cJSON* folders = cJSON_AddArrayToObject(o, "shared_folders");
        for (const auto& sf : prof.shared_folders) {
            cJSON* fo = cJSON_CreateObject();
            cJSON_AddStringToObject(fo, "name", sf.name.c_str());
            cJSON_AddStringToObject(fo, "path", sf.path.c_str());
            cJSON_AddBoolToObject(fo, "read_only", sf.read_only);
            cJSON_AddItemToArray(folders, fo);
        }

        cJSON_AddStringToObject(o, "security", to_string(prof.security));
        cJSON_AddBoolToObject(o, "ignore_certificate", prof.ignore_certificate);
        cJSON_AddStringToObject(o, "gateway_hostname", prof.gateway_hostname.c_str());
        cJSON_AddNumberToObject(o, "gateway_port", prof.gateway_port);
        cJSON_AddStringToObject(o, "gateway_username", prof.gateway_username.c_str());
        cJSON_AddStringToObject(o, "gateway_domain", prof.gateway_domain.c_str());
        cJSON_AddBoolToObject(o, "gateway_use_same_creds", prof.gateway_use_same_creds);
        cJSON_AddStringToObject(o, "remote_app", prof.remote_app.c_str());
        cJSON_AddStringToObject(o, "extra_notes", prof.extra_notes.c_str());

        cJSON_AddItemToArray(arr, o);
    }

    char* rendered = cJSON_Print(root);
    cJSON_Delete(root);
    if (!rendered) {
        return false;
    }

    const std::string tmp_path = file_path_ + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            cJSON_free(rendered);
            return false;
        }
        out << rendered << '\n';
        out.flush();
        if (!out.good()) {
            cJSON_free(rendered);
            return false;
        }
    }
    cJSON_free(rendered);

    fs::rename(tmp_path, file_path_, ec);
    return !ec;
}

std::optional<ConnectionProfile> ProfileStore::find_by_id(const std::string& id) const {
    for (const auto& p : profiles_) {
        if (p.id == id) {
            return p;
        }
    }
    return std::nullopt;
}

void ProfileStore::upsert(ConnectionProfile profile) {
    if (profile.id.empty()) {
        profile.id = generate_uuid();
    }
    for (auto& existing : profiles_) {
        if (existing.id == profile.id) {
            existing = std::move(profile);
            return;
        }
    }
    profiles_.push_back(std::move(profile));
}

bool ProfileStore::remove(const std::string& id) {
    auto it = std::remove_if(profiles_.begin(), profiles_.end(),
                             [&](const ConnectionProfile& p) { return p.id == id; });
    if (it == profiles_.end()) {
        return false;
    }
    profiles_.erase(it, profiles_.end());
    return true;
}

void ProfileStore::touch_last_connected(const std::string& id, int64_t epoch) {
    for (auto& p : profiles_) {
        if (p.id == id) {
            p.last_connected_epoch = epoch;
            return;
        }
    }
}

std::vector<std::string> ProfileStore::groups() const {
    std::set<std::string> unique_groups;
    for (const auto& p : profiles_) {
        if (!p.group.empty()) {
            unique_groups.insert(p.group);
        }
    }
    return {unique_groups.begin(), unique_groups.end()};
}

const char* to_string(ConnectionBarMode m) {
    switch (m) {
        case ConnectionBarMode::AutoHide: return "AutoHide";
        case ConnectionBarMode::Pinned: return "Pinned";
        case ConnectionBarMode::Hidden: return "Hidden";
    }
    return "AutoHide";
}

const char* to_string(CodecMode c) {
    switch (c) {
        case CodecMode::Auto: return "Auto";
        case CodecMode::AVC444: return "AVC444";
        case CodecMode::AVC420: return "AVC420";
        case CodecMode::RemoteFX: return "RemoteFX";
        case CodecMode::Standard: return "Standard";
    }
    return "Auto";
}

const char* to_string(SecurityMode s) {
    switch (s) {
        case SecurityMode::Auto: return "Auto";
        case SecurityMode::NLA: return "NLA";
        case SecurityMode::TLS: return "TLS";
        case SecurityMode::RDP: return "RDP";
    }
    return "Auto";
}

const char* to_string(AudioMode a) {
    switch (a) {
        case AudioMode::Local: return "Local";
        case AudioMode::Remote: return "Remote";
        case AudioMode::None: return "None";
    }
    return "Local";
}

const char* to_string(KeyboardGrabMode k) {
    switch (k) {
        case KeyboardGrabMode::FullscreenOnly: return "FullscreenOnly";
        case KeyboardGrabMode::AlwaysWhenFocused: return "AlwaysWhenFocused";
        case KeyboardGrabMode::Never: return "Never";
    }
    return "FullscreenOnly";
}

const char* to_string(DisplayMode d) {
    switch (d) {
        case DisplayMode::Fullscreen: return "Fullscreen";
        case DisplayMode::Windowed: return "Windowed";
        case DisplayMode::MultiMonitor: return "MultiMonitor";
    }
    return "Fullscreen";
}

ConnectionBarMode parse_bar_mode(const std::string& s, ConnectionBarMode fallback) {
    if (s == "AutoHide") return ConnectionBarMode::AutoHide;
    if (s == "Pinned") return ConnectionBarMode::Pinned;
    if (s == "Hidden") return ConnectionBarMode::Hidden;
    return fallback;
}

CodecMode parse_codec_mode(const std::string& s, CodecMode fallback) {
    if (s == "Auto") return CodecMode::Auto;
    if (s == "AVC444") return CodecMode::AVC444;
    if (s == "AVC420") return CodecMode::AVC420;
    if (s == "RemoteFX") return CodecMode::RemoteFX;
    if (s == "Standard") return CodecMode::Standard;
    return fallback;
}

SecurityMode parse_security_mode(const std::string& s, SecurityMode fallback) {
    if (s == "Auto") return SecurityMode::Auto;
    if (s == "NLA") return SecurityMode::NLA;
    if (s == "TLS") return SecurityMode::TLS;
    if (s == "RDP") return SecurityMode::RDP;
    return fallback;
}

AudioMode parse_audio_mode(const std::string& s, AudioMode fallback) {
    if (s == "Local") return AudioMode::Local;
    if (s == "Remote") return AudioMode::Remote;
    if (s == "None") return AudioMode::None;
    return fallback;
}

KeyboardGrabMode parse_grab_mode(const std::string& s, KeyboardGrabMode fallback) {
    if (s == "FullscreenOnly") return KeyboardGrabMode::FullscreenOnly;
    if (s == "AlwaysWhenFocused") return KeyboardGrabMode::AlwaysWhenFocused;
    if (s == "Never") return KeyboardGrabMode::Never;
    return fallback;
}

DisplayMode parse_display_mode(const std::string& s, DisplayMode fallback) {
    if (s == "Fullscreen") return DisplayMode::Fullscreen;
    if (s == "Windowed") return DisplayMode::Windowed;
    if (s == "MultiMonitor") return DisplayMode::MultiMonitor;
    return fallback;
}

bool parse_quick_connect_target(const std::string& input, ConnectionProfile& out_profile) {
    std::string raw = trim_copy(input);
    if (raw.empty()) {
        return false;
    }

    if (raw.rfind("rdp://", 0) == 0 || raw.rfind("RDP://", 0) == 0) {
        raw = raw.substr(6);
        while (!raw.empty() && raw.back() == '/') {
            raw.pop_back();
        }
    }

    // Strip query string if any
    auto qpos = raw.find('?');
    if (qpos != std::string::npos) {
        raw = raw.substr(0, qpos);
    }

    // Optional userinfo before '@'
    auto at_pos = raw.rfind('@');
    std::string host_part = raw;
    if (at_pos != std::string::npos) {
        std::string user_part = raw.substr(0, at_pos);
        host_part = raw.substr(at_pos + 1);
        auto slash_pos = user_part.find('\\');
        if (slash_pos == std::string::npos) {
            slash_pos = user_part.find('/');
        }
        if (slash_pos != std::string::npos) {
            out_profile.domain = user_part.substr(0, slash_pos);
            out_profile.username = user_part.substr(slash_pos + 1);
        } else {
            out_profile.username = user_part;
        }
    }

    host_part = trim_copy(host_part);
    if (host_part.empty()) {
        return false;
    }

    uint16_t port = 3389;
    std::string host = host_part;

    if (host_part.front() == '[') {
        auto close_bracket = host_part.find(']');
        if (close_bracket == std::string::npos) {
            return false;
        }
        host = host_part.substr(1, close_bracket - 1);
        if (close_bracket + 1 < host_part.size() && host_part[close_bracket + 1] == ':') {
            auto port_str = host_part.substr(close_bracket + 2);
            char* endp = nullptr;
            long val = std::strtol(port_str.c_str(), &endp, 10);
            if (!port_str.empty() && endp && *endp == '\0' && val >= 1 && val <= 65535) {
                port = static_cast<uint16_t>(val);
            } else {
                return false;
            }
        }
    } else {
        auto colon = host_part.rfind(':');
        if (colon != std::string::npos && host_part.find(':') == colon) {
            auto port_str = host_part.substr(colon + 1);
            host = host_part.substr(0, colon);
            char* endp = nullptr;
            long val = std::strtol(port_str.c_str(), &endp, 10);
            if (!port_str.empty() && endp && *endp == '\0' && val >= 1 && val <= 65535) {
                port = static_cast<uint16_t>(val);
            } else {
                return false;
            }
        }
    }

    if (host.empty()) {
        return false;
    }

    out_profile.hostname = host;
    out_profile.port = port;
    if (out_profile.name.empty()) {
        out_profile.name = (port == 3389) ? host : (host + ":" + std::to_string(port));
    }
    if (out_profile.id.empty()) {
        out_profile.id = ProfileStore::generate_uuid();
    }
    return true;
}

} // namespace winrdp
