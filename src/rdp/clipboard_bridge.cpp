#include "rdp/clipboard_bridge.hpp"

#include <freerdp/channels/cliprdr.h>
#include <freerdp/client/client_cliprdr_file.h>
#include <freerdp/utils/cliprdr_utils.h>
#include <winpr/clipboard.h>
#include <winpr/image.h>
#include <winpr/synch.h>
#include <winpr/user.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace winrdp {

namespace {

constexpr const char* kMimeTextUtf8 = "text/plain;charset=utf-8";
constexpr const char* kMimeTextPlain = "text/plain";
constexpr const char* kMimeHtml = "text/html";
constexpr const char* kMimePng = "image/png";
constexpr const char* kMimeJpeg = "image/jpeg";
constexpr const char* kMimeWebp = "image/webp";
constexpr const char* kMimeBmp = "image/bmp";
constexpr const char* kMimeUriList = "text/uri-list";
constexpr const char* kMimeGnomeFiles = "x-special/gnome-copied-files";

constexpr const char* kTypeHtmlFormat = "HTML Format";
constexpr const char* kTypeFileGroupW = "FileGroupDescriptorW";

const std::vector<const char*>& text_mimes() {
    static const std::vector<const char*> v = {
        kMimeTextUtf8, "UTF8_STRING", kMimeTextPlain, "STRING", "TEXT"
    };
    return v;
}

const std::vector<const char*>& bmp_mimes() {
    static const std::vector<const char*> v = {
        kMimeBmp, "image/x-bmp", "image/x-MS-bmp", "image/x-win-bitmap"
    };
    return v;
}

bool is_in_list(const char* s, const std::vector<const char*>& list) {
    if (!s) return false;
    for (const char* item : list) {
        if (std::strcmp(s, item) == 0) return true;
    }
    return false;
}

bool mime_is_text(const std::string& m) {
    return is_in_list(m.c_str(), text_mimes());
}

bool mime_is_bmp(const std::string& m) {
    return is_in_list(m.c_str(), bmp_mimes());
}

bool mime_is_image(const std::string& m) {
    return m == kMimePng || m == kMimeJpeg || m == kMimeWebp || mime_is_bmp(m);
}

bool mime_is_file(const std::string& m) {
    return m == kMimeUriList || m == kMimeGnomeFiles;
}

uint32_t text_length_until_nul(const void* data, uint32_t size) {
    if (!data || size == 0) return 0;
    const void* nul = std::memchr(data, '\0', size);
    if (!nul) return size;
    return static_cast<uint32_t>(static_cast<const char*>(nul) - static_cast<const char*>(data));
}

struct ServerFormat {
    uint32_t id = 0;
    std::string name;
};

struct PendingClientRequest {
    uint32_t format_id = 0;
    std::string mime;
    bool success = false;
};

struct CacheBlob {
    std::vector<uint8_t> bytes;
};

} // namespace

struct ClipboardBridge::Impl {
    std::weak_ptr<Impl> self_weak;
    std::mutex mutex;
    bool destroyed = false;
    bool enable_files = true;
    bool sync_ready = false;
    bool reading_server = false;

    rdpContext* rdp_ctx = nullptr;
    CliprdrClientContext* ctx = nullptr;
    CliprdrFileContext* file = nullptr;
    wClipboard* system_clip = nullptr;
    HANDLE data_event = nullptr;

    GdkClipboard* gtk_clipboard = nullptr;
    gulong gtk_changed_id = 0;

    std::vector<ServerFormat> server_formats;
    std::vector<std::string> current_mimes;
    std::map<std::string, CacheBlob> cache;
    std::deque<PendingClientRequest> client_requests;
    std::deque<uint32_t> server_requests;

    Impl()
        : file(cliprdr_file_context_new(this)),
          system_clip(ClipboardCreate()),
          data_event(CreateEventA(nullptr, TRUE, FALSE, nullptr)) {}

    ~Impl() {
        if (file) {
            cliprdr_file_context_free(file);
            file = nullptr;
        }
        if (system_clip) {
            ClipboardDestroy(system_clip);
            system_clip = nullptr;
        }
        if (data_event) {
            CloseHandle(data_event);
            data_event = nullptr;
        }
    }

    std::string server_format_name(uint32_t id) const {
        for (const auto& f : server_formats) {
            if (f.id == id) return f.name;
        }
        return {};
    }

    uint32_t pick_server_id_for_mime(const std::string& mime) const {
        if (mime == kMimeHtml) {
            for (const auto& f : server_formats) {
                if (f.name == kTypeHtmlFormat) return f.id;
            }
        }
        if (mime_is_file(mime)) {
            for (const auto& f : server_formats) {
                if (f.name == kTypeFileGroupW) return f.id;
            }
        }
        for (const auto& f : server_formats) {
            if (!f.name.empty() && f.name == mime) return f.id;
        }
        if (mime_is_image(mime)) {
            uint32_t named_img = 0;
            for (const auto& f : server_formats) {
                if (f.id == CF_DIB || f.id == CF_DIBV5) return f.id;
                if (!named_img && !f.name.empty() && mime_is_image(f.name)) {
                    named_img = f.id;
                }
            }
            return named_img ? named_img : CF_DIB;
        }
        if (mime_is_text(mime)) {
            for (const auto& f : server_formats) {
                if (f.id == CF_UNICODETEXT) return CF_UNICODETEXT;
            }
            for (const auto& f : server_formats) {
                if (f.id == CF_TEXT) return CF_TEXT;
            }
            return CF_UNICODETEXT;
        }
        return 0;
    }

