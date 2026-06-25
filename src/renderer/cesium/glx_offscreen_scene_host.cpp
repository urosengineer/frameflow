#include "renderer/cesium/glx_offscreen_scene_host.hpp"

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <utility>

namespace frameflow::renderer::cesium {

namespace {

using SteadyClock = std::chrono::steady_clock;

std::int64_t elapsed_millis(const SteadyClock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - start).count();
}

} // namespace

class GlxOffscreenSceneHost::Impl {
public:
    Display* display = nullptr;
    int screen = 0;
    XVisualInfo* visual_info = nullptr;
    Pixmap pixmap = 0;
    GLXPixmap glx_pixmap = 0;
    GLXContext context = nullptr;
    int width = 1;
    int height = 1;

    ~Impl() {
        if (display != nullptr && context != nullptr) {
            glXMakeCurrent(display, None, nullptr);
        }
        if (display != nullptr && glx_pixmap != 0) {
            glXDestroyGLXPixmap(display, glx_pixmap);
        }
        if (display != nullptr && pixmap != 0) {
            XFreePixmap(display, pixmap);
        }
        if (display != nullptr && context != nullptr) {
            glXDestroyContext(display, context);
        }
        if (visual_info != nullptr) {
            XFree(visual_info);
        }
        if (display != nullptr) {
            XCloseDisplay(display);
        }
    }

    bool make_current(std::string* error_message) const {
        if (display == nullptr || context == nullptr || glx_pixmap == 0) {
            if (error_message != nullptr) {
                *error_message = "GLX offscreen scene host is not fully initialized";
            }
            return false;
        }
        if (!glXMakeCurrent(display, glx_pixmap, context)) {
            if (error_message != nullptr) {
                *error_message = "glXMakeCurrent failed for the offscreen GLX pixmap";
            }
            return false;
        }
        return true;
    }

    bool recreate_pixmap(std::string* error_message) {
        if (display == nullptr || visual_info == nullptr) {
            if (error_message != nullptr) {
                *error_message = "GLX offscreen scene host is missing display or visual info";
            }
            return false;
        }

        if (display != nullptr && context != nullptr) {
            glXMakeCurrent(display, None, nullptr);
        }

        if (glx_pixmap != 0) {
            glXDestroyGLXPixmap(display, glx_pixmap);
            glx_pixmap = 0;
        }
        if (pixmap != 0) {
            XFreePixmap(display, pixmap);
            pixmap = 0;
        }

        pixmap = XCreatePixmap(
            display,
            RootWindow(display, screen),
            static_cast<unsigned int>(std::max(1, width)),
            static_cast<unsigned int>(std::max(1, height)),
            static_cast<unsigned int>(visual_info->depth)
        );
        if (pixmap == 0) {
            if (error_message != nullptr) {
                *error_message = "XCreatePixmap failed for the offscreen GLX scene host";
            }
            return false;
        }

        glx_pixmap = glXCreateGLXPixmap(display, visual_info, pixmap);
        if (glx_pixmap == 0) {
            if (error_message != nullptr) {
                *error_message = "glXCreateGLXPixmap failed for the offscreen GLX scene host";
            }
            return false;
        }

        return make_current(error_message);
    }
};

std::unique_ptr<GlxOffscreenSceneHost> GlxOffscreenSceneHost::create(
    const int width,
    const int height,
    const RenderedGlobeScene::CameraState& initial_camera,
    RenderedGlobeScene::Options scene_options,
    std::string* error_message
) {
    auto impl = std::make_unique<Impl>();
    impl->display = XOpenDisplay(nullptr);
    if (impl->display == nullptr) {
        if (error_message != nullptr) {
            *error_message = "XOpenDisplay failed; Cesium offscreen rendering requires an X11 display";
        }
        return nullptr;
    }
    impl->screen = DefaultScreen(impl->display);
    impl->width = std::max(1, width);
    impl->height = std::max(1, height);

    int attributes[] = {GLX_RGBA, GLX_DEPTH_SIZE, 24, None};
    impl->visual_info = glXChooseVisual(impl->display, impl->screen, attributes);
    if (impl->visual_info == nullptr) {
        if (error_message != nullptr) {
            *error_message = "glXChooseVisual failed for the offscreen GLX scene host";
        }
        return nullptr;
    }

    impl->context = glXCreateContext(impl->display, impl->visual_info, nullptr, True);
    if (impl->context == nullptr) {
        if (error_message != nullptr) {
            *error_message = "glXCreateContext failed for the offscreen GLX scene host";
        }
        return nullptr;
    }
    if (!impl->recreate_pixmap(error_message)) {
        return nullptr;
    }

    RenderedGlobeScene scene(
        CesiumGeospatial::Cartographic::fromDegrees(
            initial_camera.longitude,
            initial_camera.latitude,
            initial_camera.height_meters
        ),
        std::move(scene_options)
    );
    scene.resize(width, height);
    scene.set_camera_state(initial_camera);

    return std::unique_ptr<GlxOffscreenSceneHost>(
        new GlxOffscreenSceneHost(std::move(impl), std::move(scene))
    );
}

GlxOffscreenSceneHost::GlxOffscreenSceneHost(
    std::unique_ptr<Impl> impl,
    RenderedGlobeScene scene
) : impl_(std::move(impl)),
    scene_(std::move(scene)) {}

