#include "rdp/rdp_session.hpp"

#include <freerdp/autodetect.h>
#include <freerdp/build-config.h>
#include <freerdp/config.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/disp.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/constants.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/input.h>
#include <freerdp/scancode.h>
#include <freerdp/settings.h>
#include <freerdp/version.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unistd.h>

namespace winrdp {

namespace {

uint64_t steady_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void extract_pem_details(const char* pem_or_fp, DWORD flags, CertPromptInfo& info, bool is_old) {
    if (!pem_or_fp) return;
    if ((flags & VERIFY_CERT_FLAG_FP_IS_PEM) != 0) {
        rdpCertificate* cert = freerdp_certificate_new_from_pem(pem_or_fp);
        if (cert) {
            char* fp = freerdp_certificate_get_fingerprint(cert);
            char* from = freerdp_certificate_get_validity(cert, TRUE);
            char* to = freerdp_certificate_get_validity(cert, FALSE);
            if (is_old) {
                if (fp) info.old_fingerprint = fp;
            } else {
                if (fp) info.fingerprint = fp;
                if (from) info.valid_from = from;
                if (to) info.valid_to = to;
            }
            free(fp);
            free(from);
            free(to);
            freerdp_certificate_free(cert);
            return;
        }
    }
    if (is_old) {
        info.old_fingerprint = pem_or_fp;
    } else {
        info.fingerprint = pem_or_fp;
    }
}

std::string build_error_message(rdpContext* ctx) {
    if (!ctx || !ctx->instance) return "Connection failed";
    const DWORD err_info = freerdp_error_info(ctx->instance);
    const UINT32 last = freerdp_get_last_error(ctx);
    if (last != FREERDP_ERROR_SUCCESS) {
        const char* name = freerdp_get_last_error_name(last);
        const char* str = freerdp_get_last_error_string(last);
        return std::string(str ? str : (name ? name : "Connection error"));
    }
    if (err_info != ERRINFO_SUCCESS) {
        const char* str = freerdp_get_error_info_string(err_info);
        if (str && str[0] != '\0') return str;
    }
    return "Disconnected from remote host";
}

} // namespace

std::string RdpSession::freerdp_version_string() {
    return freerdp_get_version_string();
}

bool RdpSession::has_h264_hwaccel_support() {
    const char* cfg = freerdp_get_build_config();
    if (!cfg) return false;
    const std::string_view sv(cfg);
    return sv.find("WITH_GFX_H264=ON") != std::string_view::npos &&
           sv.find("WITH_FFMPEG=ON") != std::string_view::npos &&
           sv.find("WITH_FFMPEG_HWACCEL=ON") != std::string_view::npos;
}

RdpSession::RdpSession(ConnectionProfile profile,
                       std::vector<MonitorInfo> monitors,
                       SessionCallbacks callbacks)
    : profile_(std::move(profile)),
      monitors_(std::move(monitors)),
      callbacks_(std::move(callbacks)) {
    if (monitors_.empty()) {
        monitors_.push_back(MonitorInfo{0, 0, static_cast<int32_t>(profile_.width),
                                        static_cast<int32_t>(profile_.height), 0, 0, 1, true});
    }
    telemetry_.width = profile_.width;
    telemetry_.height = profile_.height;
    telemetry_.hw_accel = profile_.hardware_decode && has_h264_hwaccel_support();
}

RdpSession::~RdpSession() {
    disconnect();
}

BOOL RdpSession::on_client_new(freerdp* instance, rdpContext* context) {
    if (!instance || !context) return FALSE;
    auto* wctx = reinterpret_cast<WinRdpClientContext*>(context);
    wctx->session = nullptr;

    instance->PreConnect = &RdpSession::on_pre_connect;
    instance->PostConnect = &RdpSession::on_post_connect;
    instance->PostDisconnect = &RdpSession::on_post_disconnect;
    instance->PostFinalDisconnect = &RdpSession::on_post_final_disconnect;
    instance->VerifyCertificateEx = &RdpSession::on_verify_certificate_ex;
    instance->VerifyChangedCertificateEx = &RdpSession::on_verify_changed_certificate_ex;
    instance->AuthenticateEx = &RdpSession::on_authenticate_ex;
    instance->RetryDialog = &RdpSession::on_retry_dialog;
    return TRUE;
}

void RdpSession::on_client_free(freerdp* /*instance*/, rdpContext* /*context*/) {
}

bool RdpSession::start() {
    if (worker_.joinable()) {
        return false;
    }
    stop_requested_.store(false, std::memory_order_release);

    RDP_CLIENT_ENTRY_POINTS entry{};
    entry.Version = RDP_CLIENT_INTERFACE_VERSION;
    entry.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
    entry.ContextSize = sizeof(WinRdpClientContext);
    entry.ClientNew = &RdpSession::on_client_new;
    entry.ClientFree = &RdpSession::on_client_free;

    ctx_ = freerdp_client_context_new(&entry);
    if (!ctx_) {
        set_state(SessionState::Error, "Failed to allocate FreeRDP client context");
        return false;
    }

    reinterpret_cast<WinRdpClientContext*>(ctx_)->session = this;

    if (!configure_settings()) {
        freerdp_client_context_free(ctx_);
        ctx_ = nullptr;
        set_state(SessionState::Error, "Invalid connection settings");
        return false;
    }

    set_state(SessionState::Connecting, "Connecting to " + profile_.hostname + "...");
    worker_ = std::thread(&RdpSession::worker_main, this);
    return true;
}

void RdpSession::disconnect() {
    stop_requested_.store(true, std::memory_order_release);
    if (ctx_) {
        freerdp_set_last_error(ctx_, FREERDP_ERROR_CONNECT_CANCELLED);
        freerdp_abort_connect_context(ctx_);
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    pointer_.detach_widget();
    clipboard_.detach_gtk_clipboard();
    if (ctx_) {
        freerdp_client_context_free(ctx_);
        ctx_ = nullptr;
    }
}

bool RdpSession::configure_settings() {
    if (!ctx_ || !ctx_->settings) return false;
    rdpSettings* s = ctx_->settings;

    bool ok = true;
    ok &= freerdp_settings_set_string(s, FreeRDP_ServerHostname, profile_.hostname.c_str()) == TRUE;
    ok &= freerdp_settings_set_uint32(s, FreeRDP_ServerPort, profile_.port) == TRUE;
    if (!profile_.username.empty()) {
        ok &= freerdp_settings_set_string(s, FreeRDP_Username, profile_.username.c_str()) == TRUE;
    }
    if (!profile_.domain.empty()) {
        ok &= freerdp_settings_set_string(s, FreeRDP_Domain, profile_.domain.c_str()) == TRUE;
    }
    if (!profile_.runtime_password.empty()) {
        ok &= freerdp_settings_set_string(s, FreeRDP_Password, profile_.runtime_password.c_str()) == TRUE;
    }

    char hostbuf[128] = {};
    if (gethostname(hostbuf, sizeof(hostbuf) - 1) == 0 && hostbuf[0] != '\0') {
        std::ignore = freerdp_settings_set_string(s, FreeRDP_ClientHostname, hostbuf);
    }

    ok &= freerdp_settings_set_bool(s, FreeRDP_CertificateCallbackPreferPEM, TRUE) == TRUE;
    ok &= freerdp_settings_set_uint32(s, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX) == TRUE;
    ok &= freerdp_settings_set_uint32(s, FreeRDP_OsMinorType, OSMINORTYPE_NATIVE_XSERVER) == TRUE;

    // Security
    switch (profile_.security) {
        case SecurityMode::Auto:
            ok &= freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, TRUE) == TRUE;
            ok &= freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, TRUE) == TRUE;
            ok &= freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, TRUE) == TRUE;
            break;
        case SecurityMode::NLA:
            ok &= freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, TRUE) == TRUE;
            ok &= freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, FALSE) == TRUE;
            ok &= freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, FALSE) == TRUE;
            break;
        case SecurityMode::TLS:
            ok &= freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, FALSE) == TRUE;
            ok &= freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, TRUE) == TRUE;
            ok &= freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, FALSE) == TRUE;
            break;
        case SecurityMode::RDP:
            ok &= freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, FALSE) == TRUE;
            ok &= freerdp_settings_set_bool(s, FreeRDP_TlsSecurity, FALSE) == TRUE;
            ok &= freerdp_settings_set_bool(s, FreeRDP_RdpSecurity, TRUE) == TRUE;
            break;
    }
    ok &= freerdp_settings_set_bool(s, FreeRDP_IgnoreCertificate, profile_.ignore_certificate ? TRUE : FALSE) == TRUE;

    // RD Gateway
    if (!profile_.gateway_hostname.empty()) {
        ok &= freerdp_settings_set_bool(s, FreeRDP_GatewayEnabled, TRUE) == TRUE;
        ok &= freerdp_settings_set_string(s, FreeRDP_GatewayHostname, profile_.gateway_hostname.c_str()) == TRUE;
        ok &= freerdp_settings_set_uint32(s, FreeRDP_GatewayPort, profile_.gateway_port) == TRUE;
        ok &= freerdp_settings_set_bool(s, FreeRDP_GatewayUseSameCredentials,
                                        profile_.gateway_use_same_creds ? TRUE : FALSE) == TRUE;
        if (!profile_.gateway_use_same_creds) {
            if (!profile_.gateway_username.empty()) {
                ok &= freerdp_settings_set_string(s, FreeRDP_GatewayUsername,
                                                  profile_.gateway_username.c_str()) == TRUE;
            }
            if (!profile_.gateway_domain.empty()) {
                ok &= freerdp_settings_set_string(s, FreeRDP_GatewayDomain,
                                                  profile_.gateway_domain.c_str()) == TRUE;
            }
            if (!profile_.runtime_gateway_password.empty()) {
                ok &= freerdp_settings_set_string(s, FreeRDP_GatewayPassword,
                                                  profile_.runtime_gateway_password.c_str()) == TRUE;
            }
        }
    }

    // Display & Monitors
    const bool is_multimon = (profile_.display_mode == DisplayMode::MultiMonitor && monitors_.size() > 1);
    const bool is_fs = (profile_.display_mode != DisplayMode::Windowed);
    ok &= freerdp_settings_set_bool(s, FreeRDP_Fullscreen, is_fs ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_UseMultimon, is_multimon ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_SmartSizing, profile_.smart_sizing ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_DynamicResolutionUpdate,
                                    profile_.dynamic_resolution ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, 32) == TRUE;

    uint32_t desktop_w = std::clamp<uint32_t>(profile_.width & ~1U, 320, 8192);
    uint32_t desktop_h = std::clamp<uint32_t>(profile_.height & ~1U, 200, 8192);

    if (is_multimon) {
        auto rdp_mons = build_freerdp_monitors(monitors_, profile_.scale_factor);
        ok &= freerdp_settings_set_monitor_def_array_sorted(s, rdp_mons.data(), rdp_mons.size()) == TRUE;
        int32_t min_x = INT32_MAX, min_y = INT32_MAX, max_x = INT32_MIN, max_y = INT32_MIN;
        for (const auto& rm : rdp_mons) {
            min_x = std::min(min_x, rm.x);
            min_y = std::min(min_y, rm.y);
            max_x = std::max(max_x, rm.x + rm.width);
            max_y = std::max(max_y, rm.y + rm.height);
        }
        desktop_w = static_cast<uint32_t>(std::max(320, max_x - min_x));
        desktop_h = static_cast<uint32_t>(std::max(200, max_y - min_y));
    }

    ok &= freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth, desktop_w) == TRUE;
    ok &= freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight, desktop_h) == TRUE;
    if (profile_.scale_factor > 100) {
        ok &= freerdp_settings_set_uint32(s, FreeRDP_DesktopScaleFactor, profile_.scale_factor) == TRUE;
        ok &= freerdp_settings_set_uint32(s, FreeRDP_DeviceScaleFactor, 100) == TRUE;
    }

    // Graphics pipeline & codecs (Thincast / Windows mstsc parity)
    const bool use_gfx = (profile_.codec != CodecMode::Standard);
    const bool use_h264 = (profile_.codec == CodecMode::Auto ||
                           profile_.codec == CodecMode::AVC444 ||
                           profile_.codec == CodecMode::AVC420);
    const bool use_avc444 = (profile_.codec == CodecMode::Auto ||
                             profile_.codec == CodecMode::AVC444);

    ok &= freerdp_settings_set_bool(s, FreeRDP_SupportGraphicsPipeline, use_gfx ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxH264, use_h264 ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxAVC444, use_avc444 ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxAVC444v2, use_avc444 ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_RemoteFxCodec, use_gfx ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_GfxProgressive, use_gfx ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_FastPathOutput, TRUE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_FastPathInput, TRUE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_FrameMarkerCommandEnabled, TRUE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_SurfaceFrameMarkerEnabled, TRUE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_SupportDynamicChannels, TRUE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_SupportDisplayControl,
                                    profile_.dynamic_resolution ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_SupportDynamicTimeZone, TRUE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_NetworkAutoDetect,
                                    profile_.network_auto_detect ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_AutoReconnectionEnabled,
                                    profile_.auto_reconnect ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_uint32(s, FreeRDP_AutoReconnectMaxRetries,
                                      profile_.max_reconnect_attempts) == TRUE;

    // Performance flags
    UINT32 perf = PERF_FLAG_NONE;
    if (profile_.font_smoothing) perf |= PERF_ENABLE_FONT_SMOOTHING;
    if (profile_.desktop_composition) perf |= PERF_ENABLE_DESKTOP_COMPOSITION;
    if (!profile_.wallpaper) perf |= PERF_DISABLE_WALLPAPER;
    if (!profile_.full_window_drag) perf |= PERF_DISABLE_FULLWINDOWDRAG;
    if (!profile_.menu_animations) perf |= PERF_DISABLE_MENUANIMATIONS;
    ok &= freerdp_settings_set_uint32(s, FreeRDP_PerformanceFlags, perf) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_AllowFontSmoothing, profile_.font_smoothing ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_AllowDesktopComposition,
                                    profile_.desktop_composition ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_DisableWallpaper, profile_.wallpaper ? FALSE : TRUE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_DisableFullWindowDrag,
                                    profile_.full_window_drag ? FALSE : TRUE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_DisableMenuAnims,
                                    profile_.menu_animations ? FALSE : TRUE) == TRUE;

    // Redirection channels
    ok &= freerdp_settings_set_bool(s, FreeRDP_RedirectClipboard,
                                    profile_.clipboard_redirection ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_AudioPlayback,
                                    profile_.audio_mode == AudioMode::Local ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_RemoteConsoleAudio,
                                    profile_.audio_mode == AudioMode::Remote ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_AudioCapture,
                                    profile_.mic_redirection ? TRUE : FALSE) == TRUE;
    ok &= freerdp_settings_set_bool(s, FreeRDP_RedirectPrinters,
                                    profile_.printer_redirection ? TRUE : FALSE) == TRUE;

    for (const auto& sf : profile_.shared_folders) {
        if (sf.path.empty()) continue;
        const std::string share_name = sf.name.empty() ? "Share" : sf.name;
        const char* params[3] = {"drive", share_name.c_str(), sf.path.c_str()};
        std::ignore = freerdp_client_add_device_channel(s, 3, params);
    }

    return ok;
}

