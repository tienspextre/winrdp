#include "rdp/pointer_bridge.hpp"

#include <freerdp/codec/color.h>
#include <freerdp/gdi/gdi.h>

#include <algorithm>
#include <cstring>

namespace winrdp {

namespace {

struct WinRdpPointer {
    rdpPointer base;
    std::shared_ptr<CursorBitmap>* bitmap_holder;
};

// Map from rdpContext* -> PointerBridge* (safe for multi-session support)
std::mutex g_bridge_map_mutex;
std::vector<std::pair<rdpContext*, PointerBridge*>> g_bridge_map;

void register_ctx_bridge(rdpContext* ctx, PointerBridge* bridge) {
    std::lock_guard<std::mutex> lock(g_bridge_map_mutex);
    for (auto& kv : g_bridge_map) {
        if (kv.first == ctx) {
            kv.second = bridge;
            return;
        }
    }
    g_bridge_map.emplace_back(ctx, bridge);
}

void unregister_bridge(PointerBridge* bridge) {
    std::lock_guard<std::mutex> lock(g_bridge_map_mutex);
    g_bridge_map.erase(
        std::remove_if(g_bridge_map.begin(), g_bridge_map.end(),
                       [bridge](const auto& kv) { return kv.second == bridge; }),
        g_bridge_map.end());
}

PointerBridge* find_bridge(rdpContext* ctx) {
    std::lock_guard<std::mutex> lock(g_bridge_map_mutex);
    for (const auto& kv : g_bridge_map) {
        if (kv.first == ctx) {
            return kv.second;
        }
    }
    return nullptr;
}

GdkCursor* make_invisible_cursor() {
    GdkCursor* c = gdk_cursor_new_from_name("none", nullptr);
    if (c) return c;
    static const uint8_t zero[4] = {0, 0, 0, 0};
    GBytes* bytes = g_bytes_new_static(zero, sizeof(zero));
    GdkTexture* tex = gdk_memory_texture_new(1, 1, GDK_MEMORY_B8G8R8A8, bytes, 4);
    g_bytes_unref(bytes);
    c = gdk_cursor_new_from_texture(tex, 0, 0, nullptr);
    g_object_unref(tex);
    return c;
}

GdkCursor* create_gdk_cursor(const std::shared_ptr<CursorBitmap>& bmp, bool is_null, bool is_default) {
    if (is_default) {
        return nullptr;
    }
    if (is_null) {
        return make_invisible_cursor();
    }
    if (bmp && bmp->width > 0 && bmp->height > 0 && !bmp->bgra.empty()) {
        const uint32_t cw = bmp->width;
        const uint32_t ch = bmp->height;
        GBytes* bytes = g_bytes_new(bmp->bgra.data(), bmp->bgra.size());
        GdkTexture* tex = gdk_memory_texture_new(
            static_cast<int>(cw),
            static_cast<int>(ch),
            GDK_MEMORY_B8G8R8A8,
            bytes,
            static_cast<gsize>(cw) * 4U);
        g_bytes_unref(bytes);

        const int hx = std::clamp(bmp->hotspot_x, 0, static_cast<int>(cw) - 1);
        const int hy = std::clamp(bmp->hotspot_y, 0, static_cast<int>(ch) - 1);
        GdkCursor* cursor = gdk_cursor_new_from_texture(tex, hx, hy, nullptr);
        g_object_unref(tex);
        return cursor;
    }
    return nullptr;
}

} // namespace

PointerBridge::~PointerBridge() {
    detach_widget();
    unregister_bridge(this);
}

void PointerBridge::attach_widget(GtkWidget* widget) {
    if (!widget) return;
    bool need_apply_cached = false;
    std::shared_ptr<CursorBitmap> bmp;
    bool is_null = false;
    bool is_default = true;
    {
        std::lock_guard<std::mutex> lock(token_->mutex);
        if (std::find(token_->widgets.begin(), token_->widgets.end(), widget) == token_->widgets.end()) {
            token_->widgets.push_back(widget);
        }
        token_->alive = true;
        if (token_->has_last) {
            need_apply_cached = true;
            bmp = token_->last_bmp;
            is_null = token_->last_is_null;
            is_default = token_->last_is_default;
        }
    }
    if (need_apply_cached) {
        dispatch_cursor(std::move(bmp), is_null, is_default);
    }
}

void PointerBridge::detach_widget(GtkWidget* widget) {
    if (!widget) return;
    std::lock_guard<std::mutex> lock(token_->mutex);
    token_->widgets.erase(
        std::remove(token_->widgets.begin(), token_->widgets.end(), widget),
        token_->widgets.end());
    token_->alive = !token_->widgets.empty();
}

void PointerBridge::detach_widget() {
    std::lock_guard<std::mutex> lock(token_->mutex);
    token_->widgets.clear();
    token_->alive = false;
}

void PointerBridge::set_position_callback(PositionCallback cb) {
    std::lock_guard<std::mutex> lock(pos_mutex_);
    pos_cb_ = std::move(cb);
}

void PointerBridge::reset_to_default() {
    dispatch_cursor(nullptr, false, true);
}

bool PointerBridge::register_callbacks(rdpContext* context) {
    if (!context || !context->graphics) {
        return false;
    }
    register_ctx_bridge(context, this);

    rdpPointer pointer{};
    pointer.size = sizeof(WinRdpPointer);
    pointer.New = &PointerBridge::on_pointer_new;
    pointer.Free = &PointerBridge::on_pointer_free;
    pointer.Set = &PointerBridge::on_pointer_set;
    pointer.SetNull = &PointerBridge::on_pointer_set_null;
    pointer.SetDefault = &PointerBridge::on_pointer_set_default;
    pointer.SetPosition = &PointerBridge::on_pointer_set_position;
    graphics_register_pointer(context->graphics, &pointer);
    return true;
}

void PointerBridge::dispatch_cursor(std::shared_ptr<CursorBitmap> bmp, bool is_null, bool is_default) {
    {
        std::lock_guard<std::mutex> lock(token_->mutex);
        token_->has_last = true;
        token_->last_bmp = bmp;
        token_->last_is_null = is_null;
        token_->last_is_default = is_default;
    }

    struct IdleArg {
        std::shared_ptr<AliveToken> token;
        std::shared_ptr<CursorBitmap> bmp;
        bool is_null;
        bool is_default;
    };

    auto* arg = new IdleArg{token_, std::move(bmp), is_null, is_default};
    g_idle_add(
        [](gpointer data) -> gboolean {
            std::unique_ptr<IdleArg> req(static_cast<IdleArg*>(data));
            std::vector<GtkWidget*> targets;
            {
                std::lock_guard<std::mutex> lock(req->token->mutex);
                if (!req->token->alive || req->token->widgets.empty()) {
                    return G_SOURCE_REMOVE;
                }
                for (GtkWidget* w : req->token->widgets) {
                    if (w && GTK_IS_WIDGET(w)) {
                        g_object_ref(w);
                        targets.push_back(w);
                    }
                }
            }

            if (targets.empty()) {
                return G_SOURCE_REMOVE;
            }

            if (req->is_default) {
                for (GtkWidget* w : targets) {
                    gtk_widget_set_cursor(w, nullptr);
                    g_object_unref(w);
                }
                return G_SOURCE_REMOVE;
            }

            GdkCursor* cursor = create_gdk_cursor(req->bmp, req->is_null, req->is_default);
            for (GtkWidget* w : targets) {
                if (cursor || req->is_null) {
                    gtk_widget_set_cursor(w, cursor);
                }
                g_object_unref(w);
            }
            if (cursor) {
                g_object_unref(cursor);
            }
            return G_SOURCE_REMOVE;
        },
        arg);
}

BOOL PointerBridge::on_pointer_new(rdpContext* context, rdpPointer* pointer) {
    if (!context || !context->gdi || !pointer) {
        return FALSE;
    }
    auto* wp = reinterpret_cast<WinRdpPointer*>(pointer);
    if (pointer->width == 0 || pointer->height == 0 || pointer->width > 512 || pointer->height > 512) {
        wp->bitmap_holder = nullptr;
        return TRUE;
    }

    auto bmp = std::make_shared<CursorBitmap>();
    bmp->width = pointer->width;
    bmp->height = pointer->height;
    bmp->hotspot_x = static_cast<int>(pointer->xPos);
    bmp->hotspot_y = static_cast<int>(pointer->yPos);
    bmp->bgra.assign(static_cast<size_t>(bmp->width) * bmp->height * 4U, 0);

    const BOOL ok = freerdp_image_copy_from_pointer_data(
        bmp->bgra.data(),
        PIXEL_FORMAT_BGRA32,
        0, 0, 0,
        pointer->width,
        pointer->height,
        pointer->xorMaskData,
        pointer->lengthXorMask,
        pointer->andMaskData,
        pointer->lengthAndMask,
        pointer->xorBpp,
        &context->gdi->palette);

    if (!ok) {
        wp->bitmap_holder = nullptr;
        return FALSE;
    }

    wp->bitmap_holder = new std::shared_ptr<CursorBitmap>(std::move(bmp));
    return TRUE;
}

void PointerBridge::on_pointer_free(rdpContext* /*context*/, rdpPointer* pointer) {
    if (!pointer) return;
    auto* wp = reinterpret_cast<WinRdpPointer*>(pointer);
    delete wp->bitmap_holder;
    wp->bitmap_holder = nullptr;
}

BOOL PointerBridge::on_pointer_set(rdpContext* context, rdpPointer* pointer) {
    auto* bridge = find_bridge(context);
    if (!bridge || !pointer) return TRUE;
    auto* wp = reinterpret_cast<WinRdpPointer*>(pointer);
    if (!wp->bitmap_holder || !(*wp->bitmap_holder)) {
        bridge->dispatch_cursor(nullptr, false, true);
        return TRUE;
    }
    bridge->dispatch_cursor(*wp->bitmap_holder, false, false);
    return TRUE;
}

BOOL PointerBridge::on_pointer_set_null(rdpContext* context) {
    auto* bridge = find_bridge(context);
    if (bridge) {
        bridge->dispatch_cursor(nullptr, true, false);
    }
    return TRUE;
}

BOOL PointerBridge::on_pointer_set_default(rdpContext* context) {
    auto* bridge = find_bridge(context);
    if (bridge) {
        bridge->dispatch_cursor(nullptr, false, true);
    }
    return TRUE;
}

BOOL PointerBridge::on_pointer_set_position(rdpContext* context, UINT32 x, UINT32 y) {
    auto* bridge = find_bridge(context);
    if (!bridge) return TRUE;
    PositionCallback cb;
    {
        std::lock_guard<std::mutex> lock(bridge->pos_mutex_);
        cb = bridge->pos_cb_;
    }
    if (cb) {
        struct PosReq {
            PositionCallback cb;
            uint32_t x;
            uint32_t y;
        };
        g_idle_add(
            [](gpointer data) -> gboolean {
                std::unique_ptr<PosReq> r(static_cast<PosReq*>(data));
                if (r->cb) r->cb(r->x, r->y);
                return G_SOURCE_REMOVE;
            },
            new PosReq{std::move(cb), x, y});
    }
    return TRUE;
}

} // namespace winrdp
