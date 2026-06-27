#include "software_globe_renderer.hpp"

#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
#include "renderer/cesium/glx_offscreen_scene_host.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

struct RgbaColor {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
    std::uint8_t alpha;
};

constexpr std::uint32_t kOffscreenBytesPerPixel = 4u;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegreesToRadians = kPi / 180.0;
constexpr double kRadiansToDegrees = 180.0 / kPi;
constexpr double kBitmapCameraMinHeightMeters = 1'500.0;
constexpr double kBitmapCameraMaxHeightMeters = 28'000'000.0;

struct GlobeCameraBasis {
    std::array<double, 3u> east;
    std::array<double, 3u> north;
    std::array<double, 3u> forward;
};

struct GlobeViewport {
    int center_x{};
    int center_y{};
    int radius{};
};

struct ProjectedPoint {
    int x{};
    int y{};
    double depth{};
    bool visible{false};
};

struct LonLatVertex {
    double longitude{};
    double latitude{};
};

bool has_offscreen_output_target(const frameflow_engine* engine) {
    return engine != nullptr && engine->surface_backend == FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP;
}

bool has_latest_offscreen_frame(const frameflow_engine* engine) {
    return has_offscreen_output_target(engine) && !engine->offscreen_frame.rgba_pixels.empty();
}

std::size_t offscreen_required_bytes(const frameflow_engine& engine) {
    return static_cast<std::size_t>(engine.offscreen_frame.stride_bytes) *
        static_cast<std::size_t>(std::max(0, engine.height));
}

void clear_offscreen_frame(frameflow_engine* engine) {
    if (engine == nullptr) {
        return;
    }
    engine->offscreen_frame.rgba_pixels.clear();
    engine->offscreen_frame.stride_bytes = 0u;
}

void set_pixel(
    frameflow_engine::OffscreenFrame& frame,
    const int width,
    const int height,
    const int x,
    const int y,
    const RgbaColor color
) {
    if (x < 0 || y < 0 || x >= width || y >= height || frame.stride_bytes == 0u) {
        return;
    }
    const auto offset = (static_cast<std::size_t>(y) * frame.stride_bytes) +
        (static_cast<std::size_t>(x) * kOffscreenBytesPerPixel);
    frame.rgba_pixels[offset + 0u] = color.red;
    frame.rgba_pixels[offset + 1u] = color.green;
    frame.rgba_pixels[offset + 2u] = color.blue;
    frame.rgba_pixels[offset + 3u] = color.alpha;
}

RgbaColor blend_colors(const RgbaColor base, const RgbaColor overlay, const double alpha) {
    const double clamped_alpha = std::clamp(alpha, 0.0, 1.0);
    const double inverse = 1.0 - clamped_alpha;
    auto channel = [clamped_alpha, inverse](const std::uint8_t base_channel, const std::uint8_t overlay_channel) {
        return static_cast<std::uint8_t>(std::clamp(
            (static_cast<double>(base_channel) * inverse) + (static_cast<double>(overlay_channel) * clamped_alpha),
            0.0,
            255.0
        ));
    };
    return RgbaColor{
        channel(base.red, overlay.red),
        channel(base.green, overlay.green),
        channel(base.blue, overlay.blue),
        255u
    };
}

RgbaColor lerp_color(const RgbaColor from, const RgbaColor to, const double progress) {
    return blend_colors(from, to, std::clamp(progress, 0.0, 1.0));
}

RgbaColor pixel_color_at(
    const frameflow_engine::OffscreenFrame& frame,
    const int width,
    const int height,
    const int x,
    const int y
) {
    if (x < 0 || y < 0 || x >= width || y >= height || frame.stride_bytes == 0u) {
        return RgbaColor{0u, 0u, 0u, 0u};
    }
    const auto offset = (static_cast<std::size_t>(y) * frame.stride_bytes) +
        (static_cast<std::size_t>(x) * kOffscreenBytesPerPixel);
    return RgbaColor{
        frame.rgba_pixels[offset + 0u],
        frame.rgba_pixels[offset + 1u],
        frame.rgba_pixels[offset + 2u],
        frame.rgba_pixels[offset + 3u]
    };
}

void blend_pixel(
    frameflow_engine::OffscreenFrame& frame,
    const int width,
    const int height,
    const int x,
    const int y,
    const RgbaColor overlay,
    const double alpha
) {
    if (x < 0 || y < 0 || x >= width || y >= height || frame.stride_bytes == 0u) {
        return;
    }
    const auto blended = blend_colors(pixel_color_at(frame, width, height, x, y), overlay, alpha);
    set_pixel(frame, width, height, x, y, blended);
}