BOOL RdpSession::on_pre_connect(freerdp* instance) {
    if (!instance || !instance->context) return FALSE;
    PubSub_SubscribeChannelConnected(instance->context->pubSub, &RdpSession::on_channel_connected);
    PubSub_SubscribeChannelDisconnected(instance->context->pubSub, &RdpSession::on_channel_disconnected);
    return TRUE;
}

BOOL RdpSession::on_post_connect(freerdp* instance) {
    if (!instance || !instance->context) return FALSE;
    rdpContext* context = instance->context;
    auto* self = reinterpret_cast<WinRdpClientContext*>(context)->session;
    if (!self) return FALSE;

    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
        return FALSE;
    }

    rdpGdi* gdi = context->gdi;
    if (gdi) {
        self->fb_.resize(static_cast<uint32_t>(gdi->width), static_cast<uint32_t>(gdi->height));
        std::lock_guard<std::mutex> lock(self->telemetry_mutex_);
        self->telemetry_.width = static_cast<uint32_t>(gdi->width);
        self->telemetry_.height = static_cast<uint32_t>(gdi->height);
    }

    std::ignore = self->pointer_.register_callbacks(context);

    if (context->update) {
        context->update->BeginPaint = &RdpSession::on_begin_paint;
        context->update->EndPaint = &RdpSession::on_end_paint;
        context->update->DesktopResize = &RdpSession::on_desktop_resize;
    }

    if (self->callbacks_.on_desktop_resized && gdi) {
        self->callbacks_.on_desktop_resized(static_cast<uint32_t>(gdi->width),
                                            static_cast<uint32_t>(gdi->height));
    }

    self->set_state(SessionState::Connected, "Connected");
    return TRUE;
}

