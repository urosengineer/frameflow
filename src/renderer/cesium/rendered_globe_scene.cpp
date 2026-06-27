#include "renderer/cesium/rendered_globe_scene.hpp"

#include <Cesium3DTilesSelection/EllipsoidTilesetLoader.h>
#include <Cesium3DTilesSelection/IPrepareRendererResources.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileContent.h>
#include <Cesium3DTilesSelection/TileID.h>
#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetExternals.h>
#include <Cesium3DTilesSelection/TilesetOptions.h>
#include <Cesium3DTilesSelection/TilesetViewGroup.h>
#include <Cesium3DTilesSelection/ViewState.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/CachingAssetAccessor.h>
#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>
#include <CesiumAsync/ITaskProcessor.h>
#include <CesiumAsync/SqliteCache.h>
#include <CesiumCurl/CurlAssetAccessor.h>
#include <CesiumGeometry/QuadtreeTileID.h>
#include <CesiumGltf/AccessorView.h>
#include <CesiumGltf/ImageAsset.h>
#include <CesiumGltf/Ktx2TranscodeTargets.h>
#include <CesiumGltfReader/ImageDecoder.h>
#include <CesiumRasterOverlays/GoogleMapTilesRasterOverlay.h>
#include <CesiumRasterOverlays/RasterOverlay.h>
#include <CesiumRasterOverlays/RasterOverlayTile.h>
#include <CesiumRasterOverlays/UrlTemplateRasterOverlay.h>
#include <CesiumUtility/CreditSystem.h>
#include <CesiumUtility/Math.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/geometric.hpp>
#include <spdlog/spdlog.h>

namespace frameflow::renderer::cesium {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegreesToRadians = kPi / 180.0;
constexpr double kClusterThresholdPixels = 9.0;
constexpr double kDefaultMinCameraHeightMeters = 50.0;
constexpr double kAbsoluteMinCameraHeightMeters = 5.0;
constexpr double kMaxCameraHeightMeters = 28000000.0;
constexpr double kCameraZoomInMultiplier = 0.70;
constexpr double kCameraZoomOutMultiplier = 1.0 / kCameraZoomInMultiplier;
constexpr double kNearGlobeRadius = 0.94;
constexpr double kFarGlobeRadius = 0.46;
constexpr double kHorizontalFieldOfViewDegrees = 55.0;
constexpr int kDefaultTilesetActivePumpIterations = 1;
constexpr int kDefaultTilesetSettlePumpIterations = 3;
constexpr std::uint32_t kDefaultTilesetMaxSimultaneousLoads = 16u;
constexpr std::uint32_t kDefaultOverlayMaxSimultaneousLoads = 16u;
constexpr std::uint32_t kDefaultExternalRasterOverlayMaxSimultaneousLoads = 8u;
constexpr std::uint32_t kDefaultLoadingDescendantLimit = 64u;
constexpr const char* kFrameflowLogLevelEnv = "FRAMEFLOW_LOG_LEVEL";
constexpr const char* kBasemapProviderEnv = "FRAMEFLOW_BASEMAP_PROVIDER";
constexpr const char* kBasemapHiDpiEnv = "FRAMEFLOW_BASEMAP_HIDPI";
constexpr const char* kGoogleMapsApiKeyEnv = "FRAMEFLOW_GOOGLE_MAPS_API_KEY";
constexpr const char* kGoogleMapsLanguageEnv = "FRAMEFLOW_GOOGLE_MAPS_LANGUAGE";
constexpr const char* kGoogleMapsRegionEnv = "FRAMEFLOW_GOOGLE_MAPS_REGION";
constexpr const char* kGoogleMapsTilesetLoadsEnv = "FRAMEFLOW_GOOGLE_TILE_LOADS";
constexpr const char* kGoogleMapsOverlayLoadsEnv = "FRAMEFLOW_GOOGLE_OVERLAY_LOADS";
constexpr const char* kGoogleMapsQuotaBackoffSecondsEnv = "FRAMEFLOW_GOOGLE_QUOTA_BACKOFF_SECONDS";
constexpr const char* kGoogleMapsPreloadSiblingsEnv = "FRAMEFLOW_GOOGLE_PRELOAD_SIBLINGS";
constexpr const char* kMapTilerApiKeyEnv = "FRAMEFLOW_MAPTILER_API_KEY";
constexpr const char* kMapTilerMapIdEnv = "FRAMEFLOW_MAPTILER_MAP_ID";
constexpr const char* kStadiaApiKeyEnv = "FRAMEFLOW_STADIA_API_KEY";
constexpr const char* kStadiaStyleEnv = "FRAMEFLOW_STADIA_STYLE";
constexpr const char* kTilesetPumpIterationsEnv = "FRAMEFLOW_TILESET_PUMP_ITERATIONS";
constexpr const char* kTilesetActivePumpIterationsEnv = "FRAMEFLOW_TILESET_ACTIVE_PUMP_ITERATIONS";
constexpr const char* kTilesetSettlePumpIterationsEnv = "FRAMEFLOW_TILESET_SETTLE_PUMP_ITERATIONS";
constexpr const char* kTilesetMaxSseEnv = "FRAMEFLOW_TILESET_MAX_SSE";
constexpr const char* kRasterMaxSseEnv = "FRAMEFLOW_RASTER_MAX_SSE";
constexpr const char* kMinCameraHeightMetersEnv = "FRAMEFLOW_MIN_CAMERA_HEIGHT_METERS";
constexpr const char* kCountryBoundaryOverlayEnv = "FRAMEFLOW_COUNTRY_BOUNDARY_OVERLAY";
constexpr const char* kDebugTileLodColorsEnv = "FRAMEFLOW_DEBUG_TILE_LOD_COLORS";
constexpr const char* kDebugSingleTileLevelEnv = "FRAMEFLOW_DEBUG_SINGLE_TILE_LEVEL";
constexpr const char* kDebugOnlyMaxLodEnv = "FRAMEFLOW_DEBUG_ONLY_MAX_LOD";
constexpr const char* kDebugDisableParentTilesEnv = "FRAMEFLOW_DEBUG_DISABLE_PARENT_TILES";
constexpr const char* kDebugUvClipEnv = "FRAMEFLOW_DEBUG_UV_CLIP";
constexpr double kHudMarginPixels = 14.0;
constexpr double kHudPanelPaddingPixels = 9.0;
constexpr double kHudButtonGapPixels = 6.0;
constexpr double kHudButtonMinDiameterPixels = 30.0;
constexpr double kHudButtonMaxDiameterPixels = 36.0;
constexpr double kHudStatusHeightPixels = 6.0;
constexpr double kHudStatusGapPixels = 9.0;
constexpr double kHudCompassGapPixels = 9.0;
constexpr double kHudCompassMinDiameterPixels = 36.0;

using SceneClock = std::chrono::steady_clock;

std::int64_t scene_elapsed_millis(const SceneClock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SceneClock::now() - start).count();
}
constexpr double kHudCompassMaxDiameterPixels = 44.0;
constexpr double kHudPanelMinWidthPixels = 82.0;

struct MarkerColor {
    GLfloat red;
    GLfloat green;
    GLfloat blue;
};

struct CountryBorderStyle {
    float line_red = 0.72F;
    float line_green = 0.86F;
    float line_blue = 0.98F;
    float line_alpha = 0.85F;
    float halo_red = 0.03F;
    float halo_green = 0.06F;
    float halo_blue = 0.10F;
    float halo_alpha = 0.54F;
    float line_width = 1.3F;
    float halo_width = 2.9F;
};

struct ProjectedPoint {
    double x = 0.0;
    double y = 0.0;
    double depth = 1.0;
    bool visible = false;
};

struct ProjectedBoundarySegment {
    double start_x = 0.0;
    double start_y = 0.0;
    double end_x = 0.0;
    double end_y = 0.0;
};

struct ProjectedCountryBoundaries {
    std::vector<ProjectedBoundarySegment> segments;
    std::size_t visible_boundary_count = 0u;
};

struct PerspectiveProjection {
    glm::dvec3 position{0.0, 0.0, 0.0};
    glm::dvec3 right{1.0, 0.0, 0.0};
    glm::dvec3 up{0.0, 1.0, 0.0};
    glm::dvec3 forward{0.0, 0.0, -1.0};
    double tan_half_horizontal_fov = 1.0;
    double tan_half_vertical_fov = 1.0;
    double near_distance = 1.0;
    double far_distance = 1.0;
};

constexpr std::array<MarkerColor, 6> kMarkerPalette{{
    {0.95F, 0.38F, 0.34F},
    {0.95F, 0.61F, 0.24F},
    {0.28F, 0.72F, 0.43F},
    {0.22F, 0.66F, 0.88F},
    {0.61F, 0.42F, 0.89F},
    {0.91F, 0.32F, 0.62F},
}};

std::uint32_t fnv1a_hash(const std::string_view value) {
    std::uint32_t hash = 2166136261u;
    for (const char byte : value) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(byte));
        hash *= 16777619u;
    }
    return hash;
}

std::int64_t stable_cluster_id(std::vector<std::int64_t> location_ids) {
    std::sort(location_ids.begin(), location_ids.end());

    std::uint64_t hash = 1469598103934665603ull;
    for (const std::int64_t location_id : location_ids) {
        std::uint64_t value = static_cast<std::uint64_t>(location_id);
        for (int byte_index = 0; byte_index < 8; ++byte_index) {
            const auto byte = static_cast<unsigned char>((value >> (byte_index * 8)) & 0xffull);
            hash ^= static_cast<std::uint64_t>(byte);
            hash *= 1099511628211ull;
        }
    }

    hash &= 0x7fffffffffffffffull;
    if (hash == 0ull) {
        hash = 1ull;
    }
    return static_cast<std::int64_t>(hash);
}

double marker_radius(
    const std::int32_t story_count,
    const frameflow_location_kind kind,
    const bool selected
) {
    const double clamped_story_count = static_cast<double>(std::clamp(story_count, 1, 24));
    double radius = 0.018 + (clamped_story_count * 0.0016);
    if (kind == FRAMEFLOW_LOCATION_KIND_REGION) {
        radius += 0.004;
    } else if (kind == FRAMEFLOW_LOCATION_KIND_COUNTRY) {
        radius += 0.008;
    }
    if (selected) {
        radius += 0.010;
    }
    return radius;
}

double normalized_radius_to_pixels(const double radius_normalized, const int width, const int height) {
    const double shortest_edge = static_cast<double>(std::max(1, std::min(width, height)));
    return radius_normalized * (shortest_edge * 0.5);
}

double pixels_to_normalized_radius(const double radius_pixels, const int width, const int height) {
    const double shortest_edge = static_cast<double>(std::max(1, std::min(width, height)));
    return radius_pixels / (shortest_edge * 0.5);
}

double cluster_radius_pixels(const std::int32_t point_count) {
    const double clamped_count = static_cast<double>(std::clamp(point_count, 2, 12));
    return 18.0 + ((clamped_count - 2.0) * 2.4);
}

double normalize_longitude_degrees(double longitude) {
    while (longitude <= -180.0) {
        longitude += 360.0;
    }
    while (longitude > 180.0) {
        longitude -= 360.0;
    }
    return longitude;
}