    void clear_server_state_locked() {
        server_formats.clear();
        cache.clear();
        if (system_clip) {
            ClipboardLock(system_clip);
            ClipboardEmpty(system_clip);
            ClipboardUnlock(system_clip);
        }
        if (file) {
            cliprdr_file_context_clear(file);
        }
    }

    UINT send_client_capabilities() {
        if (!ctx || !ctx->ClientCapabilities) return ERROR_INTERNAL_ERROR;
        const UINT32 file_flags = file ? cliprdr_file_context_current_flags(file) : 0;
        CLIPRDR_GENERAL_CAPABILITY_SET general = {
            CB_CAPSTYPE_GENERAL, 12, CB_CAPS_VERSION_2,
            CB_USE_LONG_FORMAT_NAMES | file_flags
        };
        CLIPRDR_CAPABILITIES caps = {
            {CB_TYPE_NONE, 0, 0},
            1,
            reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general)
        };
        return ctx->ClientCapabilities(ctx, &caps);
    }

    UINT send_format_list_response(BOOL ok) {
        if (!ctx || !ctx->ClientFormatListResponse) return CHANNEL_RC_OK;
        CLIPRDR_FORMAT_LIST_RESPONSE resp = {
            {CB_FORMAT_LIST_RESPONSE, static_cast<UINT16>(ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL), 0}
        };
        return ctx->ClientFormatListResponse(ctx, &resp);
    }

    UINT send_data_response(const BYTE* data, size_t size) {
        if (!ctx || !ctx->ClientFormatDataResponse) return CHANNEL_RC_OK;
        if (size > UINT32_MAX) return ERROR_INVALID_PARAMETER;
        CLIPRDR_FORMAT_DATA_RESPONSE resp{};
        resp.common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
        resp.common.dataLen = static_cast<UINT32>(size);
        resp.requestedFormatData = data;
        return ctx->ClientFormatDataResponse(ctx, &resp);
    }

    bool fetch_remote_mime(const std::string& target_mime, std::vector<uint8_t>& out_bytes);
    void announce_local_clipboard_to_server();
};

// ---------------- Custom GdkContentProvider subclass ----------------

struct WinRdpContentProvider {
    GdkContentProvider parent_instance;
    std::weak_ptr<ClipboardBridge::Impl>* impl_weak;
    std::vector<std::string>* mimes;
};

struct WinRdpContentProviderClass {
    GdkContentProviderClass parent_class;
};

G_DEFINE_TYPE(WinRdpContentProvider, winrdp_content_provider, GDK_TYPE_CONTENT_PROVIDER)

static void winrdp_content_provider_finalize(GObject* object) {
    auto* self = reinterpret_cast<WinRdpContentProvider*>(object);
    delete self->impl_weak;
    self->impl_weak = nullptr;
    delete self->mimes;
    self->mimes = nullptr;
    G_OBJECT_CLASS(winrdp_content_provider_parent_class)->finalize(object);
}

static GdkContentFormats* winrdp_content_provider_ref_formats(GdkContentProvider* provider) {
    auto* self = reinterpret_cast<WinRdpContentProvider*>(provider);
    if (!self->mimes || self->mimes->empty()) {
        return gdk_content_formats_new(nullptr, 0);
    }
    std::vector<const char*> ptrs;
    ptrs.reserve(self->mimes->size());
    for (const auto& m : *self->mimes) {
        ptrs.push_back(m.c_str());
    }
    return gdk_content_formats_new(ptrs.data(), ptrs.size());
}

struct WriteMimeTaskData {
    std::weak_ptr<ClipboardBridge::Impl> impl_weak;
    std::string mime;
    GOutputStream* stream = nullptr;

    ~WriteMimeTaskData() {
        if (stream) g_object_unref(stream);
    }
};

static void write_mime_task_thread(GTask* task,
                                   gpointer /*source_object*/,
                                   gpointer task_data,
                                   GCancellable* cancellable) {
    auto* td = static_cast<WriteMimeTaskData*>(task_data);
    auto impl = td->impl_weak.lock();
    if (!impl) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED, "RDP clipboard closed");
        return;
    }

    std::vector<uint8_t> bytes;
    if (!impl->fetch_remote_mime(td->mime, bytes)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to fetch clipboard format %s", td->mime.c_str());
        return;
    }

    if (!bytes.empty()) {
        gsize written = 0;
        GError* err = nullptr;
        if (!g_output_stream_write_all(td->stream, bytes.data(), bytes.size(),
                                       &written, cancellable, &err)) {
            g_task_return_error(task, err);
            return;
        }
    }
    g_task_return_boolean(task, TRUE);
}