void RdpSession::on_post_disconnect(freerdp* instance) {
    if (!instance || !instance->context) return;
    auto* self = reinterpret_cast<WinRdpClientContext*>(instance->context)->session;
    if (self) {
        std::lock_guard<std::mutex> lock(self->channel_mutex_);
        self->disp_ = nullptr;
        self->gfx_ = nullptr;
        self->last_disp_layout_.clear();
    }
    gdi_free(instance);
}

void RdpSession::on_post_final_disconnect(freerdp* instance) {
    if (!instance || !instance->context) return;
    PubSub_UnsubscribeChannelConnected(instance->context->pubSub, &RdpSession::on_channel_connected);
    PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub, &RdpSession::on_channel_disconnected);
}

BOOL RdpSession::on_begin_paint(rdpContext* context) {
    if (!context || !context->gdi || !context->gdi->primary || !context->gdi->primary->hdc) {
        return TRUE;
    }
    HGDI_WND hwnd = context->gdi->primary->hdc->hwnd;
    if (!hwnd || !hwnd->invalid) return TRUE;
    hwnd->invalid->null = TRUE;
    hwnd->ninvalid = 0;
    return TRUE;
}

BOOL RdpSession::on_end_paint(rdpContext* context) {
    if (!context || !context->gdi) return TRUE;
    auto* self = reinterpret_cast<WinRdpClientContext*>(context)->session;
    rdpGdi* gdi = context->gdi;
    if (!self || !gdi->primary || !gdi->primary->hdc || !gdi->primary->hdc->hwnd) {
        return TRUE;
    }

    HGDI_WND hwnd = gdi->primary->hdc->hwnd;
    if (gdi->suppressOutput || !hwnd->invalid || hwnd->invalid->null || hwnd->ninvalid <= 0) {
        return TRUE;
    }

    const INT32 n = hwnd->ninvalid;
    for (INT32 i = 0; i < n; ++i) {
        const GDI_RGN& r = hwnd->cinvalid[i];
        self->fb_.update_region(gdi->primary_buffer, gdi->stride, r.x, r.y, r.w, r.h);
    }

    hwnd->invalid->null = TRUE;
    hwnd->ninvalid = 0;

    if (!self->frame_pending_.exchange(true, std::memory_order_acq_rel)) {
        if (self->callbacks_.on_frame_ready) {
            self->callbacks_.on_frame_ready();
        }
    }

    self->update_telemetry(false);
    return TRUE;
}

