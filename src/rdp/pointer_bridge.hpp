#pragma once

#include <freerdp/freerdp.h>
#include <freerdp/graphics.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace winrdp {

struct CursorBitmap {
    uint32_t width = 0;
    uint32_t height = 0;
    int hotspot_x = 0;
    int hotspot_y = 0;
    std::vector<uint8_t> bgra;
};

class PointerBridge {
public:
    using PositionCallback = std::function<void(uint32_t x, uint32_t y)>;

    PointerBridge() = default;
    ~PointerBridge();

    PointerBridge(const PointerBridge&) = delete;
    PointerBridge& operator=(const PointerBridge&) = delete;

    void attach_widget(GtkWidget* widget);
    void detach_widget();
    void set_position_callback(PositionCallback cb);

    bool register_callbacks(rdpContext* context);
    void reset_to_default();

private:
    static BOOL on_pointer_new(rdpContext* context, rdpPointer* pointer);
    static void on_pointer_free(rdpContext* context, rdpPointer* pointer);
    static BOOL on_pointer_set(rdpContext* context, rdpPointer* pointer);
    static BOOL on_pointer_set_null(rdpContext* context);
    static BOOL on_pointer_set_default(rdpContext* context);
    static BOOL on_pointer_set_position(rdpContext* context, UINT32 x, UINT32 y);

    void dispatch_cursor(std::shared_ptr<CursorBitmap> bmp, bool is_null, bool is_default);

    struct AliveToken {
        std::mutex mutex;
        GtkWidget* widget = nullptr;
        bool alive = true;
    };

    std::shared_ptr<AliveToken> token_ = std::make_shared<AliveToken>();
    std::mutex pos_mutex_;
    PositionCallback pos_cb_;
};

} // namespace winrdp
