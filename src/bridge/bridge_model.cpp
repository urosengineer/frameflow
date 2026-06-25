#include "bridge_model.hpp"

#include <cmath>
#include <sstream>
#include <utility>

namespace {

constexpr std::uint64_t kMaxPointSnapshotCount = 100'000u;
constexpr std::uint64_t kMaxTopCategoriesPerPoint = 8u;

frameflow::LocationKind to_cpp_kind(const frameflow_location_kind kind) {
    using frameflow::LocationKind;
    switch (kind) {
        case FRAMEFLOW_LOCATION_KIND_POINT:
            return LocationKind::Point;
        case FRAMEFLOW_LOCATION_KIND_CITY:
            return LocationKind::City;
        case FRAMEFLOW_LOCATION_KIND_REGION:
            return LocationKind::Region;
        case FRAMEFLOW_LOCATION_KIND_COUNTRY:
            return LocationKind::Country;
        case FRAMEFLOW_LOCATION_KIND_UNKNOWN:
        default:
            return LocationKind::Unknown;
    }
}

bool nearly_equal(const double left, const double right) {
    return std::abs(left - right) <= 1e-9;
}

} // namespace

std::vector<frameflow::GeoPointAggregate> copy_points(
    const frameflow_point* points,
    const std::uint64_t point_count
) {
    std::vector<frameflow::GeoPointAggregate> copied;
    copied.reserve(static_cast<std::size_t>(point_count));

    for (std::uint64_t index = 0; index < point_count; ++index) {
        const auto& point = points[index];
        frameflow::GeoPointAggregate aggregate;
        aggregate.location_id = point.location_id;
        aggregate.label = point.label != nullptr ? point.label : "";
        aggregate.kind = to_cpp_kind(point.kind);
        if (point.country_code != nullptr && point.country_code[0] != '\0') {
            aggregate.country_code = std::string(point.country_code);
        }
        aggregate.latitude = point.latitude;
        aggregate.longitude = point.longitude;
        aggregate.story_count = point.story_count;
        if (point.latest_story_epoch_millis > 0) {
            aggregate.latest_story_epoch_millis = point.latest_story_epoch_millis;
        }
        if (point.style_key != nullptr && point.style_key[0] != '\0') {
            aggregate.style_key = std::string(point.style_key);
        }
        for (std::uint64_t category_index = 0; category_index < point.top_category_count; ++category_index) {
            const char* const category = point.top_categories[category_index];
            if (category != nullptr && category[0] != '\0') {
                aggregate.top_categories.emplace_back(category);
            }
        }
        copied.push_back(std::move(aggregate));
    }

    return copied;
}

std::optional<std::string> validate_points_input(
    const frameflow_point* points,
    const std::uint64_t point_count
) {
    if (point_count > kMaxPointSnapshotCount) {
        std::ostringstream out;
        out << "point_count exceeds maximum supported point snapshot size of " << kMaxPointSnapshotCount;
        return out.str();
    }
    for (std::uint64_t index = 0; index < point_count; ++index) {
        const auto& point = points[index];
        if (point.location_id <= 0) {
            std::ostringstream out;
            out << "point[" << index << "] requires positive location_id";
            return out.str();
        }
        if (!has_valid_geo_coordinates(point.longitude, point.latitude)) {
            std::ostringstream out;
            out << "point[" << index << "] requires finite longitude/latitude and latitude in [-90, 90]";
            return out.str();
        }
        if (point.story_count < 0) {
            std::ostringstream out;
            out << "point[" << index << "] requires non-negative story_count";
            return out.str();
        }
        if (point.latest_story_epoch_millis < 0) {
            std::ostringstream out;
            out << "point[" << index << "] requires non-negative latest_story_epoch_millis";
            return out.str();
        }
        if (point.top_category_count > kMaxTopCategoriesPerPoint) {
            std::ostringstream out;
            out << "point[" << index << "] exceeds maximum top_category_count of " << kMaxTopCategoriesPerPoint;
            return out.str();
        }
        if (point.top_category_count > 0u && point.top_categories == nullptr) {
            std::ostringstream out;
            out << "point[" << index << "] has top_category_count but no top_categories array";
            return out.str();
        }
        for (std::uint64_t category_index = 0; category_index < point.top_category_count; ++category_index) {
            const char* const category = point.top_categories[category_index];
            if (category == nullptr || category[0] == '\0') {
                std::ostringstream out;
                out << "point[" << index << "].top_categories[" << category_index << "] must not be null or empty";
                return out.str();
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> copy_nullable_string(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

frameflow::GlobeFilter copy_filter(const frameflow_filter& filter) {
    frameflow::GlobeFilter copied;
    copied.query = copy_nullable_string(filter.query);
    copied.category_code = copy_nullable_string(filter.category_code);
    copied.country_code = copy_nullable_string(filter.country_code);
    if (filter.has_location_id != 0u) {
        copied.location_id = filter.location_id;
    }
    if (filter.has_from_epoch_millis != 0u) {
        copied.from_epoch_millis = filter.from_epoch_millis;
    }
    if (filter.has_to_epoch_millis != 0u) {
        copied.to_epoch_millis = filter.to_epoch_millis;
    }
    return copied;
}

bool has_valid_geo_coordinates(const double longitude, const double latitude) {
    return std::isfinite(longitude) &&
        std::isfinite(latitude) &&
        latitude >= -90.0 &&
        latitude <= 90.0;
}

bool runtime_camera_changed(
    const std::optional<frameflow_engine::RuntimeCameraState>& current,
    const frameflow_engine::RuntimeCameraState& next
) {
    if (!current.has_value()) {
        return true;
    }

    return !nearly_equal(current->longitude, next.longitude) ||
        !nearly_equal(current->latitude, next.latitude) ||
        !nearly_equal(current->height_meters, next.height_meters) ||
        !nearly_equal(current->heading_degrees, next.heading_degrees) ||
        !nearly_equal(current->pitch_degrees, next.pitch_degrees) ||
        !nearly_equal(current->roll_degrees, next.roll_degrees);
}