BOOL RdpSession::on_desktop_resize(rdpContext* context) {
    if (!context || !context->gdi || !context->settings) return FALSE;
    auto* self = reinterpret_cast<WinRdpClientContext*>(context)->session;
    if (!self) return FALSE;

    const UINT32 w = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
    const UINT32 h = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
    if (!gdi_resize(context->gdi, w, h)) {
        return FALSE;
    }
    self->fb_.resize(w, h);
    {
        std::lock_guard<std::mutex> lock(self->telemetry_mutex_);
        self->telemetry_.width = w;
        self->telemetry_.height = h;
    }
    if (!self->frame_pending_.exchange(true, std::memory_order_acq_rel)) {
        if (self->callbacks_.on_frame_ready) self->callbacks_.on_frame_ready();
    }
    if (self->callbacks_.on_desktop_resized) {
        self->callbacks_.on_desktop_resized(w, h);
    }
    self->update_telemetry(true);
    return TRUE;
}

DWORD RdpSession::on_verify_certificate_ex(freerdp* instance, const char* host, UINT16 port,
                                           const char* common_name, const char* subject,
                                           const char* issuer, const char* fingerprint, DWORD flags) {
    if (!instance || !instance->context) return 0;
    auto* self = reinterpret_cast<WinRdpClientContext*>(instance->context)->session;
    if (!self) return 0;
    if (self->profile_.ignore_certificate) return 2;
    if (!self->callbacks_.on_verify_certificate) return 2;

    CertPromptInfo info;
    info.host = host ? host : "";
    info.port = port;
    info.common_name = common_name ? common_name : "";
    info.subject = subject ? subject : "";
    info.issuer = issuer ? issuer : "";
    info.changed = false;
    info.is_gateway = (flags & VERIFY_CERT_FLAG_GATEWAY) != 0;
    extract_pem_details(fingerprint, flags, info, false);

    return self->callbacks_.on_verify_certificate(info);
}

