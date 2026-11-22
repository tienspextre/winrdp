#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace winrdp {

enum class ConnectionBarMode {
    AutoHide = 0,
    Pinned = 1,
    Hidden = 2,
};

enum class CodecMode {
    Auto = 0,     // AVC444 -> AVC420 -> RemoteFX -> Standard
    AVC444 = 1,
    AVC420 = 2,
    RemoteFX = 3,
    Standard = 4,
};

enum class SecurityMode {
    Auto = 0,
    NLA = 1,
    TLS = 2,
    RDP = 3,
};

enum class AudioMode {
    Local = 0,
    Remote = 1,
    None = 2,
};

enum class KeyboardGrabMode {
    FullscreenOnly = 0,
    AlwaysWhenFocused = 1,
    Never = 2,
};

enum class DisplayMode {
    Fullscreen = 0,
    Windowed = 1,
    MultiMonitor = 2,
};

struct SharedFolder {
    std::string name;
    std::string path;
    bool read_only = false;

    bool operator==(const SharedFolder&) const = default;
};

struct ConnectionProfile {
    // Identity
    std::string id;
    std::string name;
    std::string group;
    bool favorite = false;
    int64_t last_connected_epoch = 0;

    // Connection
    std::string hostname;
    uint16_t port = 3389;
    std::string username;
    std::string domain;
    bool save_password = true;

    // Display
    DisplayMode display_mode = DisplayMode::Fullscreen;
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool dynamic_resolution = true;
    bool smart_sizing = false;
    uint32_t scale_factor = 100; // 100 = auto/1x, 125, 150, 200
    std::vector<int32_t> selected_monitors; // empty = all in MultiMonitor or auto/primary in Fullscreen
    ConnectionBarMode connection_bar_mode = ConnectionBarMode::AutoHide;
    std::string bar_toggle_shortcut = "Ctrl+Alt+Home";
    std::string grab_release_key = "RightCtrl";

    // Experience & Performance
    CodecMode codec = CodecMode::Auto;
    bool hardware_decode = true;
    bool font_smoothing = true;
    bool desktop_composition = true;
    bool full_window_drag = true;
    bool menu_animations = true;
    bool wallpaper = true;
    bool auto_reconnect = true;
    uint32_t max_reconnect_attempts = 10;
    bool network_auto_detect = true;

    // Local Resources
    AudioMode audio_mode = AudioMode::Local;
    bool mic_redirection = false;
    bool clipboard_redirection = true;
    bool clipboard_files = true;
    bool printer_redirection = false;
    KeyboardGrabMode keyboard_grab_mode = KeyboardGrabMode::FullscreenOnly;
    std::vector<SharedFolder> shared_folders;

    // Advanced
    SecurityMode security = SecurityMode::Auto;
    bool ignore_certificate = false;
    std::string gateway_hostname;
    uint16_t gateway_port = 443;
    std::string gateway_username;
    std::string gateway_domain;
    bool gateway_use_same_creds = true;
    std::string remote_app;
    std::string extra_notes;

    // Transient (never written to profiles.json)
    std::string runtime_password;
    std::string runtime_gateway_password;

    std::string display_title() const;
};

class ProfileStore {
public:
    explicit ProfileStore(std::string file_path = default_file_path());

    static std::string default_file_path();
    static std::string generate_uuid();

    bool load();
    bool save() const;

    const std::vector<ConnectionProfile>& profiles() const { return profiles_; }
    std::vector<ConnectionProfile>& profiles_mut() { return profiles_; }

    std::optional<ConnectionProfile> find_by_id(const std::string& id) const;
    void upsert(ConnectionProfile profile);
    bool remove(const std::string& id);
    void touch_last_connected(const std::string& id, int64_t epoch);

    std::vector<std::string> groups() const;
    const std::string& file_path() const { return file_path_; }

private:
    std::string file_path_;
    std::vector<ConnectionProfile> profiles_;
};

// Enum string helpers for JSON / UI / diagnostics
const char* to_string(ConnectionBarMode m);
const char* to_string(CodecMode c);
const char* to_string(SecurityMode s);
const char* to_string(AudioMode a);
const char* to_string(KeyboardGrabMode k);
const char* to_string(DisplayMode d);

ConnectionBarMode parse_bar_mode(const std::string& s, ConnectionBarMode fallback = ConnectionBarMode::AutoHide);
CodecMode parse_codec_mode(const std::string& s, CodecMode fallback = CodecMode::Auto);
SecurityMode parse_security_mode(const std::string& s, SecurityMode fallback = SecurityMode::Auto);
AudioMode parse_audio_mode(const std::string& s, AudioMode fallback = AudioMode::Local);
KeyboardGrabMode parse_grab_mode(const std::string& s, KeyboardGrabMode fallback = KeyboardGrabMode::FullscreenOnly);
DisplayMode parse_display_mode(const std::string& s, DisplayMode fallback = DisplayMode::Fullscreen);

// Parses "rdp://DOMAIN\\user@host:3389", "user@host:3390", "[::1]:3389", "192.168.1.50"
bool parse_quick_connect_target(const std::string& input, ConnectionProfile& out_profile);

} // namespace winrdp
