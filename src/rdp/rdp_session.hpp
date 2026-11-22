#pragma once

#include "config/profile.hpp"
#include "input/monitor_layout.hpp"
#include "rdp/clipboard_bridge.hpp"
#include "rdp/framebuffer.hpp"
#include "rdp/pointer_bridge.hpp"

#include <freerdp/client.h>
#include <freerdp/client/disp.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/freerdp.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace winrdp {

enum class SessionState {
    Idle,
    Connecting,
    Connected,
    Reconnecting,
    Disconnected,
    Error,
};

struct SessionTelemetry {
    double fps = 0.0;
    int32_t rtt_ms = -1;
    std::string codec = "GDI";
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t monitor_count = 1;
    bool hw_accel = false;
    bool dynamic_resize_ready = false;
};

struct CertPromptInfo {
    std::string host;
    uint16_t port = 3389;
    std::string common_name;
    std::string subject;
    std::string issuer;
    std::string fingerprint;
    std::string valid_from;
    std::string valid_to;
    bool changed = false;
    std::string old_subject;
    std::string old_issuer;
    std::string old_fingerprint;
    bool is_gateway = false;
};

struct AuthPromptInfo {
    std::string target;
    std::string username;
    std::string domain;
    std::string password;
    bool save_password = true;
    bool is_gateway = false;
};

struct SessionCallbacks {
    std::function<void(SessionState state, const std::string& msg, uint32_t attempt, uint32_t max_attempts)> on_state_changed;
    std::function<void()> on_frame_ready;
    std::function<void(uint32_t width, uint32_t height)> on_desktop_resized;
    std::function<void(const SessionTelemetry& telemetry)> on_telemetry;
    // Synchronous prompts called on RDP thread; return 0=cancel, 1=trust+store, 2=trust once
    std::function<uint32_t(const CertPromptInfo& info)> on_verify_certificate;
    // Synchronous credential prompt; return true if accepted
    std::function<bool(AuthPromptInfo& info)> on_authenticate;
};

class RdpSession {
public:
    RdpSession(ConnectionProfile profile,
               std::vector<MonitorInfo> monitors,
               SessionCallbacks callbacks);
    ~RdpSession();

    RdpSession(const RdpSession&) = delete;
    RdpSession& operator=(const RdpSession&) = delete;

    bool start();
    void disconnect();

    Framebuffer& framebuffer() { return fb_; }
    PointerBridge& pointer_bridge() { return pointer_; }
    ClipboardBridge& clipboard_bridge() { return clipboard_; }

    const ConnectionProfile& profile() const { return profile_; }
    VirtualDesktopLayout active_layout() const;
    SessionState state() const { return state_.load(std::memory_order_acquire); }
    bool is_connected() const { return state() == SessionState::Connected; }
    SessionTelemetry telemetry() const;

    // Input forwarding (safe to call from GTK thread)
    void send_key(uint32_t rdp_scancode, bool down, bool repeat);
    void send_unicode_key(uint16_t code, bool down);
    void send_sync_locks(uint32_t sync_flags);
    void send_focus_in(uint16_t toggle_states);
    void send_mouse_move(int x, int y);
    void send_mouse_button(uint16_t flags, int x, int y, bool extended);
    void send_mouse_wheel(uint16_t flags);
    void send_ctrl_alt_del();

    // Dynamic resolution & suppression
    void request_resize(uint32_t width, uint32_t height, uint32_t scale_percent = 100);
    void request_monitor_layout(const std::vector<MonitorInfo>& monitors, uint32_t scale_percent = 100);
    void set_suppress_output(bool suppress);

    // Clear pending frame coalescing bit when UI consumes a draw tick
    void clear_frame_pending() { frame_pending_.store(false, std::memory_order_release); }

    static std::string freerdp_version_string();
    static bool has_h264_hwaccel_support();

private:
    struct WinRdpClientContext {
        rdpClientContext common;
        RdpSession* session;
    };

    static BOOL on_client_new(freerdp* instance, rdpContext* context);
    static void on_client_free(freerdp* instance, rdpContext* context);
    static BOOL on_pre_connect(freerdp* instance);
    static BOOL on_post_connect(freerdp* instance);
    static void on_post_disconnect(freerdp* instance);
    static void on_post_final_disconnect(freerdp* instance);
    static BOOL on_begin_paint(rdpContext* context);
    static BOOL on_end_paint(rdpContext* context);
    static BOOL on_desktop_resize(rdpContext* context);
    static DWORD on_verify_certificate_ex(freerdp* instance, const char* host, UINT16 port,
                                          const char* common_name, const char* subject,
                                          const char* issuer, const char* fingerprint, DWORD flags);
    static DWORD on_verify_changed_certificate_ex(freerdp* instance, const char* host, UINT16 port,
                                                  const char* common_name, const char* subject,
                                                  const char* issuer, const char* new_fingerprint,
                                                  const char* old_subject, const char* old_issuer,
                                                  const char* old_fingerprint, DWORD flags);
    static BOOL on_authenticate_ex(freerdp* instance, char** username, char** password,
                                   char** domain, rdp_auth_reason reason);
    static SSIZE_T on_retry_dialog(freerdp* instance, const char* what, size_t current, void* userarg);
    static void on_channel_connected(void* context, const ChannelConnectedEventArgs* e);
    static void on_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e);

    bool configure_settings();
    void worker_main();
    void set_state(SessionState s, const std::string& msg = "", uint32_t attempt = 0, uint32_t max_attempts = 0);
    void update_telemetry(bool force_emit = false);
    std::string detect_active_codec() const;

    ConnectionProfile profile_;
    std::vector<MonitorInfo> monitors_;
    SessionCallbacks callbacks_;
    mutable std::mutex layout_mutex_;
    VirtualDesktopLayout active_layout_;

    rdpContext* ctx_ = nullptr;
    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<SessionState> state_{SessionState::Idle};
    std::atomic<bool> frame_pending_{false};

    Framebuffer fb_;
    PointerBridge pointer_;
    ClipboardBridge clipboard_;

    mutable std::mutex channel_mutex_;
    DispClientContext* disp_ = nullptr;
    RdpgfxClientContext* gfx_ = nullptr;
    std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> last_disp_layout_;

    mutable std::mutex telemetry_mutex_;
    SessionTelemetry telemetry_{};
    uint32_t fps_frame_counter_ = 0;
    uint64_t fps_window_start_ms_ = 0;
    uint64_t last_telemetry_emit_ms_ = 0;
    int32_t handshake_rtt_ms_ = -1;
};

} // namespace winrdp