DWORD RdpSession::on_verify_changed_certificate_ex(freerdp* instance, const char* host, UINT16 port,
                                                   const char* common_name, const char* subject,
                                                   const char* issuer, const char* new_fingerprint,
                                                   const char* old_subject, const char* old_issuer,
                                                   const char* old_fingerprint, DWORD flags) {
    if (!instance || !instance->context) return 0;
    auto* self = reinterpret_cast<WinRdpClientContext*>(instance->context)->session;
    if (!self) return 0;
    if (self->profile_.ignore_certificate) return 2;
    if (!self->callbacks_.on_verify_certificate) return 0;

    CertPromptInfo info;
    info.host = host ? host : "";
    info.port = port;
    info.common_name = common_name ? common_name : "";
    info.subject = subject ? subject : "";
    info.issuer = issuer ? issuer : "";
    info.changed = true;
    info.old_subject = old_subject ? old_subject : "";
    info.old_issuer = old_issuer ? old_issuer : "";
    info.is_gateway = (flags & VERIFY_CERT_FLAG_GATEWAY) != 0;
    extract_pem_details(new_fingerprint, flags, info, false);
    extract_pem_details(old_fingerprint, flags, info, true);

    return self->callbacks_.on_verify_certificate(info);
}

BOOL RdpSession::on_authenticate_ex(freerdp* instance, char** username, char** password,
                                    char** domain, rdp_auth_reason reason) {
    if (!instance || !instance->context) return FALSE;
    auto* self = reinterpret_cast<WinRdpClientContext*>(instance->context)->session;
    if (!self || !self->callbacks_.on_authenticate) return FALSE;

    if ((reason == AUTH_TLS || reason == AUTH_RDP) &&
        username && *username && (*username)[0] != '\0' &&
        password && *password && (*password)[0] != '\0') {
        return TRUE;
    }

    AuthPromptInfo info;
    info.is_gateway = (reason == GW_AUTH_HTTP || reason == GW_AUTH_RDG || reason == GW_AUTH_RPC);
    info.target = info.is_gateway ? self->profile_.gateway_hostname : self->profile_.hostname;
    info.username = (username && *username) ? *username : self->profile_.username;
    info.domain = (domain && *domain) ? *domain : self->profile_.domain;
    info.password = (password && *password) ? *password : "";
    info.save_password = self->profile_.save_password;

    if (!self->callbacks_.on_authenticate(info)) {
        return FALSE;
    }

    auto replace_cstr = [](char** dst, const std::string& val) {
        if (!dst) return;
        free(*dst);
        *dst = strdup(val.c_str());
    };
    replace_cstr(username, info.username);
    replace_cstr(domain, info.domain);
    replace_cstr(password, info.password);
    return TRUE;
}