void fill_disc(
    frameflow_engine::OffscreenFrame& frame,
    const int width,
    const int height,
    const int center_x,
    const int center_y,
    const int radius,
    const RgbaColor color
) {
    for (int y = center_y - radius; y <= center_y + radius; ++y) {
        for (int x = center_x - radius; x <= center_x + radius; ++x) {
            const int dx = x - center_x;
            const int dy = y - center_y;
            if ((dx * dx) + (dy * dy) <= (radius * radius)) {
                set_pixel(frame, width, height, x, y, color);
            }
        }
    }
}

RgbaColor background_color_for_theme(const std::string& theme) {
    if (theme == "light") {
        return RgbaColor{239u, 244u, 251u, 255u};
    }
    return RgbaColor{15u, 23u, 42u, 255u};
}

RgbaColor globe_ocean_color_for_theme(const std::string& theme) {
    if (theme == "light") {
        return RgbaColor{114u, 163u, 242u, 255u};
    }
    return RgbaColor{39u, 102u, 196u, 255u};
}

RgbaColor globe_deep_ocean_color_for_theme(const std::string& theme) {
    if (theme == "light") {
        return RgbaColor{56u, 112u, 201u, 255u};
    }
    return RgbaColor{17u, 49u, 111u, 255u};
}

RgbaColor globe_land_color_for_theme(const std::string& theme) {
    if (theme == "light") {
        return RgbaColor{118u, 168u, 116u, 255u};
    }
    return RgbaColor{67u, 140u, 90u, 255u};
}

RgbaColor globe_land_highlight_color_for_theme(const std::string& theme) {
    if (theme == "light") {
        return RgbaColor{168u, 201u, 126u, 255u};
    }
    return RgbaColor{123u, 179u, 106u, 255u};
}

RgbaColor globe_grid_color_for_theme(const std::string& theme) {
    if (theme == "light") {
        return RgbaColor{255u, 255u, 255u, 255u};
    }
    return RgbaColor{191u, 219u, 254u, 255u};
}

RgbaColor globe_atmosphere_color_for_theme(const std::string& theme) {
    if (theme == "light") {
        return RgbaColor{195u, 224u, 255u, 255u};
    }
    return RgbaColor{96u, 165u, 250u, 255u};
}

RgbaColor globe_horizon_color_for_theme(const std::string& theme) {
    if (theme == "light") {
        return RgbaColor{255u, 255u, 255u, 255u};
    }
    return RgbaColor{218u, 236u, 255u, 255u};
}

double normalize_longitude_degrees(const double longitude) {
    double normalized = std::fmod(longitude + 180.0, 360.0);
    if (normalized < 0.0) {
        normalized += 360.0;
    }
    return normalized - 180.0;
}

double wrapped_longitude_difference(const double left, const double right) {
    return normalize_longitude_degrees(left - right);
}

GlobeCameraBasis make_globe_camera_basis(const double longitude_degrees, const double latitude_degrees) {
    const double longitude_radians = normalize_longitude_degrees(longitude_degrees) * kDegreesToRadians;
    const double latitude_radians = std::clamp(latitude_degrees, -85.0, 85.0) * kDegreesToRadians;
    const double sin_longitude = std::sin(longitude_radians);
    const double cos_longitude = std::cos(longitude_radians);
    const double sin_latitude = std::sin(latitude_radians);
    const double cos_latitude = std::cos(latitude_radians);
    return GlobeCameraBasis{
        .east = {-sin_longitude, cos_longitude, 0.0},
        .north = {-sin_latitude * cos_longitude, -sin_latitude * sin_longitude, cos_latitude},
        .forward = {cos_latitude * cos_longitude, cos_latitude * sin_longitude, sin_latitude}
    };
}

double lerp_double(const double start, const double end, const double progress) {
    return start + ((end - start) * std::clamp(progress, 0.0, 1.0));
}

double normalized_bitmap_camera_height(const double height_meters) {
    const double span = kBitmapCameraMaxHeightMeters - kBitmapCameraMinHeightMeters;
    if (span <= 0.0) {
        return 0.0;
    }
    return std::clamp((height_meters - kBitmapCameraMinHeightMeters) / span, 0.0, 1.0);
}

