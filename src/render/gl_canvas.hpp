#pragma once

#include "input/scancode_map.hpp"
#include "rdp/framebuffer.hpp"

#include <epoxy/gl.h>
#include <gtk/gtk.h>

#include <cstdint>
#include <string>

namespace winrdp {

struct SourceRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0; }
};

class GlCanvas {
public:
    GlCanvas();
    ~GlCanvas();

    GlCanvas(const GlCanvas&) = delete;
    GlCanvas& operator=(const GlCanvas&) = delete;

    GtkWidget* widget() const { return GTK_WIDGET(area_); }

    void bind_framebuffer(Framebuffer* fb);
    void set_smart_sizing(bool enabled);
    bool smart_sizing() const { return smart_sizing_; }

    void set_source_rect(int x, int y, int width, int height);
    void clear_source_rect();
    SourceRect source_rect() const { return src_rect_; }

    void request_redraw();
    bool map_to_remote(double widget_x, double widget_y, int& out_rx, int& out_ry) const;
    ViewportTransform current_viewport() const { return last_vp_; }

    // Diagnostics for --smoke-test and telemetry
    bool gl_initialized() const { return gl_ok_; }
    const std::string& gl_vendor() const { return gl_vendor_; }
    const std::string& gl_renderer() const { return gl_renderer_; }
    const std::string& gl_version() const { return gl_version_; }
    uint64_t dirty_upload_count() const { return dirty_uploads_; }
    GLenum last_gl_error() const { return last_gl_err_; }

private:
    static void on_realize(GtkGLArea* area, gpointer user_data);
    static void on_unrealize(GtkGLArea* area, gpointer user_data);
    static gboolean on_render(GtkGLArea* area, GdkGLContext* context, gpointer user_data);
    static void on_resize(GtkGLArea* area, gint width, gint height, gpointer user_data);

    void upload_framebuffer_locked(uint32_t fb_w,
                                   uint32_t fb_h,
                                   uint32_t stride_bytes,
                                   const uint8_t* data,
                                   const DirtyRect& dirty,
                                   uint64_t resize_gen,
                                   uint64_t frame_gen);

    GtkGLArea* area_ = nullptr;
    Framebuffer* fb_ = nullptr;
    bool smart_sizing_ = false;
    SourceRect src_rect_{};
    bool src_rect_dirty_ = true;

    bool gl_ok_ = false;
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint texture_ = 0;
    GLint uniform_tex_ = -1;

    uint32_t tex_w_ = 0;
    uint32_t tex_h_ = 0;
    uint64_t consumer_resize_gen_ = 0;
    uint64_t consumer_frame_gen_ = 0;
    uint64_t last_resize_gen_ = 0;
    uint64_t last_frame_gen_ = 0;
    uint64_t dirty_uploads_ = 0;
    GLenum last_gl_err_ = GL_NO_ERROR;

    int widget_w_ = 0;
    int widget_h_ = 0;
    mutable ViewportTransform last_vp_{};

    std::string gl_vendor_;
    std::string gl_renderer_;
    std::string gl_version_;
};

} // namespace winrdp