static void winrdp_content_provider_write_mime_type_async(GdkContentProvider* provider,
                                                          const char* mime_type,
                                                          GOutputStream* stream,
                                                          int io_priority,
                                                          GCancellable* cancellable,
                                                          GAsyncReadyCallback callback,
                                                          gpointer user_data) {
    auto* self = reinterpret_cast<WinRdpContentProvider*>(provider);
    GTask* task = g_task_new(provider, cancellable, callback, user_data);
    g_task_set_priority(task, io_priority);

    auto* td = new WriteMimeTaskData{
        self->impl_weak ? *self->impl_weak : std::weak_ptr<ClipboardBridge::Impl>{},
        mime_type ? mime_type : "",
        stream ? G_OUTPUT_STREAM(g_object_ref(stream)) : nullptr
    };
    g_task_set_task_data(task, td, [](gpointer p) { delete static_cast<WriteMimeTaskData*>(p); });
    g_task_run_in_thread(task, write_mime_task_thread);
    g_object_unref(task);
}

static gboolean winrdp_content_provider_write_mime_type_finish(GdkContentProvider* /*provider*/,
                                                               GAsyncResult* result,
                                                               GError** error) {
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void winrdp_content_provider_init(WinRdpContentProvider* self) {
    self->impl_weak = nullptr;
    self->mimes = nullptr;
}

static void winrdp_content_provider_class_init(WinRdpContentProviderClass* klass) {
    GObjectClass* obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = winrdp_content_provider_finalize;

    GdkContentProviderClass* cp_class = GDK_CONTENT_PROVIDER_CLASS(klass);
    cp_class->ref_formats = winrdp_content_provider_ref_formats;
    cp_class->ref_storable_formats = winrdp_content_provider_ref_formats;
    cp_class->write_mime_type_async = winrdp_content_provider_write_mime_type_async;
    cp_class->write_mime_type_finish = winrdp_content_provider_write_mime_type_finish;
}

static GdkContentProvider* create_winrdp_provider(const std::weak_ptr<ClipboardBridge::Impl>& impl_weak,
                                                  std::vector<std::string> mimes) {
    auto* prov = static_cast<WinRdpContentProvider*>(
        g_object_new(winrdp_content_provider_get_type(), nullptr));
    prov->impl_weak = new std::weak_ptr<ClipboardBridge::Impl>(impl_weak);
    prov->mimes = new std::vector<std::string>(std::move(mimes));
    return GDK_CONTENT_PROVIDER(prov);
}

static bool is_winrdp_provider(GdkContentProvider* cp) {
    return cp && G_TYPE_CHECK_INSTANCE_TYPE(cp, winrdp_content_provider_get_type());
}

// ---------------- Impl Methods ----------------

bool ClipboardBridge::Impl::fetch_remote_mime(const std::string& raw_mime,
                                              std::vector<uint8_t>& out_bytes) {
    out_bytes.clear();
    std::string effective_mime = raw_mime;
    if (mime_is_text(raw_mime)) {
        effective_mime = (raw_mime == kMimeTextUtf8 || raw_mime == "UTF8_STRING")
                             ? kMimeTextUtf8
                             : kMimeTextPlain;
    }

    HANDLE abort_hdl = nullptr;
    uint32_t format_id = 0;

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (destroyed || !ctx || !system_clip) return false;

        auto it = cache.find(effective_mime);
        if (it != cache.end()) {
            out_bytes = it->second.bytes;
            return true;
        }

        ClipboardLock(system_clip);
        const UINT32 mime_fmt_id = ClipboardRegisterFormat(system_clip, effective_mime.c_str());
        uint32_t synth_len = 0;
        void* synth_ptr = ClipboardGetData(system_clip, mime_fmt_id, &synth_len);
        ClipboardUnlock(system_clip);
        if (synth_ptr) {
            if (mime_is_text(effective_mime)) {
                synth_len = text_length_until_nul(synth_ptr, synth_len);
            }
            const auto* b = static_cast<const uint8_t*>(synth_ptr);
            out_bytes.assign(b, b + synth_len);
            free(synth_ptr);
            cache[effective_mime] = CacheBlob{out_bytes};
            return true;
        }

        format_id = pick_server_id_for_mime(effective_mime);
        if (format_id == 0) return false;

        reading_server = true;
        while (!server_requests.empty()) {
            server_requests.pop_front();
            send_data_response(nullptr, 0);
        }

        ResetEvent(data_event);
        client_requests.push_back(PendingClientRequest{format_id, effective_mime, false});

        CLIPRDR_FORMAT_DATA_REQUEST req = {{CB_TYPE_NONE, 0, 0}, format_id};
        if (!ctx->ClientFormatDataRequest || ctx->ClientFormatDataRequest(ctx, &req) != CHANNEL_RC_OK) {
            client_requests.pop_back();
            reading_server = false;
            return false;
        }
        abort_hdl = rdp_ctx ? freerdp_abort_event(rdp_ctx) : nullptr;
    }

    HANDLE wait_hdls[2] = {data_event, abort_hdl};
    const DWORD n_hdls = abort_hdl ? 2 : 1;
    const DWORD wait_rc = WaitForMultipleObjects(n_hdls, wait_hdls, FALSE, 5000);

    std::lock_guard<std::mutex> lock(mutex);
    reading_server = false;
    if (wait_rc != WAIT_OBJECT_0 || client_requests.empty()) {
        if (!client_requests.empty()) {
            client_requests.pop_front();
        }
        return false;
    }

    auto req = std::move(client_requests.front());
    client_requests.pop_front();
    if (client_requests.empty()) {
        ResetEvent(data_event);
    }

    if (!req.success || !system_clip) {
        return false;
    }

    ClipboardLock(system_clip);
    const UINT32 mime_fmt_id = ClipboardRegisterFormat(system_clip, effective_mime.c_str());
    uint32_t out_len = 0;
    void* ptr = ClipboardGetData(system_clip, mime_fmt_id, &out_len);
    ClipboardUnlock(system_clip);

    if (!ptr) {
        return false;
    }

    if (mime_is_text(effective_mime)) {
        out_len = text_length_until_nul(ptr, out_len);
    }
    const auto* b = static_cast<const uint8_t*>(ptr);
    out_bytes.assign(b, b + out_len);
    free(ptr);
    cache[effective_mime] = CacheBlob{out_bytes};
    return true;
}

