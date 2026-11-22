#include "render/gl_canvas.hpp"

#include <algorithm>
#include <cmath>

namespace winrdp {

namespace {

constexpr const char* kVertexShaderSrc = R"GLSL(
#version 330 core
out vec2 v_uv;
void main() {
    // Fullscreen triangle strip: 0=(-1,1), 1=(1,1), 2=(-1,-1), 3=(1,-1)
    vec2 pos = vec2((gl_VertexID & 1) == 0 ? -1.0 : 1.0,
                    (gl_VertexID & 2) == 0 ?  1.0 : -1.0);
    v_uv = vec2((gl_VertexID & 1) == 0 ? 0.0 : 1.0,
                (gl_VertexID & 2) == 0 ? 0.0 : 1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
}
)GLSL";

constexpr const char* kFragmentShaderSrc = R"GLSL(
#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 frag_color;
void main() {
    vec3 rgb = texture(u_tex, v_uv).rgb;
    frag_color = vec4(rgb, 1.0);
}
)GLSL";

GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        g_warning("OpenGL shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

GlCanvas::GlCanvas() {
    area_ = GTK_GL_AREA(gtk_gl_area_new());
    gtk_gl_area_set_allowed_apis(area_, GDK_GL_API_GL);
    gtk_gl_area_set_required_version(area_, 3, 3);
    gtk_gl_area_set_auto_render(area_, FALSE);
    gtk_gl_area_set_has_depth_buffer(area_, FALSE);
    gtk_gl_area_set_has_stencil_buffer(area_, FALSE);

    gtk_widget_set_hexpand(GTK_WIDGET(area_), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(area_), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(area_), TRUE);
    gtk_widget_set_can_focus(GTK_WIDGET(area_), TRUE);

    g_signal_connect(area_, "realize", G_CALLBACK(&GlCanvas::on_realize), this);
    g_signal_connect(area_, "unrealize", G_CALLBACK(&GlCanvas::on_unrealize), this);
    g_signal_connect(area_, "render", G_CALLBACK(&GlCanvas::on_render), this);
    g_signal_connect(area_, "resize", G_CALLBACK(&GlCanvas::on_resize), this);
}

GlCanvas::~GlCanvas() {
    if (area_) {
        g_signal_handlers_disconnect_by_data(area_, this);
    }
}

void GlCanvas::bind_framebuffer(Framebuffer* fb) {
    fb_ = fb;
    last_resize_gen_ = 0;
    last_frame_gen_ = 0;
    request_redraw();
}

void GlCanvas::set_smart_sizing(bool enabled) {
    smart_sizing_ = enabled;
    request_redraw();
}

void GlCanvas::request_redraw() {
    if (area_ && gtk_widget_get_realized(GTK_WIDGET(area_))) {
        gtk_gl_area_queue_render(area_);
    }
}

bool GlCanvas::map_to_remote(double widget_x, double widget_y, int& out_rx, int& out_ry) const {
    return map_widget_to_remote(last_vp_, widget_x, widget_y, out_rx, out_ry);
}

void GlCanvas::on_realize(GtkGLArea* area, gpointer user_data) {
    auto* self = static_cast<GlCanvas*>(user_data);
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != nullptr) {
        self->gl_ok_ = false;
        return;
    }

    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    self->gl_vendor_ = vendor ? vendor : "";
    self->gl_renderer_ = renderer ? renderer : "";
    self->gl_version_ = version ? version : "";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShaderSrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragmentShaderSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        self->gl_ok_ = false;
        return;
    }

    self->program_ = glCreateProgram();
    glAttachShader(self->program_, vs);
    glAttachShader(self->program_, fs);
    glLinkProgram(self->program_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(self->program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512] = {};
        glGetProgramInfoLog(self->program_, sizeof(log), nullptr, log);
        g_warning("OpenGL program link failed: %s", log);
        glDeleteProgram(self->program_);
        self->program_ = 0;
        self->gl_ok_ = false;
        return;
    }

    self->uniform_tex_ = glGetUniformLocation(self->program_, "u_tex");
    glGenVertexArrays(1, &self->vao_);
    glGenTextures(1, &self->texture_);
    glBindTexture(GL_TEXTURE_2D, self->texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    self->last_gl_err_ = glGetError();
    self->gl_ok_ = (self->last_gl_err_ == GL_NO_ERROR);
}

void GlCanvas::on_unrealize(GtkGLArea* area, gpointer user_data) {
    auto* self = static_cast<GlCanvas*>(user_data);
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) == nullptr) {
        if (self->texture_) {
            glDeleteTextures(1, &self->texture_);
            self->texture_ = 0;
        }
        if (self->vao_) {
            glDeleteVertexArrays(1, &self->vao_);
            self->vao_ = 0;
        }
        if (self->program_) {
            glDeleteProgram(self->program_);
            self->program_ = 0;
        }
    }
    self->tex_w_ = 0;
    self->tex_h_ = 0;
    self->gl_ok_ = false;
}