GlobeViewport globe_viewport_for_frame(const int width, const int height, const double camera_height_meters) {
    const int min_dimension = std::max(1, std::min(width, height));
    const double radius_ratio = lerp_double(0.52, 0.22, normalized_bitmap_camera_height(camera_height_meters));
    const int max_radius = std::max(24, (min_dimension / 2) - 8);
    const int radius = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(min_dimension) * radius_ratio)),
        24,
        max_radius
    );
    const int vertical_padding = std::max(8, height / 32);
    return GlobeViewport{
        .center_x = width / 2,
        .center_y = std::clamp((height / 2) + vertical_padding, radius + 4, std::max(radius + 4, height - radius - 4)),
        .radius = radius
    };
}

template <std::size_t N>
bool point_in_polygon(
    const std::array<LonLatVertex, N>& polygon,
    const double longitude_degrees,
    const double latitude_degrees
) {
    if constexpr (N < 3u) {
        return false;
    }
    const double sample_longitude = normalize_longitude_degrees(longitude_degrees);
    bool inside = false;
    std::size_t previous_index = N - 1u;
    for (std::size_t current_index = 0u; current_index < N; ++current_index) {
        const auto& previous = polygon[previous_index];
        const auto& current = polygon[current_index];
        const double previous_longitude =
            sample_longitude + wrapped_longitude_difference(previous.longitude, sample_longitude);
        const double current_longitude =
            sample_longitude + wrapped_longitude_difference(current.longitude, sample_longitude);
        const bool crosses = (previous.latitude > latitude_degrees) != (current.latitude > latitude_degrees);
        if (crosses) {
            const double latitude_span = current.latitude - previous.latitude;
            if (std::abs(latitude_span) > 1e-9) {
                const double intersect_longitude = previous_longitude +
                    ((current_longitude - previous_longitude) * (latitude_degrees - previous.latitude) / latitude_span);
                if (sample_longitude < intersect_longitude) {
                    inside = !inside;
                }
            }
        }
        previous_index = current_index;
    }
    return inside;
}

