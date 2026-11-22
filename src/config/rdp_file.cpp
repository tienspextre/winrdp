#include "config/rdp_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace winrdp {

namespace {

void append_utf8(uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string decode_utf16_to_utf8(const std::string& raw, bool big_endian) {
    std::string out;
    out.reserve(raw.size() / 2);
    size_t i = 2; // skip 2-byte BOM
    while (i + 1 < raw.size()) {
        uint8_t b0 = static_cast<uint8_t>(raw[i]);
        uint8_t b1 = static_cast<uint8_t>(raw[i + 1]);
        uint16_t u1 = big_endian ? static_cast<uint16_t>((b0 << 8) | b1)
                                 : static_cast<uint16_t>(b0 | (b1 << 8));
        i += 2;

        if (u1 >= 0xD800 && u1 <= 0xDBFF && i + 1 < raw.size()) {
            uint8_t c0 = static_cast<uint8_t>(raw[i]);
            uint8_t c1 = static_cast<uint8_t>(raw[i + 1]);
            uint16_t u2 = big_endian ? static_cast<uint16_t>((c0 << 8) | c1)
                                     : static_cast<uint16_t>(c0 | (c1 << 8));
            if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
                i += 2;
                uint32_t cp = 0x10000U + ((static_cast<uint32_t>(u1 - 0xD800U) << 10) |
                                          static_cast<uint32_t>(u2 - 0xDC00U));
                append_utf8(cp, out);
                continue;
            }
        }
        append_utf8(u1, out);
    }
    return out;
}

std::string normalize_to_utf8(const std::string& raw) {
    if (raw.size() >= 2) {
        uint8_t b0 = static_cast<uint8_t>(raw[0]);
        uint8_t b1 = static_cast<uint8_t>(raw[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
            return decode_utf16_to_utf8(raw, false);
        }
        if (b0 == 0xFE && b1 == 0xFF) {
            return decode_utf16_to_utf8(raw, true);
        }
    }
    if (raw.size() >= 3 &&
        static_cast<uint8_t>(raw[0]) == 0xEF &&
        static_cast<uint8_t>(raw[1]) == 0xBB &&
        static_cast<uint8_t>(raw[2]) == 0xBF) {
        return raw.substr(3);
    }
    return raw;
}

std::string to_lower_trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool parse_int_val(const std::string& v, int& out) {
    try {
        size_t pos = 0;
        long val = std::stol(v, &pos, 10);
        if (pos == 0) return false;
        out = static_cast<int>(val);
        return true;
    } catch (...) {
        return false;
    }
}

void split_host_and_optional_port(const std::string& addr, std::string& host, uint16_t& port, bool& had_port) {
    had_port = false;
    std::string trimmed = addr;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());

    if (trimmed.empty()) return;
    if (trimmed.front() == '[') {
        auto endb = trimmed.find(']');
        if (endb != std::string::npos) {
            host = trimmed.substr(1, endb - 1);
            if (endb + 1 < trimmed.size() && trimmed[endb + 1] == ':') {
                int p = 0;
                if (parse_int_val(trimmed.substr(endb + 2), p) && p >= 1 && p <= 65535) {
                    port = static_cast<uint16_t>(p);
                    had_port = true;
                }
            }
            return;
        }
    }
    auto first_colon = trimmed.find(':');
    auto last_colon = trimmed.rfind(':');
    if (first_colon != std::string::npos && first_colon == last_colon) {
        int p = 0;
        if (parse_int_val(trimmed.substr(last_colon + 1), p) && p >= 1 && p <= 65535) {
            host = trimmed.substr(0, last_colon);
            port = static_cast<uint16_t>(p);
            had_port = true;
            return;
        }
    }
    host = trimmed;
}

} // namespace