SSIZE_T RdpSession::on_retry_dialog(freerdp* instance, const char* /*what*/, size_t current, void* /*userarg*/) {
    if (!instance || !instance->context) return -1;
    auto* self = reinterpret_cast<WinRdpClientContext*>(instance->context)->session;
    if (!self || self->stop_requested_.load(std::memory_order_acquire)) {
        return -1;
    }
    const uint32_t max_retries = self->profile_.max_reconnect_attempts;
    if (!self->profile_.auto_reconnect || (max_retries > 0 && current >= max_retries)) {
        return -1;
    }
    self->set_state(SessionState::Reconnecting,
                    "Connection lost. Reconnecting...",
                    static_cast<uint32_t>(current + 1),
                    max_retries);
    // Progressive backoff in ms: 1s, 2s, 3s, max 5s
    const SSIZE_T delay_ms = static_cast<SSIZE_T>(std::min<size_t>(5000, (current + 1) * 1000));
    return delay_ms;
}

void RdpSession::on_channel_connected(void* context, const ChannelConnectedEventArgs* e) {
    if (!context || !e) return;
    auto* rdp_ctx = static_cast<rdpContext*>(context);
    auto* self = reinterpret_cast<WinRdpClientContext*>(context)->session;
    if (!self) return;

    if (std::strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
        if (self->profile_.clipboard_redirection) {
            self->clipboard_.bind_channel(static_cast<CliprdrClientContext*>(e->pInterface), rdp_ctx);
        }
    } else if (std::strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
        {
            std::lock_guard<std::mutex> lock(self->channel_mutex_);
            self->disp_ = static_cast<DispClientContext*>(e->pInterface);
            self->last_disp_layout_.clear();
        }
        {
            std::lock_guard<std::mutex> tlock(self->telemetry_mutex_);
            self->telemetry_.dynamic_resize_ready = true;
        }
        self->update_telemetry(true);
    } else if (std::strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        freerdp_client_OnChannelConnectedEventHandler(context, e);
        std::lock_guard<std::mutex> lock(self->channel_mutex_);
        self->gfx_ = static_cast<RdpgfxClientContext*>(e->pInterface);
    } else {
        freerdp_client_OnChannelConnectedEventHandler(context, e);
    }
}

void RdpSession::on_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e) {
    if (!context || !e) return;
    auto* self = reinterpret_cast<WinRdpClientContext*>(context)->session;
    if (!self) return;

    if (std::strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
        self->clipboard_.unbind_channel(static_cast<CliprdrClientContext*>(e->pInterface));
    } else if (std::strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
        std::lock_guard<std::mutex> lock(self->channel_mutex_);
        self->disp_ = nullptr;
        self->last_disp_layout_.clear();
    } else if (std::strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
        {
            std::lock_guard<std::mutex> lock(self->channel_mutex_);
            self->gfx_ = nullptr;
        }
        freerdp_client_OnChannelDisconnectedEventHandler(context, e);
    } else {
        freerdp_client_OnChannelDisconnectedEventHandler(context, e);
    }
}