bool is_land_pixel(const double longitude_degrees, const double latitude_degrees) {
    static constexpr std::array<LonLatVertex, 25u> kNorthAmerica{{
        {-168.0, 71.0}, {-154.0, 72.0}, {-144.0, 70.0}, {-136.0, 64.0}, {-130.0, 57.0},
        {-126.0, 50.0}, {-124.0, 44.0}, {-121.0, 38.0}, {-117.0, 33.0}, {-112.0, 30.0},
        {-106.0, 28.0}, {-100.0, 24.0}, {-94.0, 20.0}, {-89.0, 18.0}, {-85.0, 18.0},
        {-82.0, 22.0}, {-80.0, 27.0}, {-79.0, 31.0}, {-75.0, 37.0}, {-70.0, 44.0},
        {-63.0, 50.0}, {-58.0, 55.0}, {-60.0, 61.0}, {-72.0, 67.0}, {-96.0, 73.0}
    }};
    static constexpr std::array<LonLatVertex, 10u> kGreenland{{
        {-74.0, 60.0}, {-62.0, 60.0}, {-46.0, 63.0}, {-30.0, 69.0}, {-22.0, 76.0},
        {-28.0, 82.0}, {-44.0, 84.0}, {-60.0, 82.0}, {-72.0, 75.0}, {-75.0, 67.0}
    }};
    static constexpr std::array<LonLatVertex, 20u> kSouthAmerica{{
        {-81.0, 12.0}, {-78.0, 8.0}, {-76.0, 2.0}, {-79.0, -5.0}, {-81.0, -12.0},
        {-77.0, -18.0}, {-73.0, -25.0}, {-70.0, -33.0}, {-67.0, -40.0}, {-63.0, -47.0},
        {-56.0, -54.0}, {-49.0, -55.0}, {-43.0, -48.0}, {-38.0, -36.0}, {-35.0, -22.0},
        {-38.0, -10.0}, {-44.0, -2.0}, {-50.0, 4.0}, {-58.0, 8.0}, {-68.0, 12.0}
    }};
    static constexpr std::array<LonLatVertex, 19u> kAfrica{{
        {-18.0, 37.0}, {-10.0, 34.0}, {1.0, 37.0}, {12.0, 36.0}, {24.0, 33.0},
        {33.0, 31.0}, {39.0, 23.0}, {43.0, 12.0}, {51.0, 11.0}, {48.0, 2.0},
        {44.0, -11.0}, {39.0, -20.0}, {33.0, -28.0}, {24.0, -34.0}, {13.0, -35.0},
        {4.0, -30.0}, {-4.0, -20.0}, {-10.0, -5.0}, {-17.0, 20.0}
    }};
    static constexpr std::array<LonLatVertex, 35u> kEurasia{{
        {-10.0, 36.0}, {-9.0, 43.0}, {-5.0, 50.0}, {1.0, 57.0}, {9.0, 64.0},
        {20.0, 69.0}, {34.0, 71.0}, {52.0, 72.0}, {70.0, 73.0}, {88.0, 73.0},
        {106.0, 74.0}, {124.0, 73.0}, {140.0, 69.0}, {152.0, 62.0}, {160.0, 56.0},
        {155.0, 50.0}, {146.0, 44.0}, {135.0, 40.0}, {128.0, 34.0}, {123.0, 25.0},
        {118.0, 18.0}, {112.0, 12.0}, {107.0, 4.0}, {100.0, 1.0}, {93.0, 6.0},
        {87.0, 18.0}, {80.0, 23.0}, {72.0, 26.0}, {63.0, 25.0}, {57.0, 20.0},
        {51.0, 18.0}, {44.0, 17.0}, {33.0, 30.0}, {20.0, 39.0}, {5.0, 44.0}
    }};
    static constexpr std::array<LonLatVertex, 8u> kArabia{{
        {34.0, 31.0}, {44.0, 30.0}, {56.0, 25.0}, {56.0, 18.0},
        {51.0, 13.0}, {44.0, 12.0}, {39.0, 18.0}, {35.0, 25.0}
    }};
    static constexpr std::array<LonLatVertex, 8u> kIndia{{
        {68.0, 24.0}, {73.0, 29.0}, {82.0, 28.0}, {89.0, 23.0},
        {88.0, 17.0}, {82.0, 8.0}, {75.0, 7.0}, {70.0, 13.0}
    }};
    static constexpr std::array<LonLatVertex, 8u> kSoutheastAsia{{
        {95.0, 21.0}, {105.0, 22.0}, {113.0, 16.0}, {119.0, 9.0},
        {118.0, 0.0}, {112.0, -5.0}, {104.0, -6.0}, {97.0, 1.0}
    }};
    static constexpr std::array<LonLatVertex, 6u> kJapan{{
        {129.0, 31.0}, {138.0, 31.0}, {146.0, 40.0},
        {143.0, 45.0}, {136.0, 45.0}, {131.0, 39.0}
    }};
    static constexpr std::array<LonLatVertex, 4u> kBritishIsles{{
        {-11.0, 50.0}, {2.0, 50.0}, {2.0, 59.0}, {-7.0, 58.0}
    }};
    static constexpr std::array<LonLatVertex, 10u> kAustralia{{
        {112.0, -11.0}, {125.0, -10.0}, {138.0, -14.0}, {151.0, -24.0}, {153.0, -34.0},
        {146.0, -42.0}, {133.0, -44.0}, {121.0, -39.0}, {114.0, -30.0}, {111.0, -20.0}
    }};
    static constexpr std::array<LonLatVertex, 5u> kMadagascar{{
        {43.0, -13.0}, {50.0, -16.0}, {50.0, -25.0}, {45.0, -28.0}, {43.0, -21.0}
    }};
    static constexpr std::array<LonLatVertex, 4u> kNewZealand{{
        {166.0, -34.0}, {178.0, -37.0}, {176.0, -47.0}, {169.0, -47.0}
    }};

    const bool continental_land =
        latitude_degrees <= -72.0 ||
        point_in_polygon(kNorthAmerica, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kGreenland, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kSouthAmerica, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kAfrica, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kEurasia, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kArabia, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kIndia, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kSoutheastAsia, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kJapan, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kBritishIsles, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kAustralia, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kMadagascar, longitude_degrees, latitude_degrees) ||
        point_in_polygon(kNewZealand, longitude_degrees, latitude_degrees);
    if (continental_land) {
        return true;
    }

    auto island_blob = [longitude_degrees, latitude_degrees](
                           const double center_longitude,
                           const double center_latitude,
                           const double longitude_radius,
                           const double latitude_radius,
                           const double weight
                       ) {
        const double longitude_delta =
            wrapped_longitude_difference(longitude_degrees, center_longitude) / longitude_radius;
        const double latitude_delta = (latitude_degrees - center_latitude) / latitude_radius;
        return weight * std::exp(-((longitude_delta * longitude_delta) + (latitude_delta * latitude_delta)));
    };

    const double island_score =
        island_blob(-76.0, 20.0, 10.0, 5.5, 0.72) +   // Caribbean
        island_blob(122.0, -3.0, 18.0, 8.0, 0.92) +  // Indonesia
        island_blob(123.0, 12.0, 9.0, 7.0, 0.55) +   // Philippines
        island_blob(47.0, -19.0, 4.5, 7.0, 0.40) +   // Madagascar coastal reinforcement
        island_blob(173.0, -41.0, 5.5, 5.5, 0.45);   // New Zealand

    return island_score > 0.68;
}