bool parse_rdp_content(const std::string& raw_bytes, ConnectionProfile& out_profile, std::string* err) {
    std::string text = normalize_to_utf8(raw_bytes);
    if (text.empty()) {
        if (err) *err = "Empty .rdp content";
        return false;
    }

    ConnectionProfile p = out_profile;
    if (p.id.empty()) {
        p.id = ProfileStore::generate_uuid();
    }

    bool saw_displayconnectionbar = false;
    int displayconnectionbar_val = 1;
    bool saw_pinconnectionbar = false;
    int pinconnectionbar_val = 0;
    bool explicit_port_seen = false;
    bool addr_had_port = false;

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        // Standard .rdp format: key:type:value  (where type is 's' or 'i')
        auto c1 = line.find(':');
        if (c1 == std::string::npos) {
            continue;
        }
        auto c2 = line.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            continue;
        }

        const std::string key = to_lower_trim(line.substr(0, c1));
        const std::string type = to_lower_trim(line.substr(c1 + 1, c2 - c1 - 1));
        std::string val = line.substr(c2 + 1);

        int ival = 0;
        const bool is_int = (type == "i") && parse_int_val(val, ival);

        if (key == "full address") {
            std::string h;
            uint16_t port = p.port;
            split_host_and_optional_port(val, h, port, addr_had_port);
            if (!h.empty()) {
                p.hostname = h;
                if (addr_had_port && !explicit_port_seen) {
                    p.port = port;
                }
            }
        } else if (key == "server port" && is_int) {
            if (ival >= 1 && ival <= 65535) {
                p.port = static_cast<uint16_t>(ival);
                explicit_port_seen = true;
            }
        } else if (key == "username") {
            auto slash = val.find('\\');
            if (slash != std::string::npos && p.domain.empty()) {
                p.domain = val.substr(0, slash);
                p.username = val.substr(slash + 1);
            } else {
                p.username = val;
            }
        } else if (key == "domain") {
            p.domain = val;
        } else if (key == "screen mode id" && is_int) {
            if (ival == 1) {
                if (p.display_mode != DisplayMode::MultiMonitor) {
                    p.display_mode = DisplayMode::Windowed;
                }
            } else if (ival == 2) {
                if (p.display_mode != DisplayMode::MultiMonitor) {
                    p.display_mode = DisplayMode::Fullscreen;
                }
            }
        } else if (key == "use multimon" && is_int) {
            if (ival == 1) {
                p.display_mode = DisplayMode::MultiMonitor;
            }
        } else if (key == "desktopwidth" && is_int) {
            if (ival >= 320 && ival <= 16384) p.width = static_cast<uint32_t>(ival);
        } else if (key == "desktopheight" && is_int) {
            if (ival >= 200 && ival <= 16384) p.height = static_cast<uint32_t>(ival);
        } else if (key == "smart sizing" && is_int) {
            p.smart_sizing = (ival != 0);
        } else if (key == "dynamic resolution" && is_int) {
            p.dynamic_resolution = (ival != 0);
        } else if (key == "displayconnectionbar" && is_int) {
            saw_displayconnectionbar = true;
            displayconnectionbar_val = ival;
        } else if (key == "pinconnectionbar" && is_int) {
            saw_pinconnectionbar = true;
            pinconnectionbar_val = ival;
        } else if (key == "audiomode" && is_int) {
            if (ival == 0) p.audio_mode = AudioMode::Local;
            else if (ival == 1) p.audio_mode = AudioMode::Remote;
            else if (ival == 2) p.audio_mode = AudioMode::None;
        } else if (key == "audiocapturemode" && is_int) {
            p.mic_redirection = (ival != 0);
        } else if (key == "redirectclipboard" && is_int) {
            p.clipboard_redirection = (ival != 0);
        } else if (key == "redirectprinters" && is_int) {
            p.printer_redirection = (ival != 0);
        } else if (key == "drivestoredirect") {
            // Supports Linux paths separated by ';' (or Name=/path)
            std::stringstream dss(val);
            std::string entry;
            while (std::getline(dss, entry, ';')) {
                if (entry.empty()) continue;
                if (entry == "*" || entry == "DynamicDrives") {
                    continue;
                }
                SharedFolder sf;
                auto eq = entry.find('=');
                if (eq != std::string::npos) {
                    sf.name = entry.substr(0, eq);
                    sf.path = entry.substr(eq + 1);
                } else if (entry.front() == '/') {
                    sf.path = entry;
                    sf.name = fs::path(entry).filename().string();
                    if (sf.name.empty()) sf.name = "Share";
                }
                if (!sf.path.empty()) {
                    p.shared_folders.push_back(std::move(sf));
                }
            }
        } else if (key == "keyboardhook" && is_int) {
            if (ival == 0) p.keyboard_grab_mode = KeyboardGrabMode::Never;
            else if (ival == 1) p.keyboard_grab_mode = KeyboardGrabMode::FullscreenOnly;
            else if (ival == 2) p.keyboard_grab_mode = KeyboardGrabMode::AlwaysWhenFocused;
        } else if (key == "gatewayhostname") {
            std::string gwh;
            uint16_t gwp = 443;
            bool gwh_port = false;
            split_host_and_optional_port(val, gwh, gwp, gwh_port);
            p.gateway_hostname = gwh;
            if (gwh_port) p.gateway_port = gwp;
        } else if (key == "allow font smoothing" && is_int) {
            p.font_smoothing = (ival != 0);
        } else if (key == "allow desktop composition" && is_int) {
            p.desktop_composition = (ival != 0);
        } else if (key == "disable wallpaper" && is_int) {
            p.wallpaper = (ival == 0);
        } else if (key == "disable full window drag" && is_int) {
            p.full_window_drag = (ival == 0);
        } else if (key == "disable menu anims" && is_int) {
            p.menu_animations = (ival == 0);
        } else if (key == "autoreconnection enabled" && is_int) {
            p.auto_reconnect = (ival != 0);
        } else if (key == "authentication level" && is_int) {
            if (ival == 0) p.ignore_certificate = true;
        } else if (key == "negotiate security layer" && is_int) {
            if (ival == 0) p.security = SecurityMode::NLA;
        } else if (key == "enablecredsspsupport" && is_int) {
            if (ival == 1 && p.security == SecurityMode::Auto) {
                p.security = SecurityMode::NLA;
            }
        } else if (key == "remoteapplicationprogram") {
            p.remote_app = val;
        }
    }

    if (saw_displayconnectionbar && displayconnectionbar_val == 0) {
        p.connection_bar_mode = ConnectionBarMode::Hidden;
    } else if (saw_pinconnectionbar && pinconnectionbar_val != 0) {
        p.connection_bar_mode = ConnectionBarMode::Pinned;
    } else if (saw_displayconnectionbar) {
        p.connection_bar_mode = ConnectionBarMode::AutoHide;
    }

    if (p.hostname.empty()) {
        if (err) *err = "Missing 'full address:s:' in .rdp file";
        return false;
    }

    if (p.name.empty()) {
        p.name = p.display_title();
    }

    out_profile = std::move(p);
    return true;
}