GlxOffscreenSceneHost::~GlxOffscreenSceneHost() {
    std::string ignored_error;
    if (impl_) {
        impl_->make_current(&ignored_error);
    }
}

void GlxOffscreenSceneHost::resize(const int width, const int height) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const int next_width = std::max(1, width);
    const int next_height = std::max(1, height);
    if (impl_->width == next_width && impl_->height == next_height) {
        return;
    }
    impl_->width = next_width;
    impl_->height = next_height;
    std::string ignored_error;
    if (impl_->recreate_pixmap(&ignored_error)) {
        scene_.resize(impl_->width, impl_->height);
    }
}

void GlxOffscreenSceneHost::pause() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    scene_.pause();
}

void GlxOffscreenSceneHost::resume() noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    scene_.resume();
}

void GlxOffscreenSceneHost::set_points(std::span<const frameflow::GeoPointAggregate> points) {
    const std::lock_guard<std::mutex> lock(mutex_);
    scene_.set_points(points);
}

void GlxOffscreenSceneHost::set_selected_location(std::optional<std::int64_t> location_id) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    scene_.set_selected_location(location_id);
}

void GlxOffscreenSceneHost::set_focus_location(std::optional<std::int64_t> location_id) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    scene_.set_focus_location(location_id);
}

void GlxOffscreenSceneHost::set_camera_state(const RenderedGlobeScene::CameraState& camera_state) noexcept {
    const std::lock_guard<std::mutex> lock(mutex_);
    scene_.set_camera_state(camera_state);
}

bool GlxOffscreenSceneHost::render_into_rgba(
    std::vector<std::uint8_t>& rgba_pixels,
    const std::uint32_t stride_bytes,
    std::string* error_message
) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto total_start = SteadyClock::now();

    const auto make_current_start = SteadyClock::now();
    if (!impl_->make_current(error_message)) {
        last_make_current_millis_ = elapsed_millis(make_current_start);
        last_total_millis_ = elapsed_millis(total_start);
        return false;
    }
    last_make_current_millis_ = elapsed_millis(make_current_start);
    if (stride_bytes < static_cast<std::uint32_t>(impl_->width * 4)) {
        if (error_message != nullptr) {
            *error_message = "offscreen RGBA stride is too small for the GLX Cesium scene readback";
        }
        last_total_millis_ = elapsed_millis(total_start);
        return false;
    }
    const auto required_size = static_cast<std::size_t>(stride_bytes) * static_cast<std::size_t>(impl_->height);
    if (rgba_pixels.size() < required_size) {
        if (error_message != nullptr) {
            *error_message = "offscreen RGBA destination buffer is smaller than the GLX Cesium readback size";
        }
        last_total_millis_ = elapsed_millis(total_start);
        return false;
    }

    const auto scene_start = SteadyClock::now();
    scene_.render_frame();
    glFlush();
    last_scene_render_millis_ = elapsed_millis(scene_start);

    const auto readback_start = SteadyClock::now();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_FRONT);
    glReadPixels(
        0,
        0,
        impl_->width,
        impl_->height,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        rgba_pixels.data()
    );
    last_readback_millis_ = elapsed_millis(readback_start);

    const auto row_bytes = static_cast<std::size_t>(impl_->width) * 4u;
    const auto flip_start = SteadyClock::now();
    if (stride_bytes == row_bytes) {
        flip_row_buffer_.resize(row_bytes);
        for (int row = 0; row < impl_->height / 2; ++row) {
            auto* top = rgba_pixels.data() + (static_cast<std::size_t>(row) * row_bytes);
            auto* bottom = rgba_pixels.data() + (static_cast<std::size_t>(impl_->height - row - 1) * row_bytes);
            std::memcpy(flip_row_buffer_.data(), top, row_bytes);
            std::memcpy(top, bottom, row_bytes);
            std::memcpy(bottom, flip_row_buffer_.data(), row_bytes);
        }
    } else {
        std::vector<std::uint8_t> raw_pixels(
            static_cast<std::size_t>(impl_->width) * static_cast<std::size_t>(impl_->height) * 4u,
            0u
        );
        std::memcpy(raw_pixels.data(), rgba_pixels.data(), raw_pixels.size());
        for (int row = 0; row < impl_->height; ++row) {
            const auto source_offset = static_cast<std::size_t>(impl_->height - row - 1) * row_bytes;
            const auto destination_offset = static_cast<std::size_t>(row) * stride_bytes;
            std::memcpy(rgba_pixels.data() + destination_offset, raw_pixels.data() + source_offset, row_bytes);
        }
    }
    last_flip_millis_ = elapsed_millis(flip_start);
    last_total_millis_ = elapsed_millis(total_start);
    return true;
}

std::optional<RenderedGlobeScene::ScreenPosition> GlxOffscreenSceneHost::screen_position_for_location(
    const std::int64_t location_id
) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return scene_.screen_position_for_location(location_id);
}

std::string GlxOffscreenSceneHost::diagnostics_summary() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out
        << "offscreen_total_ms=" << last_total_millis_
        << " offscreen_make_current_ms=" << last_make_current_millis_
        << " offscreen_scene_ms=" << last_scene_render_millis_
        << " offscreen_readback_ms=" << last_readback_millis_
        << " offscreen_flip_ms=" << last_flip_millis_
        << " scene=[" << scene_.diagnostics_summary() << "]";
    return out.str();
}

} // namespace frameflow::renderer::cesium