std::optional<std::string> env_string(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool renderer_debug_logging_enabled() {
    const auto level = env_string(kFrameflowLogLevelEnv);
    if (!level.has_value()) {
        return false;
    }
    const std::string normalized = lowercase_ascii(*level);
    return normalized == "debug" || normalized == "trace";
}

bool env_flag_enabled(const char* name) {
    const auto value = env_string(name);
    if (!value.has_value()) {
        return false;
    }

    return *value != "0" && *value != "false" && *value != "FALSE" && *value != "off" && *value != "OFF";
}

bool env_flag_enabled(const char* name, const bool default_value) {
    const auto value = env_string(name);
    if (!value.has_value()) {
        return default_value;
    }

    return *value != "0" && *value != "false" && *value != "FALSE" && *value != "off" && *value != "OFF";
}

std::optional<int> env_int(const char* name) {
    const auto value = env_string(name);
    if (!value.has_value()) {
        return std::nullopt;
    }

    try {
        return std::stoi(*value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> env_double(const char* name) {
    const auto value = env_string(name);
    if (!value.has_value()) {
        return std::nullopt;
    }

    try {
        return std::stod(*value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

double min_camera_height_meters() {
    const auto configured = env_double(kMinCameraHeightMetersEnv);
    if (!configured.has_value() || !std::isfinite(*configured)) {
        return kDefaultMinCameraHeightMeters;
    }
    return std::clamp(*configured, kAbsoluteMinCameraHeightMeters, kMaxCameraHeightMeters - 1.0);
}

enum class BasemapProviderKind {
    Disabled,
    GoogleSatellite,
    MapTilerRaster,
    StadiaRaster
};

struct BasemapProviderConfig {
    BasemapProviderKind kind = BasemapProviderKind::Disabled;
    std::string provider_id = "none";
    std::string api_key;
    std::string language = "en-US";
    std::string region = "US";
    std::string style_id;
    bool hidpi = false;
};

std::uint32_t default_overlay_max_simultaneous_loads(const BasemapProviderKind provider_kind) {
    switch (provider_kind) {
        case BasemapProviderKind::MapTilerRaster:
        case BasemapProviderKind::StadiaRaster:
            return kDefaultExternalRasterOverlayMaxSimultaneousLoads;
        case BasemapProviderKind::GoogleSatellite:
        case BasemapProviderKind::Disabled:
            return kDefaultOverlayMaxSimultaneousLoads;
    }
    return kDefaultOverlayMaxSimultaneousLoads;
}

std::string normalize_provider_id(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::optional<BasemapProviderConfig> basemap_provider_from_request(
    std::string requested_provider,
    std::string requested_style_id,
    std::string* status_message
) {
    requested_provider = normalize_provider_id(std::move(requested_provider));
    const auto maptiler_key = env_string(kMapTilerApiKeyEnv);
    const auto stadia_key = env_string(kStadiaApiKeyEnv);
    const auto google_key = env_string(kGoogleMapsApiKeyEnv);
    const bool hidpi = env_flag_enabled(kBasemapHiDpiEnv);

    auto missing_provider_key = [status_message](const char* provider, const char* env_name) {
        if (status_message != nullptr) {
            *status_message = std::string("provider=") + provider + " status=disabled missing_env=" + env_name;
        }
        return std::optional<BasemapProviderConfig>{};
    };

    const bool wants_maptiler = requested_provider == "maptiler" || requested_provider == "maptiler-raster";
    if (wants_maptiler || (requested_provider == "auto" && maptiler_key.has_value())) {
        if (!maptiler_key.has_value()) {
            return missing_provider_key("maptiler-raster", kMapTilerApiKeyEnv);
        }
        return BasemapProviderConfig{
            .kind = BasemapProviderKind::MapTilerRaster,
            .provider_id = "maptiler-raster",
            .api_key = *maptiler_key,
            .style_id = requested_style_id.empty()
                ? env_string(kMapTilerMapIdEnv).value_or("streets-v4")
                : std::move(requested_style_id),
            .hidpi = hidpi
        };
    }

    const bool wants_stadia = requested_provider == "stadia" || requested_provider == "stadia-raster";
    if (wants_stadia || (requested_provider == "auto" && stadia_key.has_value())) {
        if (!stadia_key.has_value()) {
            return missing_provider_key("stadia-raster", kStadiaApiKeyEnv);
        }
        return BasemapProviderConfig{
            .kind = BasemapProviderKind::StadiaRaster,
            .provider_id = "stadia-raster",
            .api_key = *stadia_key,
            .style_id = requested_style_id.empty()
                ? env_string(kStadiaStyleEnv).value_or("alidade_smooth_dark")
                : std::move(requested_style_id),
            .hidpi = hidpi
        };
    }

    const bool wants_google = requested_provider == "google" ||
        requested_provider == "google-satellite" ||
        requested_provider == "google-maps-satellite";
    if (wants_google || (requested_provider == "auto" && google_key.has_value())) {
        if (!google_key.has_value()) {
            return missing_provider_key("google-maps-satellite", kGoogleMapsApiKeyEnv);
        }
        return BasemapProviderConfig{
            .kind = BasemapProviderKind::GoogleSatellite,
            .provider_id = "google-maps-satellite",
            .api_key = *google_key,
            .language = env_string(kGoogleMapsLanguageEnv).value_or("en-US"),
            .region = env_string(kGoogleMapsRegionEnv).value_or("US"),
            .style_id = requested_style_id.empty() ? "satellite" : std::move(requested_style_id),
            .hidpi = hidpi
        };
    }

    if (status_message != nullptr) {
        *status_message = "provider=none status=disabled missing_env=FRAMEFLOW_MAPTILER_API_KEY";
    }
    return std::nullopt;
}

std::optional<BasemapProviderConfig> basemap_provider_from_environment(std::string* status_message) {
    return basemap_provider_from_request(
        env_string(kBasemapProviderEnv).value_or("auto"),
        std::string{},
        status_message
    );
}

std::optional<BasemapProviderConfig> basemap_provider_from_options(
    const RenderedGlobeScene::Options& options,
    std::string* status_message
) {
    if (options.basemap_provider_id.empty()) {
        return basemap_provider_from_environment(status_message);
    }
    return basemap_provider_from_request(options.basemap_provider_id, options.basemap_style_id, status_message);
}

std::uint64_t sqlite_cache_max_items_from_bytes(std::uint64_t max_cache_bytes);

std::shared_ptr<CesiumAsync::IAssetAccessor> create_frameflow_asset_accessor(
    const RenderedGlobeScene::Options& scene_options,
    std::string* asset_cache_status
) {
    auto curl_accessor = std::make_shared<CesiumCurl::CurlAssetAccessor>();
    if (scene_options.tile_cache_path.empty() || scene_options.max_tile_cache_bytes == 0u) {
        if (asset_cache_status != nullptr) {
            *asset_cache_status = "disabled";
        }
        return curl_accessor;
    }

    try {
        const std::filesystem::path cache_root(scene_options.tile_cache_path);
        std::filesystem::create_directories(cache_root);
        const std::filesystem::path database_path = cache_root / "cesium-assets.sqlite3";
        const auto max_items = sqlite_cache_max_items_from_bytes(scene_options.max_tile_cache_bytes);
        auto cache_database = std::make_shared<CesiumAsync::SqliteCache>(
            spdlog::default_logger(),
            database_path.string(),
            max_items
        );
        if (asset_cache_status != nullptr) {
            *asset_cache_status = "sqlite:" + std::to_string(max_items);
        }
        return std::make_shared<CesiumAsync::CachingAssetAccessor>(
            spdlog::default_logger(),
            curl_accessor,
            cache_database
        );
    } catch (const std::exception& ex) {
        if (asset_cache_status != nullptr) {
            *asset_cache_status = std::string("failed:") + ex.what();
        }
        return curl_accessor;
    }
}

std::vector<std::byte> flip_image_vertically_rgba(const CesiumGltf::ImageAsset& image) {
    const auto row_bytes = static_cast<std::size_t>(image.width * image.channels * image.bytesPerChannel);
    const auto height = static_cast<std::size_t>(image.height);
    if (row_bytes == 0u || height == 0u || image.pixelData.size() < row_bytes * height) {
        return image.pixelData;
    }

    std::vector<std::byte> flipped(image.pixelData.size());
    for (std::size_t row = 0u; row < height; ++row) {
        const auto source_offset = row * row_bytes;
        const auto target_offset = (height - 1u - row) * row_bytes;
        std::copy_n(
            image.pixelData.data() + source_offset,
            row_bytes,
            flipped.data() + target_offset
        );
    }
    return flipped;
}

std::string redact_url_query_value(std::string message, const std::string_view key_name) {
    std::size_t search_from = 0u;
    const std::string needle = std::string(key_name) + "=";
    while (true) {
        const std::size_t key_start = message.find(needle, search_from);
        if (key_start == std::string::npos) {
            return message;
        }
        const std::size_t value_start = key_start + needle.size();
        const std::size_t value_end = message.find_first_of("& \n\r\t.", value_start);
        const std::size_t replace_count = (value_end == std::string::npos)
            ? std::string::npos
            : value_end - value_start;
        message.replace(value_start, replace_count, "<redacted>");
        search_from = value_start + 10u;
    }
}

std::string sanitize_tile_error_message(const std::string_view message) {
    return redact_url_query_value(redact_url_query_value(std::string(message), "api_key"), "key");
}

bool is_google_quota_error(const std::string_view message) {
    return message.find("429") != std::string_view::npos ||
        message.find("Too Many Requests") != std::string_view::npos ||
        message.find("quotaExceeded") != std::string_view::npos ||
        message.find("rateLimitExceeded") != std::string_view::npos ||
        message.find("RESOURCE_EXHAUSTED") != std::string_view::npos;
}

std::uint64_t sqlite_cache_max_items_from_bytes(const std::uint64_t max_cache_bytes) {
    if (max_cache_bytes == 0u) {
        return 0u;
    }

    constexpr std::uint64_t kApproxAverageTileBytes = 64u * 1024u;
    constexpr std::uint64_t kMinCacheItems = 1024u;
    constexpr std::uint64_t kMaxCacheItems = 200000u;
    return std::clamp(max_cache_bytes / kApproxAverageTileBytes, kMinCacheItems, kMaxCacheItems);
}

double normalize_signed_degrees(double value) {
    while (value <= -180.0) {
        value += 360.0;
    }
    while (value > 180.0) {
        value -= 360.0;
    }
    return value;
}

glm::dvec3 rotate_vector_around_axis(
    const glm::dvec3& vector,
    const glm::dvec3& axis,
    const double angle_radians
) {
    const glm::dvec3 normalized_axis = glm::normalize(axis);
    return (vector * std::cos(angle_radians)) +
        (glm::cross(normalized_axis, vector) * std::sin(angle_radians)) +
        (normalized_axis * glm::dot(normalized_axis, vector) * (1.0 - std::cos(angle_radians)));
}

PerspectiveProjection make_perspective_projection(
    const RenderedGlobeScene::CameraState& camera,
    const int width,
    const int height
) {
    constexpr double kWgs84RadiusMeters = 6378137.0;

    PerspectiveProjection projection;
    const CesiumGeospatial::Cartographic cartographic = CesiumGeospatial::Cartographic::fromDegrees(
        camera.longitude,
        camera.latitude,
        camera.height_meters
    );
    projection.position = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(cartographic);

    const glm::dvec3 focus = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(
        CesiumGeospatial::Cartographic::fromDegrees(camera.longitude, camera.latitude, 0.0)
    );
    projection.forward = glm::normalize(focus - projection.position);

    glm::dvec3 up_hint(0.0, 0.0, 1.0);
    projection.right = glm::cross(projection.forward, up_hint);
    if (glm::length(projection.right) <= 1e-9) {
        up_hint = glm::dvec3(0.0, 1.0, 0.0);
        projection.right = glm::cross(projection.forward, up_hint);
    }
    projection.right = glm::normalize(projection.right);
    projection.up = glm::normalize(glm::cross(projection.right, projection.forward));

    const double screen_rotation_radians = (-camera.heading_degrees + camera.roll_degrees) * kDegreesToRadians;
    if (std::abs(screen_rotation_radians) > 1e-9) {
        projection.right = glm::normalize(rotate_vector_around_axis(projection.right, projection.forward, screen_rotation_radians));
        projection.up = glm::normalize(rotate_vector_around_axis(projection.up, projection.forward, screen_rotation_radians));
    }

    const double horizontal_field_of_view = kHorizontalFieldOfViewDegrees * kDegreesToRadians;
    const double aspect_ratio = static_cast<double>(std::max(1, width)) / static_cast<double>(std::max(1, height));
    const double vertical_field_of_view =
        std::atan(std::tan(horizontal_field_of_view * 0.5) / std::max(0.0001, aspect_ratio)) * 2.0;

    projection.tan_half_horizontal_fov = std::tan(horizontal_field_of_view * 0.5);
    projection.tan_half_vertical_fov = std::tan(vertical_field_of_view * 0.5);
    projection.near_distance = std::max(25.0, camera.height_meters * 0.02);
    projection.far_distance = std::max(
        projection.near_distance + 1.0,
        camera.height_meters + (kWgs84RadiusMeters * 3.0)
    );
    return projection;
}

ProjectedPoint project_world_point(
    const glm::dvec3& ecef,
    const PerspectiveProjection& projection
) {
    // Surface content on the far side of the globe should not project through the planet.
    const glm::dvec3 surface_normal = glm::normalize(ecef);
    const glm::dvec3 to_camera = projection.position - ecef;
    if (glm::dot(surface_normal, to_camera) <= 0.0) {
        return {};
    }

    const glm::dvec3 delta = ecef - projection.position;
    const double forward_distance = glm::dot(delta, projection.forward);
    if (forward_distance <= projection.near_distance || forward_distance >= projection.far_distance) {
        return {};
    }

    const double half_width = forward_distance * projection.tan_half_horizontal_fov;
    const double half_height = forward_distance * projection.tan_half_vertical_fov;
    if (half_width <= 1e-9 || half_height <= 1e-9) {
        return {};
    }

    ProjectedPoint projected;
    projected.x = glm::dot(delta, projection.right) / half_width;
    projected.y = glm::dot(delta, projection.up) / half_height;
    projected.depth =
        (((forward_distance - projection.near_distance) /
          (projection.far_distance - projection.near_distance)) * 2.0) - 1.0;
    projected.visible = true;
    return projected;
}

void set_tile_perspective_matrices(const PerspectiveProjection& projection) {
    const double near_distance = std::max(0.001, projection.near_distance);
    const double far_distance = std::max(near_distance + 1.0, projection.far_distance);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(
        -near_distance * projection.tan_half_horizontal_fov,
        near_distance * projection.tan_half_horizontal_fov,
        -near_distance * projection.tan_half_vertical_fov,
        near_distance * projection.tan_half_vertical_fov,
        near_distance,
        far_distance
    );

    const GLdouble modelview[16] = {
        projection.right.x,
        projection.up.x,
        -projection.forward.x,
        0.0,
        projection.right.y,
        projection.up.y,
        -projection.forward.y,
        0.0,
        projection.right.z,
        projection.up.z,
        -projection.forward.z,
        0.0,
        -glm::dot(projection.right, projection.position),
        -glm::dot(projection.up, projection.position),
        glm::dot(projection.forward, projection.position),
        1.0
    };
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixd(modelview);
}

void set_normalized_ortho_matrices() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

bool projected_within_viewport(const ProjectedPoint& point) {
    return point.visible &&
        point.x >= -1.0 &&
        point.x <= 1.0 &&
        point.y >= -1.0 &&
        point.y <= 1.0;
}

int clip_code(const double x, const double y) {
    int code = 0;
    if (x < -1.0) {
        code |= 1;
    } else if (x > 1.0) {
        code |= 2;
    }
    if (y < -1.0) {
        code |= 4;
    } else if (y > 1.0) {
        code |= 8;
    }
    return code;
}

bool clip_segment_to_viewport(double* start_x, double* start_y, double* end_x, double* end_y) {
    int start_code = clip_code(*start_x, *start_y);
    int end_code = clip_code(*end_x, *end_y);

    while (true) {
        if ((start_code | end_code) == 0) {
            return true;
        }
        if ((start_code & end_code) != 0) {
            return false;
        }

        const int outside_code = start_code != 0 ? start_code : end_code;
        double clipped_x = 0.0;
        double clipped_y = 0.0;

        if ((outside_code & 8) != 0) {
            clipped_x = *start_x + ((*end_x - *start_x) * (1.0 - *start_y) / (*end_y - *start_y));
            clipped_y = 1.0;
        } else if ((outside_code & 4) != 0) {
            clipped_x = *start_x + ((*end_x - *start_x) * (-1.0 - *start_y) / (*end_y - *start_y));
            clipped_y = -1.0;
        } else if ((outside_code & 2) != 0) {
            clipped_y = *start_y + ((*end_y - *start_y) * (1.0 - *start_x) / (*end_x - *start_x));
            clipped_x = 1.0;
        } else {
            clipped_y = *start_y + ((*end_y - *start_y) * (-1.0 - *start_x) / (*end_x - *start_x));
            clipped_x = -1.0;
        }

        if (outside_code == start_code) {
            *start_x = clipped_x;
            *start_y = clipped_y;
            start_code = clip_code(*start_x, *start_y);
        } else {
            *end_x = clipped_x;
            *end_y = clipped_y;
            end_code = clip_code(*end_x, *end_y);
        }
    }
}

frameflow_location_kind to_bridge_kind(const frameflow::LocationKind kind) {
    switch (kind) {
        case frameflow::LocationKind::Point:
            return FRAMEFLOW_LOCATION_KIND_POINT;
        case frameflow::LocationKind::City:
            return FRAMEFLOW_LOCATION_KIND_CITY;
        case frameflow::LocationKind::Region:
            return FRAMEFLOW_LOCATION_KIND_REGION;
        case frameflow::LocationKind::Country:
            return FRAMEFLOW_LOCATION_KIND_COUNTRY;
        case frameflow::LocationKind::Unknown:
        default:
            return FRAMEFLOW_LOCATION_KIND_UNKNOWN;
    }
}

MarkerColor marker_color(
    const std::optional<std::string>& style_key,
    const std::optional<std::string>& primary_category,
    const bool selected
) {
    if (selected) {
        return MarkerColor{1.0F, 0.84F, 0.20F};
    }

    if (style_key.has_value()) {
        return kMarkerPalette[fnv1a_hash(*style_key) % kMarkerPalette.size()];
    }
    if (primary_category.has_value()) {
        return kMarkerPalette[fnv1a_hash(*primary_category) % kMarkerPalette.size()];
    }
    return MarkerColor{0.82F, 0.88F, 0.95F};
}

MarkerColor cluster_color() {
    return MarkerColor{0.96F, 0.69F, 0.24F};
}

double normalized_camera_height(const double height_meters) {
    const double min_camera_height = min_camera_height_meters();
    const double span = kMaxCameraHeightMeters - min_camera_height;
    if (span <= 0.0) {
        return 0.0;
    }
    return std::clamp((height_meters - min_camera_height) / span, 0.0, 1.0);
}

double globe_radius_for_camera_height(const double height_meters) {
    const double progress = normalized_camera_height(height_meters);
    return kNearGlobeRadius + ((kFarGlobeRadius - kNearGlobeRadius) * progress);
}

ProjectedPoint project_point(
    const double latitude_degrees,
    const double longitude_degrees,
    const RenderedGlobeScene::CameraState& camera,
    const int width,
    const int height
) {
    const glm::dvec3 point = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(
        CesiumGeospatial::Cartographic(
            longitude_degrees * kDegreesToRadians,
            latitude_degrees * kDegreesToRadians,
            0.0
        )
    );
    return project_world_point(point, make_perspective_projection(camera, width, height));
}

ProjectedPoint project_coordinate(
    const frameflow::renderer::cartography::CartographyCoordinate& coordinate,
    const PerspectiveProjection& projection
) {
    const glm::dvec3 point = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(
        CesiumGeospatial::Cartographic(
            coordinate.longitude * kDegreesToRadians,
            coordinate.latitude * kDegreesToRadians,
            0.0
        )
    );
    return project_world_point(point, projection);
}

double normalized_to_screen_x(const double normalized_x, const int width) {
    return ((normalized_x + 1.0) * 0.5) * static_cast<double>(width);
}

double normalized_to_screen_y(const double normalized_y, const int height) {
    return ((1.0 - normalized_y) * 0.5) * static_cast<double>(height);
}

void draw_filled_circle(
    double center_x,
    double center_y,
    double radius,
    float red,
    float green,
    float blue,
    float alpha
);

void draw_circle_outline(
    double center_x,
    double center_y,
    double radius,
    float red,
    float green,
    float blue,
    float alpha,
    float line_width
);

void draw_filled_triangle(
    double x0,
    double y0,
    double x1,
    double y1,
    double x2,
    double y2,
    float red,
    float green,
    float blue,
    float alpha
);

void draw_filled_circle(
    const double center_x,
    const double center_y,
    const double radius,
    const float red,
    const float green,
    const float blue,
    const float alpha
) {
    constexpr int circle_segments = 48;
    glColor4f(red, green, blue, alpha);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(static_cast<GLfloat>(center_x), static_cast<GLfloat>(center_y));
    for (int segment = 0; segment <= circle_segments; ++segment) {
        const double angle = (2.0 * kPi * static_cast<double>(segment)) / static_cast<double>(circle_segments);
        glVertex2f(
            static_cast<GLfloat>(center_x + (std::cos(angle) * radius)),
            static_cast<GLfloat>(center_y + (std::sin(angle) * radius))
        );
    }
    glEnd();
}

void draw_circle_outline(
    const double center_x,
    const double center_y,
    const double radius,
    const float red,
    const float green,
    const float blue,
    const float alpha,
    const float line_width
) {
    constexpr int circle_segments = 48;
    glLineWidth(line_width);
    glColor4f(red, green, blue, alpha);
    glBegin(GL_LINE_LOOP);
    for (int segment = 0; segment < circle_segments; ++segment) {
        const double angle = (2.0 * kPi * static_cast<double>(segment)) / static_cast<double>(circle_segments);
        glVertex2f(
            static_cast<GLfloat>(center_x + (std::cos(angle) * radius)),
            static_cast<GLfloat>(center_y + (std::sin(angle) * radius))
        );
    }
    glEnd();
}

void draw_filled_triangle(
    const double x0,
    const double y0,
    const double x1,
    const double y1,
    const double x2,
    const double y2,
    const float red,
    const float green,
    const float blue,
    const float alpha
) {
    glColor4f(red, green, blue, alpha);
    glBegin(GL_TRIANGLES);
    glVertex2f(static_cast<GLfloat>(x0), static_cast<GLfloat>(y0));
    glVertex2f(static_cast<GLfloat>(x1), static_cast<GLfloat>(y1));
    glVertex2f(static_cast<GLfloat>(x2), static_cast<GLfloat>(y2));
    glEnd();
}

void draw_marker(
    const double x,
    const double y,
    const double radius,
    const MarkerColor color,
    const bool selected
) {
    glBegin(GL_TRIANGLE_FAN);
    glColor3f(color.red, color.green, color.blue);
    glVertex2f(static_cast<GLfloat>(x), static_cast<GLfloat>(y));
    for (int segment = 0; segment <= 20; ++segment) {
        const double angle = (2.0 * kPi * static_cast<double>(segment)) / 20.0;
        glVertex2f(
            static_cast<GLfloat>(x + (std::cos(angle) * radius)),
            static_cast<GLfloat>(y + (std::sin(angle) * radius))
        );
    }
    glEnd();

    glLineWidth(selected ? 2.6F : 1.6F);
    glBegin(GL_LINE_LOOP);
    glColor3f(0.05F, 0.07F, 0.12F);
    for (int segment = 0; segment < 20; ++segment) {
        const double angle = (2.0 * kPi * static_cast<double>(segment)) / 20.0;
        glVertex2f(
            static_cast<GLfloat>(x + (std::cos(angle) * radius)),
            static_cast<GLfloat>(y + (std::sin(angle) * radius))
        );
    }
    glEnd();

    if (selected) {
        const double halo_radius = radius + 0.010;
        glLineWidth(2.0F);
        glBegin(GL_LINE_LOOP);
        glColor3f(0.98F, 0.93F, 0.65F);
        for (int segment = 0; segment < 20; ++segment) {
            const double angle = (2.0 * kPi * static_cast<double>(segment)) / 20.0;
            glVertex2f(
                static_cast<GLfloat>(x + (std::cos(angle) * halo_radius)),
                static_cast<GLfloat>(y + (std::sin(angle) * halo_radius))
            );
        }
        glEnd();
    }
}

void draw_location_pin_marker(
    const double x,
    const double y,
    const double radius,
    const MarkerColor accent,
    const bool selected,
    const bool focused,
    const bool dimmed
) {
    const double visual_radius = radius * (dimmed ? 0.74 : (selected ? 1.16 : 1.0));
    const double alpha = dimmed ? 0.40 : 0.98;
    const double head_radius = visual_radius * 0.88;
    const double head_center_y = y + (radius * 1.18);
    const double shoulder_y = head_center_y - (head_radius * 0.34);
    const double left_shoulder_x = x - (head_radius * 0.58);
    const double right_shoulder_x = x + (head_radius * 0.58);
    const double shadow_offset = visual_radius * 0.11;

    if (selected || focused) {
        const double glow_head_radius = head_radius + (visual_radius * (selected ? 0.34 : 0.22));
        const double glow_shoulder_y = head_center_y - (glow_head_radius * 0.34);
        draw_filled_triangle(
            x - (glow_head_radius * 0.62),
            glow_shoulder_y,
            x,
            y - (visual_radius * 0.10),
            x + (glow_head_radius * 0.62),
            glow_shoulder_y,
            selected ? 0.98F : 0.40F,
            selected ? 0.76F : 0.66F,
            selected ? 0.18F : 0.96F,
            selected ? 0.38F : 0.24F
        );
        draw_filled_circle(
            x,
            head_center_y,
            glow_head_radius,
            selected ? 0.98F : 0.40F,
            selected ? 0.76F : 0.66F,
            selected ? 0.18F : 0.96F,
            selected ? 0.38F : 0.24F
        );
    }

    draw_filled_triangle(
        left_shoulder_x + shadow_offset,
        shoulder_y - shadow_offset,
        x + shadow_offset,
        y - shadow_offset,
        right_shoulder_x + shadow_offset,
        shoulder_y - shadow_offset,
        0.0F,
        0.0F,
        0.0F,
        dimmed ? 0.12F : 0.28F
    );
    draw_filled_circle(
        x + shadow_offset,
        head_center_y - shadow_offset,
        head_radius,
        0.0F,
        0.0F,
        0.0F,
        dimmed ? 0.10F : 0.26F
    );

    draw_filled_triangle(
        left_shoulder_x,
        shoulder_y,
        x,
        y,
        right_shoulder_x,
        shoulder_y,
        0.92F,
        0.12F,
        0.16F,
        static_cast<float>(alpha)
    );
    draw_filled_circle(x, head_center_y, head_radius, 0.92F, 0.12F, 0.16F, static_cast<float>(alpha));

    glLineWidth(selected ? 2.8F : 1.9F);
    glColor4f(0.05F, 0.06F, 0.08F, dimmed ? 0.38F : 0.88F);
    glBegin(GL_LINE_STRIP);
    glVertex2f(static_cast<GLfloat>(left_shoulder_x), static_cast<GLfloat>(shoulder_y));
    glVertex2f(static_cast<GLfloat>(x), static_cast<GLfloat>(y));
    glVertex2f(static_cast<GLfloat>(right_shoulder_x), static_cast<GLfloat>(shoulder_y));
    glEnd();
    draw_circle_outline(x, head_center_y, head_radius, 0.05F, 0.06F, 0.08F, dimmed ? 0.38F : 0.88F, selected ? 2.8F : 1.9F);

    draw_filled_circle(x, head_center_y, head_radius * 0.46, 0.96F, 0.98F, 1.0F, dimmed ? 0.48F : 0.96F);
    draw_filled_circle(x, head_center_y, head_radius * 0.31, accent.red, accent.green, accent.blue, static_cast<float>(alpha));
}

void draw_cluster_marker(const double x, const double y, const double radius) {
    draw_marker(x, y, radius, cluster_color(), false);

    glLineWidth(2.0F);
    glBegin(GL_LINE_LOOP);
    glColor3f(0.99F, 0.93F, 0.74F);
    for (int segment = 0; segment < 20; ++segment) {
        const double angle = (2.0 * kPi * static_cast<double>(segment)) / 20.0;
        glVertex2f(
            static_cast<GLfloat>(x + (std::cos(angle) * (radius * 0.62))),
            static_cast<GLfloat>(y + (std::sin(angle) * (radius * 0.62)))
        );
    }
    glEnd();
}

CountryBorderStyle country_border_style(const double camera_height_meters) {
    const double near_progress = 1.0 - normalized_camera_height(camera_height_meters);
    CountryBorderStyle style;
    style.line_alpha = static_cast<float>(0.34 + (near_progress * 0.46));
    style.halo_alpha = static_cast<float>(0.18 + (near_progress * 0.36));
    style.line_width = static_cast<float>(0.85 + (near_progress * 1.05));
    style.halo_width = style.line_width + 1.4F;
    return style;
}

struct BoundarySegmentKey {
    std::string country_code;
    std::int64_t longitude_a = 0;
    std::int64_t latitude_a = 0;
    std::int64_t longitude_b = 0;
    std::int64_t latitude_b = 0;

    [[nodiscard]] bool operator==(const BoundarySegmentKey& other) const noexcept {
        return country_code == other.country_code &&
            longitude_a == other.longitude_a &&
            latitude_a == other.latitude_a &&
            longitude_b == other.longitude_b &&
            latitude_b == other.latitude_b;
    }
};

struct BoundarySegmentKeyHash {
    [[nodiscard]] std::size_t operator()(const BoundarySegmentKey& key) const noexcept {
        std::size_t seed = std::hash<std::string>{}(key.country_code);
        const auto combine = [&seed](const std::int64_t value) {
            seed ^= std::hash<std::int64_t>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
        };
        combine(key.longitude_a);
        combine(key.latitude_a);
        combine(key.longitude_b);
        combine(key.latitude_b);
        return seed;
    }
};

struct BoundarySourceSegment {
    std::string country_code;
    frameflow::renderer::cartography::CartographyCoordinate start;
    frameflow::renderer::cartography::CartographyCoordinate end;
};

std::int64_t quantized_degrees(const double value) {
    return static_cast<std::int64_t>(std::llround(value * 1'000'000.0));
}

BoundarySegmentKey boundary_segment_key(
    const std::string& country_code,
    const frameflow::renderer::cartography::CartographyCoordinate& start,
    const frameflow::renderer::cartography::CartographyCoordinate& end
) {
    auto longitude_a = quantized_degrees(start.longitude);
    auto latitude_a = quantized_degrees(start.latitude);
    auto longitude_b = quantized_degrees(end.longitude);
    auto latitude_b = quantized_degrees(end.latitude);
    if (std::tie(longitude_b, latitude_b) < std::tie(longitude_a, latitude_a)) {
        std::swap(longitude_a, longitude_b);
        std::swap(latitude_a, latitude_b);
    }

    return BoundarySegmentKey{
        .country_code = country_code,
        .longitude_a = longitude_a,
        .latitude_a = latitude_a,
        .longitude_b = longitude_b,
        .latitude_b = latitude_b,
    };
}

std::unordered_set<BoundarySegmentKey, BoundarySegmentKeyHash> internal_same_country_segments(
    const frameflow::renderer::cartography::CartographyDataset& dataset
) {
    std::unordered_map<BoundarySegmentKey, std::size_t, BoundarySegmentKeyHash> segment_counts;
    for (const auto& boundary : dataset.boundaries) {
        if (boundary.kind != frameflow::renderer::cartography::CartographyBoundaryKind::Country ||
            boundary.vertices.size() < 2u ||
            boundary.country_code.empty()) {
            continue;
        }

        for (std::size_t index = 1; index < boundary.vertices.size(); ++index) {
            segment_counts[boundary_segment_key(boundary.country_code, boundary.vertices[index - 1], boundary.vertices[index])] += 1u;
        }
    }

    std::unordered_set<BoundarySegmentKey, BoundarySegmentKeyHash> internal_segments;
    for (const auto& [segment, count] : segment_counts) {
        if (count > 1u) {
            internal_segments.insert(segment);
        }
    }
    return internal_segments;
}

std::vector<BoundarySourceSegment> external_country_boundary_segments(
    const frameflow::renderer::cartography::CartographyDataset& dataset
) {
    std::vector<BoundarySourceSegment> segments;
    const auto internal_segments = internal_same_country_segments(dataset);
    for (const auto& boundary : dataset.boundaries) {
        if (boundary.kind != frameflow::renderer::cartography::CartographyBoundaryKind::Country ||
            boundary.vertices.size() < 2u) {
            continue;
        }

        for (std::size_t index = 1; index < boundary.vertices.size(); ++index) {
            if (internal_segments.contains(boundary_segment_key(
                    boundary.country_code,
                    boundary.vertices[index - 1],
                    boundary.vertices[index]
                ))) {
                continue;
            }
            segments.push_back(BoundarySourceSegment{
                .country_code = boundary.country_code,
                .start = boundary.vertices[index - 1],
                .end = boundary.vertices[index],
            });
        }
    }
    return segments;
}

std::shared_ptr<const std::vector<BoundarySourceSegment>> cached_external_country_boundary_segments(
    const frameflow::renderer::cartography::CartographyDataset& dataset
) {
    static std::mutex cache_mutex;
    static std::unordered_map<
        const frameflow::renderer::cartography::CartographyDataset*,
        std::shared_ptr<const std::vector<BoundarySourceSegment>>
    > cache;

    const std::lock_guard<std::mutex> lock(cache_mutex);
    const auto cached = cache.find(&dataset);
    if (cached != cache.end()) {
        return cached->second;
    }

    auto segments = std::make_shared<const std::vector<BoundarySourceSegment>>(
        external_country_boundary_segments(dataset)
    );
    cache.emplace(&dataset, segments);
    return segments;
}

ProjectedCountryBoundaries build_projected_country_boundaries(
    const frameflow::renderer::cartography::CartographyDataset& dataset,
    const RenderedGlobeScene::CameraState& camera,
    const int width,
    const int height
) {
    ProjectedCountryBoundaries projected_boundaries;
    if (width <= 0 || height <= 0 || !dataset.available()) {
        return projected_boundaries;
    }

    const auto projection = make_perspective_projection(camera, width, height);
    const auto source_segments = cached_external_country_boundary_segments(dataset);
    std::unordered_set<std::string> visible_countries;
    for (const auto& segment : *source_segments) {
        const ProjectedPoint start = project_coordinate(segment.start, projection);
        const ProjectedPoint end = project_coordinate(segment.end, projection);
        if (!start.visible || !end.visible) {
            continue;
        }

        double start_x = start.x;
        double start_y = start.y;
        double end_x = end.x;
        double end_y = end.y;
        if (!clip_segment_to_viewport(&start_x, &start_y, &end_x, &end_y)) {
            continue;
        }

        projected_boundaries.segments.push_back(ProjectedBoundarySegment{
            .start_x = start_x,
            .start_y = start_y,
            .end_x = end_x,
            .end_y = end_y,
        });
        if (!segment.country_code.empty()) {
            visible_countries.insert(segment.country_code);
        }
    }
    projected_boundaries.visible_boundary_count = visible_countries.size();

    return projected_boundaries;
}

void draw_country_boundaries(
    const ProjectedCountryBoundaries& boundaries,
    const CountryBorderStyle& style
) {
    if (boundaries.segments.empty()) {
        return;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glLineWidth(style.halo_width);
    glColor4f(style.halo_red, style.halo_green, style.halo_blue, style.halo_alpha);
    glBegin(GL_LINES);
    for (const auto& segment : boundaries.segments) {
        glVertex2f(static_cast<GLfloat>(segment.start_x), static_cast<GLfloat>(segment.start_y));
        glVertex2f(static_cast<GLfloat>(segment.end_x), static_cast<GLfloat>(segment.end_y));
    }
    glEnd();

    glLineWidth(style.line_width);
    glColor4f(style.line_red, style.line_green, style.line_blue, style.line_alpha);
    glBegin(GL_LINES);
    for (const auto& segment : boundaries.segments) {
        glVertex2f(static_cast<GLfloat>(segment.start_x), static_cast<GLfloat>(segment.start_y));
        glVertex2f(static_cast<GLfloat>(segment.end_x), static_cast<GLfloat>(segment.end_y));
    }
    glEnd();

    glDisable(GL_BLEND);
}

} // namespace

class JoiningTaskProcessor final : public CesiumAsync::ITaskProcessor {
public:
    JoiningTaskProcessor() {
        const auto worker_count = default_worker_count();
        workers_.reserve(worker_count);
        for (std::size_t index = 0u; index < worker_count; ++index) {
            workers_.emplace_back([this]() {
                worker_loop();
            });
        }
    }

    ~JoiningTaskProcessor() override {
        shutdown();
    }

    void startTask(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            tasks_.push_back(std::move(task));
        }
        condition_.notify_one();
    }

    void shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    static std::size_t default_worker_count() noexcept {
        const auto hardware_threads = std::thread::hardware_concurrency();
        if (hardware_threads == 0u) {
            return 4u;
        }
        return std::clamp<std::size_t>(hardware_threads / 2u, 4u, 12u);
    }

    void worker_loop() noexcept {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }

            try {
                task();
            } catch (...) {
                // Cesium tasks must not terminate the process if a request
                // completes during renderer teardown.
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

class GoogleTilesetRuntime {
public:
    struct Snapshot {
        std::size_t tiles_ready = 0u;
        std::size_t overlays_ready = 0u;
        float load_progress = 0.0f;
        std::string last_error;
    };

    struct DebugOptions {
        bool tile_lod_colors = false;
        bool only_max_lod = false;
        bool uv_clip = false;
        std::optional<int> single_tile_level;

        static DebugOptions from_environment() {
            return DebugOptions{
                .tile_lod_colors = env_flag_enabled(kDebugTileLodColorsEnv),
                .only_max_lod = env_flag_enabled(kDebugOnlyMaxLodEnv) ||
                    env_flag_enabled(kDebugDisableParentTilesEnv),
                .uv_clip = env_flag_enabled(kDebugUvClipEnv),
                .single_tile_level = env_int(kDebugSingleTileLevelEnv)
            };
        }

        [[nodiscard]] bool enabled() const noexcept {
            return tile_lod_colors || only_max_lod || uv_clip || single_tile_level.has_value();
        }
    };

    static std::unique_ptr<GoogleTilesetRuntime> create(
        BasemapProviderConfig basemap_config,
        const RenderedGlobeScene::Options& scene_options,
        std::string* status_message
    ) {
        if (basemap_config.kind == BasemapProviderKind::MapTilerRaster ||
            basemap_config.kind == BasemapProviderKind::StadiaRaster) {
            if (status_message != nullptr) {
                *status_message = "provider=" + basemap_config.provider_id + " status=delegated uniform_raster=1";
            }
            return nullptr;
        }

        auto runtime = std::unique_ptr<GoogleTilesetRuntime>(new GoogleTilesetRuntime(
            std::move(basemap_config),
            scene_options,
            DebugOptions::from_environment()
        ));
        if (!runtime->initialize(status_message)) {
            return nullptr;
        }
        if (status_message != nullptr) {
            *status_message = runtime->diagnostics_summary();
        }
        return runtime;
    }

    ~GoogleTilesetRuntime() {
        if (task_processor_) {
            task_processor_->shutdown();
        }
        destroy_raster_clip_shader();
    }

    void resize(const int width, const int height) noexcept {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
    }

    bool render(
        const RenderedGlobeScene::CameraState& camera,
        const int width,
        const int height,
        std::string* status_message
    ) {
        if (!tileset_) {
            if (status_message != nullptr) {
                *status_message = diagnostics_summary();
            }
            return false;
        }
        if (is_quota_backoff_active()) {
            if (status_message != nullptr) {
                *status_message = diagnostics_summary();
            }
            return false;
        }

        resize(width, height);
        const auto view_state = make_view_state(camera);
        const std::vector<Cesium3DTilesSelection::ViewState> views{view_state};
        const Cesium3DTilesSelection::ViewUpdateResult* update_result = nullptr;
        last_camera_settled_ = is_camera_settled(camera);
        last_pump_iterations_ = last_camera_settled_
            ? tileset_settle_pump_iterations_
            : tileset_active_pump_iterations_;
        for (int iteration = 0; iteration < last_pump_iterations_; ++iteration) {
            async_system_.dispatchMainThreadTasks();
            update_result = &tileset_->updateViewGroup(view_group_, views, 1.0f / 60.0f);
            async_system_.dispatchMainThreadTasks();
            tileset_->loadTiles();
            async_system_.dispatchMainThreadTasks();
        }
        previous_render_camera_ = camera;
        if (update_result == nullptr) {
            if (status_message != nullptr) {
                *status_message = diagnostics_summary();
            }
            return false;
        }

        last_ready_tile_count_ = 0u;
        last_ready_overlay_count_ = 0u;
        last_rendered_tile_count_ = 0u;
        last_skipped_tile_count_ = 0u;
        last_drawn_overlay_binding_count_ = 0u;
        last_suspect_parent_raster_binding_count_ = 0u;
        last_overlay_uv_outside_vertex_count_ = 0u;
        last_overlay_partially_clipped_binding_count_ = 0u;
        last_overlay_fully_outside_binding_count_ = 0u;
        last_imagery_level_counts_.clear();
        last_imagery_lod_span_summary_ = "none";
        last_tiles_fading_out_count_ = update_result->tilesFadingOut.size();
        last_parent_child_overlap_count_ = 0u;
        last_min_overlay_scale_ = 1.0;
        last_max_overlay_scale_ = 1.0;
        last_min_lod_fade_ = 1.0F;
        last_max_lod_fade_ = 1.0F;
        last_tile_level_summary_ = "none";
        last_tile_lod_span_summary_ = "none";
        last_sse_max_ = 0.0;
        last_sse_avg_ = 0.0;
        last_max_depth_visited_ = 0u;
        last_worker_queue_length_ = 0;
        last_main_queue_length_ = 0;
        std::size_t rendered_triangle_count = 0u;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.01F);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor4f(1.0F, 1.0F, 1.0F, 1.0F);

        const PerspectiveProjection projection = make_perspective_projection(camera, width_, height_);
        set_tile_perspective_matrices(projection);
        std::vector<RenderTileCandidate> candidates;
        candidates.reserve(update_result->tilesToRenderThisFrame.size());
        std::map<int, std::size_t> tile_levels;
        int min_tile_level = -1;
        int max_tile_level = -1;
        bool have_lod_fade = false;

        if (!update_result->tileScreenSpaceErrorThisFrame.empty()) {
            double sse_sum = 0.0;
            for (const double sse : update_result->tileScreenSpaceErrorThisFrame) {
                last_sse_max_ = std::max(last_sse_max_, sse);
                sse_sum += sse;
            }
            last_sse_avg_ = sse_sum / static_cast<double>(update_result->tileScreenSpaceErrorThisFrame.size());
        }
        last_max_depth_visited_ = update_result->maxDepthVisited;
        last_worker_queue_length_ = update_result->workerThreadTileLoadQueueLength;
        last_main_queue_length_ = update_result->mainThreadTileLoadQueueLength;

        for (const auto& tile : update_result->tilesToRenderThisFrame) {
            if (!tile) {
                continue;
            }
            const auto* render_content = tile->getContent().getRenderContent();
            if (render_content == nullptr) {
                continue;
            }
            auto* tile_resources = reinterpret_cast<TileRenderResources*>(render_content->getRenderResources());
            if (tile_resources == nullptr) {
                continue;
            }
            const int level = tile_level(*tile);
            const float lod_fade = render_content->getLodTransitionFadePercentage();
            last_ready_tile_count_ += 1u;
            last_ready_overlay_count_ += count_overlay_bindings(*tile_resources);
            tile_levels[level] += 1u;
            if (level >= 0) {
                min_tile_level = min_tile_level < 0 ? level : std::min(min_tile_level, level);
                max_tile_level = std::max(max_tile_level, level);
            }
            last_min_lod_fade_ = have_lod_fade ? std::min(last_min_lod_fade_, lod_fade) : lod_fade;
            last_max_lod_fade_ = have_lod_fade ? std::max(last_max_lod_fade_, lod_fade) : lod_fade;
            have_lod_fade = true;
            candidates.push_back(RenderTileCandidate{
                .resources = tile_resources,
                .quadtree_id = quadtree_tile_id(*tile),
                .level = level
            });
        }

        last_tile_level_summary_ = summarize_tile_levels(tile_levels);
        last_tile_lod_span_summary_ = summarize_tile_lod_span(min_tile_level, max_tile_level);
        last_parent_child_overlap_count_ = count_rendered_descendants_with_rendered_ancestor(candidates);
        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const RenderTileCandidate& left, const RenderTileCandidate& right) {
                return left.level < right.level;
            }
        );
        for (const auto& candidate : candidates) {
            if (candidate.resources == nullptr) {
                continue;
            }
            if (debug_options_.single_tile_level.has_value() &&
                candidate.level != *debug_options_.single_tile_level) {
                last_skipped_tile_count_ += 1u;
                continue;
            }
            if (debug_options_.only_max_lod && max_tile_level >= 0 && candidate.level != max_tile_level) {
                last_skipped_tile_count_ += 1u;
                continue;
            }

            last_rendered_tile_count_ += 1u;
            rendered_triangle_count += draw_tile_resources(*candidate.resources, projection, candidate.level);
        }

        set_normalized_ortho_matrices();
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);

        last_load_progress_ = view_group_.getPreviousLoadProgressPercentage();
        if (status_message != nullptr) {
            *status_message = diagnostics_summary();
        }
        return rendered_triangle_count > 0u;
    }

    [[nodiscard]] std::string diagnostics_summary() const {
        std::ostringstream out;
        out
            << "provider=" << basemap_config_.provider_id
            << " style=" << (basemap_config_.style_id.empty() ? "default" : basemap_config_.style_id)
            << " hidpi=" << (basemap_config_.hidpi ? "1" : "0")
            << " asset_cache=" << asset_cache_status_
            << " language=" << basemap_config_.language
            << " region=" << basemap_config_.region
            << " tiles_ready=" << last_ready_tile_count_
            << " tiles_rendered=" << last_rendered_tile_count_
            << " tiles_skipped=" << last_skipped_tile_count_
            << " tile_levels=" << last_tile_level_summary_
            << " tile_lod_span=" << last_tile_lod_span_summary_
            << " tiles_fading_out=" << last_tiles_fading_out_count_
            << " lod_fade_min=" << last_min_lod_fade_
            << " lod_fade_max=" << last_max_lod_fade_
            << " parent_child_overlap_count=" << last_parent_child_overlap_count_
            << " overlays_ready=" << last_ready_overlay_count_
            << " overlays_drawn=" << last_drawn_overlay_binding_count_
            << " suspect_parent_rasters=" << last_suspect_parent_raster_binding_count_
            << " overlay_scale_min=" << last_min_overlay_scale_
            << " overlay_scale_max=" << last_max_overlay_scale_
            << " raster_clip_shader=" << raster_clip_shader_status_
            << " overlay_uv_outside_vertices=" << last_overlay_uv_outside_vertex_count_
            << " overlay_partial_clip_bindings=" << last_overlay_partially_clipped_binding_count_
            << " overlay_fully_outside_bindings=" << last_overlay_fully_outside_binding_count_
            << " imagery_levels=" << summarize_imagery_levels(last_imagery_level_counts_)
            << " imagery_lod_span=" << last_imagery_lod_span_summary_
            << " tileset_max_sse=" << tileset_max_sse_
            << " raster_max_sse=" << raster_max_sse_
            << " pump_iterations=" << last_pump_iterations_
            << " active_pump_iterations=" << tileset_active_pump_iterations_
            << " settle_pump_iterations=" << tileset_settle_pump_iterations_
            << " camera_settled=" << (last_camera_settled_ ? "1" : "0")
            << " tile_loads=" << tileset_max_simultaneous_loads_
            << " overlay_loads=" << overlay_max_simultaneous_loads_
            << " preload_siblings=" << (preload_siblings_ ? "1" : "0")
            << " quota_backoff_seconds=" << quota_backoff_remaining_seconds()
            << " quota_error_count=" << quota_error_count_
            << " sse_max=" << last_sse_max_
            << " sse_avg=" << last_sse_avg_
            << " max_depth=" << last_max_depth_visited_
            << " worker_queue=" << last_worker_queue_length_
            << " main_queue=" << last_main_queue_length_
            << " load_progress=" << last_load_progress_;
        if (debug_options_.enabled()) {
            out
                << " debug_lod_colors=" << (debug_options_.tile_lod_colors ? "1" : "0")
                << " debug_only_max_lod=" << (debug_options_.only_max_lod ? "1" : "0")
                << " debug_uv_clip=" << (debug_options_.uv_clip ? "1" : "0");
            if (debug_options_.single_tile_level.has_value()) {
                out << " debug_single_tile_level=" << *debug_options_.single_tile_level;
            }
        }
        if (!last_error_.empty()) {
            out << " last_error=[" << last_error_ << "]";
        }
        return out.str();
    }

    [[nodiscard]] Snapshot snapshot() const {
        return Snapshot{
            .tiles_ready = last_ready_tile_count_,
            .overlays_ready = last_ready_overlay_count_,
            .load_progress = last_load_progress_,
            .last_error = last_error_
        };
    }

private:
    struct RasterTextureResources {
        GLuint texture_id = 0u;
        int width = 0;
        int height = 0;
    };

    struct OverlayBinding {
        GLuint texture_id = 0u;
        glm::dvec2 translation{0.0, 0.0};
        glm::dvec2 scale{1.0, 1.0};
        int estimated_imagery_level = -1;
        glm::dvec2 target_screen_pixels{0.0, 0.0};
    };

    struct PrimitiveResources {
        std::vector<glm::dvec3> positions;
        std::vector<GLdouble> position_components;
        std::vector<std::uint32_t> indices;
        std::unordered_map<std::int32_t, std::vector<glm::vec2>> overlay_texcoords;
    };

    struct TileRenderResources {
        std::vector<PrimitiveResources> primitives;
        std::unordered_map<std::int32_t, std::vector<OverlayBinding>> overlay_bindings;
    };

    struct RenderTileCandidate {
        const TileRenderResources* resources = nullptr;
        std::optional<CesiumGeometry::QuadtreeTileID> quadtree_id;
        int level = -1;
    };

    struct RasterClipShaderApi {
        using CreateShaderFn = GLuint (*)(GLenum);
        using ShaderSourceFn = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
        using CompileShaderFn = void (*)(GLuint);
        using GetShaderivFn = void (*)(GLuint, GLenum, GLint*);
        using GetShaderInfoLogFn = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
        using DeleteShaderFn = void (*)(GLuint);
        using CreateProgramFn = GLuint (*)();
        using AttachShaderFn = void (*)(GLuint, GLuint);
        using LinkProgramFn = void (*)(GLuint);
        using GetProgramivFn = void (*)(GLuint, GLenum, GLint*);
        using GetProgramInfoLogFn = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
        using DeleteProgramFn = void (*)(GLuint);
        using UseProgramFn = void (*)(GLuint);
        using GetUniformLocationFn = GLint (*)(GLuint, const GLchar*);
        using Uniform1iFn = void (*)(GLint, GLint);
        using Uniform1fFn = void (*)(GLint, GLfloat);

        CreateShaderFn create_shader = nullptr;
        ShaderSourceFn shader_source = nullptr;
        CompileShaderFn compile_shader = nullptr;
        GetShaderivFn get_shader_iv = nullptr;
        GetShaderInfoLogFn get_shader_info_log = nullptr;
        DeleteShaderFn delete_shader = nullptr;
        CreateProgramFn create_program = nullptr;
        AttachShaderFn attach_shader = nullptr;
        LinkProgramFn link_program = nullptr;
        GetProgramivFn get_program_iv = nullptr;
        GetProgramInfoLogFn get_program_info_log = nullptr;
        DeleteProgramFn delete_program = nullptr;
        UseProgramFn use_program = nullptr;
        GetUniformLocationFn get_uniform_location = nullptr;
        Uniform1iFn uniform_1i = nullptr;
        Uniform1fFn uniform_1f = nullptr;

        template <typename Function>
        static Function load(const char* name) noexcept {
            return reinterpret_cast<Function>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(name)));
        }

        [[nodiscard]] bool load() noexcept {
            create_shader = load<CreateShaderFn>("glCreateShader");
            shader_source = load<ShaderSourceFn>("glShaderSource");
            compile_shader = load<CompileShaderFn>("glCompileShader");
            get_shader_iv = load<GetShaderivFn>("glGetShaderiv");
            get_shader_info_log = load<GetShaderInfoLogFn>("glGetShaderInfoLog");
            delete_shader = load<DeleteShaderFn>("glDeleteShader");
            create_program = load<CreateProgramFn>("glCreateProgram");
            attach_shader = load<AttachShaderFn>("glAttachShader");
            link_program = load<LinkProgramFn>("glLinkProgram");
            get_program_iv = load<GetProgramivFn>("glGetProgramiv");
            get_program_info_log = load<GetProgramInfoLogFn>("glGetProgramInfoLog");
            delete_program = load<DeleteProgramFn>("glDeleteProgram");
            use_program = load<UseProgramFn>("glUseProgram");
            get_uniform_location = load<GetUniformLocationFn>("glGetUniformLocation");
            uniform_1i = load<Uniform1iFn>("glUniform1i");
            uniform_1f = load<Uniform1fFn>("glUniform1f");
            return create_shader != nullptr &&
                shader_source != nullptr &&
                compile_shader != nullptr &&
                get_shader_iv != nullptr &&
                get_shader_info_log != nullptr &&
                delete_shader != nullptr &&
                create_program != nullptr &&
                attach_shader != nullptr &&
                link_program != nullptr &&
                get_program_iv != nullptr &&
                get_program_info_log != nullptr &&
                delete_program != nullptr &&
                use_program != nullptr &&
                get_uniform_location != nullptr &&
                uniform_1i != nullptr &&
                uniform_1f != nullptr;
        }
    };

    struct RasterClipShader {
        GLuint program_id = 0u;
        GLint sampler_location = -1;
        GLint uv_epsilon_location = -1;

        [[nodiscard]] bool ready() const noexcept {
            return program_id != 0u;
        }
    };

    class PrepareRendererResources final : public Cesium3DTilesSelection::IPrepareRendererResources {
    public:
        explicit PrepareRendererResources(GoogleTilesetRuntime& owner) noexcept : owner_(owner) {}

        CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources> prepareInLoadThread(
            const CesiumAsync::AsyncSystem& async_system,
            Cesium3DTilesSelection::TileLoadResult&& tile_load_result,
            const glm::dmat4& transform,
            const std::any&
        ) override {
            void* resources = nullptr;
            if (const auto* model = std::get_if<CesiumGltf::Model>(&tile_load_result.contentKind); model != nullptr) {
                resources = build_tile_render_resources(*model, transform);
            }
            return async_system.createResolvedFuture(
                Cesium3DTilesSelection::TileLoadResultAndRenderResources{std::move(tile_load_result), resources}
            );
        }

        void* prepareInMainThread(Cesium3DTilesSelection::Tile&, void* p_load_thread_result) override {
            return p_load_thread_result;
        }

        void free(Cesium3DTilesSelection::Tile&, void*, void* p_main_thread_result) noexcept override {
            delete reinterpret_cast<TileRenderResources*>(p_main_thread_result);
        }

        void* prepareRasterInLoadThread(CesiumGltf::ImageAsset& image, const std::any&) override {
            if (image.bytesPerChannel != 1) {
                owner_.last_error_ = "basemap imagery uses unsupported bytesPerChannel";
                return nullptr;
            }
            if (image.channels != 4) {
                image.changeNumberOfChannels(4, std::byte{0xff});
            }
            auto* resources = new RasterTextureResources{};
            resources->width = image.width;
            resources->height = image.height;
            return resources;
        }

        void* prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile& raster_tile, void* p_load_thread_result) override {
            auto* resources = reinterpret_cast<RasterTextureResources*>(p_load_thread_result);
            if (resources == nullptr) {
                return nullptr;
            }

            const auto image = raster_tile.getImage();
            if (!image || image->pixelData.empty()) {
                owner_.last_error_ = "basemap imagery tile was loaded without pixel data";
                return resources;
            }

            glGenTextures(1, &resources->texture_id);
            glBindTexture(GL_TEXTURE_2D, resources->texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            const std::vector<std::byte> flipped_pixels = flip_image_vertically(*image);
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA,
                image->width,
                image->height,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                flipped_pixels.data()
            );
            if (const auto generate_mipmap = reinterpret_cast<void (*)(GLenum)>(
                    glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGenerateMipmap")));
                generate_mipmap != nullptr) {
                generate_mipmap(GL_TEXTURE_2D);
            } else {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            return resources;
        }

        void freeRaster(const CesiumRasterOverlays::RasterOverlayTile&, void*, void* p_main_thread_result) noexcept override {
            auto* resources = reinterpret_cast<RasterTextureResources*>(p_main_thread_result);
            if (resources == nullptr) {
                return;
            }
            if (resources->texture_id != 0u) {
                glDeleteTextures(1, &resources->texture_id);
            }
            delete resources;
        }

        static int estimate_imagery_level(const CesiumRasterOverlays::RasterOverlayTile& raster_tile) noexcept {
            const auto& coverage_rectangle = raster_tile.getTileProvider().getCoverageRectangle();
            const auto& tile_rectangle = raster_tile.getRectangle();
            const double coverage_width = coverage_rectangle.computeWidth();
            const double tile_width = tile_rectangle.computeWidth();
            if (!std::isfinite(coverage_width) ||
                !std::isfinite(tile_width) ||
                coverage_width <= 0.0 ||
                tile_width <= 0.0 ||
                tile_width > coverage_width) {
                return -1;
            }

            const double level = std::log2(coverage_width / tile_width);
            if (!std::isfinite(level)) {
                return -1;
            }
            return static_cast<int>(std::max(0.0, std::round(level)));
        }

        void attachRasterInMainThread(
            const Cesium3DTilesSelection::Tile& tile,
            const std::int32_t overlay_texture_coordinate_id,
            const CesiumRasterOverlays::RasterOverlayTile& raster_tile,
            void* p_main_thread_renderer_resources,
            const glm::dvec2& translation,
            const glm::dvec2& scale
        ) override {
            auto* tile_resources = tile.getContent().getRenderContent() == nullptr
                ? nullptr
                : reinterpret_cast<TileRenderResources*>(tile.getContent().getRenderContent()->getRenderResources());
            auto* raster_resources = reinterpret_cast<RasterTextureResources*>(p_main_thread_renderer_resources);
            if (tile_resources == nullptr || raster_resources == nullptr || raster_resources->texture_id == 0u) {
                return;
            }

            tile_resources->overlay_bindings[overlay_texture_coordinate_id].push_back(OverlayBinding{
                .texture_id = raster_resources->texture_id,
                .translation = translation,
                .scale = scale,
                .estimated_imagery_level = estimate_imagery_level(raster_tile),
                .target_screen_pixels = raster_tile.getTargetScreenPixels()
            });
        }

        void detachRasterInMainThread(
            const Cesium3DTilesSelection::Tile& tile,
            const std::int32_t overlay_texture_coordinate_id,
            const CesiumRasterOverlays::RasterOverlayTile&,
            void* p_main_thread_renderer_resources
        ) noexcept override {
            auto* tile_resources = tile.getContent().getRenderContent() == nullptr
                ? nullptr
                : reinterpret_cast<TileRenderResources*>(tile.getContent().getRenderContent()->getRenderResources());
            if (tile_resources == nullptr) {
                return;
            }
            auto bindings = tile_resources->overlay_bindings.find(overlay_texture_coordinate_id);
            if (bindings == tile_resources->overlay_bindings.end()) {
                return;
            }

            auto* raster_resources = reinterpret_cast<RasterTextureResources*>(p_main_thread_renderer_resources);
            if (raster_resources == nullptr) {
                tile_resources->overlay_bindings.erase(bindings);
                return;
            }

            auto& values = bindings->second;
            values.erase(
                std::remove_if(values.begin(), values.end(), [texture_id = raster_resources->texture_id](const auto& value) {
                    return value.texture_id == texture_id;
                }),
                values.end()
            );
            if (values.empty()) {
                tile_resources->overlay_bindings.erase(bindings);
            }
        }

    private:
        template <typename T>
        static bool fill_indices_from_accessor(
            const CesiumGltf::Model& model,
            const std::int32_t accessor_index,
            std::vector<std::uint32_t>& indices
        ) {
            const CesiumGltf::AccessorView<T> view(model, accessor_index);
            if (view.status() != CesiumGltf::AccessorViewStatus::Valid) {
                return false;
            }
            indices.reserve(static_cast<std::size_t>(view.size()));
            for (int64_t index = 0; index < view.size(); ++index) {
                indices.push_back(static_cast<std::uint32_t>(view[index]));
            }
            return true;
        }

        static void fill_sequential_indices(const std::size_t vertex_count, std::vector<std::uint32_t>& indices) {
            indices.resize(vertex_count);
            std::iota(indices.begin(), indices.end(), 0u);
        }

        static std::vector<std::byte> flip_image_vertically(const CesiumGltf::ImageAsset& image) {
            const auto row_bytes = static_cast<std::size_t>(image.width * image.channels * image.bytesPerChannel);
            const auto height = static_cast<std::size_t>(image.height);
            if (row_bytes == 0u || height == 0u || image.pixelData.size() < row_bytes * height) {
                return image.pixelData;
            }

            std::vector<std::byte> flipped(image.pixelData.size());
            for (std::size_t row = 0u; row < height; ++row) {
                const auto source_offset = row * row_bytes;
                const auto target_offset = (height - 1u - row) * row_bytes;
                std::copy_n(
                    image.pixelData.data() + source_offset,
                    row_bytes,
                    flipped.data() + target_offset
                );
            }
            return flipped;
        }

        static std::optional<std::vector<glm::vec2>> load_overlay_texcoords(
            const CesiumGltf::Model& model,
            const CesiumGltf::MeshPrimitive& primitive,
            const std::int32_t overlay_texture_coordinate_id
        ) {
            const std::string attribute_name = "_CESIUMOVERLAY_" + std::to_string(overlay_texture_coordinate_id);
            const auto attribute = primitive.attributes.find(attribute_name);
            if (attribute == primitive.attributes.end()) {
                return std::nullopt;
            }

            const CesiumGltf::AccessorView<glm::vec2> view(model, attribute->second);
            if (view.status() != CesiumGltf::AccessorViewStatus::Valid) {
                return std::nullopt;
            }

            std::vector<glm::vec2> texcoords;
            texcoords.reserve(static_cast<std::size_t>(view.size()));
            for (int64_t index = 0; index < view.size(); ++index) {
                texcoords.push_back(view[index]);
            }
            return texcoords;
        }

        static void* build_tile_render_resources(const CesiumGltf::Model& model, const glm::dmat4& transform) {
            auto* tile_resources = new TileRenderResources{};
            tile_resources->primitives.reserve(model.meshes.size());

            for (const auto& mesh : model.meshes) {
                for (const auto& primitive : mesh.primitives) {
                    const auto position_attribute = primitive.attributes.find("POSITION");
                    if (position_attribute == primitive.attributes.end()) {
                        continue;
                    }

                    const CesiumGltf::AccessorView<glm::vec3> positions_view(model, position_attribute->second);
                    if (positions_view.status() != CesiumGltf::AccessorViewStatus::Valid || positions_view.size() <= 0) {
                        continue;
                    }

                    PrimitiveResources primitive_resources;
                    primitive_resources.positions.reserve(static_cast<std::size_t>(positions_view.size()));
                    primitive_resources.position_components.reserve(static_cast<std::size_t>(positions_view.size()) * 3u);
                    for (int64_t index = 0; index < positions_view.size(); ++index) {
                        const glm::vec3 value = positions_view[index];
                        const glm::dvec4 transformed = transform * glm::dvec4(
                            static_cast<double>(value.x),
                            static_cast<double>(value.y),
                            static_cast<double>(value.z),
                            1.0
                        );
                        primitive_resources.positions.emplace_back(
                            transformed.x,
                            transformed.y,
                            transformed.z
                        );
                        primitive_resources.position_components.push_back(transformed.x);
                        primitive_resources.position_components.push_back(transformed.y);
                        primitive_resources.position_components.push_back(transformed.z);
                    }

                    bool have_indices = false;
                    if (primitive.indices >= 0) {
                        have_indices = fill_indices_from_accessor<std::uint16_t>(
                            model,
                            primitive.indices,
                            primitive_resources.indices
                        ) || fill_indices_from_accessor<std::uint32_t>(
                            model,
                            primitive.indices,
                            primitive_resources.indices
                        ) || fill_indices_from_accessor<std::uint8_t>(
                            model,
                            primitive.indices,
                            primitive_resources.indices
                        );
                    }
                    if (!have_indices) {
                        fill_sequential_indices(primitive_resources.positions.size(), primitive_resources.indices);
                    }

                    for (int overlay_id = 0; overlay_id < 4; ++overlay_id) {
                        auto overlay_texcoords = load_overlay_texcoords(model, primitive, overlay_id);
                        if (overlay_texcoords.has_value()) {
                            primitive_resources.overlay_texcoords.emplace(overlay_id, std::move(*overlay_texcoords));
                        }
                    }

                    tile_resources->primitives.push_back(std::move(primitive_resources));
                }
            }

            return tile_resources;
        }

        GoogleTilesetRuntime& owner_;
    };

    GoogleTilesetRuntime(
        BasemapProviderConfig basemap_config,
        RenderedGlobeScene::Options scene_options,
        DebugOptions debug_options
    )
        : basemap_config_(std::move(basemap_config)),
          scene_options_(std::move(scene_options)),
          debug_options_(debug_options),
          task_processor_(std::make_shared<JoiningTaskProcessor>()),
          async_system_(task_processor_),
          asset_accessor_(create_asset_accessor()),
          credit_system_(std::make_shared<CesiumUtility::CreditSystem>()),
          renderer_resources_(std::make_shared<PrepareRendererResources>(*this)) {}

    bool initialize(std::string* status_message) {
        tileset_max_sse_ = std::clamp(env_double(kTilesetMaxSseEnv).value_or(4.0), 0.05, 64.0);
        raster_max_sse_ = std::clamp(env_double(kRasterMaxSseEnv).value_or(2.0), 0.05, 64.0);
        tileset_max_simultaneous_loads_ =
            static_cast<std::uint32_t>(
                std::clamp(
                    env_int(kGoogleMapsTilesetLoadsEnv).value_or(
                        static_cast<int>(kDefaultTilesetMaxSimultaneousLoads)
                    ),
                    1,
                    32
                )
            );
        overlay_max_simultaneous_loads_ =
            static_cast<std::uint32_t>(
                std::clamp(
                    env_int(kGoogleMapsOverlayLoadsEnv).value_or(
                        static_cast<int>(default_overlay_max_simultaneous_loads(basemap_config_.kind))
                    ),
                    1,
                    32
                )
            );
        const auto legacy_pump_iterations = env_int(kTilesetPumpIterationsEnv);
        tileset_active_pump_iterations_ = std::clamp(
            env_int(kTilesetActivePumpIterationsEnv).value_or(
                legacy_pump_iterations.value_or(kDefaultTilesetActivePumpIterations)
            ),
            1,
            8
        );
        tileset_settle_pump_iterations_ = std::clamp(
            env_int(kTilesetSettlePumpIterationsEnv).value_or(
                legacy_pump_iterations.value_or(kDefaultTilesetSettlePumpIterations)
            ),
            tileset_active_pump_iterations_,
            8
        );
        quota_backoff_seconds_ = std::clamp(env_int(kGoogleMapsQuotaBackoffSecondsEnv).value_or(600), 30, 86400);
        preload_siblings_ = env_flag_enabled(kGoogleMapsPreloadSiblingsEnv, false);

        Cesium3DTilesSelection::TilesetExternals externals{
            asset_accessor_,
            renderer_resources_,
            async_system_,
            credit_system_
        };

        Cesium3DTilesSelection::TilesetOptions tileset_options;
        tileset_options.maximumScreenSpaceError = tileset_max_sse_;
        tileset_options.maximumSimultaneousTileLoads = tileset_max_simultaneous_loads_;
        tileset_options.maximumCachedBytes = 768LL * 1024 * 1024;
        tileset_options.preloadAncestors = true;
        tileset_options.preloadSiblings = preload_siblings_;
        tileset_options.forbidHoles = true;
        tileset_options.enableOcclusionCulling = false;
        tileset_options.enableFogCulling = false;
        tileset_options.loadingDescendantLimit =
            std::max(kDefaultLoadingDescendantLimit, tileset_max_simultaneous_loads_ * 4u);

        tileset_ = Cesium3DTilesSelection::EllipsoidTilesetLoader::createTileset(externals, tileset_options);
        if (!tileset_) {
            if (status_message != nullptr) {
                *status_message = "provider=" + basemap_config_.provider_id + " status=failed stage=tileset_create";
            }
            return false;
        }

        CesiumRasterOverlays::RasterOverlayOptions overlay_options;
        overlay_options.maximumSimultaneousTileLoads = static_cast<std::int32_t>(overlay_max_simultaneous_loads_);
        overlay_options.maximumTextureSize = 4096;
        overlay_options.maximumScreenSpaceError = raster_max_sse_;
        overlay_options.subTileCacheBytes = 256LL * 1024 * 1024;
        overlay_options.showCreditsOnScreen = true;
        overlay_options.loadErrorCallback = [this](const auto& details) {
            this->last_error_ = sanitize_tile_error_message(details.message);
            if (is_google_quota_error(details.message)) {
                this->quota_error_count_ += 1u;
                this->quota_backoff_until_ =
                    std::chrono::steady_clock::now() + std::chrono::seconds(this->quota_backoff_seconds_);
            }
        };

        switch (basemap_config_.kind) {
            case BasemapProviderKind::GoogleSatellite: {
                CesiumRasterOverlays::GoogleMapTilesNewSessionParameters session_parameters;
                session_parameters.key = basemap_config_.api_key;
                session_parameters.mapType = CesiumRasterOverlays::GoogleMapTilesMapType::satellite;
                session_parameters.language = basemap_config_.language;
                session_parameters.region = basemap_config_.region;
                session_parameters.imageFormat = CesiumRasterOverlays::GoogleMapTilesImageFormat::jpeg;
                session_parameters.scale = CesiumRasterOverlays::GoogleMapTilesScale::scaleFactor1x;
                session_parameters.highDpi = false;

                tileset_->getOverlays().add(
                    CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay>(
                        new CesiumRasterOverlays::GoogleMapTilesRasterOverlay(
                            basemap_config_.provider_id,
                            session_parameters,
                            overlay_options
                        )
                    )
                );
                break;
            }
            case BasemapProviderKind::MapTilerRaster: {
                CesiumRasterOverlays::UrlTemplateRasterOverlayOptions url_options;
                url_options.credit = "MapTiler";
                url_options.tileWidth = basemap_config_.hidpi ? 512u : 256u;
                url_options.tileHeight = basemap_config_.hidpi ? 512u : 256u;
                url_options.maximumLevel = 20u;
                const std::string scale_suffix = basemap_config_.hidpi ? "@2x" : "";
                const std::string url = "https://api.maptiler.com/maps/" +
                    basemap_config_.style_id +
                    "/256/{z}/{x}/{reverseY}" +
                    scale_suffix +
                    ".png?key=" +
                    basemap_config_.api_key;

                tileset_->getOverlays().add(
                    CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay>(
                        new CesiumRasterOverlays::UrlTemplateRasterOverlay(
                            basemap_config_.provider_id,
                            url,
                            {},
                            url_options,
                            overlay_options
                        )
                    )
                );
                break;
            }
            case BasemapProviderKind::StadiaRaster: {
                CesiumRasterOverlays::UrlTemplateRasterOverlayOptions url_options;
                url_options.credit = "Stadia Maps";
                url_options.tileWidth = basemap_config_.hidpi ? 512u : 256u;
                url_options.tileHeight = basemap_config_.hidpi ? 512u : 256u;
                url_options.maximumLevel = 20u;
                const std::string scale_suffix = basemap_config_.hidpi ? "@2x" : "";
                const std::string url = "https://tiles.stadiamaps.com/tiles/" +
                    basemap_config_.style_id +
                    "/{z}/{x}/{reverseY}" +
                    scale_suffix +
                    ".png?api_key=" +
                    basemap_config_.api_key;

                tileset_->getOverlays().add(
                    CesiumUtility::IntrusivePointer<CesiumRasterOverlays::RasterOverlay>(
                        new CesiumRasterOverlays::UrlTemplateRasterOverlay(
                            basemap_config_.provider_id,
                            url,
                            {},
                            url_options,
                            overlay_options
                        )
                    )
                );
                break;
            }
            case BasemapProviderKind::Disabled:
                if (status_message != nullptr) {
                    *status_message = "provider=none status=failed stage=overlay_create";
                }
                return false;
        }

        return true;
    }

    [[nodiscard]] bool is_quota_backoff_active() const noexcept {
        return quota_backoff_until_ > std::chrono::steady_clock::now();
    }

    std::shared_ptr<CesiumAsync::IAssetAccessor> create_asset_accessor() {
        return create_frameflow_asset_accessor(scene_options_, &asset_cache_status_);
    }

    [[nodiscard]] int quota_backoff_remaining_seconds() const noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (quota_backoff_until_ <= now) {
            return 0;
        }
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(quota_backoff_until_ - now).count()
        );
    }

    [[nodiscard]] Cesium3DTilesSelection::ViewState make_view_state(
        const RenderedGlobeScene::CameraState& camera
    ) const {
        const PerspectiveProjection projection = make_perspective_projection(camera, width_, height_);
        const double horizontal_field_of_view = kHorizontalFieldOfViewDegrees * kDegreesToRadians;
        const double aspect_ratio = static_cast<double>(width_) / static_cast<double>(std::max(1, height_));
        const double vertical_field_of_view =
            std::atan(std::tan(horizontal_field_of_view * 0.5) / std::max(0.0001, aspect_ratio)) * 2.0;
        return Cesium3DTilesSelection::ViewState(
            projection.position,
            projection.forward,
            projection.up,
            glm::dvec2(static_cast<double>(width_), static_cast<double>(height_)),
            horizontal_field_of_view,
            vertical_field_of_view
        );
    }

    static glm::dvec3 normalize_position(const glm::dvec3& position) {
        const double length = glm::length(position);
        if (length <= 1e-9) {
            return glm::dvec3(0.0, 0.0, 1.0);
        }
        return position / length;
    }

    static std::size_t count_overlay_bindings(const TileRenderResources& resources) {
        std::size_t count = 0u;
        for (const auto& [overlay_id, bindings] : resources.overlay_bindings) {
            (void)overlay_id;
            count += bindings.size();
        }
        return count;
    }

    static int tile_level(const Cesium3DTilesSelection::Tile& tile) {
        const Cesium3DTilesSelection::TileID& tile_id = tile.getTileID();
        if (const auto* quadtree_id = std::get_if<CesiumGeometry::QuadtreeTileID>(&tile_id)) {
            return static_cast<int>(quadtree_id->level);
        }
        if (const auto* upsampled_node = std::get_if<CesiumGeometry::UpsampledQuadtreeNode>(&tile_id)) {
            return static_cast<int>(upsampled_node->tileID.level);
        }
        return -1;
    }

    static std::optional<CesiumGeometry::QuadtreeTileID> quadtree_tile_id(const Cesium3DTilesSelection::Tile& tile) {
        const Cesium3DTilesSelection::TileID& tile_id = tile.getTileID();
        if (const auto* quadtree_id = std::get_if<CesiumGeometry::QuadtreeTileID>(&tile_id)) {
            return *quadtree_id;
        }
        if (const auto* upsampled_node = std::get_if<CesiumGeometry::UpsampledQuadtreeNode>(&tile_id)) {
            return upsampled_node->tileID;
        }
        return std::nullopt;
    }

    static std::string summarize_tile_levels(const std::map<int, std::size_t>& tile_levels) {
        if (tile_levels.empty()) {
            return "none";
        }

        std::ostringstream out;
        bool first = true;
        for (const auto& [level, count] : tile_levels) {
            if (!first) {
                out << ",";
            }
            first = false;
            out << "L" << level << ":" << count;
        }
        return out.str();
    }

    static std::string summarize_tile_lod_span(const int min_level, const int max_level) {
        if (min_level < 0 || max_level < 0) {
            return "none";
        }
        std::ostringstream out;
        out << "L" << min_level << "..L" << max_level;
        return out.str();
    }

    static std::string summarize_imagery_levels(const std::map<int, std::size_t>& imagery_levels) {
        if (imagery_levels.empty()) {
            return "none";
        }

        std::ostringstream out;
        bool first = true;
        for (const auto& [level, count] : imagery_levels) {
            if (!first) {
                out << ",";
            }
            first = false;
            out << "Z" << level << ":" << count;
        }
        return out.str();
    }

    static std::string summarize_imagery_lod_span(const std::map<int, std::size_t>& imagery_levels) {
        if (imagery_levels.empty()) {
            return "none";
        }
        std::ostringstream out;
        out << "Z" << imagery_levels.begin()->first << "..Z" << imagery_levels.rbegin()->first;
        return out.str();
    }

    static std::size_t count_rendered_descendants_with_rendered_ancestor(
        const std::vector<RenderTileCandidate>& candidates
    ) {
        std::unordered_set<CesiumGeometry::QuadtreeTileID> rendered_ids;
        rendered_ids.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            if (candidate.quadtree_id.has_value()) {
                rendered_ids.insert(*candidate.quadtree_id);
            }
        }

        std::size_t overlap_count = 0u;
        for (const auto& candidate : candidates) {
            if (!candidate.quadtree_id.has_value()) {
                continue;
            }

            auto ancestor = *candidate.quadtree_id;
            while (ancestor.level > 0u) {
                ancestor = ancestor.getParent();
                if (rendered_ids.find(ancestor) != rendered_ids.end()) {
                    overlap_count += 1u;
                    break;
                }
            }
        }
        return overlap_count;
    }

    static MarkerColor debug_lod_color(const int level) {
        if (level < 0) {
            return MarkerColor{1.0F, 1.0F, 1.0F};
        }
        return kMarkerPalette[static_cast<std::size_t>(level) % kMarkerPalette.size()];
    }

    static glm::dvec2 transform_uv(const glm::vec2& uv, const OverlayBinding& binding) {
        return glm::dvec2(
            (static_cast<double>(uv.x) * binding.scale.x) + binding.translation.x,
            (static_cast<double>(uv.y) * binding.scale.y) + binding.translation.y
        );
    }

    static bool uv_inside_unit_square(const glm::dvec2& uv) {
        constexpr double kUvEpsilon = 1e-6;
        return uv.x >= -kUvEpsilon &&
            uv.x <= 1.0 + kUvEpsilon &&
            uv.y >= -kUvEpsilon &&
            uv.y <= 1.0 + kUvEpsilon;
    }

    static std::vector<const OverlayBinding*> bindings_sorted_by_resolution(
        const std::vector<OverlayBinding>& bindings
    ) {
        std::vector<const OverlayBinding*> sorted;
        sorted.reserve(bindings.size());
        for (const auto& binding : bindings) {
            sorted.push_back(&binding);
        }
        std::stable_sort(
            sorted.begin(),
            sorted.end(),
            [](const OverlayBinding* left, const OverlayBinding* right) {
                const int left_level = left == nullptr ? -1 : left->estimated_imagery_level;
                const int right_level = right == nullptr ? -1 : right->estimated_imagery_level;
                return left_level < right_level;
            }
        );
        return sorted;
    }

    std::string shader_info_log(const GLuint shader) const {
        std::array<GLchar, 1024> buffer{};
        GLsizei written = 0;
        raster_clip_shader_api_.get_shader_info_log(
            shader,
            static_cast<GLsizei>(buffer.size()),
            &written,
            buffer.data()
        );
        return std::string(buffer.data(), static_cast<std::size_t>(std::max(0, written)));
    }

    std::string program_info_log(const GLuint program) const {
        std::array<GLchar, 1024> buffer{};
        GLsizei written = 0;
        raster_clip_shader_api_.get_program_info_log(
            program,
            static_cast<GLsizei>(buffer.size()),
            &written,
            buffer.data()
        );
        return std::string(buffer.data(), static_cast<std::size_t>(std::max(0, written)));
    }

    GLuint compile_shader_stage(const GLenum type, const GLchar* source, std::string* error_message) const {
        const GLuint shader = raster_clip_shader_api_.create_shader(type);
        if (shader == 0u) {
            if (error_message != nullptr) {
                *error_message = "glCreateShader returned 0";
            }
            return 0u;
        }

        raster_clip_shader_api_.shader_source(shader, 1, &source, nullptr);
        raster_clip_shader_api_.compile_shader(shader);

        GLint compiled = GL_FALSE;
        raster_clip_shader_api_.get_shader_iv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            if (error_message != nullptr) {
                *error_message = shader_info_log(shader);
                if (error_message->empty()) {
                    *error_message = "shader compile failed without an info log";
                }
            }
            raster_clip_shader_api_.delete_shader(shader);
            return 0u;
        }
        return shader;
    }

    [[nodiscard]] bool ensure_raster_clip_shader() const {
        if (raster_clip_shader_.ready()) {
            return true;
        }
        if (raster_clip_shader_attempted_) {
            return false;
        }
        raster_clip_shader_attempted_ = true;

        if (!raster_clip_shader_api_.load()) {
            raster_clip_shader_status_ = "unavailable:missing_glsl_api";
            return false;
        }

        static constexpr const GLchar* kVertexShader = R"(
#version 110
varying vec2 vRasterUv;

void main() {
    vRasterUv = gl_MultiTexCoord0.xy;
    gl_FrontColor = gl_Color;
    gl_Position = ftransform();
}
)";

        static constexpr const GLchar* kFragmentShader = R"(
#version 110
uniform sampler2D uRasterTexture;
uniform float uUvEpsilon;
varying vec2 vRasterUv;

void main() {
    if (vRasterUv.x < -uUvEpsilon || vRasterUv.x > 1.0 + uUvEpsilon ||
        vRasterUv.y < -uUvEpsilon || vRasterUv.y > 1.0 + uUvEpsilon) {
        discard;
    }
    gl_FragColor = texture2D(uRasterTexture, clamp(vRasterUv, 0.0, 1.0)) * gl_Color;
}
)";

        std::string error_message;
        const GLuint vertex_shader = compile_shader_stage(GL_VERTEX_SHADER, kVertexShader, &error_message);
        if (vertex_shader == 0u) {
            raster_clip_shader_status_ = "compile_failed:vertex:" + error_message;
            return false;
        }

        const GLuint fragment_shader = compile_shader_stage(GL_FRAGMENT_SHADER, kFragmentShader, &error_message);
        if (fragment_shader == 0u) {
            raster_clip_shader_api_.delete_shader(vertex_shader);
            raster_clip_shader_status_ = "compile_failed:fragment:" + error_message;
            return false;
        }

        const GLuint program = raster_clip_shader_api_.create_program();
        raster_clip_shader_api_.attach_shader(program, vertex_shader);
        raster_clip_shader_api_.attach_shader(program, fragment_shader);
        raster_clip_shader_api_.link_program(program);

        raster_clip_shader_api_.delete_shader(vertex_shader);
        raster_clip_shader_api_.delete_shader(fragment_shader);

        GLint linked = GL_FALSE;
        raster_clip_shader_api_.get_program_iv(program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            raster_clip_shader_status_ = "link_failed:" + program_info_log(program);
            raster_clip_shader_api_.delete_program(program);
            return false;
        }

        raster_clip_shader_.program_id = program;
        raster_clip_shader_.sampler_location =
            raster_clip_shader_api_.get_uniform_location(program, "uRasterTexture");
        raster_clip_shader_.uv_epsilon_location =
            raster_clip_shader_api_.get_uniform_location(program, "uUvEpsilon");
        raster_clip_shader_status_ = "ready";
        return true;
    }

    void destroy_raster_clip_shader() noexcept {
        if (raster_clip_shader_.program_id == 0u || raster_clip_shader_api_.delete_program == nullptr) {
            return;
        }
        raster_clip_shader_api_.delete_program(raster_clip_shader_.program_id);
        raster_clip_shader_ = RasterClipShader{};
    }

    static bool triangle_faces_camera(
        const glm::dvec3& position0,
        const glm::dvec3& position1,
        const glm::dvec3& position2,
        const PerspectiveProjection& projection
    ) {
        const glm::dvec3 center = (position0 + position1 + position2) / 3.0;
        const glm::dvec3 outward = normalize_position(center);
        const glm::dvec3 to_camera = normalize_position(projection.position - center);
        return glm::dot(outward, to_camera) > -0.04;
    }

    std::size_t draw_tile_resources(
        const TileRenderResources& resources,
        const PerspectiveProjection& projection,
        const int tile_level
    ) const {
        (void)projection;
        std::size_t drawn_triangles = 0u;
        const bool raster_clip_enabled = ensure_raster_clip_shader();
        if (raster_clip_enabled) {
            raster_clip_shader_api_.use_program(raster_clip_shader_.program_id);
            if (raster_clip_shader_.sampler_location >= 0) {
                raster_clip_shader_api_.uniform_1i(raster_clip_shader_.sampler_location, 0);
            }
            if (raster_clip_shader_.uv_epsilon_location >= 0) {
                raster_clip_shader_api_.uniform_1f(raster_clip_shader_.uv_epsilon_location, 1e-5F);
            }
        }

        for (const auto& [overlay_id, bindings] : resources.overlay_bindings) {
            for (const auto& primitive : resources.primitives) {
                const auto texcoords_it = primitive.overlay_texcoords.find(overlay_id);
                if (texcoords_it == primitive.overlay_texcoords.end()) {
                    continue;
                }
                const auto& texcoords = texcoords_it->second;
                if (texcoords.size() != primitive.positions.size() ||
                    primitive.position_components.size() != primitive.positions.size() * 3u ||
                    primitive.indices.size() < 3u) {
                    continue;
                }

                const auto sorted_bindings = bindings_sorted_by_resolution(bindings);
                for (const auto* binding : sorted_bindings) {
                    if (binding == nullptr || binding->texture_id == 0u) {
                        continue;
                    }

                    std::vector<GLfloat> texture_coordinates;
                    texture_coordinates.reserve(texcoords.size() * 2u);
                    bool has_inside_uv = false;
                    bool has_outside_uv = false;
                    for (const auto& uv : texcoords) {
                        const glm::dvec2 transformed_uv = transform_uv(uv, *binding);
                        const bool inside = uv_inside_unit_square(transformed_uv);
                        has_inside_uv = has_inside_uv || inside;
                        has_outside_uv = has_outside_uv || !inside;
                        if (!inside) {
                            last_overlay_uv_outside_vertex_count_ += 1u;
                        }
                        texture_coordinates.push_back(static_cast<GLfloat>(transformed_uv.x));
                        texture_coordinates.push_back(static_cast<GLfloat>(transformed_uv.y));
                    }
                    if (texture_coordinates.size() != texcoords.size() * 2u) {
                        continue;
                    }

                    record_overlay_binding_diagnostics(*binding);
                    if (has_inside_uv && has_outside_uv) {
                        last_overlay_partially_clipped_binding_count_ += 1u;
                    } else if (has_outside_uv) {
                        last_overlay_fully_outside_binding_count_ += 1u;
                    }
                    glBindTexture(GL_TEXTURE_2D, binding->texture_id);
                    if (debug_options_.tile_lod_colors) {
                        const MarkerColor color = debug_lod_color(tile_level);
                        glColor4f(color.red, color.green, color.blue, 0.74F);
                    } else {
                        glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
                    }

                    glEnableClientState(GL_VERTEX_ARRAY);
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    glTexCoordPointer(2, GL_FLOAT, 0, texture_coordinates.data());
                    glVertexPointer(3, GL_DOUBLE, 0, primitive.position_components.data());
                    glDrawElements(
                        GL_TRIANGLES,
                        static_cast<GLsizei>(primitive.indices.size()),
                        GL_UNSIGNED_INT,
                        primitive.indices.data()
                    );
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                    glDisableClientState(GL_VERTEX_ARRAY);
                    drawn_triangles += primitive.indices.size() / 3u;
                }
            }
        }
        if (raster_clip_enabled) {
            raster_clip_shader_api_.use_program(0u);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return drawn_triangles;
    }

    [[nodiscard]] bool is_camera_settled(const RenderedGlobeScene::CameraState& camera) const noexcept {
        if (!previous_render_camera_.has_value()) {
            return false;
        }
        const auto& previous = *previous_render_camera_;
        return std::abs(previous.longitude - camera.longitude) < 0.0001 &&
            std::abs(previous.latitude - camera.latitude) < 0.0001 &&
            std::abs(previous.height_meters - camera.height_meters) < 1.0 &&
            std::abs(previous.heading_degrees - camera.heading_degrees) < 0.001 &&
            std::abs(previous.pitch_degrees - camera.pitch_degrees) < 0.001 &&
            std::abs(previous.roll_degrees - camera.roll_degrees) < 0.001;
    }

    void record_overlay_binding_diagnostics(const OverlayBinding& binding) const noexcept {
        const double scale_x = std::abs(binding.scale.x);
        const double scale_y = std::abs(binding.scale.y);
        const double min_scale = std::min(scale_x, scale_y);
        const double max_scale = std::max(scale_x, scale_y);
        last_drawn_overlay_binding_count_ += 1u;
        last_min_overlay_scale_ = last_drawn_overlay_binding_count_ == 1u
            ? min_scale
            : std::min(last_min_overlay_scale_, min_scale);
        last_max_overlay_scale_ = last_drawn_overlay_binding_count_ == 1u
            ? max_scale
            : std::max(last_max_overlay_scale_, max_scale);
        if (min_scale < 0.999 || max_scale > 1.001) {
            last_suspect_parent_raster_binding_count_ += 1u;
        }
        if (binding.estimated_imagery_level >= 0) {
            last_imagery_level_counts_[binding.estimated_imagery_level] += 1u;
            last_imagery_lod_span_summary_ = summarize_imagery_lod_span(last_imagery_level_counts_);
        }
    }

    BasemapProviderConfig basemap_config_;
    RenderedGlobeScene::Options scene_options_;
    DebugOptions debug_options_;
    std::string asset_cache_status_ = "disabled";
    std::shared_ptr<JoiningTaskProcessor> task_processor_;
    CesiumAsync::AsyncSystem async_system_;
    std::shared_ptr<CesiumAsync::IAssetAccessor> asset_accessor_;
    std::shared_ptr<CesiumUtility::CreditSystem> credit_system_;
    std::shared_ptr<PrepareRendererResources> renderer_resources_;
    std::unique_ptr<Cesium3DTilesSelection::Tileset> tileset_;
    Cesium3DTilesSelection::TilesetViewGroup view_group_;
    int width_ = 1;
    int height_ = 1;
    std::size_t last_ready_tile_count_ = 0u;
    std::size_t last_ready_overlay_count_ = 0u;
    std::size_t last_rendered_tile_count_ = 0u;
    std::size_t last_skipped_tile_count_ = 0u;
    std::size_t last_tiles_fading_out_count_ = 0u;
    std::size_t last_parent_child_overlap_count_ = 0u;
    mutable std::size_t last_drawn_overlay_binding_count_ = 0u;
    mutable std::size_t last_suspect_parent_raster_binding_count_ = 0u;
    mutable std::size_t last_overlay_uv_outside_vertex_count_ = 0u;
    mutable std::size_t last_overlay_partially_clipped_binding_count_ = 0u;
    mutable std::size_t last_overlay_fully_outside_binding_count_ = 0u;
    mutable double last_min_overlay_scale_ = 1.0;
    mutable double last_max_overlay_scale_ = 1.0;
    mutable std::map<int, std::size_t> last_imagery_level_counts_;
    mutable std::string last_imagery_lod_span_summary_ = "none";
    mutable RasterClipShaderApi raster_clip_shader_api_;
    mutable RasterClipShader raster_clip_shader_;
    mutable bool raster_clip_shader_attempted_ = false;
    mutable std::string raster_clip_shader_status_ = "uninitialized";
    float last_min_lod_fade_ = 1.0F;
    float last_max_lod_fade_ = 1.0F;
    float last_load_progress_ = 0.0f;
    std::string last_tile_level_summary_ = "none";
    std::string last_tile_lod_span_summary_ = "none";
    double tileset_max_sse_ = 4.0;
    double raster_max_sse_ = 2.0;
    int tileset_active_pump_iterations_ = kDefaultTilesetActivePumpIterations;
    int tileset_settle_pump_iterations_ = kDefaultTilesetSettlePumpIterations;
    int last_pump_iterations_ = 0;
    bool last_camera_settled_ = false;
    std::optional<RenderedGlobeScene::CameraState> previous_render_camera_;
    std::uint32_t tileset_max_simultaneous_loads_ = 4u;
    std::uint32_t overlay_max_simultaneous_loads_ = 4u;
    int quota_backoff_seconds_ = 600;
    bool preload_siblings_ = false;
    std::chrono::steady_clock::time_point quota_backoff_until_{};
    std::uint32_t quota_error_count_ = 0u;
    double last_sse_max_ = 0.0;
    double last_sse_avg_ = 0.0;
    std::uint32_t last_max_depth_visited_ = 0u;
    std::int32_t last_worker_queue_length_ = 0;
    std::int32_t last_main_queue_length_ = 0;
    std::string last_error_;
};