bool load_rdp_file(const std::string& path, ConnectionProfile& out_profile, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (err) *err = "Failed to open file: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (out_profile.name.empty()) {
        out_profile.name = fs::path(path).stem().string();
    }
    return parse_rdp_content(ss.str(), out_profile, err);
}

std::string export_rdp_content(const ConnectionProfile& p) {
    std::ostringstream out;
    if (p.port == 3389) {
        out << "full address:s:" << p.hostname << "\r\n";
    } else {
        out << "full address:s:" << p.hostname << ":" << p.port << "\r\n";
    }
    out << "server port:i:" << p.port << "\r\n";
    if (!p.username.empty()) {
        out << "username:s:" << p.username << "\r\n";
    }
    if (!p.domain.empty()) {
        out << "domain:s:" << p.domain << "\r\n";
    }
    out << "screen mode id:i:" << (p.display_mode == DisplayMode::Windowed ? 1 : 2) << "\r\n";
    out << "use multimon:i:" << (p.display_mode == DisplayMode::MultiMonitor ? 1 : 0) << "\r\n";
    out << "desktopwidth:i:" << p.width << "\r\n";
    out << "desktopheight:i:" << p.height << "\r\n";
    out << "session bpp:i:32\r\n";
    out << "smart sizing:i:" << (p.smart_sizing ? 1 : 0) << "\r\n";
    out << "dynamic resolution:i:" << (p.dynamic_resolution ? 1 : 0) << "\r\n";

    if (p.connection_bar_mode == ConnectionBarMode::Hidden) {
        out << "displayconnectionbar:i:0\r\n";
        out << "pinconnectionbar:i:0\r\n";
    } else if (p.connection_bar_mode == ConnectionBarMode::Pinned) {
        out << "displayconnectionbar:i:1\r\n";
        out << "pinconnectionbar:i:1\r\n";
    } else {
        out << "displayconnectionbar:i:1\r\n";
        out << "pinconnectionbar:i:0\r\n";
    }

    int audiomode = 0;
    if (p.audio_mode == AudioMode::Remote) audiomode = 1;
    else if (p.audio_mode == AudioMode::None) audiomode = 2;
    out << "audiomode:i:" << audiomode << "\r\n";
    out << "audiocapturemode:i:" << (p.mic_redirection ? 1 : 0) << "\r\n";
    out << "redirectclipboard:i:" << (p.clipboard_redirection ? 1 : 0) << "\r\n";
    out << "redirectprinters:i:" << (p.printer_redirection ? 1 : 0) << "\r\n";

    int kbdhook = 1;
    if (p.keyboard_grab_mode == KeyboardGrabMode::Never) kbdhook = 0;
    else if (p.keyboard_grab_mode == KeyboardGrabMode::AlwaysWhenFocused) kbdhook = 2;
    out << "keyboardhook:i:" << kbdhook << "\r\n";

    if (!p.shared_folders.empty()) {
        out << "drivestoredirect:s:";
        for (size_t i = 0; i < p.shared_folders.size(); ++i) {
            if (i > 0) out << ";";
            out << p.shared_folders[i].name << "=" << p.shared_folders[i].path;
        }
        out << "\r\n";
    }

    out << "allow font smoothing:i:" << (p.font_smoothing ? 1 : 0) << "\r\n";
    out << "allow desktop composition:i:" << (p.desktop_composition ? 1 : 0) << "\r\n";
    out << "disable wallpaper:i:" << (p.wallpaper ? 0 : 1) << "\r\n";
    out << "disable full window drag:i:" << (p.full_window_drag ? 0 : 1) << "\r\n";
    out << "disable menu anims:i:" << (p.menu_animations ? 0 : 1) << "\r\n";
    out << "autoreconnection enabled:i:" << (p.auto_reconnect ? 1 : 0) << "\r\n";
    out << "authentication level:i:" << (p.ignore_certificate ? 0 : 2) << "\r\n";
    if (p.security == SecurityMode::NLA) {
        out << "enablecredsspsupport:i:1\r\n";
        out << "negotiate security layer:i:0\r\n";
    }
    if (!p.gateway_hostname.empty()) {
        out << "gatewayhostname:s:" << p.gateway_hostname;
        if (p.gateway_port != 443) {
            out << ":" << p.gateway_port;
        }
        out << "\r\n";
        out << "gatewayusagemethod:i:1\r\n";
    }
    if (!p.remote_app.empty()) {
        out << "remoteapplicationmode:i:1\r\n";
        out << "remoteapplicationprogram:s:" << p.remote_app << "\r\n";
    }
    return out.str();
}

bool save_rdp_file(const std::string& path, const ConnectionProfile& profile, std::string* err) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (err) *err = "Failed to open file for writing: " + path;
        return false;
    }
    const std::string data = export_rdp_content(profile);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out.good()) {
        if (err) *err = "Failed writing .rdp file: " + path;
        return false;
    }
    return true;
}

} // namespace winrdp
