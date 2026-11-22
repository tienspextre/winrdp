#pragma once

#include <freerdp/client/cliprdr.h>
#include <freerdp/freerdp.h>
#include <gdk/gdk.h>

#include <memory>

namespace winrdp {

class ClipboardBridge {
public:
    ClipboardBridge();
    ~ClipboardBridge();

    ClipboardBridge(const ClipboardBridge&) = delete;
    ClipboardBridge& operator=(const ClipboardBridge&) = delete;

    void attach_gtk_clipboard(GdkClipboard* clipboard, bool enable_files);
    void detach_gtk_clipboard();

    bool bind_channel(CliprdrClientContext* clip, rdpContext* rdp_ctx);
    void unbind_channel(CliprdrClientContext* clip);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace winrdp