void ClipboardBridge::Impl::announce_local_clipboard_to_server() {
    GdkClipboard* clip = nullptr;
    bool files_allowed = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (destroyed || !ctx || !sync_ready) return;
        clip = gtk_clipboard;
        files_allowed = enable_files;
    }
    if (!clip || !GDK_IS_CLIPBOARD(clip)) return;
    if (gdk_clipboard_is_local(clip) && is_winrdp_provider(gdk_clipboard_get_content(clip))) {
        return;
    }

    GdkContentFormats* formats = gdk_clipboard_get_formats(clip);
    gsize n_mimes = 0;
    const char* const* mimes = formats ? gdk_content_formats_get_mime_types(formats, &n_mimes) : nullptr;

    std::vector<std::string> custom_names;
    std::vector<CLIPRDR_FORMAT> rdp_formats;
    bool pushed_text = false;
    bool pushed_img = false;
    bool pushed_file = false;

    for (gsize i = 0; mimes && i < n_mimes; ++i) {
        const std::string m = mimes[i] ? mimes[i] : "";
        if (m.empty()) continue;
        if (mime_is_text(m)) {
            if (!pushed_text) {
                rdp_formats.push_back({CF_UNICODETEXT, nullptr});
                rdp_formats.push_back({CF_TEXT, nullptr});
                rdp_formats.push_back({CF_OEMTEXT, nullptr});
                pushed_text = true;
            }
        } else if (m == kMimeHtml) {
            custom_names.emplace_back(kTypeHtmlFormat);
        } else if (mime_is_image(m)) {
            if (!pushed_img) {
                rdp_formats.push_back({CF_DIB, nullptr});
                if (winpr_image_format_is_supported(WINPR_IMAGE_PNG)) custom_names.emplace_back(kMimePng);
                if (winpr_image_format_is_supported(WINPR_IMAGE_JPEG)) custom_names.emplace_back(kMimeJpeg);
                if (winpr_image_format_is_supported(WINPR_IMAGE_WEBP)) custom_names.emplace_back(kMimeWebp);
                for (const char* bm : bmp_mimes()) custom_names.emplace_back(bm);
                pushed_img = true;
            }
        } else if (files_allowed && mime_is_file(m)) {
            if (!pushed_file) {
                custom_names.emplace_back(kTypeFileGroupW);
                pushed_file = true;
            }
        }
    }

    std::sort(custom_names.begin(), custom_names.end());
    custom_names.erase(std::unique(custom_names.begin(), custom_names.end()), custom_names.end());

    std::lock_guard<std::mutex> lock(mutex);
    if (destroyed || !ctx || !sync_ready || !system_clip) return;

    clear_server_state_locked();

    ClipboardLock(system_clip);
    for (auto& name : custom_names) {
        const UINT32 id = ClipboardRegisterFormat(system_clip, name.c_str());
        rdp_formats.push_back({id, name.data()});
    }
    ClipboardUnlock(system_clip);

    if (file) {
        std::ignore = cliprdr_file_context_notify_new_client_format_list(file);
    }

    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = static_cast<UINT32>(rdp_formats.size());
    list.formats = rdp_formats.empty() ? nullptr : rdp_formats.data();

    if (ctx->ClientFormatList) {
        ctx->ClientFormatList(ctx, &list);
    }
}

namespace {

struct ServerReadCtx {
    std::shared_ptr<ClipboardBridge::Impl> impl;
    uint32_t fmt_id = 0;
    std::string chosen_mime;
    bool is_file_fmt = false;
    GOutputStream* mem_out = nullptr;

    ~ServerReadCtx() {
        if (mem_out) g_object_unref(mem_out);
    }
};

static void on_splice_done(GObject* source, GAsyncResult* res, gpointer user_data) {
    std::unique_ptr<ServerReadCtx> rctx(static_cast<ServerReadCtx*>(user_data));
    GError* err = nullptr;
    gssize spliced = g_output_stream_splice_finish(G_OUTPUT_STREAM(source), res, &err);
    if (err) g_error_free(err);

    if (spliced < 0) {
        std::lock_guard<std::mutex> lock(rctx->impl->mutex);
        if (!rctx->impl->destroyed && rctx->impl->ctx) {
            rctx->impl->send_data_response(nullptr, 0);
        }
        return;
    }

    GBytes* gbytes = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(rctx->mem_out));
    gsize raw_len = 0;
    const void* raw_ptr = g_bytes_get_data(gbytes, &raw_len);