RgbaColor marker_color_for_point(const frameflow::GeoPointAggregate& point, const bool selected) {
    if (selected) {
        return RgbaColor{252u, 211u, 77u, 255u};
    }
    switch (point.kind) {
        case frameflow::LocationKind::Country:
            return RgbaColor{244u, 114u, 182u, 255u};
        case frameflow::LocationKind::Region:
            return RgbaColor{249u, 115u, 22u, 255u};
        case frameflow::LocationKind::City:
            return RgbaColor{52u, 211u, 153u, 255u};
        case frameflow::LocationKind::Point:
            return RgbaColor{96u, 165u, 250u, 255u};
        case frameflow::LocationKind::Unknown:
        default:
            return RgbaColor{148u, 163u, 184u, 255u};
    }
}

int point_radius_pixels(const frameflow::GeoPointAggregate& point, const bool selected) {
    int radius = 4 + std::clamp(point.story_count, 1, 6);
    if (point.kind == frameflow::LocationKind::Region) {
        radius += 1;
    } else if (point.kind == frameflow::LocationKind::Country) {
        radius += 2;
    }
    if (selected) {
        radius += 2;
    }
    return radius;
}

ProjectedPoint project_point_to_bitmap(
    const frameflow::GeoPointAggregate& point,
    const GlobeCameraBasis& basis,
    const GlobeViewport& viewport
) {
    const double longitude_radians = point.longitude * kDegreesToRadians;
    const double latitude_radians = point.latitude * kDegreesToRadians;
    const std::array<double, 3u> world{
        std::cos(latitude_radians) * std::cos(longitude_radians),
        std::cos(latitude_radians) * std::sin(longitude_radians),
        std::sin(latitude_radians)
    };

    const double local_x = (world[0] * basis.east[0]) + (world[1] * basis.east[1]) + (world[2] * basis.east[2]);
    const double local_y = (world[0] * basis.north[0]) + (world[1] * basis.north[1]) + (world[2] * basis.north[2]);
    const double local_z = (world[0] * basis.forward[0]) + (world[1] * basis.forward[1]) + (world[2] * basis.forward[2]);
    if (local_z <= 0.0) {
        return ProjectedPoint{};
    }

    return ProjectedPoint{
        .x = static_cast<int>(std::lround(static_cast<double>(viewport.center_x) + (local_x * static_cast<double>(viewport.radius)))),
        .y = static_cast<int>(std::lround(static_cast<double>(viewport.center_y) - (local_y * static_cast<double>(viewport.radius)))),
        .depth = local_z,
        .visible = true
    };
}

void draw_line(
    frameflow_engine::OffscreenFrame& frame,
    const int width,
    const int height,
    const int start_x,
    const int start_y,
    const int end_x,
    const int end_y,
    const RgbaColor color,
    const double alpha
) {
    const int dx = end_x - start_x;
    const int dy = end_y - start_y;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps <= 0) {
        blend_pixel(frame, width, height, start_x, start_y, color, alpha);
        return;
    }

    for (int step = 0; step <= steps; ++step) {
        const double progress = static_cast<double>(step) / static_cast<double>(steps);
        const int x = static_cast<int>(std::lround(static_cast<double>(start_x) + (static_cast<double>(dx) * progress)));
        const int y = static_cast<int>(std::lround(static_cast<double>(start_y) + (static_cast<double>(dy) * progress)));
        blend_pixel(frame, width, height, x, y, color, alpha);
    }
}

std::optional<frameflow_engine::RuntimeCameraState> bitmap_camera_state_for_engine(
    const frameflow_engine* engine
) {
    if (engine == nullptr) {
        return std::nullopt;
    }
    if (engine->runtime_camera_state.has_value()) {
        return engine->runtime_camera_state;
    }
    return frameflow_engine::RuntimeCameraState{
        .longitude = 15.0,
        .latitude = 20.0,
        .height_meters = 8'500'000.0,
        .heading_degrees = 0.0,
        .pitch_degrees = -35.0,
        .roll_degrees = 0.0
    };
}

frameflow::FrameflowCameraState to_native_camera_state(
    const frameflow_engine::RuntimeCameraState& camera
) {
    return frameflow::FrameflowCameraState{
        .longitude = camera.longitude,
        .latitude = camera.latitude,
        .height_meters = camera.height_meters,
        .heading_degrees = camera.heading_degrees,
        .pitch_degrees = camera.pitch_degrees,
        .roll_degrees = camera.roll_degrees,
    };
}

