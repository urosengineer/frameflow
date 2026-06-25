#pragma once

#include "frameflow/types.hpp"
#include "renderer/cesium/rendered_globe_scene.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frameflow::renderer::cesium {

class GlxOffscreenSceneHost {
public:
    static std::unique_ptr<GlxOffscreenSceneHost> create(
        int width,
        int height,
        const RenderedGlobeScene::CameraState& initial_camera,
        RenderedGlobeScene::Options scene_options = RenderedGlobeScene::Options(),
        std::string* error_message = nullptr
    );

    ~GlxOffscreenSceneHost();

    GlxOffscreenSceneHost(const GlxOffscreenSceneHost&) = delete;
    GlxOffscreenSceneHost& operator=(const GlxOffscreenSceneHost&) = delete;

    void resize(int width, int height);
    void pause() noexcept;
    void resume() noexcept;
    void set_points(std::span<const frameflow::GeoPointAggregate> points);
    void set_selected_location(std::optional<std::int64_t> location_id) noexcept;
    void set_focus_location(std::optional<std::int64_t> location_id) noexcept;
    void set_camera_state(const RenderedGlobeScene::CameraState& camera_state) noexcept;
    bool render_into_rgba(
        std::vector<std::uint8_t>& rgba_pixels,
        std::uint32_t stride_bytes,
        std::string* error_message = nullptr
    );

    [[nodiscard]] std::optional<RenderedGlobeScene::ScreenPosition> screen_position_for_location(
        std::int64_t location_id
    ) const;
    [[nodiscard]] std::string diagnostics_summary() const;

private:
    class Impl;

    GlxOffscreenSceneHost(
        std::unique_ptr<Impl> impl,
        RenderedGlobeScene scene
    );

    std::unique_ptr<Impl> impl_;
    RenderedGlobeScene scene_;
    mutable std::mutex mutex_;
    std::int64_t last_total_millis_{0};
    std::int64_t last_make_current_millis_{0};
    std::int64_t last_scene_render_millis_{0};
    std::int64_t last_readback_millis_{0};
    std::int64_t last_flip_millis_{0};
    std::vector<std::uint8_t> flip_row_buffer_;
};

} // namespace frameflow::renderer::cesium