    std::lock_guard<std::mutex> lock(rctx->impl->mutex);
    if (rctx->impl->destroyed || !rctx->impl->ctx || !rctx->impl->system_clip || raw_len > UINT32_MAX) {
        g_bytes_unref(gbytes);
        return;
    }

    bool sent = false;
    if (rctx->is_file_fmt && rctx->impl->file) {
        if (cliprdr_file_context_update_client_data(
                rctx->impl->file, static_cast<const char*>(raw_ptr), raw_len)) {
            ClipboardLock(rctx->impl->system_clip);
            const UINT32 store_id = ClipboardRegisterFormat(rctx->impl->system_clip, rctx->chosen_mime.c_str());
            if (ClipboardSetData(rctx->impl->system_clip, store_id, raw_ptr, static_cast<uint32_t>(raw_len))) {
                uint32_t flen = 0;
                void* fptr = ClipboardGetData(rctx->impl->system_clip, rctx->fmt_id, &flen);
                if (fptr) {
                    BYTE* serialized = nullptr;
                    UINT32 slen = 0;
                    const UINT32 flags = cliprdr_file_context_remote_get_flags(rctx->impl->file);
                    const UINT32 rc = cliprdr_serialize_file_list_ex(
                        flags, static_cast<const FILEDESCRIPTORW*>(fptr),
                        flen / sizeof(FILEDESCRIPTORW), &serialized, &slen);
                    free(fptr);
                    if (rc == 0 && serialized) {
                        rctx->impl->send_data_response(serialized, slen);
                        free(serialized);
                        sent = true;
                    }
                }
            }
            ClipboardEmpty(rctx->impl->system_clip);
            ClipboardUnlock(rctx->impl->system_clip);
        }
    } else {
        ClipboardLock(rctx->impl->system_clip);
        const UINT32 store_id = ClipboardRegisterFormat(rctx->impl->system_clip, rctx->chosen_mime.c_str());
        if (ClipboardSetData(rctx->impl->system_clip, store_id, raw_ptr, static_cast<uint32_t>(raw_len))) {
            uint32_t out_len = 0;
            void* out_ptr = ClipboardGetData(rctx->impl->system_clip, rctx->fmt_id, &out_len);
            if (out_ptr) {
                rctx->impl->send_data_response(static_cast<const BYTE*>(out_ptr), out_len);
                free(out_ptr);
                sent = true;
            }
        }
        ClipboardEmpty(rctx->impl->system_clip);
        ClipboardUnlock(rctx->impl->system_clip);
    }

    g_bytes_unref(gbytes);
    if (!sent) {
        rctx->impl->send_data_response(nullptr, 0);
    }
}

static void on_clipboard_read_done(GObject* source, GAsyncResult* res, gpointer user_data) {
    std::unique_ptr<ServerReadCtx> rctx(static_cast<ServerReadCtx*>(user_data));
    GError* err = nullptr;
    const char* actual_mime = nullptr;
    GInputStream* in = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), res, &actual_mime, &err);
    if (err) g_error_free(err);
    if (!in) {
        std::lock_guard<std::mutex> lock(rctx->impl->mutex);
        if (!rctx->impl->destroyed && rctx->impl->ctx) {
            rctx->impl->send_data_response(nullptr, 0);
        }
        return;
    }

    if (actual_mime) rctx->chosen_mime = actual_mime;
    rctx->mem_out = g_memory_output_stream_new_resizable();
    GOutputStream* out_ref = rctx->mem_out;
    g_output_stream_splice_async(
        out_ref,
        in,
        static_cast<GOutputStreamSpliceFlags>(G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
        G_PRIORITY_DEFAULT,
        nullptr,
        on_splice_done,
        rctx.release());
    g_object_unref(in);
}

