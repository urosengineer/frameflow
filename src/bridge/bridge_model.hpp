#pragma once

#include "bridge_engine.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

std::vector<frameflow::GeoPointAggregate> copy_points(
    const frameflow_point* points,
    std::uint64_t point_count
);

std::optional<std::string> validate_points_input(
    const frameflow_point* points,
    std::uint64_t point_count
);

std::optional<std::string> copy_nullable_string(const char* value);

frameflow::GlobeFilter copy_filter(const frameflow_filter& filter);

bool has_valid_geo_coordinates(double longitude, double latitude);

bool runtime_camera_changed(
    const std::optional<frameflow_engine::RuntimeCameraState>& current,
    const frameflow_engine::RuntimeCameraState& next
);
