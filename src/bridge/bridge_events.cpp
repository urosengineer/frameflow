#include "bridge_events.hpp"

#include "frameflow/version.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

void queue_pending_event(frameflow_engine* engine, PendingEvent event) {
    if (engine == nullptr) {
        return;
    }
    engine->pending_events.push_back(std::move(event));
    engine->pending_events_high_watermark = std::max(
        engine->pending_events_high_watermark,
        static_cast<std::uint64_t>(engine->pending_events.size())
    );
}

} // namespace

void queue_ready_event(frameflow_engine* engine) {
    if (engine == nullptr || engine->callbacks.on_ready == nullptr) {
        return;
    }

    PendingEvent event;
    event.type = PendingEvent::Type::Ready;
    event.width = engine->width;
    event.height = engine->height;
    event.scale_factor = engine->scale_factor;
    queue_pending_event(engine, std::move(event));
}

void queue_location_selected_event(
    frameflow_engine* engine,
    const std::int64_t location_id,
    std::optional<std::string> category_code,
    const double screen_x,
    const double screen_y,
    std::string interaction
) {
    if (engine == nullptr || engine->callbacks.on_location_selected == nullptr) {
        return;
    }

    PendingEvent event;
    event.type = PendingEvent::Type::LocationSelected;
    event.location_id = location_id;
    event.category_code = std::move(category_code);
    event.screen_x = screen_x;
    event.screen_y = screen_y;
    event.interaction = std::move(interaction);
    queue_pending_event(engine, std::move(event));
}

void queue_cluster_selected_event(
    frameflow_engine* engine,
    const std::int64_t cluster_id,
    const std::int32_t point_count,
    const double longitude,
    const double latitude
) {
    if (engine == nullptr || engine->callbacks.on_cluster_selected == nullptr) {
        return;
    }

    PendingEvent event;
    event.type = PendingEvent::Type::ClusterSelected;
    event.cluster_id = cluster_id;
    event.point_count = point_count;
    event.longitude = longitude;
    event.latitude = latitude;
    queue_pending_event(engine, std::move(event));
}

void queue_camera_changed_event(
    frameflow_engine* engine,
    const frameflow_engine::RuntimeCameraState& camera
) {
    if (engine == nullptr || engine->callbacks.on_camera_changed == nullptr) {
        return;
    }

    for (auto pending = engine->pending_events.rbegin(); pending != engine->pending_events.rend(); ++pending) {
        if (pending->type != PendingEvent::Type::CameraChanged) {
            continue;
        }
        pending->longitude = camera.longitude;
        pending->latitude = camera.latitude;
        pending->height_meters = camera.height_meters;
        pending->heading_degrees = camera.heading_degrees;
        pending->pitch_degrees = camera.pitch_degrees;
        pending->roll_degrees = camera.roll_degrees;
        engine->coalesced_camera_event_count += 1u;
        return;
    }

    PendingEvent event;
    event.type = PendingEvent::Type::CameraChanged;
    event.longitude = camera.longitude;
    event.latitude = camera.latitude;
    event.height_meters = camera.height_meters;
    event.heading_degrees = camera.heading_degrees;
    event.pitch_degrees = camera.pitch_degrees;
    event.roll_degrees = camera.roll_degrees;
    queue_pending_event(engine, std::move(event));
}

void queue_error_event(
    frameflow_engine* engine,
    const frameflow_result code,
    const std::string& message,
    const bool recoverable
) {
    if (engine == nullptr || engine->callbacks.on_engine_error == nullptr) {
        return;
    }

    PendingEvent event;
    event.type = PendingEvent::Type::Error;
    event.error_code = code;
    event.error_message = message;
    event.error_recoverable = recoverable;
    queue_pending_event(engine, std::move(event));
}

void clear_callbacks_and_pending_events(frameflow_engine* engine) {
    if (engine == nullptr) {
        return;
    }
    engine->callbacks.clear();
    engine->pending_events.clear();
}

std::uint64_t dispatch_pending_callbacks(
    const RegisteredCallbacks& callbacks,
    const std::vector<PendingEvent>& pending_events
) {
    if (!callbacks.has_any() || pending_events.empty()) {
        return 0u;
    }

    std::uint64_t dispatched = 0u;
    for (const auto& event : pending_events) {
        switch (event.type) {
            case PendingEvent::Type::Ready:
                if (callbacks.on_ready != nullptr) {
                    frameflow_ready_event ready_event{};
                    ready_event.width = event.width;
                    ready_event.height = event.height;
                    ready_event.scale_factor = event.scale_factor;
                    ready_event.bridge_abi_version = frameflow::bridge_abi_version;
                    ready_event.engine_version_major = frameflow::version_major;
                    ready_event.engine_version_minor = frameflow::version_minor;
                    ready_event.engine_version_patch = frameflow::version_patch;
                    ready_event.command_version_major = frameflow::command_version_major;
                    ready_event.command_version_minor = frameflow::command_version_minor;
                    callbacks.on_ready(callbacks.user, &ready_event);
                    dispatched += 1u;
                }
                break;
            case PendingEvent::Type::LocationSelected:
                if (callbacks.on_location_selected != nullptr) {
                    frameflow_location_selection_event location_event{};
                    location_event.location_id = event.location_id;
                    location_event.category_code = event.category_code.has_value()
                        ? event.category_code->c_str()
                        : nullptr;
                    location_event.screen_x = event.screen_x;
                    location_event.screen_y = event.screen_y;
                    location_event.interaction = event.interaction.c_str();
                    callbacks.on_location_selected(callbacks.user, &location_event);
                    dispatched += 1u;
                }
                break;
            case PendingEvent::Type::ClusterSelected:
                if (callbacks.on_cluster_selected != nullptr) {
                    frameflow_cluster_selection_event cluster_event{};
                    cluster_event.cluster_id = event.cluster_id;
                    cluster_event.point_count = event.point_count;
                    cluster_event.longitude = event.longitude;
                    cluster_event.latitude = event.latitude;
                    callbacks.on_cluster_selected(callbacks.user, &cluster_event);
                    dispatched += 1u;
                }
                break;
            case PendingEvent::Type::CameraChanged:
                if (callbacks.on_camera_changed != nullptr) {
                    frameflow_camera_state camera_event{};
                    camera_event.longitude = event.longitude;
                    camera_event.latitude = event.latitude;
                    camera_event.height_meters = event.height_meters;
                    camera_event.heading_degrees = event.heading_degrees;
                    camera_event.pitch_degrees = event.pitch_degrees;
                    camera_event.roll_degrees = event.roll_degrees;
                    callbacks.on_camera_changed(callbacks.user, &camera_event);
                    dispatched += 1u;
                }
                break;
            case PendingEvent::Type::Error:
                if (callbacks.on_engine_error != nullptr) {
                    frameflow_error_event error_event{};
                    error_event.code = event.error_code;
                    error_event.message = event.error_message.c_str();
                    error_event.recoverable = event.error_recoverable ? 1u : 0u;
                    error_event.bridge_abi_version = frameflow::bridge_abi_version;
                    error_event.engine_version_major = frameflow::version_major;
                    error_event.engine_version_minor = frameflow::version_minor;
                    error_event.engine_version_patch = frameflow::version_patch;
                    error_event.command_version_major = frameflow::command_version_major;
                    error_event.command_version_minor = frameflow::command_version_minor;
                    callbacks.on_engine_error(callbacks.user, &error_event);
                    dispatched += 1u;
                }
                break;
        }
    }

    return dispatched;
}