frameflow::FrameflowSceneSnapshot native_scene_snapshot_for_engine(
    const frameflow_engine* engine
) {
    frameflow::FrameflowSceneSnapshot snapshot;
    if (engine == nullptr) {
        return snapshot;
    }
    const auto points = engine->engine.points();
    snapshot.points.assign(points.begin(), points.end());
    snapshot.selected_location_id = engine->engine.selected_location_id();
    snapshot.focus_location_id = engine->engine.filter().location_id;
    if (const auto camera = bitmap_camera_state_for_engine(engine); camera.has_value()) {
        snapshot.camera = to_native_camera_state(*camera);
    }
    return snapshot;
}

void sync_native_surface_runtime_scene(frameflow_engine* engine) {
    if (engine == nullptr || !engine->native_surface_runtime) {
        return;
    }
    engine->native_surface_runtime->update_scene_snapshot(native_scene_snapshot_for_engine(engine));
    engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
}

#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
frameflow::renderer::cesium::RenderedGlobeScene::CameraState
to_cesium_camera_state(const frameflow_engine::RuntimeCameraState& camera) {
    return frameflow::renderer::cesium::RenderedGlobeScene::CameraState{
        .longitude = camera.longitude,
        .latitude = camera.latitude,
        .height_meters = camera.height_meters,
        .heading_degrees = camera.heading_degrees,
        .pitch_degrees = camera.pitch_degrees,
        .roll_degrees = camera.roll_degrees,
    };
}

void sync_cesium_offscreen_renderer(frameflow_engine* engine) {
    if (engine == nullptr || !engine->cesium_offscreen_renderer) {
        return;
    }
    engine->cesium_offscreen_renderer->resize(std::max(1, engine->width), std::max(1, engine->height));
    engine->cesium_offscreen_renderer->set_points(engine->engine.points());
    engine->cesium_offscreen_renderer->set_selected_location(engine->engine.selected_location_id());
    engine->cesium_offscreen_renderer->set_focus_location(engine->engine.filter().location_id);
    if (engine->runtime_camera_state.has_value()) {
        engine->cesium_offscreen_renderer->set_camera_state(to_cesium_camera_state(*engine->runtime_camera_state));
    } else if (const auto camera = bitmap_camera_state_for_engine(engine); camera.has_value()) {
        engine->cesium_offscreen_renderer->set_camera_state(to_cesium_camera_state(*camera));
    }
    if (engine->lifecycle_state == FRAMEFLOW_LIFECYCLE_PAUSED) {
        engine->cesium_offscreen_renderer->pause();
    } else {
        engine->cesium_offscreen_renderer->resume();
    }
}

void ensure_cesium_offscreen_renderer(frameflow_engine* engine) {
    if (engine == nullptr ||
        !has_offscreen_output_target(engine) ||
        engine->cesium_offscreen_renderer ||
        engine->cesium_offscreen_renderer_attempted) {
        return;
    }

    engine->cesium_offscreen_renderer_attempted = true;
    const auto camera = bitmap_camera_state_for_engine(engine).value_or(
        frameflow_engine::RuntimeCameraState{
            .longitude = 15.0,
            .latitude = 20.0,
            .height_meters = 8'500'000.0,
            .heading_degrees = 0.0,
            .pitch_degrees = -35.0,
            .roll_degrees = 0.0,
        }
    );
    std::string error_message;
    engine->cesium_offscreen_renderer = frameflow::renderer::cesium::GlxOffscreenSceneHost::create(
        std::max(1, engine->width),
        std::max(1, engine->height),
        to_cesium_camera_state(camera),
        frameflow::renderer::cesium::RenderedGlobeScene::Options{
            .tile_cache_path = engine->tile_cache_path,
            .max_tile_cache_bytes = engine->max_tile_cache_bytes,
            .basemap_provider_id = engine->basemap_provider_id,
            .basemap_style_id = engine->basemap_style_id,
        },
        &error_message
    );
    if (!engine->cesium_offscreen_renderer) {
        engine->render_backend = "software-offscreen";
        engine->render_backend_diag = error_message;
        return;
    }
    engine->render_backend = "cesium-glx-offscreen";
    sync_cesium_offscreen_renderer(engine);
    engine->render_backend_diag = engine->cesium_offscreen_renderer->diagnostics_summary();
}
#endif