void GlCanvas::on_resize(GtkGLArea* /*area*/, gint width, gint height, gpointer user_data) {
    auto* self = static_cast<GlCanvas*>(user_data);
    self->widget_w_ = width;
    self->widget_h_ = height;
}

void GlCanvas::upload_framebuffer_locked(uint32_t fb_w,
                                         uint32_t fb_h,
                                         uint32_t /*stride_bytes*/,
                                         const uint8_t* data,
                                         const DirtyRect& dirty,
                                         uint64_t resize_gen,
                                         uint64_t frame_gen) {
    if (!data || fb_w == 0 || fb_h == 0 || texture_ == 0) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (resize_gen != last_resize_gen_ || fb_w != tex_w_ || fb_h != tex_h_) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA8,
                     static_cast<GLsizei>(fb_w),
                     static_cast<GLsizei>(fb_h),
                     0,
                     GL_BGRA,
                     GL_UNSIGNED_BYTE,
                     data);
        tex_w_ = fb_w;
        tex_h_ = fb_h;
        last_resize_gen_ = resize_gen;
        last_frame_gen_ = frame_gen;
        ++dirty_uploads_;
    } else if (frame_gen != last_frame_gen_ && !dirty.empty()) {
        const int dx = std::clamp(dirty.x, 0, static_cast<int>(fb_w) - 1);
        const int dy = std::clamp(dirty.y, 0, static_cast<int>(fb_h) - 1);
        const int dw = std::min(dirty.width, static_cast<int>(fb_w) - dx);
        const int dh = std::min(dirty.height, static_cast<int>(fb_h) - dy);
        if (dw > 0 && dh > 0) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(fb_w));
            const uint8_t* sub_ptr = data + (static_cast<size_t>(dy) * fb_w + static_cast<size_t>(dx)) * 4U;
            glTexSubImage2D(GL_TEXTURE_2D,
                            0,
                            dx,
                            dy,
                            dw,
                            dh,
                            GL_BGRA,
                            GL_UNSIGNED_BYTE,
                            sub_ptr);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            ++dirty_uploads_;
        }
        last_frame_gen_ = frame_gen;
    }
}

gboolean GlCanvas::on_render(GtkGLArea* area, GdkGLContext* /*context*/, gpointer user_data) {
    auto* self = static_cast<GlCanvas*>(user_data);
    if (!self->gl_ok_ || self->program_ == 0) {
        glClearColor(0.0627f, 0.0627f, 0.0784f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return TRUE;
    }

    const int sf = std::max(1, gtk_widget_get_scale_factor(GTK_WIDGET(area)));
    const int ww = std::max(1, gtk_widget_get_width(GTK_WIDGET(area)));
    const int wh = std::max(1, gtk_widget_get_height(GTK_WIDGET(area)));
    const int gl_w = std::max(1, ww * sf);
    const int gl_h = std::max(1, wh * sf);

    glViewport(0, 0, gl_w, gl_h);
    glClearColor(0.0627f, 0.0627f, 0.0784f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (self->fb_) {
        self->fb_->consume([self](uint32_t fb_w,
                                  uint32_t fb_h,
                                  uint32_t stride_bytes,
                                  const uint8_t* data,
                                  const DirtyRect& dirty,
                                  uint64_t resize_gen,
                                  uint64_t frame_gen) {
            self->upload_framebuffer_locked(
                fb_w, fb_h, stride_bytes, data, dirty, resize_gen, frame_gen);
        });
    }

    if (self->tex_w_ > 0 && self->tex_h_ > 0) {
        self->last_vp_ = compute_viewport_transform(
            static_cast<double>(ww),
            static_cast<double>(wh),
            sf,
            self->tex_w_,
            self->tex_h_,
            self->smart_sizing_);

        const int vx = static_cast<int>(std::lround(self->last_vp_.offset_x * sf));
        const int vw = std::max(1, static_cast<int>(std::lround(self->last_vp_.draw_w * sf)));
        const int vh = std::max(1, static_cast<int>(std::lround(self->last_vp_.draw_h * sf)));
        const int vy = gl_h - static_cast<int>(std::lround(self->last_vp_.offset_y * sf)) - vh;

        glViewport(vx, vy, vw, vh);
        glUseProgram(self->program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, self->texture_);

        const GLint filter = self->last_vp_.exact_1_to_1 ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glUniform1i(self->uniform_tex_, 0);

        glBindVertexArray(self->vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    }

    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        self->last_gl_err_ = err;
    }
    return TRUE;
}

} // namespace winrdp
