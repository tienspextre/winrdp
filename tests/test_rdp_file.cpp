#include "config/rdp_file.hpp"

#include <iostream>
#include <string>

#define WINRDP_CHECK(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK FAILED: " #cond " (" << __FILE__ << ":"        \
                      << __LINE__ << ")\n";                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

using namespace winrdp;

static std::string to_utf16le_with_bom(const std::string& ascii) {
    std::string out;
    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xFE));
    for (unsigned char c : ascii) {
        out.push_back(static_cast<char>(c));
        out.push_back('\0');
    }
    return out;
}

int main() {
    // 1. UTF-8 Windows mstsc/Thincast .rdp with hidden connection bar & custom port
    const std::string utf8_rdp =
        "\xEF\xBB\xBF"
        "full address:s:win11.lan:3390\r\n"
        "username:s:CORP\\alice\r\n"
        "screen mode id:i:2\r\n"
        "use multimon:i:1\r\n"
        "selectedmonitors:s:0,2\r\n"
        "desktopwidth:i:2560\r\n"
        "desktopheight:i:1440\r\n"
        "smart sizing:i:1\r\n"
        "dynamic resolution:i:0\r\n"
        "displayconnectionbar:i:0\r\n"
        "audiomode:i:1\r\n"
        "audiocapturemode:i:1\r\n"
        "redirectclipboard:i:1\r\n"
        "redirectprinters:i:1\r\n"
        "keyboardhook:i:2\r\n"
        "drivestoredirect:s:Docs=/home/alice/Documents;/tmp/share\r\n"
        "allow font smoothing:i:1\r\n"
        "disable wallpaper:i:1\r\n"
        "autoreconnection enabled:i:1\r\n";

    ConnectionProfile p1;
    std::string err;
    WINRDP_CHECK(parse_rdp_content(utf8_rdp, p1, &err));
    WINRDP_CHECK(p1.hostname == "win11.lan");
    WINRDP_CHECK(p1.port == 3390);
    WINRDP_CHECK(p1.domain == "CORP");
    WINRDP_CHECK(p1.username == "alice");
    WINRDP_CHECK(p1.display_mode == DisplayMode::MultiMonitor);
    WINRDP_CHECK(p1.selected_monitors.size() == 2 &&
                 p1.selected_monitors[0] == 0 && p1.selected_monitors[1] == 2);
    WINRDP_CHECK(p1.width == 2560 && p1.height == 1440);
    WINRDP_CHECK(p1.smart_sizing == true);
    WINRDP_CHECK(p1.dynamic_resolution == false);
    WINRDP_CHECK(p1.connection_bar_mode == ConnectionBarMode::Hidden);
    WINRDP_CHECK(p1.audio_mode == AudioMode::Remote);
    WINRDP_CHECK(p1.mic_redirection == true);
    WINRDP_CHECK(p1.printer_redirection == true);
    WINRDP_CHECK(p1.keyboard_grab_mode == KeyboardGrabMode::AlwaysWhenFocused);
    WINRDP_CHECK(p1.shared_folders.size() == 2);
    WINRDP_CHECK(p1.shared_folders[0].name == "Docs");
    WINRDP_CHECK(p1.shared_folders[0].path == "/home/alice/Documents");
    WINRDP_CHECK(p1.shared_folders[1].name == "share");
    WINRDP_CHECK(p1.wallpaper == false);

    // 2. UTF-16LE with BOM (native Windows mstsc.exe save format) + pinned bar
    const std::string utf16_src =
        "full address:s:10.20.30.40\r\n"
        "server port:i:3389\r\n"
        "username:s:bob\r\n"
        "screen mode id:i:1\r\n"
        "displayconnectionbar:i:1\r\n"
        "pinconnectionbar:i:1\r\n";
    ConnectionProfile p2;
    WINRDP_CHECK(parse_rdp_content(to_utf16le_with_bom(utf16_src), p2, &err));
    WINRDP_CHECK(p2.hostname == "10.20.30.40");
    WINRDP_CHECK(p2.port == 3389);
    WINRDP_CHECK(p2.username == "bob");
    WINRDP_CHECK(p2.display_mode == DisplayMode::Windowed);
    WINRDP_CHECK(p2.connection_bar_mode == ConnectionBarMode::Pinned);

    // 3. Export -> re-parse round-trip
    const std::string exported = export_rdp_content(p1);
    ConnectionProfile p3;
    WINRDP_CHECK(parse_rdp_content(exported, p3, &err));
    WINRDP_CHECK(p3.hostname == p1.hostname);
    WINRDP_CHECK(p3.port == p1.port);
    WINRDP_CHECK(p3.username == p1.username);
    WINRDP_CHECK(p3.domain == p1.domain);
    WINRDP_CHECK(p3.display_mode == p1.display_mode);
    WINRDP_CHECK(p3.selected_monitors == p1.selected_monitors);
    WINRDP_CHECK(p3.connection_bar_mode == p1.connection_bar_mode);
    WINRDP_CHECK(p3.shared_folders.size() == 2);

    std::cout << "test_rdp_file: PASS\n";
    return 0;
}