void RdpSession::worker_main() {
    const uint64_t t0 = steady_ms();
    const BOOL connected = freerdp_connect(ctx_->instance);
    if (!connected) {
        if (stop_requested_.load(std::memory_order_acquire)) {
            set_state(SessionState::Disconnected, "Disconnected");
        } else {
            set_state(SessionState::Error, build_error_message(ctx_));
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        handshake_rtt_ms_ = static_cast<int32_t>(std::min<uint64_t>(9999, steady_ms() - t0));
        fps_window_start_ms_ = steady_ms();
    }
    update_telemetry(true);

    bool error_ended = false;
    std::string err_msg;

    while (!stop_requested_.load(std::memory_order_acquire) &&
           !freerdp_shall_disconnect_context(ctx_)) {
        HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
        const DWORD n = freerdp_get_event_handles(ctx_, handles, ARRAYSIZE(handles));
        if (n == 0) {
            error_ended = true;
            err_msg = "Failed to query FreeRDP event handles";
            break;
        }

        const DWORD status = WaitForMultipleObjects(n, handles, FALSE, 2000);
        if (status == WAIT_FAILED) {
            error_ended = true;
            err_msg = "Event wait failed";
            break;
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }

        if (!freerdp_check_event_handles(ctx_)) {
            if (!stop_requested_.load(std::memory_order_acquire) &&
                profile_.auto_reconnect &&
                client_auto_reconnect(ctx_->instance)) {
                set_state(SessionState::Connected, "Reconnected");
                update_telemetry(true);
                continue;
            }
            if (!stop_requested_.load(std::memory_order_acquire)) {
                const DWORD einfo = freerdp_error_info(ctx_->instance);
                const UINT32 lerr = freerdp_get_last_error(ctx_);
                if (einfo != ERRINFO_LOGOFF_BY_USER &&
                    lerr != FREERDP_ERROR_CONNECT_CANCELLED) {
                    error_ended = true;
                    err_msg = build_error_message(ctx_);
                }
            }
            break;
        }

        update_telemetry(false);
    }

    freerdp_disconnect(ctx_->instance);

    if (stop_requested_.load(std::memory_order_acquire) || !error_ended) {
        set_state(SessionState::Disconnected, "Session closed");
    } else {
        set_state(SessionState::Error, err_msg);
    }
}

void RdpSession::set_state(SessionState s, const std::string& msg, uint32_t attempt, uint32_t max_attempts) {
    state_.store(s, std::memory_order_release);
    if (callbacks_.on_state_changed) {
        callbacks_.on_state_changed(s, msg, attempt, max_attempts);
    }
}

std::string RdpSession::detect_active_codec() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (!gfx_) {
        return profile_.codec == CodecMode::Standard ? "GDI" : "RFX/GDI";
    }
    const uint64_t avc444v2 = rdpgfx_stats_value_for_index(gfx_, RDPGFX_CODECID_AVC444v2);
    const uint64_t avc444 = rdpgfx_stats_value_for_index(gfx_, RDPGFX_CODECID_AVC444);
    const uint64_t avc420 = rdpgfx_stats_value_for_index(gfx_, RDPGFX_CODECID_AVC420);
    const uint64_t prog2 = rdpgfx_stats_value_for_index(gfx_, RDPGFX_CODECID_CAPROGRESSIVE_V2);
    const uint64_t prog = rdpgfx_stats_value_for_index(gfx_, RDPGFX_CODECID_CAPROGRESSIVE);
    const uint64_t clear = rdpgfx_stats_value_for_index(gfx_, RDPGFX_CODECID_CLEARCODEC);

    if (avc444v2 > 0 || avc444 > 0) return "AVC444";
    if (avc420 > 0) return "AVC420";
    if (prog2 > 0 || prog > 0) return "RFX-Prog";
    if (clear > 0) return "ClearCodec";
    return "EGFX";
}

void RdpSession::update_telemetry(bool force_emit) {
    const uint64_t now = steady_ms();
    SessionTelemetry snapshot;
    bool emit = force_emit;

    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        if (!force_emit) {
            ++fps_frame_counter_;
        }
        if (fps_window_start_ms_ == 0) {
            fps_window_start_ms_ = now;
        }
        const uint64_t elapsed = now - fps_window_start_ms_;
        if (elapsed >= 1000) {
            telemetry_.fps = static_cast<double>(fps_frame_counter_) * 1000.0 / static_cast<double>(elapsed);
            fps_frame_counter_ = 0;
            fps_window_start_ms_ = now;
        }

        if (ctx_ && ctx_->autodetect && ctx_->autodetect->netCharAverageRTT > 0) {
            telemetry_.rtt_ms = static_cast<int32_t>(ctx_->autodetect->netCharAverageRTT);
        } else if (handshake_rtt_ms_ >= 0 && telemetry_.rtt_ms < 0) {
            telemetry_.rtt_ms = handshake_rtt_ms_;
        }

        if (force_emit || (now - last_telemetry_emit_ms_) >= 1500) {
            last_telemetry_emit_ms_ = now;
            emit = true;
        }
        if (emit) {
            telemetry_.codec = detect_active_codec();
            snapshot = telemetry_;
        }
    }

    if (emit && callbacks_.on_telemetry) {
        callbacks_.on_telemetry(snapshot);
    }
}

SessionTelemetry RdpSession::telemetry() const {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    return telemetry_;
}

void RdpSession::send_key(uint32_t rdp_scancode, bool down, bool repeat) {
    if (!is_connected() || !ctx_ || !ctx_->input || rdp_scancode == 0) return;
    freerdp_input_send_keyboard_event_ex(ctx_->input, down ? TRUE : FALSE, repeat ? TRUE : FALSE, rdp_scancode);
}

void RdpSession::send_unicode_key(uint16_t code, bool down) {
    if (!is_connected() || !ctx_ || !ctx_->input) return;
    freerdp_input_send_unicode_keyboard_event(ctx_->input, down ? 0 : KBD_FLAGS_RELEASE, code);
}

void RdpSession::send_sync_locks(uint32_t sync_flags) {
    if (!is_connected() || !ctx_ || !ctx_->input) return;
    freerdp_input_send_synchronize_event(ctx_->input, sync_flags);
}

