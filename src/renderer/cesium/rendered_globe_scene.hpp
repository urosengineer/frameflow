#pragma once

#include "renderer/cartography/cartography_dataset.hpp"
#include "frameflow/c/bridge.h"
#include "frameflow/types.hpp"

#include <CesiumGeospatial/Cartographic.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frameflow::renderer::cesium {

class GoogleTilesetRuntime;
class UniformRasterGlobeLayer;

class RenderedGlobeScene {
public:
    struct ScreenPosition {
        double x = 0.0;
        double y = 0.0;
    };

    struct CameraState {
        double longitude = 0.0;
        double latitude = 0.0;
        double height_meters = 0.0;
        double heading_degrees = 0.0;
        double pitch_degrees = -45.0;
        double roll_degrees = 0.0;
    };

    struct Options {
        std::string tile_cache_path;
        std::uint64_t max_tile_cache_bytes = 0u;
    };

    struct CartographyRenderStats {
        std::size_t visible_country_boundaries = 0u;
        std::size_t visible_country_boundary_segments = 0u;
    };

    struct InteractionHit {
        enum class Kind {
            Location,
            Cluster
        };

        Kind kind = Kind::Location;
        std::int64_t target_id = 0;
        std::int32_t point_count = 1;
        double longitude = 0.0;
        double latitude = 0.0;
        std::optional<std::string> primary_category;
    };

    enum class HudAction {
        ZoomIn,
        ZoomOut,
        ResetCamera,
        ResetOrientation
    };

    explicit RenderedGlobeScene(CesiumGeospatial::Cartographic initial_camera);
    RenderedGlobeScene(CesiumGeospatial::Cartographic initial_camera, Options options);
    ~RenderedGlobeScene();
    RenderedGlobeScene(RenderedGlobeScene&&) noexcept;
    RenderedGlobeScene& operator=(RenderedGlobeScene&&) noexcept;
    RenderedGlobeScene(const RenderedGlobeScene&) = delete;
    RenderedGlobeScene& operator=(const RenderedGlobeScene&) = delete;

    void resize(int width, int height);
    void pause() noexcept;
    void resume() noexcept;
    void set_points(const frameflow_point* points, std::uint64_t point_count);
    void set_points(std::span<const frameflow::GeoPointAggregate> points);
    void set_selected_location(std::optional<std::int64_t> location_id) noexcept;
    [[nodiscard]] std::optional<std::int64_t> selected_location_id() const noexcept;
    void set_focus_location(std::optional<std::int64_t> location_id) noexcept;
    [[nodiscard]] CameraState camera_state() const noexcept;
    void set_camera_state(const CameraState& state) noexcept;
    bool adjust_camera(double longitude_delta_degrees, double latitude_delta_degrees, double height_delta_meters) noexcept;
    bool adjust_orientation(double heading_delta_degrees, double pitch_delta_degrees, double roll_delta_degrees) noexcept;
    [[nodiscard]] std::optional<InteractionHit> hit_test(double screen_x, double screen_y) const;
    [[nodiscard]] std::optional<HudAction> hud_hit_test(double screen_x, double screen_y) const noexcept;
    [[nodiscard]] std::optional<ScreenPosition> hud_action_screen_position(HudAction action) const noexcept;
    bool activate_hud_action(HudAction action) noexcept;
    [[nodiscard]] std::optional<ScreenPosition> screen_position_for_location(std::int64_t location_id) const;
    [[nodiscard]] std::optional<ScreenPosition> first_location_screen_position() const;
    [[nodiscard]] std::optional<ScreenPosition> first_cluster_screen_position() const;
    [[nodiscard]] CartographyRenderStats cartography_render_stats() const;
    void render_frame();

    [[nodiscard]] std::string diagnostics_summary() const;

private:
    struct ScenePoint {
        std::int64_t location_id = 0;
        frameflow_location_kind kind = FRAMEFLOW_LOCATION_KIND_UNKNOWN;
        double latitude = 0.0;
        double longitude = 0.0;
        std::int32_t story_count = 0;
        std::optional<std::string> primary_category;
        std::optional<std::string> style_key;
    };

    struct RenderNode {
        enum class Kind {
            Location,
            Cluster
        };

        Kind kind = Kind::Location;
        std::int64_t target_id = 0;
        std::int32_t point_count = 1;
        double longitude = 0.0;
        double latitude = 0.0;
        double normalized_x = 0.0;
        double normalized_y = 0.0;
        double screen_x = 0.0;
        double screen_y = 0.0;
        double radius_normalized = 0.0;
        double radius_pixels = 0.0;
        std::optional<std::string> primary_category;
        std::optional<std::string> style_key;
        bool selected = false;
        bool focused = false;
        bool dimmed = false;
    };

    struct HudButtonLayout {
        HudAction action = HudAction::ZoomIn;
        double center_x = 0.0;
        double center_y = 0.0;
        double radius = 0.0;
    };

    struct HudStatusLayout {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    struct HudCompassLayout {
        double center_x = 0.0;
        double center_y = 0.0;
        double radius = 0.0;
    };

    struct HudLayout {
        double panel_x = 0.0;
        double panel_y = 0.0;
        double panel_width = 0.0;
        double panel_height = 0.0;
        std::array<HudButtonLayout, 3> buttons{};
        HudStatusLayout status{};
        HudCompassLayout compass{};
    };

    [[nodiscard]] std::vector<RenderNode> build_render_nodes() const;
    [[nodiscard]] HudLayout build_hud_layout() const noexcept;
    void log_imagery_runtime_state(bool rendered_imagery);

    CesiumGeospatial::Cartographic home_camera_;
    double home_heading_degrees_ = 0.0;
    double home_pitch_degrees_ = -35.0;
    double home_roll_degrees_ = 0.0;
    CesiumGeospatial::Cartographic camera_;
    double heading_degrees_ = 0.0;
    double pitch_degrees_ = -35.0;
    double roll_degrees_ = 0.0;
    std::vector<ScenePoint> points_;
    std::optional<std::int64_t> selected_location_id_;
    std::optional<std::int64_t> focus_location_id_;
    int width_ = 1;
    int height_ = 1;
    bool paused_ = false;
    unsigned int frames_rendered_ = 0;
    std::size_t visible_point_count_ = 0;
    std::size_t visible_cluster_count_ = 0;
    std::size_t rendered_marker_count_ = 0;
    std::size_t visible_country_boundary_count_ = 0;
    std::size_t rendered_country_boundary_segment_count_ = 0;
    std::int64_t last_scene_total_millis_ = 0;
    std::int64_t last_scene_clear_millis_ = 0;
    std::int64_t last_scene_tiles_millis_ = 0;
    std::int64_t last_scene_boundaries_millis_ = 0;
    std::int64_t last_scene_markers_millis_ = 0;
    std::int64_t last_scene_hud_millis_ = 0;
    std::string imagery_runtime_status_{"provider=none"};
    std::string last_logged_imagery_error_;
    bool imagery_disabled_logged_ = false;
    bool imagery_ready_logged_ = false;
    unsigned int last_imagery_wait_log_frame_ = 0;
    frameflow::renderer::cartography::CartographyDataset cartography_dataset_;
    std::unique_ptr<GoogleTilesetRuntime> google_tileset_runtime_;
    std::unique_ptr<UniformRasterGlobeLayer> uniform_raster_layer_;
};

} // namespace frameflow::renderer::cesium
