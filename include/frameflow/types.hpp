#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace frameflow {

enum class LocationKind {
    Unknown,
    Point,
    City,
    Region,
    Country
};

struct GeoPointAggregate {
    std::int64_t location_id{};
    std::string label;
    LocationKind kind{LocationKind::Unknown};
    std::optional<std::string> country_code;
    double latitude{};
    double longitude{};
    std::int32_t story_count{};
    std::optional<std::int64_t> latest_story_epoch_millis;
    std::vector<std::string> top_categories;
    std::optional<std::string> style_key;
};

struct GlobeFilter {
    std::optional<std::string> query;
    std::optional<std::string> category_code;
    std::optional<std::int64_t> location_id;
    std::optional<std::string> country_code;
    std::optional<std::int64_t> from_epoch_millis;
    std::optional<std::int64_t> to_epoch_millis;
};

} // namespace frameflow