void render_globe_base(
    frameflow_engine* engine,
    const GlobeViewport& viewport,
    const GlobeCameraBasis& basis,
    const int width,
    const int height
) {
    auto& frame = engine->offscreen_frame;
    const auto atmosphere = globe_atmosphere_color_for_theme(engine->theme);
    const auto horizon = globe_horizon_color_for_theme(engine->theme);
    const auto deep_ocean = globe_deep_ocean_color_for_theme(engine->theme);
    const auto ocean = globe_ocean_color_for_theme(engine->theme);
    const auto land = globe_land_color_for_theme(engine->theme);
    const auto land_highlight = globe_land_highlight_color_for_theme(engine->theme);

    const int atmosphere_radius = viewport.radius + std::max(6, viewport.radius / 18);
    for (int y = viewport.center_y - atmosphere_radius; y <= viewport.center_y + atmosphere_radius; ++y) {
        for (int x = viewport.center_x - atmosphere_radius; x <= viewport.center_x + atmosphere_radius; ++x) {
            const double normalized_x = static_cast<double>(x - viewport.center_x) / static_cast<double>(viewport.radius);
            const double normalized_y = static_cast<double>(viewport.center_y - y) / static_cast<double>(viewport.radius);
            const double distance_squared = (normalized_x * normalized_x) + (normalized_y * normalized_y);

            if (distance_squared <= 1.0) {
                const double depth = std::sqrt(std::max(0.0, 1.0 - distance_squared));
                const std::array<double, 3u> world{
                    (normalized_x * basis.east[0]) + (normalized_y * basis.north[0]) + (depth * basis.forward[0]),
                    (normalized_x * basis.east[1]) + (normalized_y * basis.north[1]) + (depth * basis.forward[1]),
                    (normalized_x * basis.east[2]) + (normalized_y * basis.north[2]) + (depth * basis.forward[2])
                };

                const double latitude_degrees =
                    std::asin(std::clamp(world[2], -1.0, 1.0)) * kRadiansToDegrees;
                const double longitude_degrees =
                    std::atan2(world[1], world[0]) * kRadiansToDegrees;

                const bool is_land = is_land_pixel(longitude_degrees, latitude_degrees);
                const double lighting = std::clamp(0.48 + (depth * 0.42) + (normalized_x * 0.10) - (normalized_y * 0.06), 0.0, 1.0);
                const double polar_mix = std::clamp((std::abs(latitude_degrees) - 62.0) / 18.0, 0.0, 0.82);
                RgbaColor surface = is_land
                    ? lerp_color(land, land_highlight, lighting)
                    : lerp_color(deep_ocean, ocean, lighting);
                if (polar_mix > 0.0) {
                    surface = lerp_color(surface, horizon, polar_mix);
                }
                set_pixel(frame, width, height, x, y, surface);

                const double rim_mix = std::clamp((std::sqrt(distance_squared) - 0.90) / 0.10, 0.0, 1.0);
                if (rim_mix > 0.0) {
                    blend_pixel(frame, width, height, x, y, horizon, rim_mix * 0.45);
                }
            } else {
                const double distance = std::sqrt(distance_squared);
                const double atmosphere_mix = std::clamp((static_cast<double>(atmosphere_radius) / static_cast<double>(viewport.radius) - distance) / 0.12, 0.0, 1.0);
                if (atmosphere_mix > 0.0) {
                    blend_pixel(frame, width, height, x, y, atmosphere, atmosphere_mix * 0.32);
                }
            }
        }
    }
}