class UniformRasterGlobeLayer {
public:
    struct Snapshot {
        std::size_t tiles_ready = 0u;
        std::size_t overlays_ready = 0u;
        float load_progress = 0.0F;
        std::string last_error;
    };

    static std::unique_ptr<UniformRasterGlobeLayer> create(
        BasemapProviderConfig basemap_config,
        const RenderedGlobeScene::Options& scene_options,
        std::string* status_message
    ) {
        if (basemap_config.kind != BasemapProviderKind::MapTilerRaster &&
            basemap_config.kind != BasemapProviderKind::StadiaRaster) {
            return nullptr;
        }
        auto layer = std::unique_ptr<UniformRasterGlobeLayer>(new UniformRasterGlobeLayer(
            std::move(basemap_config),
            scene_options
        ));
        if (status_message != nullptr) {
            *status_message = layer->diagnostics_summary();
        }
        return layer;
    }

    ~UniformRasterGlobeLayer() {
        state_.reset();
        if (task_processor_) {
            task_processor_->shutdown();
        }
        for (auto& [key, tile] : tiles_) {
            (void)key;
            if (tile.texture_id != 0u) {
                glDeleteTextures(1, &tile.texture_id);
                tile.texture_id = 0u;
            }
        }
    }

    void resize(const int width, const int height) noexcept {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
    }

