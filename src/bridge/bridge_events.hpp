#pragma once

#include "bridge_engine.hpp"

#include <cstdint>
#include <optional>
#include <string>

void queue_ready_event(frameflow_engine* engine);

void queue_location_selected_event(
    frameflow_engine* engine,
    std::int64_t location_id,
    std::optional<std::string> category_code,
    double screen_x,
    double screen_y,
    std::string interaction
);

void queue_cluster_selected_event(
    frameflow_engine* engine,
    std::int64_t cluster_id,
    std::int32_t point_count,
    double longitude,
    double latitude
);

void queue_camera_changed_event(
    frameflow_engine* engine,
    const frameflow_engine::RuntimeCameraState& camera
);

void queue_error_event(
    frameflow_engine* engine,
    frameflow_result code,
    const std::string& message,
    bool recoverable
);

void clear_callbacks_and_pending_events(frameflow_engine* engine);

std::uint64_t dispatch_pending_callbacks(
    const RegisteredCallbacks& callbacks,
    const std::vector<PendingEvent>& pending_events
);