void render_globe_graticule(
    frameflow_engine* engine,
    const GlobeViewport& viewport,
    const GlobeCameraBasis& basis,
    const int width,
    const int height
) {
    auto& frame = engine->offscreen_frame;
    const auto grid = globe_grid_color_for_theme(engine->theme);
    const auto horizon = globe_horizon_color_for_theme(engine->theme);

    for (int latitude = -60; latitude <= 60; latitude += 30) {
        std::optional<ProjectedPoint> previous;
        for (int longitude = -180; longitude <= 180; longitude += 2) {
            frameflow::GeoPointAggregate sample;
            sample.latitude = static_cast<double>(latitude);
            sample.longitude = static_cast<double>(longitude);
            const auto projected = project_point_to_bitmap(sample, basis, viewport);
            if (projected.visible && previous.has_value() && previous->visible) {
                draw_line(frame, width, height, previous->x, previous->y, projected.x, projected.y, grid, latitude == 0 ? 0.26 : 0.18);
            }
            previous = projected.visible ? std::optional<ProjectedPoint>(projected) : std::nullopt;
        }
    }

    for (int longitude = -150; longitude <= 180; longitude += 30) {
        std::optional<ProjectedPoint> previous;
        for (int latitude = -85; latitude <= 85; latitude += 2) {
            frameflow::GeoPointAggregate sample;
            sample.latitude = static_cast<double>(latitude);
            sample.longitude = static_cast<double>(longitude);
            const auto projected = project_point_to_bitmap(sample, basis, viewport);
            if (projected.visible && previous.has_value() && previous->visible) {
                draw_line(frame, width, height, previous->x, previous->y, projected.x, projected.y, grid, longitude == 0 ? 0.24 : 0.16);
            }
            previous = projected.visible ? std::optional<ProjectedPoint>(projected) : std::nullopt;
        }
    }

    for (int step = 0; step < 180; ++step) {
        const double angle = (static_cast<double>(step) / 180.0) * 2.0 * kPi;
        const double next_angle = (static_cast<double>(step + 1) / 180.0) * 2.0 * kPi;
        const int start_x = static_cast<int>(std::lround(static_cast<double>(viewport.center_x) + (std::cos(angle) * static_cast<double>(viewport.radius))));
        const int start_y = static_cast<int>(std::lround(static_cast<double>(viewport.center_y) + (std::sin(angle) * static_cast<double>(viewport.radius))));
        const int end_x = static_cast<int>(std::lround(static_cast<double>(viewport.center_x) + (std::cos(next_angle) * static_cast<double>(viewport.radius))));
        const int end_y = static_cast<int>(std::lround(static_cast<double>(viewport.center_y) + (std::sin(next_angle) * static_cast<double>(viewport.radius))));
        draw_line(frame, width, height, start_x, start_y, end_x, end_y, horizon, 0.35);
    }
}

void refresh_offscreen_frame(frameflow_engine* engine) {
    if (!has_offscreen_output_target(engine)) {
        clear_offscreen_frame(engine);
        return;
    }

    const int width = std::max(1, engine->width);
    const int height = std::max(1, engine->height);
    auto& frame = engine->offscreen_frame;
    frame.stride_bytes = static_cast<std::uint32_t>(width) * kOffscreenBytesPerPixel;
    frame.rgba_pixels.assign(
        static_cast<std::size_t>(frame.stride_bytes) * static_cast<std::size_t>(height),
        0u
    );

#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
    ensure_cesium_offscreen_renderer(engine);
    if (engine->cesium_offscreen_renderer) {
        sync_cesium_offscreen_renderer(engine);
        std::string error_message;
        if (engine->cesium_offscreen_renderer->render_into_rgba(
                frame.rgba_pixels,
                frame.stride_bytes,
                &error_message
            )) {
            engine->render_backend = "cesium-glx-offscreen";
            engine->render_backend_diag = engine->cesium_offscreen_renderer->diagnostics_summary();
            frame.generation += 1u;
            engine->render_callback_count += 1u;
            return;
        }
        engine->render_backend = "software-offscreen";
        engine->render_backend_diag = error_message;
    } else if (engine->render_backend_diag.empty()) {
        engine->render_backend = "software-offscreen";
    }
#else
    engine->render_backend = "software-offscreen";
#endif

    const auto background = background_color_for_theme(engine->theme);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            set_pixel(frame, width, height, x, y, background);
        }
    }

    const auto camera = bitmap_camera_state_for_engine(engine);
    if (camera.has_value()) {
        const auto viewport = globe_viewport_for_frame(width, height, camera->height_meters);
        const auto basis = make_globe_camera_basis(camera->longitude, camera->latitude);
        render_globe_base(engine, viewport, basis, width, height);
        render_globe_graticule(engine, viewport, basis, width, height);

        for (const auto& point : engine->engine.points()) {
            const bool selected = engine->engine.selected_location_id().has_value() &&
                *engine->engine.selected_location_id() == point.location_id;
            const auto projected = project_point_to_bitmap(point, basis, viewport);
            if (!projected.visible) {
                continue;
            }
            if (selected) {
                fill_disc(
                    frame,
                    width,
                    height,
                    projected.x,
                    projected.y,
                    point_radius_pixels(point, true) + 3,
                    RgbaColor{255u, 255u, 255u, 90u}
                );
            }
            fill_disc(
                frame,
                width,
                height,
                projected.x,
                projected.y,
                point_radius_pixels(point, selected),
                marker_color_for_point(point, selected)
            );
        }
    }

    frame.generation += 1u;
    engine->render_callback_count += 1u;
}