    bool render(
        const RenderedGlobeScene::CameraState& camera,
        const int width,
        const int height,
        std::string* status_message
    ) {
        resize(width, height);
        async_system_.dispatchMainThreadTasks();

        last_requested_tile_count_ = 0u;
        last_ready_tile_count_ = 0u;
        last_rendered_tile_count_ = 0u;
        last_started_load_count_ = 0u;
        last_pending_tile_count_ = 0u;
        last_exact_tile_render_count_ = 0u;
        last_parent_fallback_tile_render_count_ = 0u;
        last_missing_tile_count_ = 0u;
        last_uniform_zoom_ = estimate_zoom(camera);

        const auto visible_tiles = select_visible_tiles(camera, last_uniform_zoom_);
        last_requested_tile_count_ = visible_tiles.size();
        for (const auto& tile_key : visible_tiles) {
            ensure_parent_fallback_requested(tile_key);
            ensure_tile_requested(tile_key);
        }

        const PerspectiveProjection projection = make_perspective_projection(camera, width_, height_);
        set_tile_perspective_matrices(projection);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glEnable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glColor4f(1.0F, 1.0F, 1.0F, 1.0F);

        std::size_t rendered = 0u;
        for (const auto& tile_key : visible_tiles) {
            const auto tile = tiles_.find(tile_key.cache_key());
            if (tile != tiles_.end()) {
                tile->second.last_used_frame = frame_counter_;
                if (tile->second.state == TileState::Ready && tile->second.texture_id != 0u) {
                    last_ready_tile_count_ += 1u;
                    if (draw_tile_patch(tile_key, tile->second.texture_id, projection)) {
                        rendered += 1u;
                        last_exact_tile_render_count_ += 1u;
                        continue;
                    }
                } else if (tile->second.state == TileState::Loading) {
                    last_pending_tile_count_ += 1u;
                }
            } else {
                last_pending_tile_count_ += 1u;
            }

            const auto parent = find_ready_parent_tile(tile_key);
            if (parent.has_value()) {
                auto parent_record = tiles_.find(parent->cache_key());
                if (parent_record != tiles_.end()) {
                    parent_record->second.last_used_frame = frame_counter_;
                    if (draw_tile_patch_with_source_uv(tile_key, *parent, parent_record->second.texture_id, projection)) {
                        rendered += 1u;
                        last_parent_fallback_tile_render_count_ += 1u;
                        continue;
                    }
                }
            }
            last_missing_tile_count_ += 1u;
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
        glDepthMask(GL_TRUE);

        last_rendered_tile_count_ = rendered;
        last_load_progress_ = last_requested_tile_count_ == 0u
            ? 0.0F
            : static_cast<float>(last_ready_tile_count_) / static_cast<float>(last_requested_tile_count_);
        evict_unused_tiles();
        frame_counter_ += 1u;

        if (status_message != nullptr) {
            *status_message = diagnostics_summary();
        }
        return rendered > 0u;
    }

    [[nodiscard]] Snapshot snapshot() const {
        return Snapshot{
            .tiles_ready = last_ready_tile_count_,
            .overlays_ready = last_ready_tile_count_,
            .load_progress = last_load_progress_,
            .last_error = last_error_
        };
    }

    [[nodiscard]] std::string diagnostics_summary() const {
        std::ostringstream out;
        out
            << "provider=" << basemap_config_.provider_id
            << " style=" << (basemap_config_.style_id.empty() ? "default" : basemap_config_.style_id)
            << " hidpi=" << (basemap_config_.hidpi ? "1" : "0")
            << " asset_cache=" << asset_cache_status_
            << " uniform_raster=1"
            << " tiles_ready=" << last_ready_tile_count_
            << " tiles_rendered=" << last_rendered_tile_count_
            << " coverage_exact=" << last_exact_tile_render_count_
            << " coverage_parent=" << last_parent_fallback_tile_render_count_
            << " coverage_missing=" << last_missing_tile_count_
            << " tiles_skipped=0"
            << " tile_levels=Z" << last_uniform_zoom_ << ":" << last_rendered_tile_count_
            << " tile_lod_span=Z" << last_uniform_zoom_ << "..Z" << last_uniform_zoom_
            << " tiles_fading_out=0"
            << " lod_fade_min=1"
            << " lod_fade_max=1"
            << " parent_child_overlap_count=0"
            << " overlays_ready=" << last_ready_tile_count_
            << " overlays_drawn=" << last_rendered_tile_count_
            << " suspect_parent_rasters=" << last_parent_fallback_tile_render_count_
            << " overlay_scale_min=1"
            << " overlay_scale_max=1"
            << " raster_clip_shader=not_needed"
            << " overlay_uv_outside_vertices=0"
            << " overlay_partial_clip_bindings=0"
            << " overlay_fully_outside_bindings=0"
            << " imagery_levels=Z" << last_uniform_zoom_ << ":" << last_rendered_tile_count_
            << " imagery_lod_span=Z" << last_uniform_zoom_ << "..Z" << last_uniform_zoom_
            << " tileset_max_sse=uniform"
            << " raster_max_sse=uniform"
            << " pump_iterations=1"
            << " active_pump_iterations=1"
            << " settle_pump_iterations=1"
            << " camera_settled=1"
            << " tile_loads=" << kMaxStartsPerFrame
            << " overlay_loads=" << kMaxStartsPerFrame
            << " preload_siblings=0"
            << " quota_backoff_seconds=0"
            << " quota_error_count=0"
            << " sse_max=0"
            << " sse_avg=0"
            << " max_depth=" << last_uniform_zoom_
            << " worker_queue=" << pending_load_count_
            << " main_queue=0"
            << " load_progress=" << (last_load_progress_ * 100.0F);
        if (!last_error_.empty()) {
            out << " last_error=[" << last_error_ << "]";
        }
        return out.str();
    }

private:
    enum class TileState {
        Loading,
        Ready,
        Failed
    };

    struct TileKey {
        int z = 0;
        int x = 0;
        int y = 0;

        [[nodiscard]] std::string cache_key() const {
            return std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
        }
    };

    struct TileRecord {
        TileState state = TileState::Loading;
        GLuint texture_id = 0u;
        int width = 0;
        int height = 0;
        std::uint64_t last_used_frame = 0u;
    };

    struct LoadedTile {
        TileKey key;
        CesiumUtility::IntrusivePointer<CesiumGltf::ImageAsset> image;
        std::string error;
    };

    static constexpr int kTileSizePixels = 256;
    static constexpr int kMinimumZoom = 1;
    static constexpr int kMaximumZoom = 18;
    static constexpr int kPatchSegments = 10;
    static constexpr std::size_t kMaxStartsPerFrame = 12u;
    static constexpr std::size_t kMaxInFlightLoads = 48u;
    static constexpr std::size_t kMaxResidentTiles = 768u;
    static constexpr int kParentFallbackRequestDepth = 2;
    static constexpr double kWgs84RadiusMeters = 6378137.0;
    static constexpr double kWebMercatorMaxLatitude = 85.05112878;

    UniformRasterGlobeLayer(
        BasemapProviderConfig basemap_config,
        const RenderedGlobeScene::Options& scene_options
    )
        : basemap_config_(std::move(basemap_config)),
          task_processor_(std::make_shared<JoiningTaskProcessor>()),
          async_system_(task_processor_),
          asset_accessor_(create_frameflow_asset_accessor(scene_options, &asset_cache_status_)) {}

    static double clamp_web_mercator_latitude(const double latitude) noexcept {
        return std::clamp(latitude, -kWebMercatorMaxLatitude, kWebMercatorMaxLatitude);
    }

    static int wrap_tile_x(const int x, const int z) noexcept {
        const int n = 1 << z;
        const int wrapped = x % n;
        return wrapped < 0 ? wrapped + n : wrapped;
    }

    static int longitude_to_tile_x_floor(const double longitude, const int z) noexcept {
        const double n = static_cast<double>(1 << z);
        return static_cast<int>(std::floor(((longitude + 180.0) / 360.0) * n));
    }

    static int latitude_to_tile_y_floor(const double latitude, const int z) noexcept {
        const double clamped_latitude = clamp_web_mercator_latitude(latitude);
        const double latitude_radians = clamped_latitude * kDegreesToRadians;
        const double n = static_cast<double>(1 << z);
        const double y = (1.0 - (std::asinh(std::tan(latitude_radians)) / kPi)) * 0.5 * n;
        return static_cast<int>(std::floor(y));
    }

    static double tile_x_to_longitude(const int x, const int z) noexcept {
        const double n = static_cast<double>(1 << z);
        return (static_cast<double>(x) / n * 360.0) - 180.0;
    }

    static double tile_y_to_latitude(const int y, const int z) noexcept {
        const double n = static_cast<double>(1 << z);
        const double value = kPi * (1.0 - (2.0 * static_cast<double>(y) / n));
        return std::atan(std::sinh(value)) / kDegreesToRadians;
    }

    static double mercator_normalized_y(const double latitude) noexcept {
        const double clamped_latitude = clamp_web_mercator_latitude(latitude);
        const double latitude_radians = clamped_latitude * kDegreesToRadians;
        return (1.0 - (std::asinh(std::tan(latitude_radians)) / kPi)) * 0.5;
    }

    static int visible_tile_cap_for_axis(const int pixels) noexcept {
        // Wide Watch Desk viewports can legitimately need more than the old
        // fixed 18x14 tile window at low zoom. Keep a cap, but scale it with
        // the render target so ultrawide resize does not clip the raster edge.
        return std::max(18, ((std::max(1, pixels) + 127) / 128) + 8);
    }

    int estimate_zoom(const RenderedGlobeScene::CameraState& camera) const noexcept {
        const double horizontal_fov = kHorizontalFieldOfViewDegrees * kDegreesToRadians;
        const double ground_span_meters =
            std::max(1.0, 2.0 * camera.height_meters * std::tan(horizontal_fov * 0.5));
        const double meters_per_pixel = ground_span_meters / static_cast<double>(std::max(1, width_));
        const double latitude_factor = std::max(
            0.18,
            std::cos(clamp_web_mercator_latitude(camera.latitude) * kDegreesToRadians)
        );
        const double zoom = std::log2((2.0 * kPi * kWgs84RadiusMeters * latitude_factor) /
            (meters_per_pixel * static_cast<double>(kTileSizePixels)));
        if (!std::isfinite(zoom)) {
            return kMinimumZoom;
        }
        return std::clamp(static_cast<int>(std::round(zoom)), kMinimumZoom, kMaximumZoom);
    }

    std::vector<TileKey> select_visible_tiles(
        const RenderedGlobeScene::CameraState& camera,
        const int z
    ) const {
        const double horizontal_fov = kHorizontalFieldOfViewDegrees * kDegreesToRadians;
        const double aspect_ratio = static_cast<double>(std::max(1, width_)) / static_cast<double>(std::max(1, height_));
        const double vertical_fov = std::atan(std::tan(horizontal_fov * 0.5) / std::max(0.0001, aspect_ratio)) * 2.0;
        const double horizontal_span_meters = 2.0 * camera.height_meters * std::tan(horizontal_fov * 0.5) * 1.35;
        const double vertical_span_meters = 2.0 * camera.height_meters * std::tan(vertical_fov * 0.5) * 1.35;
        const double latitude_factor = std::max(
            0.18,
            std::cos(clamp_web_mercator_latitude(camera.latitude) * kDegreesToRadians)
        );
        const double longitude_span = (horizontal_span_meters / (kWgs84RadiusMeters * latitude_factor)) / kDegreesToRadians;
        const double latitude_span = (vertical_span_meters / kWgs84RadiusMeters) / kDegreesToRadians;

        const double west = camera.longitude - (longitude_span * 0.5);
        const double east = camera.longitude + (longitude_span * 0.5);
        const double south = clamp_web_mercator_latitude(camera.latitude - (latitude_span * 0.5));
        const double north = clamp_web_mercator_latitude(camera.latitude + (latitude_span * 0.5));

        const int n = 1 << z;
        const int x_start = longitude_to_tile_x_floor(west, z) - 1;
        const int x_end = longitude_to_tile_x_floor(east, z) + 1;
        const int y_start = std::clamp(latitude_to_tile_y_floor(north, z) - 1, 0, n - 1);
        const int y_end = std::clamp(latitude_to_tile_y_floor(south, z) + 1, 0, n - 1);

        std::vector<TileKey> result;
        const int max_x_tiles = std::min(n, visible_tile_cap_for_axis(width_));
        const int max_y_tiles = std::min(n, visible_tile_cap_for_axis(height_));
        const int clamped_x_end = std::min(x_end, x_start + max_x_tiles - 1);
        const int clamped_y_end = std::min(y_end, y_start + max_y_tiles - 1);
        result.reserve(static_cast<std::size_t>(
            std::max(0, clamped_x_end - x_start + 1) * std::max(0, clamped_y_end - y_start + 1)
        ));
        for (int y = y_start; y <= clamped_y_end; ++y) {
            for (int x = x_start; x <= clamped_x_end; ++x) {
                result.push_back(TileKey{z, wrap_tile_x(x, z), y});
            }
        }
        return result;
    }

    std::string tile_url(const TileKey& key) const {
        const std::string scale_suffix = basemap_config_.hidpi ? "@2x" : "";
        if (basemap_config_.kind == BasemapProviderKind::StadiaRaster) {
            return "https://tiles.stadiamaps.com/tiles/" +
                basemap_config_.style_id +
                "/" +
                std::to_string(key.z) +
                "/" +
                std::to_string(key.x) +
                "/" +
                std::to_string(key.y) +
                scale_suffix +
                ".png?api_key=" +
                basemap_config_.api_key;
        }
        return "https://api.maptiler.com/maps/" +
            basemap_config_.style_id +
            "/" +
            std::to_string(kTileSizePixels) +
            "/" +
            std::to_string(key.z) +
            "/" +
            std::to_string(key.x) +
            "/" +
            std::to_string(key.y) +
            scale_suffix +
            ".png?key=" +
            basemap_config_.api_key;
    }

    void ensure_tile_requested(const TileKey& key) {
        const std::string cache_key = key.cache_key();
        if (tiles_.find(cache_key) != tiles_.end()) {
            return;
        }
        if (pending_load_count_ >= kMaxInFlightLoads || last_started_load_count_ >= kMaxStartsPerFrame) {
            return;
        }

        TileRecord record;
        record.state = TileState::Loading;
        record.last_used_frame = frame_counter_;
        tiles_.emplace(cache_key, record);
        pending_load_count_ += 1u;
        last_started_load_count_ += 1u;

        std::weak_ptr<LifetimeState> weak_state = state_;
        asset_accessor_->get(async_system_, tile_url(key))
            .thenInWorkerThread([key](std::shared_ptr<CesiumAsync::IAssetRequest>&& request) {
                LoadedTile result;
                result.key = key;
                const auto* response = request != nullptr ? request->response() : nullptr;
                if (response == nullptr) {
                    result.error = "tile request failed";
                    return result;
                }
                if (response->statusCode() != 0 &&
                    (response->statusCode() < 200 || response->statusCode() >= 300)) {
                    result.error = "tile response code " + std::to_string(response->statusCode());
                    return result;
                }
                auto decoded = CesiumGltfReader::ImageDecoder::readImage(
                    response->data(),
                    CesiumGltf::Ktx2TranscodeTargets{}
                );
                if (!decoded.errors.empty()) {
                    result.error = decoded.errors.front();
                    return result;
                }
                result.image = decoded.pImage;
                return result;
            })
            .thenInMainThread([this, weak_state, cache_key](LoadedTile&& loaded) {
                if (weak_state.expired()) {
                    return;
                }
                auto tile = tiles_.find(cache_key);
                if (tile == tiles_.end()) {
                    return;
                }
                pending_load_count_ = pending_load_count_ > 0u ? pending_load_count_ - 1u : 0u;
                if (!loaded.error.empty() || !loaded.image) {
                    tile->second.state = TileState::Failed;
                    last_error_ = loaded.error.empty() ? "tile decode failed" : loaded.error;
                    return;
                }
                GLuint texture_id = upload_texture(*loaded.image);
                if (texture_id == 0u) {
                    tile->second.state = TileState::Failed;
                    last_error_ = "tile texture upload failed";
                    return;
                }
                tile->second.texture_id = texture_id;
                tile->second.width = loaded.image->width;
                tile->second.height = loaded.image->height;
                tile->second.state = TileState::Ready;
            });
    }

    static TileKey parent_key_for_zoom(const TileKey& key, const int parent_zoom) noexcept {
        const int zoom_delta = std::max(0, key.z - parent_zoom);
        return TileKey{
            parent_zoom,
            key.x >> zoom_delta,
            key.y >> zoom_delta
        };
    }

    void ensure_parent_fallback_requested(const TileKey& key) {
        if (key.z <= kMinimumZoom) {
            return;
        }
        const int coarsest_parent_zoom = std::max(kMinimumZoom, key.z - kParentFallbackRequestDepth);
        for (int parent_zoom = coarsest_parent_zoom; parent_zoom < key.z; ++parent_zoom) {
            ensure_tile_requested(parent_key_for_zoom(key, parent_zoom));
            if (pending_load_count_ >= kMaxInFlightLoads || last_started_load_count_ >= kMaxStartsPerFrame) {
                return;
            }
        }
    }

    [[nodiscard]] std::optional<TileKey> find_ready_parent_tile(const TileKey& key) const {
        for (int parent_zoom = key.z - 1; parent_zoom >= kMinimumZoom; --parent_zoom) {
            const TileKey parent = parent_key_for_zoom(key, parent_zoom);
            const auto tile = tiles_.find(parent.cache_key());
            if (tile != tiles_.end() &&
                tile->second.state == TileState::Ready &&
                tile->second.texture_id != 0u) {
                return parent;
            }
        }
        return std::nullopt;
    }

    GLuint upload_texture(CesiumGltf::ImageAsset image) const {
        if (image.bytesPerChannel != 1) {
            return 0u;
        }
        if (image.channels != 4) {
            image.changeNumberOfChannels(4, std::byte{0xff});
        }
        if (image.width <= 0 || image.height <= 0 || image.pixelData.empty()) {
            return 0u;
        }

        GLuint texture_id = 0u;
        glGenTextures(1, &texture_id);
        glBindTexture(GL_TEXTURE_2D, texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const std::vector<std::byte> flipped_pixels = flip_image_vertically_rgba(image);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            image.width,
            image.height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            flipped_pixels.data()
        );
        if (const auto generate_mipmap = reinterpret_cast<void (*)(GLenum)>(
                glXGetProcAddress(reinterpret_cast<const GLubyte*>("glGenerateMipmap")));
            generate_mipmap != nullptr) {
            generate_mipmap(GL_TEXTURE_2D);
        } else {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return texture_id;
    }

    bool draw_tile_patch_with_source_uv(
        const TileKey& geometry_key,
        const TileKey& texture_key,
        const GLuint texture_id,
        const PerspectiveProjection& projection
    ) const {
        (void)projection;
        const double west = tile_x_to_longitude(geometry_key.x, geometry_key.z);
        const double east = tile_x_to_longitude(geometry_key.x + 1, geometry_key.z);
        const double north = tile_y_to_latitude(geometry_key.y, geometry_key.z);
        const double south = tile_y_to_latitude(geometry_key.y + 1, geometry_key.z);

        const double texture_west = tile_x_to_longitude(texture_key.x, texture_key.z);
        const double texture_east = tile_x_to_longitude(texture_key.x + 1, texture_key.z);
        const double texture_lon_span = std::max(1e-12, texture_east - texture_west);
        const double texture_top_y = static_cast<double>(texture_key.y) / static_cast<double>(1 << texture_key.z);
        const double texture_bottom_y = static_cast<double>(texture_key.y + 1) / static_cast<double>(1 << texture_key.z);
        const double texture_y_span = std::max(1e-12, texture_bottom_y - texture_top_y);

        glBindTexture(GL_TEXTURE_2D, texture_id);
        for (int row = 0; row < kPatchSegments; ++row) {
            const double row_t0 = static_cast<double>(row) / static_cast<double>(kPatchSegments);
            const double row_t1 = static_cast<double>(row + 1) / static_cast<double>(kPatchSegments);
            const double lat0 = north + ((south - north) * row_t0);
            const double lat1 = north + ((south - north) * row_t1);
            const double v0 = (texture_bottom_y - mercator_normalized_y(lat0)) / texture_y_span;
            const double v1 = (texture_bottom_y - mercator_normalized_y(lat1)) / texture_y_span;
            glBegin(GL_TRIANGLE_STRIP);
            for (int column = 0; column <= kPatchSegments; ++column) {
                const double column_t = static_cast<double>(column) / static_cast<double>(kPatchSegments);
                const double lon = west + ((east - west) * column_t);
                const double u = (lon - texture_west) / texture_lon_span;
                const auto point0 = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(
                    CesiumGeospatial::Cartographic::fromDegrees(lon, lat0, 0.0)
                );
                const auto point1 = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(
                    CesiumGeospatial::Cartographic::fromDegrees(lon, lat1, 0.0)
                );
                glTexCoord2d(u, v0);
                glVertex3d(point0.x, point0.y, point0.z);
                glTexCoord2d(u, v1);
                glVertex3d(point1.x, point1.y, point1.z);
            }
            glEnd();
        }
        return true;
    }

    bool draw_tile_patch(
        const TileKey& key,
        const GLuint texture_id,
        const PerspectiveProjection& projection
    ) const {
        (void)projection;
        const double west = tile_x_to_longitude(key.x, key.z);
        const double east = tile_x_to_longitude(key.x + 1, key.z);
        const double north = tile_y_to_latitude(key.y, key.z);
        const double south = tile_y_to_latitude(key.y + 1, key.z);
        const double top_y = static_cast<double>(key.y) / static_cast<double>(1 << key.z);
        const double bottom_y = static_cast<double>(key.y + 1) / static_cast<double>(1 << key.z);
        const double y_span = std::max(1e-12, bottom_y - top_y);

        glBindTexture(GL_TEXTURE_2D, texture_id);
        for (int row = 0; row < kPatchSegments; ++row) {
            const double row_t0 = static_cast<double>(row) / static_cast<double>(kPatchSegments);
            const double row_t1 = static_cast<double>(row + 1) / static_cast<double>(kPatchSegments);
            const double lat0 = north + ((south - north) * row_t0);
            const double lat1 = north + ((south - north) * row_t1);
            const double v0 = (bottom_y - mercator_normalized_y(lat0)) / y_span;
            const double v1 = (bottom_y - mercator_normalized_y(lat1)) / y_span;
            glBegin(GL_TRIANGLE_STRIP);
            for (int column = 0; column <= kPatchSegments; ++column) {
                const double column_t = static_cast<double>(column) / static_cast<double>(kPatchSegments);
                const double lon = west + ((east - west) * column_t);
                const double u = column_t;
                const auto point0 = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(
                    CesiumGeospatial::Cartographic::fromDegrees(lon, lat0, 0.0)
                );
                const auto point1 = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(
                    CesiumGeospatial::Cartographic::fromDegrees(lon, lat1, 0.0)
                );
                glTexCoord2d(u, v0);
                glVertex3d(point0.x, point0.y, point0.z);
                glTexCoord2d(u, v1);
                glVertex3d(point1.x, point1.y, point1.z);
            }
            glEnd();
        }
        return true;
    }

    void evict_unused_tiles() {
        if (tiles_.size() <= kMaxResidentTiles) {
            return;
        }
        std::vector<std::string> candidates;
        candidates.reserve(tiles_.size());
        for (const auto& [key, tile] : tiles_) {
            if (tile.state == TileState::Ready && frame_counter_ > tile.last_used_frame + 120u) {
                candidates.push_back(key);
            }
        }
        for (const auto& key : candidates) {
            if (tiles_.size() <= kMaxResidentTiles) {
                break;
            }
            auto tile = tiles_.find(key);
            if (tile == tiles_.end()) {
                continue;
            }
            if (tile->second.texture_id != 0u) {
                glDeleteTextures(1, &tile->second.texture_id);
            }
            tiles_.erase(tile);
        }
    }

    struct LifetimeState {};

    BasemapProviderConfig basemap_config_;
    std::string asset_cache_status_ = "disabled";
    std::shared_ptr<JoiningTaskProcessor> task_processor_;
    CesiumAsync::AsyncSystem async_system_;
    std::shared_ptr<CesiumAsync::IAssetAccessor> asset_accessor_;
    std::shared_ptr<LifetimeState> state_ = std::make_shared<LifetimeState>();
    std::unordered_map<std::string, TileRecord> tiles_;
    int width_ = 1;
    int height_ = 1;
    int last_uniform_zoom_ = 1;
    std::uint64_t frame_counter_ = 0u;
    std::size_t pending_load_count_ = 0u;
    std::size_t last_requested_tile_count_ = 0u;
    std::size_t last_ready_tile_count_ = 0u;
    std::size_t last_rendered_tile_count_ = 0u;
    std::size_t last_started_load_count_ = 0u;
    std::size_t last_pending_tile_count_ = 0u;
    std::size_t last_exact_tile_render_count_ = 0u;
    std::size_t last_parent_fallback_tile_render_count_ = 0u;
    std::size_t last_missing_tile_count_ = 0u;
    float last_load_progress_ = 0.0F;
    std::string last_error_;
};

RenderedGlobeScene::RenderedGlobeScene(CesiumGeospatial::Cartographic initial_camera)
    : RenderedGlobeScene(initial_camera, Options{}) {}

RenderedGlobeScene::RenderedGlobeScene(CesiumGeospatial::Cartographic initial_camera, Options options)
    : home_camera_(initial_camera),
      camera_(initial_camera),
      cartography_dataset_(frameflow::renderer::cartography::CartographyDatasetLoader::load_default()) {
    auto basemap_config = basemap_provider_from_options(options, &imagery_runtime_status_);
    if (!basemap_config.has_value()) {
        return;
    }

    uniform_raster_layer_ = UniformRasterGlobeLayer::create(*basemap_config, options, &imagery_runtime_status_);
    if (!uniform_raster_layer_) {
        google_tileset_runtime_ = GoogleTilesetRuntime::create(
            std::move(*basemap_config),
            options,
            &imagery_runtime_status_
        );
    }
}

RenderedGlobeScene::~RenderedGlobeScene() = default;
RenderedGlobeScene::RenderedGlobeScene(RenderedGlobeScene&&) noexcept = default;
RenderedGlobeScene& RenderedGlobeScene::operator=(RenderedGlobeScene&&) noexcept = default;

void RenderedGlobeScene::log_imagery_runtime_state(const bool rendered_imagery) {
    if (!renderer_debug_logging_enabled()) {
        return;
    }

    bool imagery_available = false;
    std::size_t tiles_ready = 0u;
    std::size_t overlays_ready = 0u;
    std::string last_error;

    if (google_tileset_runtime_) {
        const auto snapshot = google_tileset_runtime_->snapshot();
        imagery_available = true;
        tiles_ready = snapshot.tiles_ready;
        overlays_ready = snapshot.overlays_ready;
        last_error = snapshot.last_error;
    } else if (uniform_raster_layer_) {
        const auto snapshot = uniform_raster_layer_->snapshot();
        imagery_available = true;
        tiles_ready = snapshot.tiles_ready;
        overlays_ready = snapshot.overlays_ready;
        last_error = snapshot.last_error;
    }

    if (!imagery_available) {
        if (!imagery_disabled_logged_) {
            std::cerr << "[Frameflow Cesium] " << imagery_runtime_status_ << '\n';
            imagery_disabled_logged_ = true;
        }
        return;
    }

    if (!last_error.empty() && last_error != last_logged_imagery_error_) {
        std::cerr
            << "[Frameflow Cesium] imagery error: "
            << last_error
            << " | "
            << imagery_runtime_status_
            << '\n';
        last_logged_imagery_error_ = last_error;
    }

    if (!imagery_ready_logged_ && tiles_ready > 0u && overlays_ready > 0u) {
        std::cerr << "[Frameflow Cesium] imagery ready | " << imagery_runtime_status_ << '\n';
        imagery_ready_logged_ = true;
        return;
    }

    if (!rendered_imagery &&
        frames_rendered_ >= 1u &&
        (last_imagery_wait_log_frame_ == 0u || frames_rendered_ - last_imagery_wait_log_frame_ >= 180u)) {
        std::cerr << "[Frameflow Cesium] waiting for imagery | " << imagery_runtime_status_ << '\n';
        last_imagery_wait_log_frame_ = frames_rendered_;
    }
}

void RenderedGlobeScene::resize(const int width, const int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    if (google_tileset_runtime_) {
        google_tileset_runtime_->resize(width_, height_);
    }
    if (uniform_raster_layer_) {
        uniform_raster_layer_->resize(width_, height_);
    }
}

void RenderedGlobeScene::pause() noexcept {
    paused_ = true;
}

void RenderedGlobeScene::resume() noexcept {
    paused_ = false;
}

void RenderedGlobeScene::set_points(const frameflow_point* points, const std::uint64_t point_count) {
    points_.clear();
    if (points == nullptr || point_count == 0u) {
        selected_location_id_.reset();
        return;
    }

    points_.reserve(static_cast<std::size_t>(point_count));
    for (std::uint64_t point_index = 0; point_index < point_count; ++point_index) {
        const auto& point = points[point_index];
        ScenePoint scene_point;
        scene_point.location_id = point.location_id;
        scene_point.kind = point.kind;
        scene_point.latitude = point.latitude;
        scene_point.longitude = point.longitude;
        scene_point.story_count = point.story_count;
        if (point.style_key != nullptr && point.style_key[0] != '\0') {
            scene_point.style_key = std::string(point.style_key);
        }
        if (point.top_categories != nullptr && point.top_category_count > 0u) {
            const char* const primary_category = point.top_categories[0];
            if (primary_category != nullptr && primary_category[0] != '\0') {
                scene_point.primary_category = std::string(primary_category);
            }
        }
        points_.push_back(std::move(scene_point));
    }

    set_selected_location(selected_location_id_);
}

void RenderedGlobeScene::set_points(std::span<const frameflow::GeoPointAggregate> points) {
    points_.clear();
    if (points.empty()) {
        selected_location_id_.reset();
        return;
    }

    points_.reserve(points.size());
    for (const auto& point : points) {
        ScenePoint scene_point;
        scene_point.location_id = point.location_id;
        scene_point.kind = to_bridge_kind(point.kind);
        scene_point.latitude = point.latitude;
        scene_point.longitude = point.longitude;
        scene_point.story_count = point.story_count;
        scene_point.primary_category = point.top_categories.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{point.top_categories.front()};
        scene_point.style_key = point.style_key;
        points_.push_back(std::move(scene_point));
    }

    set_selected_location(selected_location_id_);
}

void RenderedGlobeScene::set_selected_location(std::optional<std::int64_t> location_id) noexcept {
    if (!location_id.has_value()) {
        selected_location_id_.reset();
        return;
    }

    const auto selected_id = *location_id;
    const auto found = std::any_of(points_.begin(), points_.end(), [selected_id](const auto& point) {
        return point.location_id == selected_id;
    });
    if (!found) {
        selected_location_id_.reset();
        return;
    }

    selected_location_id_ = selected_id;
}

std::optional<std::int64_t> RenderedGlobeScene::selected_location_id() const noexcept {
    return selected_location_id_;
}

void RenderedGlobeScene::set_focus_location(std::optional<std::int64_t> location_id) noexcept {
    if (!location_id.has_value()) {
        focus_location_id_.reset();
        return;
    }

    const auto focus_id = *location_id;
    const auto found = std::any_of(points_.begin(), points_.end(), [focus_id](const auto& point) {
        return point.location_id == focus_id;
    });
    if (!found) {
        focus_location_id_.reset();
        return;
    }

    focus_location_id_ = focus_id;
}

RenderedGlobeScene::CameraState RenderedGlobeScene::camera_state() const noexcept {
    CameraState state;
    state.longitude = CesiumUtility::Math::radiansToDegrees(camera_.longitude);
    state.latitude = CesiumUtility::Math::radiansToDegrees(camera_.latitude);
    state.height_meters = camera_.height;
    state.heading_degrees = heading_degrees_;
    state.pitch_degrees = pitch_degrees_;
    state.roll_degrees = roll_degrees_;
    return state;
}

void RenderedGlobeScene::set_camera_state(const CameraState& state) noexcept {
    camera_ = CesiumGeospatial::Cartographic::fromDegrees(
        normalize_longitude_degrees(state.longitude),
        std::clamp(state.latitude, -80.0, 80.0),
        std::clamp(state.height_meters, min_camera_height_meters(), kMaxCameraHeightMeters)
    );
    heading_degrees_ = normalize_signed_degrees(state.heading_degrees);
    pitch_degrees_ = std::clamp(state.pitch_degrees, -85.0, 15.0);
    roll_degrees_ = normalize_signed_degrees(state.roll_degrees);
}

bool RenderedGlobeScene::adjust_camera(
    const double longitude_delta_degrees,
    const double latitude_delta_degrees,
    const double height_delta_meters
) noexcept {
    const CameraState current = camera_state();
    const double next_longitude = normalize_longitude_degrees(current.longitude + longitude_delta_degrees);
    const double next_latitude = std::clamp(current.latitude + latitude_delta_degrees, -80.0, 80.0);
    const double next_height = std::clamp(
        current.height_meters + height_delta_meters,
        min_camera_height_meters(),
        kMaxCameraHeightMeters
    );

    if (std::abs(next_longitude - current.longitude) <= 1e-9 &&
        std::abs(next_latitude - current.latitude) <= 1e-9 &&
        std::abs(next_height - current.height_meters) <= 1e-6) {
        return false;
    }

    camera_ = CesiumGeospatial::Cartographic::fromDegrees(next_longitude, next_latitude, next_height);
    return true;
}

bool RenderedGlobeScene::adjust_orientation(
    const double heading_delta_degrees,
    const double pitch_delta_degrees,
    const double roll_delta_degrees
) noexcept {
    const double next_heading = normalize_signed_degrees(heading_degrees_ + heading_delta_degrees);
    const double next_pitch = std::clamp(pitch_degrees_ + pitch_delta_degrees, -85.0, 15.0);
    const double next_roll = normalize_signed_degrees(roll_degrees_ + roll_delta_degrees);
    if (std::abs(next_heading - heading_degrees_) <= 1e-9 &&
        std::abs(next_pitch - pitch_degrees_) <= 1e-9 &&
        std::abs(next_roll - roll_degrees_) <= 1e-9) {
        return false;
    }

    heading_degrees_ = next_heading;
    pitch_degrees_ = next_pitch;
    roll_degrees_ = next_roll;
    return true;
}

std::vector<RenderedGlobeScene::RenderNode> RenderedGlobeScene::build_render_nodes() const {
    if (width_ <= 0 || height_ <= 0) {
        return {};
    }

    struct ProjectedCandidate {
        const ScenePoint* point = nullptr;
        double normalized_x = 0.0;
        double normalized_y = 0.0;
        double screen_x = 0.0;
        double screen_y = 0.0;
        double radius_normalized = 0.0;
        double radius_pixels = 0.0;
        bool selected = false;
        bool focused = false;
    };

    std::vector<ProjectedCandidate> candidates;
    candidates.reserve(points_.size());
    for (const auto& point : points_) {
        const ProjectedPoint projected = project_point(point.latitude, point.longitude, camera_state(), width_, height_);
        if (!projected_within_viewport(projected)) {
            continue;
        }

        const bool selected = selected_location_id_.has_value() && *selected_location_id_ == point.location_id;
        const bool focused = focus_location_id_.has_value() && *focus_location_id_ == point.location_id;
        const double radius_normalized = marker_radius(point.story_count, point.kind, selected);
        ProjectedCandidate candidate;
        candidate.point = &point;
        candidate.normalized_x = projected.x;
        candidate.normalized_y = projected.y;
        candidate.screen_x = normalized_to_screen_x(projected.x, width_);
        candidate.screen_y = normalized_to_screen_y(projected.y, height_);
        candidate.radius_normalized = radius_normalized;
        candidate.radius_pixels = normalized_radius_to_pixels(radius_normalized, width_, height_);
        candidate.selected = selected;
        candidate.focused = focused;
        candidates.push_back(candidate);
    }

    std::vector<bool> consumed(candidates.size(), false);
    std::vector<RenderNode> nodes;
    nodes.reserve(candidates.size());

    for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
        if (consumed[candidate_index]) {
            continue;
        }

        std::vector<std::size_t> members{candidate_index};
        consumed[candidate_index] = true;

        if (candidates[candidate_index].selected || candidates[candidate_index].focused) {
            const auto& candidate = candidates[candidate_index];
            const auto& point = *candidate.point;
            RenderNode node;
            node.kind = RenderNode::Kind::Location;
            node.target_id = point.location_id;
            node.point_count = 1;
            node.longitude = point.longitude;
            node.latitude = point.latitude;
            node.normalized_x = candidate.normalized_x;
            node.normalized_y = candidate.normalized_y;
            node.screen_x = candidate.screen_x;
            node.screen_y = candidate.screen_y;
            node.radius_normalized = candidate.radius_normalized;
            node.radius_pixels = candidate.radius_pixels;
            node.primary_category = point.primary_category;
            node.style_key = point.style_key;
            node.selected = candidate.selected;
            node.focused = candidate.focused;
            node.dimmed = focus_location_id_.has_value() && !node.focused && !node.selected;
            nodes.push_back(std::move(node));
            continue;
        }

        bool added = true;
        while (added) {
            added = false;

            double centroid_screen_x = 0.0;
            double centroid_screen_y = 0.0;
            for (const auto member_index : members) {
                centroid_screen_x += candidates[member_index].screen_x;
                centroid_screen_y += candidates[member_index].screen_y;
            }
            centroid_screen_x /= static_cast<double>(members.size());
            centroid_screen_y /= static_cast<double>(members.size());

            for (std::size_t peer_index = 0; peer_index < candidates.size(); ++peer_index) {
                if (consumed[peer_index]) {
                    continue;
                }
                if (candidates[peer_index].selected || candidates[peer_index].focused) {
                    continue;
                }

                const double delta_x = candidates[peer_index].screen_x - centroid_screen_x;
                const double delta_y = candidates[peer_index].screen_y - centroid_screen_y;
                const double distance_squared = (delta_x * delta_x) + (delta_y * delta_y);
                if (distance_squared <= (kClusterThresholdPixels * kClusterThresholdPixels)) {
                    consumed[peer_index] = true;
                    members.push_back(peer_index);
                    added = true;
                }
            }
        }

        if (members.size() == 1u) {
            const auto& candidate = candidates[members.front()];
            const auto& point = *candidate.point;
            RenderNode node;
            node.kind = RenderNode::Kind::Location;
            node.target_id = point.location_id;
            node.point_count = 1;
            node.longitude = point.longitude;
            node.latitude = point.latitude;
            node.normalized_x = candidate.normalized_x;
            node.normalized_y = candidate.normalized_y;
            node.screen_x = candidate.screen_x;
            node.screen_y = candidate.screen_y;
            node.radius_normalized = candidate.radius_normalized;
            node.radius_pixels = candidate.radius_pixels;
            node.primary_category = point.primary_category;
            node.style_key = point.style_key;
            node.selected = candidate.selected;
            node.focused = candidate.focused;
            node.dimmed = focus_location_id_.has_value() && !node.focused && !node.selected;
            nodes.push_back(std::move(node));
            continue;
        }

        double centroid_normalized_x = 0.0;
        double centroid_normalized_y = 0.0;
        double centroid_screen_x = 0.0;
        double centroid_screen_y = 0.0;
        double centroid_longitude = 0.0;
        double centroid_latitude = 0.0;
        std::vector<std::int64_t> location_ids;
        location_ids.reserve(members.size());

        for (const auto member_index : members) {
            const auto& candidate = candidates[member_index];
            centroid_normalized_x += candidate.normalized_x;
            centroid_normalized_y += candidate.normalized_y;
            centroid_screen_x += candidate.screen_x;
            centroid_screen_y += candidate.screen_y;
            centroid_longitude += candidate.point->longitude;
            centroid_latitude += candidate.point->latitude;
            location_ids.push_back(candidate.point->location_id);
        }

        const double member_count = static_cast<double>(members.size());
        const std::int32_t cluster_point_count = static_cast<std::int32_t>(members.size());
        const double radius_pixels = cluster_radius_pixels(cluster_point_count);
        RenderNode node;
        node.kind = RenderNode::Kind::Cluster;
        node.target_id = stable_cluster_id(std::move(location_ids));
        node.point_count = cluster_point_count;
        node.longitude = centroid_longitude / member_count;
        node.latitude = centroid_latitude / member_count;
        node.normalized_x = centroid_normalized_x / member_count;
        node.normalized_y = centroid_normalized_y / member_count;
        node.screen_x = centroid_screen_x / member_count;
        node.screen_y = centroid_screen_y / member_count;
        node.radius_normalized = pixels_to_normalized_radius(radius_pixels, width_, height_);
        node.radius_pixels = radius_pixels;
        node.dimmed = focus_location_id_.has_value();
        nodes.push_back(std::move(node));
    }

    return nodes;
}

RenderedGlobeScene::CartographyRenderStats RenderedGlobeScene::cartography_render_stats() const {
    const auto projected_boundaries = build_projected_country_boundaries(
        cartography_dataset_,
        camera_state(),
        width_,
        height_
    );
    return CartographyRenderStats{
        .visible_country_boundaries = projected_boundaries.visible_boundary_count,
        .visible_country_boundary_segments = projected_boundaries.segments.size(),
    };
}

std::optional<RenderedGlobeScene::InteractionHit> RenderedGlobeScene::hit_test(
    const double screen_x,
    const double screen_y
) const {
    const auto nodes = build_render_nodes();
    const RenderNode* best_node = nullptr;
    double best_distance_squared = 0.0;

    for (const auto& node : nodes) {
        const double delta_x = screen_x - node.screen_x;
        const double delta_y = screen_y - node.screen_y;
        const double distance_squared = (delta_x * delta_x) + (delta_y * delta_y);
        const double visual_radius_pixels = node.kind == RenderNode::Kind::Location
            ? node.radius_pixels * 2.0
            : node.radius_pixels;
        const double radius_pixels = visual_radius_pixels + (node.selected ? 6.0 : 0.0);
        if (distance_squared > (radius_pixels * radius_pixels)) {
            continue;
        }

        if (best_node == nullptr || distance_squared < best_distance_squared) {
            best_node = &node;
            best_distance_squared = distance_squared;
        }
    }

    if (best_node == nullptr) {
        return std::nullopt;
    }

    InteractionHit hit;
    hit.kind = best_node->kind == RenderNode::Kind::Cluster
        ? InteractionHit::Kind::Cluster
        : InteractionHit::Kind::Location;
    hit.target_id = best_node->target_id;
    hit.point_count = best_node->point_count;
    hit.longitude = best_node->longitude;
    hit.latitude = best_node->latitude;
    hit.primary_category = best_node->primary_category;
    return hit;
}

RenderedGlobeScene::HudLayout RenderedGlobeScene::build_hud_layout() const noexcept {
    const double button_diameter = std::clamp(
        static_cast<double>(std::min(width_, height_)) * 0.055,
        kHudButtonMinDiameterPixels,
        kHudButtonMaxDiameterPixels
    );
    const double button_radius = button_diameter * 0.5;
    const double compass_diameter = std::clamp(
        static_cast<double>(std::min(width_, height_)) * 0.07,
        kHudCompassMinDiameterPixels,
        kHudCompassMaxDiameterPixels
    );
    const double compass_radius = compass_diameter * 0.5;
    const double panel_width = std::max(
        kHudPanelMinWidthPixels,
        std::max(button_diameter, compass_diameter) + (kHudPanelPaddingPixels * 2.0)
    );
    const double button_column_x = static_cast<double>(width_) - kHudMarginPixels - (panel_width * 0.5);
    const double first_button_center_y = kHudMarginPixels + kHudPanelPaddingPixels + button_radius;
    const double second_button_center_y = first_button_center_y + button_diameter + kHudButtonGapPixels;
    const double third_button_center_y = second_button_center_y + button_diameter + kHudButtonGapPixels;
    const double status_y = third_button_center_y + button_radius + kHudStatusGapPixels;
    const double status_x = static_cast<double>(width_) - kHudMarginPixels - panel_width + kHudPanelPaddingPixels;
    const double status_width = panel_width - (kHudPanelPaddingPixels * 2.0);
    const double compass_center_x = button_column_x;
    const double compass_center_y = status_y + kHudStatusHeightPixels + kHudCompassGapPixels + compass_radius;
    const double panel_height =
        (compass_center_y - kHudMarginPixels) + compass_radius + kHudPanelPaddingPixels;
    const double panel_x = static_cast<double>(width_) - kHudMarginPixels - panel_width;
    const double panel_y = kHudMarginPixels;

    HudLayout layout;
    layout.panel_x = panel_x;
    layout.panel_y = panel_y;
    layout.panel_width = panel_width;
    layout.panel_height = panel_height;
    layout.buttons = {{
        HudButtonLayout{HudAction::ZoomIn, button_column_x, first_button_center_y, button_radius},
        HudButtonLayout{HudAction::ZoomOut, button_column_x, second_button_center_y, button_radius},
        HudButtonLayout{HudAction::ResetCamera, button_column_x, third_button_center_y, button_radius},
    }};
    layout.status = HudStatusLayout{status_x, status_y, status_width, kHudStatusHeightPixels};
    layout.compass = HudCompassLayout{compass_center_x, compass_center_y, compass_radius};
    return layout;
}

std::optional<RenderedGlobeScene::HudAction> RenderedGlobeScene::hud_hit_test(
    const double screen_x,
    const double screen_y
) const noexcept {
    const auto layout = build_hud_layout();
    for (const auto& button : layout.buttons) {
        const double delta_x = screen_x - button.center_x;
        const double delta_y = screen_y - button.center_y;
        if ((delta_x * delta_x) + (delta_y * delta_y) <= (button.radius * button.radius)) {
            return button.action;
        }
    }
    const double compass_delta_x = screen_x - layout.compass.center_x;
    const double compass_delta_y = screen_y - layout.compass.center_y;
    if ((compass_delta_x * compass_delta_x) + (compass_delta_y * compass_delta_y) <=
        (layout.compass.radius * layout.compass.radius)) {
        return HudAction::ResetOrientation;
    }
    return std::nullopt;
}

std::optional<RenderedGlobeScene::ScreenPosition> RenderedGlobeScene::hud_action_screen_position(
    const HudAction action
) const noexcept {
    const auto layout = build_hud_layout();
    const auto button = std::find_if(layout.buttons.begin(), layout.buttons.end(), [action](const auto& candidate) {
        return candidate.action == action;
    });
    if (button == layout.buttons.end()) {
        if (action != HudAction::ResetOrientation) {
            return std::nullopt;
        }
        ScreenPosition position;
        position.x = layout.compass.center_x;
        position.y = layout.compass.center_y;
        return position;
    }

    ScreenPosition position;
    position.x = button->center_x;
    position.y = button->center_y;
    return position;
}

bool RenderedGlobeScene::activate_hud_action(const HudAction action) noexcept {
    switch (action) {
        case HudAction::ZoomIn: {
            const double next_height = std::clamp(
                camera_.height * kCameraZoomInMultiplier,
                min_camera_height_meters(),
                kMaxCameraHeightMeters
            );
            if (std::abs(next_height - camera_.height) <= 1.0) {
                return false;
            }
            camera_.height = next_height;
            return true;
        }
        case HudAction::ZoomOut: {
            const double next_height = std::clamp(
                camera_.height * kCameraZoomOutMultiplier,
                min_camera_height_meters(),
                kMaxCameraHeightMeters
            );
            if (std::abs(next_height - camera_.height) <= 1.0) {
                return false;
            }
            camera_.height = next_height;
            return true;
        }
        case HudAction::ResetCamera: {
            const bool changed =
                std::abs(camera_.longitude - home_camera_.longitude) > 1e-9 ||
                std::abs(camera_.latitude - home_camera_.latitude) > 1e-9 ||
                std::abs(camera_.height - home_camera_.height) > 1e-6 ||
                std::abs(heading_degrees_ - home_heading_degrees_) > 1e-9 ||
                std::abs(pitch_degrees_ - home_pitch_degrees_) > 1e-9 ||
                std::abs(roll_degrees_ - home_roll_degrees_) > 1e-9;
            camera_ = home_camera_;
            heading_degrees_ = home_heading_degrees_;
            pitch_degrees_ = home_pitch_degrees_;
            roll_degrees_ = home_roll_degrees_;
            return changed;
        }
        case HudAction::ResetOrientation: {
            const bool changed =
                std::abs(heading_degrees_ - home_heading_degrees_) > 1e-9 ||
                std::abs(pitch_degrees_ - home_pitch_degrees_) > 1e-9 ||
                std::abs(roll_degrees_ - home_roll_degrees_) > 1e-9;
            heading_degrees_ = home_heading_degrees_;
            pitch_degrees_ = home_pitch_degrees_;
            roll_degrees_ = home_roll_degrees_;
            return changed;
        }
        default:
            return false;
    }
}

std::optional<RenderedGlobeScene::ScreenPosition> RenderedGlobeScene::screen_position_for_location(
    const std::int64_t location_id
) const {
    if (width_ <= 0 || height_ <= 0) {
        return std::nullopt;
    }

    const auto point = std::find_if(points_.begin(), points_.end(), [location_id](const auto& candidate) {
        return candidate.location_id == location_id;
    });
    if (point == points_.end()) {
        return std::nullopt;
    }

    const ProjectedPoint projected = project_point(
        point->latitude,
        point->longitude,
        camera_state(),
        width_,
        height_
    );
    if (!projected_within_viewport(projected)) {
        return std::nullopt;
    }

    ScreenPosition position;
    position.x = normalized_to_screen_x(projected.x, width_);
    position.y = normalized_to_screen_y(projected.y, height_);
    return position;
}

std::optional<RenderedGlobeScene::ScreenPosition> RenderedGlobeScene::first_location_screen_position() const {
    const auto nodes = build_render_nodes();
    const auto location = std::find_if(nodes.begin(), nodes.end(), [](const auto& node) {
        return node.kind == RenderNode::Kind::Location;
    });
    if (location == nodes.end()) {
        return std::nullopt;
    }

    ScreenPosition position;
    position.x = location->screen_x;
    position.y = location->screen_y;
    return position;
}

std::optional<RenderedGlobeScene::ScreenPosition> RenderedGlobeScene::first_cluster_screen_position() const {
    const auto nodes = build_render_nodes();
    const auto cluster = std::find_if(nodes.begin(), nodes.end(), [](const auto& node) {
        return node.kind == RenderNode::Kind::Cluster;
    });
    if (cluster == nodes.end()) {
        return std::nullopt;
    }

    ScreenPosition position;
    position.x = cluster->screen_x;
    position.y = cluster->screen_y;
    return position;
}

void RenderedGlobeScene::render_frame() {
    if (paused_) {
        return;
    }

    const auto scene_total_start = SceneClock::now();

    const auto clear_start = SceneClock::now();
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.03f, 0.04f, 0.08f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    last_scene_clear_millis_ = scene_elapsed_millis(clear_start);

    bool rendered_imagery = false;
    const auto tiles_start = SceneClock::now();
    if (uniform_raster_layer_) {
        rendered_imagery = uniform_raster_layer_->render(
            camera_state(),
            width_,
            height_,
            &imagery_runtime_status_
        );
    } else if (google_tileset_runtime_) {
        rendered_imagery = google_tileset_runtime_->render(
            camera_state(),
            width_,
            height_,
            &imagery_runtime_status_
        );
    }
    const bool rendered_surface = rendered_imagery;
    log_imagery_runtime_state(rendered_imagery);
    last_scene_tiles_millis_ = scene_elapsed_millis(tiles_start);

    const auto boundaries_start = SceneClock::now();
    if (rendered_surface && env_flag_enabled(kCountryBoundaryOverlayEnv)) {
        const auto projected_boundaries = build_projected_country_boundaries(
            cartography_dataset_,
            camera_state(),
            width_,
            height_
        );
        visible_country_boundary_count_ = projected_boundaries.visible_boundary_count;
        rendered_country_boundary_segment_count_ = projected_boundaries.segments.size();
        draw_country_boundaries(projected_boundaries, country_border_style(camera_.height));
    } else {
        visible_country_boundary_count_ = 0;
        rendered_country_boundary_segment_count_ = 0;
    }
    last_scene_boundaries_millis_ = scene_elapsed_millis(boundaries_start);

    const auto markers_start = SceneClock::now();
    visible_point_count_ = 0;
    visible_cluster_count_ = 0;
    rendered_marker_count_ = 0;
    if (rendered_surface) {
        const auto nodes = build_render_nodes();
        rendered_marker_count_ = nodes.size();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (const auto& node : nodes) {
            visible_point_count_ += static_cast<std::size_t>(node.point_count);
            if (node.kind == RenderNode::Kind::Cluster) {
                visible_cluster_count_ += 1;
                draw_cluster_marker(node.normalized_x, node.normalized_y, node.radius_normalized);
                continue;
            }

            draw_location_pin_marker(
                node.normalized_x,
                node.normalized_y,
                node.radius_normalized,
                marker_color(node.style_key, node.primary_category, node.selected),
                node.selected,
                node.focused,
                node.dimmed
            );
        }
        glDisable(GL_BLEND);
    }
    last_scene_markers_millis_ = scene_elapsed_millis(markers_start);
    last_scene_hud_millis_ = 0;

    frames_rendered_ += 1;
    last_scene_total_millis_ = scene_elapsed_millis(scene_total_start);
}

std::string RenderedGlobeScene::diagnostics_summary() const {
    std::size_t styled_points = 0;
    std::size_t categorized_points = 0;
    for (const auto& point : points_) {
        if (point.style_key.has_value()) {
            styled_points += 1;
        }
        if (point.primary_category.has_value()) {
            categorized_points += 1;
        }
    }

    std::ostringstream out;
    out << "render_backend=glx-geo-points"
        << " width=" << width_
        << " height=" << height_
        << " hud=corner-overlay"
        << " paused=" << (paused_ ? "true" : "false")
        << " frames_rendered=" << frames_rendered_
        << " points=" << points_.size()
        << " visible_points=" << visible_point_count_
        << " render_nodes=" << rendered_marker_count_
        << " clusters=" << visible_cluster_count_
        << " visible_country_boundaries=" << visible_country_boundary_count_
        << " country_boundary_segments=" << rendered_country_boundary_segment_count_
        << " scene_total_ms=" << last_scene_total_millis_
        << " scene_clear_ms=" << last_scene_clear_millis_
        << " scene_tiles_ms=" << last_scene_tiles_millis_
        << " scene_boundaries_ms=" << last_scene_boundaries_millis_
        << " scene_markers_ms=" << last_scene_markers_millis_
        << " scene_hud_ms=" << last_scene_hud_millis_
        << " styled_points=" << styled_points
        << " categorized_points=" << categorized_points;
    if (selected_location_id_.has_value()) {
        out << " selected_location_id=" << *selected_location_id_;
    } else {
        out << " selected_location_id=none";
    }
    if (focus_location_id_.has_value()) {
        out << " focus_location_id=" << *focus_location_id_;
    } else {
        out << " focus_location_id=none";
    }
    out
        << " camera_longitude_degrees=" << CesiumUtility::Math::radiansToDegrees(camera_.longitude)
        << " camera_latitude_degrees=" << CesiumUtility::Math::radiansToDegrees(camera_.latitude)
        << " camera_height_meters=" << camera_.height
        << " camera_heading_degrees=" << heading_degrees_
        << " camera_pitch_degrees=" << pitch_degrees_
        << " camera_roll_degrees=" << roll_degrees_
        << " globe_radius_normalized=" << globe_radius_for_camera_height(camera_.height)
        << " cartography=[" << cartography_dataset_.diagnostics_summary() << "]"
        << " imagery=[" << imagery_runtime_status_ << "]";
    return out.str();
}

} // namespace frameflow::renderer::cesium