static void kick_server_request(const std::shared_ptr<ClipboardBridge::Impl>& impl) {
    uint32_t fmt_id = 0;
    GdkClipboard* clip = nullptr;
    bool files_allowed = false;

    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->destroyed || !impl->ctx || impl->server_requests.empty()) return;
        fmt_id = impl->server_requests.front();
        impl->server_requests.pop_front();
        clip = impl->gtk_clipboard;
        files_allowed = impl->enable_files;
    }

    if (!clip || !GDK_IS_CLIPBOARD(clip) ||
        (gdk_clipboard_is_local(clip) && is_winrdp_provider(gdk_clipboard_get_content(clip)))) {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->destroyed && impl->ctx) impl->send_data_response(nullptr, 0);
        return;
    }

    GdkContentFormats* formats = gdk_clipboard_get_formats(clip);
    gsize n_mimes = 0;
    const char* const* offered = formats ? gdk_content_formats_get_mime_types(formats, &n_mimes) : nullptr;
    auto has_offered = [&](const char* m) {
        for (gsize i = 0; offered && i < n_mimes; ++i) {
            if (offered[i] && std::strcmp(offered[i], m) == 0) return true;
        }
        return false;
    };

    UINT32 file_fmt_id = 0;
    UINT32 html_fmt_id = 0;
    std::string fmt_name;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->system_clip) {
            ClipboardLock(impl->system_clip);
            file_fmt_id = ClipboardGetFormatId(impl->system_clip, kTypeFileGroupW);
            html_fmt_id = ClipboardGetFormatId(impl->system_clip, kTypeHtmlFormat);
            const char* n = ClipboardGetFormatName(impl->system_clip, fmt_id);
            if (n) fmt_name = n;
            ClipboardUnlock(impl->system_clip);
        }
    }

    std::vector<const char*> candidates;
    if (fmt_id == CF_UNICODETEXT || fmt_id == CF_TEXT || fmt_id == CF_OEMTEXT) {
        for (const char* tm : text_mimes()) {
            if (has_offered(tm)) candidates.push_back(tm);
        }
    } else if (fmt_id == CF_DIB || fmt_id == CF_DIBV5) {
        for (const char* bm : bmp_mimes()) {
            if (has_offered(bm)) candidates.push_back(bm);
        }
        if (has_offered(kMimePng)) candidates.push_back(kMimePng);
        if (has_offered(kMimeJpeg)) candidates.push_back(kMimeJpeg);
        if (has_offered(kMimeWebp)) candidates.push_back(kMimeWebp);
    } else if (fmt_id == file_fmt_id && file_fmt_id != 0 && files_allowed) {
        if (has_offered(kMimeUriList)) candidates.push_back(kMimeUriList);
        else if (has_offered(kMimeGnomeFiles)) candidates.push_back(kMimeGnomeFiles);
    } else if (fmt_id == html_fmt_id && html_fmt_id != 0) {
        if (has_offered(kMimeHtml)) candidates.push_back(kMimeHtml);
    } else if (!fmt_name.empty() && has_offered(fmt_name.c_str())) {
        candidates.push_back(fmt_name.c_str());
    }

    if (candidates.empty()) {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (!impl->destroyed && impl->ctx) impl->send_data_response(nullptr, 0);
        return;
    }

    std::vector<const char*> null_term = candidates;
    null_term.push_back(nullptr);

    auto* rctx = new ServerReadCtx();
    rctx->impl = impl;
    rctx->fmt_id = fmt_id;
    rctx->chosen_mime = candidates.front();
    rctx->is_file_fmt = (fmt_id == file_fmt_id && file_fmt_id != 0);

    gdk_clipboard_read_async(clip, null_term.data(), G_PRIORITY_DEFAULT, nullptr,
                             on_clipboard_read_done, rctx);
}

static ClipboardBridge::Impl* impl_from_context(CliprdrClientContext* context) {
    if (!context || !context->custom) return nullptr;
    return static_cast<ClipboardBridge::Impl*>(
        cliprdr_file_context_get_context(static_cast<CliprdrFileContext*>(context->custom)));
}

static UINT cb_monitor_ready(CliprdrClientContext* context, const CLIPRDR_MONITOR_READY* /*mr*/) {
    auto* impl = impl_from_context(context);
    if (!impl) return ERROR_INTERNAL_ERROR;

    std::weak_ptr<ClipboardBridge::Impl> weak;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        const UINT rc = impl->send_client_capabilities();
        if (rc != CHANNEL_RC_OK) return rc;
        impl->sync_ready = true;
        weak = impl->self_weak;
    }

    auto* heap_weak = new std::weak_ptr<ClipboardBridge::Impl>(std::move(weak));
    g_idle_add(
        [](gpointer data) -> gboolean {
            std::unique_ptr<std::weak_ptr<ClipboardBridge::Impl>> w(
                static_cast<std::weak_ptr<ClipboardBridge::Impl>*>(data));
            if (auto locked = w->lock()) {
                locked->announce_local_clipboard_to_server();
            }
            return G_SOURCE_REMOVE;
        },
        heap_weak);
    return CHANNEL_RC_OK;
}

static UINT cb_server_capabilities(CliprdrClientContext* context,
                                   const CLIPRDR_CAPABILITIES* capabilities) {
    auto* impl = impl_from_context(context);
    if (!impl || !capabilities) return ERROR_INVALID_PARAMETER;

    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->file) {
        std::ignore = cliprdr_file_context_remote_set_flags(impl->file, 0);
        const BYTE* ptr = reinterpret_cast<const BYTE*>(capabilities->capabilitySets);
        for (UINT32 i = 0; i < capabilities->cCapabilitiesSets; ++i) {
            const auto* cap = reinterpret_cast<const CLIPRDR_CAPABILITY_SET*>(ptr);
            if (cap->capabilitySetType == CB_CAPSTYPE_GENERAL) {
                const auto* gen = reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET*>(cap);
                std::ignore = cliprdr_file_context_remote_set_flags(impl->file, gen->generalFlags);
            }
            ptr += cap->capabilitySetLength;
        }
    }
    return CHANNEL_RC_OK;
}