void RdpSession::send_focus_in(uint16_t toggle_states) {
    if (!is_connected() || !ctx_ || !ctx_->input) return;
    freerdp_input_send_focus_in_event(ctx_->input, toggle_states);
}

void RdpSession::send_mouse_move(int x, int y) {
    if (!is_connected() || !ctx_) return;
    auto* cctx = reinterpret_cast<rdpClientContext*>(ctx_);
    freerdp_client_send_button_event(cctx, FALSE, PTR_FLAGS_MOVE, x, y);
}

void RdpSession::send_mouse_button(uint16_t flags, int x, int y, bool extended) {
    if (!is_connected() || !ctx_) return;
    auto* cctx = reinterpret_cast<rdpClientContext*>(ctx_);
    if (extended) {
        freerdp_client_send_extended_button_event(cctx, FALSE, flags, x, y);
    } else {
        freerdp_client_send_button_event(cctx, FALSE, flags, x, y);
    }
}

void RdpSession::send_mouse_wheel(uint16_t flags) {
    if (!is_connected() || !ctx_) return;
    auto* cctx = reinterpret_cast<rdpClientContext*>(ctx_);
    freerdp_client_send_wheel_event(cctx, flags);
}

void RdpSession::send_ctrl_alt_del() {
    if (!is_connected() || !ctx_ || !ctx_->input) return;
    freerdp_input_send_keyboard_event_ex(ctx_->input, TRUE, FALSE, RDP_SCANCODE_LCONTROL);
    freerdp_input_send_keyboard_event_ex(ctx_->input, TRUE, FALSE, RDP_SCANCODE_LMENU);
    freerdp_input_send_keyboard_event_ex(ctx_->input, TRUE, FALSE, RDP_SCANCODE_DELETE);
    freerdp_input_send_keyboard_event_ex(ctx_->input, FALSE, FALSE, RDP_SCANCODE_DELETE);
    freerdp_input_send_keyboard_event_ex(ctx_->input, FALSE, FALSE, RDP_SCANCODE_LMENU);
    freerdp_input_send_keyboard_event_ex(ctx_->input, FALSE, FALSE, RDP_SCANCODE_LCONTROL);
}

void RdpSession::request_resize(uint32_t width, uint32_t height, uint32_t scale_percent) {
    if (!is_connected() || !profile_.dynamic_resolution) return;
    uint32_t w = std::clamp<uint32_t>(width & ~1U, 320, 8192);
    uint32_t h = std::clamp<uint32_t>(height & ~1U, 200, 8192);

    MonitorInfo m;
    m.x = 0;
    m.y = 0;
    m.width = static_cast<int32_t>(w);
    m.height = static_cast<int32_t>(h);
    m.is_primary = true;
    request_monitor_layout({m}, scale_percent);
}

void RdpSession::request_monitor_layout(const std::vector<MonitorInfo>& monitors, uint32_t scale_percent) {
    if (!is_connected() || monitors.empty()) return;
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (!disp_ || !disp_->SendMonitorLayout) return;

    auto rdp_mons = build_freerdp_monitors(monitors, scale_percent);
    std::vector<DISPLAY_CONTROL_MONITOR_LAYOUT> layouts;
    layouts.reserve(rdp_mons.size());
    for (const auto& rm : rdp_mons) {
        DISPLAY_CONTROL_MONITOR_LAYOUT l{};
        l.Flags = rm.is_primary ? DISPLAY_CONTROL_MONITOR_PRIMARY : 0;
        l.Left = rm.x;
        l.Top = rm.y;
        l.Width = static_cast<UINT32>(std::clamp(rm.width & ~1, 200, 8192));
        l.Height = static_cast<UINT32>(std::clamp(rm.height & ~1, 200, 8192));
        l.PhysicalWidth = rm.attributes.physicalWidth;
        l.PhysicalHeight = rm.attributes.physicalHeight;
        l.Orientation = ORIENTATION_LANDSCAPE;
        l.DesktopScaleFactor = rm.attributes.desktopScaleFactor;
        l.DeviceScaleFactor = rm.attributes.deviceScaleFactor;
        layouts.push_back(l);
    }

    if (layouts.size() == last_disp_layout_.size() &&
        std::memcmp(layouts.data(), last_disp_layout_.data(),
                    layouts.size() * sizeof(DISPLAY_CONTROL_MONITOR_LAYOUT)) == 0) {
        return;
    }

    if (disp_->SendMonitorLayout(disp_, static_cast<UINT32>(layouts.size()), layouts.data()) == CHANNEL_RC_OK) {
        last_disp_layout_ = std::move(layouts);
    }
}

void RdpSession::set_suppress_output(bool suppress) {
    if (!is_connected() || !ctx_ || !ctx_->gdi) return;
    std::ignore = gdi_send_suppress_output(ctx_->gdi, suppress ? TRUE : FALSE);
}

} // namespace winrdp