static UINT cb_server_format_list(CliprdrClientContext* context,
                                  const CLIPRDR_FORMAT_LIST* format_list) {
    auto* impl = impl_from_context(context);
    if (!impl || !format_list) return ERROR_INVALID_PARAMETER;

    std::weak_ptr<ClipboardBridge::Impl> weak;
    std::vector<std::string> offered_mimes;
    UINT ret = CHANNEL_RC_OK;

    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->clear_server_state_locked();
        weak = impl->self_weak;

        bool has_text = false;
        bool has_image = false;
        bool has_html = false;
        bool has_file = false;

        for (UINT32 i = 0; i < format_list->numFormats; ++i) {
            const CLIPRDR_FORMAT& f = format_list->formats[i];
            impl->server_formats.push_back(ServerFormat{f.formatId, f.formatName ? f.formatName : ""});
            if (f.formatName) {
                if (std::strcmp(f.formatName, kTypeHtmlFormat) == 0) {
                    has_html = true;
                    has_text = true;
                } else if (std::strcmp(f.formatName, kTypeFileGroupW) == 0) {
                    has_file = true;
                } else if (mime_is_image(f.formatName)) {
                    has_image = true;
                }
            } else {
                if (f.formatId == CF_TEXT || f.formatId == CF_OEMTEXT || f.formatId == CF_UNICODETEXT) {
                    has_text = true;
                } else if (f.formatId == CF_DIB || f.formatId == CF_DIBV5) {
                    has_image = true;
                }
            }
        }

        if (impl->file) {
            const UINT frc = cliprdr_file_context_notify_new_server_format_list(impl->file);
            if (frc != CHANNEL_RC_OK) return frc;
        }

        if (has_text) {
            for (const char* m : text_mimes()) offered_mimes.emplace_back(m);
        }
        if (has_image) {
            offered_mimes.emplace_back(kMimePng);
            for (const char* m : bmp_mimes()) offered_mimes.emplace_back(m);
        }
        if (has_html) {
            offered_mimes.emplace_back(kMimeHtml);
        }
        if (has_file && impl->enable_files) {
            offered_mimes.emplace_back(kMimeUriList);
            offered_mimes.emplace_back(kMimeGnomeFiles);
        }

        impl->current_mimes = offered_mimes;
        ret = impl->send_format_list_response(TRUE);
    }

    if (!offered_mimes.empty()) {
        struct SetOfferArg {
            std::weak_ptr<ClipboardBridge::Impl> weak;
            std::vector<std::string> mimes;
        };
        auto* arg = new SetOfferArg{std::move(weak), std::move(offered_mimes)};
        g_idle_add(
            [](gpointer data) -> gboolean {
                std::unique_ptr<SetOfferArg> a(static_cast<SetOfferArg*>(data));
                auto locked = a->weak.lock();
                if (!locked) return G_SOURCE_REMOVE;
                GdkClipboard* clip = nullptr;
                {
                    std::lock_guard<std::mutex> lock(locked->mutex);
                    if (locked->destroyed) return G_SOURCE_REMOVE;
                    clip = locked->gtk_clipboard;
                }
                if (clip && GDK_IS_CLIPBOARD(clip)) {
                    GdkContentProvider* prov = create_winrdp_provider(a->weak, std::move(a->mimes));
                    gdk_clipboard_set_content(clip, prov);
                    g_object_unref(prov);
                }
                return G_SOURCE_REMOVE;
            },
            arg);
    }

    return ret;
}

static UINT cb_format_list_response(CliprdrClientContext* /*context*/,
                                    const CLIPRDR_FORMAT_LIST_RESPONSE* /*resp*/) {
    return CHANNEL_RC_OK;
}

static UINT cb_server_format_data_request(CliprdrClientContext* context,
                                          const CLIPRDR_FORMAT_DATA_REQUEST* request) {
    auto* impl = impl_from_context(context);
    if (!impl || !request) return ERROR_INVALID_PARAMETER;

    std::weak_ptr<ClipboardBridge::Impl> weak;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->reading_server) {
            return impl->send_data_response(nullptr, 0);
        }
        const UINT32 mask = impl->rdp_ctx && impl->rdp_ctx->settings
                                ? freerdp_settings_get_uint32(impl->rdp_ctx->settings,
                                                              FreeRDP_ClipboardFeatureMask)
                                : CLIPRDR_FLAG_LOCAL_TO_REMOTE;
        UINT32 file_fmt_id = 0;
        if (impl->system_clip) {
            ClipboardLock(impl->system_clip);
            file_fmt_id = ClipboardGetFormatId(impl->system_clip, kTypeFileGroupW);
            ClipboardUnlock(impl->system_clip);
        }
        if (request->requestedFormatId == file_fmt_id && file_fmt_id != 0) {
            if ((mask & CLIPRDR_FLAG_LOCAL_TO_REMOTE_FILES) == 0 || !impl->enable_files) {
                return impl->send_data_response(nullptr, 0);
            }
        } else if ((mask & CLIPRDR_FLAG_LOCAL_TO_REMOTE) == 0) {
            return impl->send_data_response(nullptr, 0);
        }
        impl->server_requests.push_back(request->requestedFormatId);
        weak = impl->self_weak;
    }

    auto* heap_weak = new std::weak_ptr<ClipboardBridge::Impl>(std::move(weak));
    g_idle_add(
        [](gpointer data) -> gboolean {
            std::unique_ptr<std::weak_ptr<ClipboardBridge::Impl>> w(
                static_cast<std::weak_ptr<ClipboardBridge::Impl>*>(data));
            if (auto locked = w->lock()) {
                kick_server_request(locked);
            }
            return G_SOURCE_REMOVE;
        },
        heap_weak);
    return CHANNEL_RC_OK;
}

static UINT cb_server_format_data_response(CliprdrClientContext* context,
                                           const CLIPRDR_FORMAT_DATA_RESPONSE* response) {
    auto* impl = impl_from_context(context);
    if (!impl || !response) return ERROR_INVALID_PARAMETER;

    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->client_requests.empty()) {
        return CHANNEL_RC_OK;
    }

    auto& req = impl->client_requests.front();
    const bool ok = (response->common.msgFlags & CB_RESPONSE_OK) != 0 &&
                    (response->common.msgFlags & CB_RESPONSE_FAIL) == 0 &&
                    response->requestedFormatData != nullptr;
    req.success = ok;

    if (ok && impl->system_clip) {
        UINT32 src_fmt_id = req.format_id;
        const std::string name = impl->server_format_name(req.format_id);
        ClipboardLock(impl->system_clip);
        if (!name.empty()) {
            if (name == kTypeFileGroupW) {
                src_fmt_id = ClipboardRegisterFormat(impl->system_clip, kTypeFileGroupW);
                if (impl->file && cliprdr_file_context_has_local_support(impl->file)) {
                    std::ignore = cliprdr_file_context_update_server_data(
                        impl->file, impl->system_clip,
                        response->requestedFormatData, response->common.dataLen);
                }
            } else {
                src_fmt_id = ClipboardRegisterFormat(impl->system_clip, name.c_str());
            }
        }
        if (!ClipboardSetData(impl->system_clip, src_fmt_id,
                              response->requestedFormatData, response->common.dataLen)) {
            req.success = false;
        }
        ClipboardUnlock(impl->system_clip);
    }

    if (impl->data_event) {
        SetEvent(impl->data_event);
    }
    return CHANNEL_RC_OK;
}

static void on_gtk_clipboard_changed(GdkClipboard* /*clipboard*/, gpointer user_data) {
    auto* weak = static_cast<std::weak_ptr<ClipboardBridge::Impl>*>(user_data);
    if (auto locked = weak->lock()) {
        locked->announce_local_clipboard_to_server();
    }
}

} // namespace

ClipboardBridge::ClipboardBridge() : impl_(std::make_shared<Impl>()) {
    impl_->self_weak = impl_;
}

ClipboardBridge::~ClipboardBridge() {
    detach_gtk_clipboard();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->destroyed = true;
    impl_->ctx = nullptr;
    if (impl_->data_event) {
        SetEvent(impl_->data_event);
    }
}

void ClipboardBridge::attach_gtk_clipboard(GdkClipboard* clipboard, bool enable_files) {
    detach_gtk_clipboard();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->enable_files = enable_files;
    if (impl_->file) {
        std::ignore = cliprdr_file_context_set_locally_available(impl_->file, enable_files ? TRUE : FALSE);
    }
    if (!clipboard) return;
    impl_->gtk_clipboard = GDK_CLIPBOARD(g_object_ref(clipboard));
    auto* heap_weak = new std::weak_ptr<Impl>(impl_);
    impl_->gtk_changed_id = g_signal_connect_data(
        clipboard,
        "changed",
        G_CALLBACK(on_gtk_clipboard_changed),
        heap_weak,
        [](gpointer p, GClosure*) { delete static_cast<std::weak_ptr<Impl>*>(p); },
        static_cast<GConnectFlags>(0));
}

void ClipboardBridge::detach_gtk_clipboard() {
    GdkClipboard* old_clip = nullptr;
    gulong sig = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        old_clip = impl_->gtk_clipboard;
        sig = impl_->gtk_changed_id;
        impl_->gtk_clipboard = nullptr;
        impl_->gtk_changed_id = 0;
    }
    if (old_clip) {
        if (sig != 0) {
            g_signal_handler_disconnect(old_clip, sig);
        }
        g_object_unref(old_clip);
    }
}

bool ClipboardBridge::bind_channel(CliprdrClientContext* clip, rdpContext* rdp_ctx) {
    if (!clip) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->rdp_ctx = rdp_ctx;
    impl_->ctx = clip;
    impl_->sync_ready = false;
    clip->custom = impl_->file;

    clip->MonitorReady = cb_monitor_ready;
    clip->ServerCapabilities = cb_server_capabilities;
    clip->ServerFormatList = cb_server_format_list;
    clip->ServerFormatListResponse = cb_format_list_response;
    clip->ServerFormatDataRequest = cb_server_format_data_request;
    clip->ServerFormatDataResponse = cb_server_format_data_response;

    if (impl_->file) {
        return cliprdr_file_context_init(impl_->file, clip) == TRUE;
    }
    return true;
}

void ClipboardBridge::unbind_channel(CliprdrClientContext* clip) {
    if (!clip) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->file) {
        std::ignore = cliprdr_file_context_uninit(impl_->file, clip);
    }
    impl_->ctx = nullptr;
    impl_->sync_ready = false;
    impl_->server_requests.clear();
    clip->custom = nullptr;
    if (impl_->data_event) {
        SetEvent(impl_->data_event);
    }
}

} // namespace winrdp
