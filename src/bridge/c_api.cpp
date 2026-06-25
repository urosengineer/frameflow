#include "frameflow/c/bridge.h"

#include "bridge_engine.hpp"
#include "bridge_events.hpp"
#include "bridge_model.hpp"
#include "software_globe_renderer.hpp"
#include "core/native_surface_runtime.hpp"
#include "frameflow/engine.hpp"
#include "frameflow/version.hpp"
#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
#include "renderer/cesium/glx_offscreen_scene_host.hpp"
#include "renderer/cesium/glx_native_scene_surface_host.hpp"
#endif

#include <algorithm>
#include <array>
#include <memory>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <exception>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

void clear_error(frameflow_engine* engine) {
    if (engine == nullptr) {
        return;
    }
    engine->last_error_code = FRAMEFLOW_RESULT_OK;
    engine->last_error_message.clear();
    engine->last_error_recoverable = false;
}

void fill_runtime_info(frameflow_runtime_info* out_info) {
    if (out_info == nullptr) {
        return;
    }
    out_info->bridge_abi_version = frameflow::bridge_abi_version;
    out_info->engine_version_major = frameflow::version_major;
    out_info->engine_version_minor = frameflow::version_minor;
    out_info->engine_version_patch = frameflow::version_patch;
    out_info->command_version_major = frameflow::command_version_major;
    out_info->command_version_minor = frameflow::command_version_minor;
}

bool is_active_state(const frameflow_lifecycle_state state) {
    return state == FRAMEFLOW_LIFECYCLE_READY || state == FRAMEFLOW_LIFECYCLE_PAUSED;
}

const char* lifecycle_state_name(const frameflow_lifecycle_state state) {
    switch (state) {
        case FRAMEFLOW_LIFECYCLE_UNINITIALIZED:
            return "UNINITIALIZED";
        case FRAMEFLOW_LIFECYCLE_INITIALIZING:
            return "INITIALIZING";
        case FRAMEFLOW_LIFECYCLE_READY:
            return "READY";
        case FRAMEFLOW_LIFECYCLE_PAUSED:
            return "PAUSED";
        case FRAMEFLOW_LIFECYCLE_FAILED:
            return "FAILED";
        case FRAMEFLOW_LIFECYCLE_DISPOSED:
            return "DISPOSED";
        default:
            return "UNKNOWN";
    }
}

const char* presentation_kind_name(const frameflow_presentation_kind kind) {
    switch (kind) {
        case FRAMEFLOW_PRESENTATION_KIND_NATIVE_SURFACE:
            return "NATIVE_SURFACE";
        case FRAMEFLOW_PRESENTATION_KIND_OFFSCREEN_BITMAP:
            return "OFFSCREEN_BITMAP";
        case FRAMEFLOW_PRESENTATION_KIND_NONE:
        default:
            return "NONE";
    }
}

const char* surface_backend_name(const frameflow_surface_backend backend) {
    switch (backend) {
        case FRAMEFLOW_SURFACE_BACKEND_AUTO:
            return "AUTO";
        case FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP:
            return "OFFSCREEN_BITMAP";
        case FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD:
            return "LINUX_X11_CHILD";
        case FRAMEFLOW_SURFACE_BACKEND_WINDOWS_HWND_CHILD:
            return "WINDOWS_HWND_CHILD";
        default:
            return "UNKNOWN";
    }
}

const char* presence_name(const bool present) {
    return present ? "present" : "none";
}

frameflow::FrameflowSurfaceBackend to_core_surface_backend(
    const frameflow_surface_backend backend
) {
    using frameflow::FrameflowSurfaceBackend;
    switch (backend) {
        case FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP:
            return FrameflowSurfaceBackend::OffscreenBitmap;
        case FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD:
            return FrameflowSurfaceBackend::LinuxX11Child;
        case FRAMEFLOW_SURFACE_BACKEND_WINDOWS_HWND_CHILD:
            return FrameflowSurfaceBackend::WindowsHwndChild;
        case FRAMEFLOW_SURFACE_BACKEND_AUTO:
        default:
            return FrameflowSurfaceBackend::Auto;
    }
}

frameflow::FrameflowSurfaceBounds to_core_surface_bounds(
    const frameflow_surface_bounds& bounds
) {
    return frameflow::FrameflowSurfaceBounds{
        .x = bounds.x,
        .y = bounds.y,
        .width = bounds.width,
        .height = bounds.height,
        .scale_factor = bounds.scale_factor,
    };
}

frameflow::FrameflowNativeSurfaceDesc to_core_native_surface_desc(
    const frameflow_native_surface_desc& surface
) {
    frameflow::FrameflowNativeSurfaceDesc desc;
    desc.backend = to_core_surface_backend(surface.backend);
    desc.bounds = to_core_surface_bounds(surface.bounds);
    switch (surface.backend) {
        case FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD:
            desc.x11 = frameflow::FrameflowX11SurfaceDesc{
                .display = surface.platform.x11.display,
                .parent_window = surface.platform.x11.parent_window,
            };
            break;
        case FRAMEFLOW_SURFACE_BACKEND_WINDOWS_HWND_CHILD:
            desc.win32 = frameflow::FrameflowWin32SurfaceDesc{
                .parent_hwnd = surface.platform.win32.parent_hwnd,
            };
            break;
        case FRAMEFLOW_SURFACE_BACKEND_AUTO:
        case FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP:
        default:
            break;
    }
    return desc;
}

const char* render_loop_mode_name(const frameflow::FrameflowRenderLoopMode mode) {
    return frameflow::frameflow_render_loop_mode_name(mode);
}

bool sync_native_surface_runtime_failure(frameflow_engine* engine);

void drain_native_surface_runtime_events(frameflow_engine* engine);

void refresh_diagnostics(frameflow_engine* engine, const bool count_update = true) {
    if (engine == nullptr) {
        return;
    }

    drain_native_surface_runtime_events(engine);

    const std::uint64_t diagnostics_count_to_report =
        engine->diagnostics_callback_count + (count_update ? 1u : 0u);
    if (engine->native_surface_runtime) {
        const auto runtime_stats = engine->native_surface_runtime->stats_snapshot();
        engine->render_backend = runtime_stats.surface_host_backend;
        engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
    }

    std::ostringstream out;
    out << "state=" << lifecycle_state_name(engine->lifecycle_state)
        << " surface_backend="
        << (engine->has_surface_target ? surface_backend_name(engine->surface_backend) : "UNSET")
        << " presentation_kind=" << presentation_kind_name(engine->presentation_kind)
        << " render_backend=" << engine->render_backend
        << " surface_x=" << engine->surface_bounds.x
        << " surface_y=" << engine->surface_bounds.y
        << " width=" << engine->width
        << " height=" << engine->height
        << " scale=" << engine->scale_factor
        << " surface_visible=" << (engine->surface_visible ? "true" : "false")
        << " legacy_surface_handle=" << presence_name(engine->legacy_surface_handle != 0u)
        << " theme=" << engine->theme
        << " tile_cache_bytes=" << engine->max_tile_cache_bytes
        << " log_level=" << engine->log_level
        << " points=" << engine->engine.point_count()
        << " pending_events=" << engine->pending_events.size()
        << " pending_events_high_watermark=" << engine->pending_events_high_watermark
        << " dispatched_events=" << engine->dispatched_event_count
        << " coalesced_camera_events=" << engine->coalesced_camera_event_count
        << " render_callback_count=" << engine->render_callback_count
        << " diagnostics_callback_count=" << diagnostics_count_to_report
        << " has_latest_frame=" << (has_latest_offscreen_frame(engine) ? "true" : "false")
        << " frame_generation=" << engine->offscreen_frame.generation
        << " frame_stride_bytes=" << engine->offscreen_frame.stride_bytes
        << " core_diag=[" << engine->engine.diagnostics_summary() << "]";

    if (engine->engine.selected_location_id().has_value()) {
        out << " selected_location_id=" << *engine->engine.selected_location_id();
    } else {
        out << " selected_location_id=none";
    }
    if (engine->runtime_camera_state.has_value()) {
        out << " camera_longitude_degrees=" << engine->runtime_camera_state->longitude
            << " camera_latitude_degrees=" << engine->runtime_camera_state->latitude
            << " camera_height_meters=" << engine->runtime_camera_state->height_meters
            << " camera_heading_degrees=" << engine->runtime_camera_state->heading_degrees
            << " camera_pitch_degrees=" << engine->runtime_camera_state->pitch_degrees
            << " camera_roll_degrees=" << engine->runtime_camera_state->roll_degrees;
    } else {
        out << " camera_state=none";
    }
    if (engine->last_error_code != FRAMEFLOW_RESULT_OK) {
        out << " last_error_code=" << engine->last_error_code
            << " last_error_recoverable=" << (engine->last_error_recoverable ? "true" : "false");
    }
    if (!engine->render_backend_diag.empty()) {
        out << " render_diag=[" << engine->render_backend_diag << "]";
    }
    out << " x11_display=" << presence_name(engine->x11_display != nullptr)
        << " x11_parent_window=" << presence_name(engine->x11_parent_window != 0u)
        << " win32_parent_hwnd=" << presence_name(engine->win32_parent_hwnd != nullptr);
    if (engine->native_surface_runtime) {
        const auto runtime_stats = engine->native_surface_runtime->stats_snapshot();
        out << " render_thread_running=" << (runtime_stats.worker_running ? "true" : "false")
            << " render_thread_mode=" << render_loop_mode_name(runtime_stats.loop_mode)
            << " render_thread_surface_created=" << (runtime_stats.surface_created ? "true" : "false")
            << " render_thread_visible=" << (runtime_stats.visible ? "true" : "false")
            << " render_thread_paused=" << (runtime_stats.paused ? "true" : "false")
            << " surface_host_backend=" << runtime_stats.surface_host_backend
            << " render_thread_start_count=" << runtime_stats.thread_start_count
            << " render_thread_stop_count=" << runtime_stats.thread_stop_count
            << " render_thread_wake_count=" << runtime_stats.wake_count
            << " render_thread_frame_requests=" << runtime_stats.frame_request_count
            << " render_thread_resize_count=" << runtime_stats.resize_count
            << " render_thread_visibility_changes=" << runtime_stats.visibility_change_count
            << " render_thread_make_current_count=" << runtime_stats.make_current_count
            << " render_thread_present_count=" << runtime_stats.present_count
            << " render_thread_destroy_count=" << runtime_stats.destroy_count
            << " render_thread_command_high_watermark=" << runtime_stats.command_high_watermark;
    }

    const auto& filter = engine->engine.filter();
    if (filter.query.has_value()) {
        out << " filter_query=" << *filter.query;
    }
    if (filter.category_code.has_value()) {
        out << " filter_category=" << *filter.category_code;
    }
    if (filter.location_id.has_value()) {
        out << " filter_location_id=" << *filter.location_id;
    }
    if (filter.country_code.has_value()) {
        out << " filter_country_code=" << *filter.country_code;
    }

    engine->diagnostics_cache = out.str();
    if (count_update) {
        engine->diagnostics_callback_count += 1u;
    }
}

std::optional<std::string> validate_presentation_target(
    const frameflow_presentation_target* presentation
) {
    if (presentation == nullptr) {
        return "initialize_with_presentation requires a non-null presentation target";
    }
    if (presentation->width <= 0 || presentation->height <= 0 || presentation->scale_factor <= 0.0) {
        return "presentation target requires positive size and positive scale factor";
    }
    switch (presentation->kind) {
        case FRAMEFLOW_PRESENTATION_KIND_NATIVE_SURFACE:
            if (presentation->surface_handle == 0u) {
                return "native surface presentation target requires a non-zero surface handle";
            }
            return std::nullopt;
        case FRAMEFLOW_PRESENTATION_KIND_OFFSCREEN_BITMAP:
            return std::nullopt;
        case FRAMEFLOW_PRESENTATION_KIND_NONE:
        default:
            return "presentation target kind must be specified";
    }
}

std::optional<std::string> validate_surface_bounds(
    const frameflow_surface_bounds* bounds,
    const char* context
) {
    if (bounds == nullptr) {
        std::ostringstream out;
        out << context << " requires non-null bounds";
        return out.str();
    }
    if (bounds->width <= 0 || bounds->height <= 0 || bounds->scale_factor <= 0.0) {
        std::ostringstream out;
        out << context << " requires positive size and positive scale factor";
        return out.str();
    }
    return std::nullopt;
}

std::optional<std::string> validate_native_surface_desc(
    const frameflow_native_surface_desc* surface
) {
    if (surface == nullptr) {
        return "initialize_with_native_surface requires a non-null surface descriptor";
    }
    if (const auto bounds_error = validate_surface_bounds(&surface->bounds, "surface descriptor");
        bounds_error.has_value()) {
        return bounds_error;
    }
    switch (surface->backend) {
        case FRAMEFLOW_SURFACE_BACKEND_AUTO:
        case FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP:
            return std::nullopt;
        case FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD:
            if (surface->platform.x11.display == nullptr) {
                return "linux X11 child surface descriptor requires a non-null display";
            }
            if (surface->platform.x11.parent_window == 0u) {
                return "linux X11 child surface descriptor requires a non-zero parent window";
            }
            return std::nullopt;
        case FRAMEFLOW_SURFACE_BACKEND_WINDOWS_HWND_CHILD:
            if (surface->platform.win32.parent_hwnd == nullptr) {
                return "windows HWND child surface descriptor requires a non-null parent HWND";
            }
            return std::nullopt;
        default:
            return "surface descriptor backend must be specified";
    }
}

void apply_options(frameflow_engine* engine, const frameflow_options& options) {
    if (engine == nullptr) {
        return;
    }
    engine->theme = options.theme != nullptr && options.theme[0] != '\0' ? options.theme : "dark";
    engine->tile_cache_path = options.tile_cache_path != nullptr ? options.tile_cache_path : "";
    engine->max_tile_cache_bytes = options.max_tile_cache_bytes;
    engine->log_level = options.log_level != nullptr && options.log_level[0] != '\0'
        ? options.log_level
        : "info";
}

void remember_surface_target(
    frameflow_engine* engine,
    const frameflow_native_surface_desc& surface,
    const std::uintptr_t legacy_surface_handle
) {
    if (engine == nullptr) {
        return;
    }
    engine->has_surface_target = true;
    engine->surface_backend = surface.backend;
    engine->surface_bounds = surface.bounds;
    engine->surface_visible = true;
    engine->x11_display = nullptr;
    engine->x11_parent_window = 0u;
    engine->win32_parent_hwnd = nullptr;
    engine->legacy_surface_handle = legacy_surface_handle;
    engine->presentation_kind = surface.backend == FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP
        ? FRAMEFLOW_PRESENTATION_KIND_OFFSCREEN_BITMAP
        : FRAMEFLOW_PRESENTATION_KIND_NATIVE_SURFACE;
    engine->width = surface.bounds.width;
    engine->height = surface.bounds.height;
    engine->scale_factor = surface.bounds.scale_factor;
    switch (surface.backend) {
        case FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD:
            engine->x11_display = surface.platform.x11.display;
            engine->x11_parent_window = surface.platform.x11.parent_window;
            break;
        case FRAMEFLOW_SURFACE_BACKEND_WINDOWS_HWND_CHILD:
            engine->win32_parent_hwnd = surface.platform.win32.parent_hwnd;
            break;
        case FRAMEFLOW_SURFACE_BACKEND_AUTO:
        case FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP:
        default:
            break;
    }
    engine->render_backend = surface.backend == FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP
        ? "software-offscreen"
        : "native-surface";
    engine->render_backend_diag.clear();
    engine->native_runtime_failure_reported = false;
#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
    engine->cesium_offscreen_renderer_attempted = false;
    engine->cesium_offscreen_renderer.reset();
#endif
}

void stop_native_surface_runtime(frameflow_engine* engine) {
    if (engine == nullptr || !engine->native_surface_runtime) {
        return;
    }
    engine->native_surface_runtime->stop();
    engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
    engine->native_surface_runtime.reset();
}

std::unique_ptr<frameflow::FrameflowSurfaceHost> create_native_surface_host_for_surface(
    const frameflow_native_surface_desc& surface
) {
#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
    if (surface.backend == FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD &&
        surface.platform.x11.display != nullptr &&
        reinterpret_cast<std::uintptr_t>(surface.platform.x11.display) > 4096u &&
        surface.platform.x11.parent_window > 4096u) {
        const char* display_env = std::getenv("DISPLAY");
        if (display_env != nullptr && display_env[0] != '\0') {
            return std::make_unique<frameflow::renderer::cesium::GlxNativeSceneSurfaceHost>(
                surface.platform.x11.parent_window
            );
        }
    }
#else
    (void)surface;
#endif
    return nullptr;
}

std::optional<std::string> start_native_surface_runtime(
    frameflow_engine* engine,
    const frameflow_native_surface_desc& surface
) {
    if (engine == nullptr || surface.backend == FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP) {
        return std::nullopt;
    }

    stop_native_surface_runtime(engine);

    auto runtime = std::make_unique<frameflow::FrameflowRenderThread>(
        to_core_native_surface_desc(surface),
        create_native_surface_host_for_surface(surface)
    );
    std::string error_message;
    if (!runtime->start(&error_message)) {
        return error_message.empty()
            ? std::optional<std::string>("failed to start native surface runtime")
            : std::optional<std::string>(error_message);
    }

    engine->render_backend = runtime->stats_snapshot().surface_host_backend;
    engine->render_backend_diag = runtime->diagnostics_summary();
    engine->native_surface_runtime = std::move(runtime);
    sync_native_surface_runtime_scene(engine);
    return std::nullopt;
}

void drain_native_surface_runtime_events(frameflow_engine* engine) {
    if (engine == nullptr || !engine->native_surface_runtime) {
        return;
    }

    auto events = engine->native_surface_runtime->drain_surface_events();
    if (events.empty()) {
        sync_native_surface_runtime_failure(engine);
        return;
    }

    for (const auto& event : events) {
        switch (event.type) {
            case frameflow::FrameflowSurfaceEvent::Type::LocationSelected:
                if (engine->engine.focus_location(event.location_id)) {
                    queue_location_selected_event(
                        engine,
                        event.location_id,
                        event.category_code,
                        event.screen_x,
                        event.screen_y,
                        event.interaction.empty() ? std::string("click") : event.interaction
                    );
                }
                break;
            case frameflow::FrameflowSurfaceEvent::Type::ClusterSelected:
                engine->engine.clear_selection();
                queue_cluster_selected_event(
                    engine,
                    event.cluster_id,
                    event.point_count,
                    event.longitude,
                    event.latitude
                );
                break;
            case frameflow::FrameflowSurfaceEvent::Type::SelectionCleared:
                engine->engine.clear_selection();
                break;
            case frameflow::FrameflowSurfaceEvent::Type::CameraChanged: {
                const frameflow_engine::RuntimeCameraState next_camera{
                    .longitude = event.longitude,
                    .latitude = event.latitude,
                    .height_meters = event.height_meters,
                    .heading_degrees = event.heading_degrees,
                    .pitch_degrees = event.pitch_degrees,
                    .roll_degrees = event.roll_degrees,
                };
                if (runtime_camera_changed(engine->runtime_camera_state, next_camera)) {
                    engine->runtime_camera_state = next_camera;
                    queue_camera_changed_event(engine, next_camera);
                }
                break;
            }
        }
    }

    engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
    sync_native_surface_runtime_failure(engine);
}

bool sync_native_surface_runtime_failure(frameflow_engine* engine) {
    if (engine == nullptr || !engine->native_surface_runtime) {
        return false;
    }

    const auto runtime_stats = engine->native_surface_runtime->stats_snapshot();
    engine->render_backend = runtime_stats.surface_host_backend;
    engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
    if (runtime_stats.loop_mode != frameflow::FrameflowRenderLoopMode::Failed) {
        return false;
    }

    const std::string detail = !runtime_stats.failure_message.empty()
        ? runtime_stats.failure_message
        : (!runtime_stats.last_status.empty() ? runtime_stats.last_status : std::string("unknown render thread failure"));
    const std::string message = "native render thread failed: " + detail;
    engine->lifecycle_state = FRAMEFLOW_LIFECYCLE_FAILED;

    if (!engine->native_runtime_failure_reported) {
        engine->last_error_code = FRAMEFLOW_RESULT_INTERNAL_ERROR;
        engine->last_error_message = message;
        engine->last_error_recoverable = false;
        queue_error_event(
            engine,
            FRAMEFLOW_RESULT_INTERNAL_ERROR,
            engine->last_error_message,
            false
        );
        engine->native_runtime_failure_reported = true;
    }
    return true;
}

frameflow_result set_error(
    frameflow_engine* engine,
    const frameflow_result code,
    std::string message,
    const bool recoverable
) {
    if (engine != nullptr) {
        engine->last_error_code = code;
        engine->last_error_message = message;
        engine->last_error_recoverable = recoverable;
        queue_error_event(engine, code, engine->last_error_message, recoverable);
        refresh_diagnostics(engine);
    }
    return code;
}

std::string exception_message_for_operation(
    const char* operation,
    const char* detail
) {
    std::ostringstream out;
    out << (operation != nullptr ? operation : "C API call")
        << " failed with an unhandled native exception";
    if (detail != nullptr && detail[0] != '\0') {
        out << ": " << detail;
    }
    return out.str();
}

frameflow_result set_exception_error(
    frameflow_engine* engine,
    const char* operation,
    const char* detail
) noexcept {
    try {
        return set_error(
            engine,
            FRAMEFLOW_RESULT_INTERNAL_ERROR,
            exception_message_for_operation(operation, detail),
            false
        );
    } catch (...) {
        if (engine != nullptr) {
            try {
                engine->last_error_code = FRAMEFLOW_RESULT_INTERNAL_ERROR;
                engine->last_error_message = "failed to record native exception";
                engine->last_error_recoverable = false;
            } catch (...) {
            }
        }
        return FRAMEFLOW_RESULT_INTERNAL_ERROR;
    }
}

template <typename Fn>
frameflow_result guard_result(
    frameflow_engine* engine,
    const char* operation,
    Fn&& fn
) noexcept {
    try {
        if (engine == nullptr) {
            return fn();
        }
        std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
        return fn();
    } catch (const std::exception& error) {
        if (engine == nullptr) {
            return set_exception_error(engine, operation, error.what());
        }
        std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
        return set_exception_error(engine, operation, error.what());
    } catch (...) {
        if (engine == nullptr) {
            return set_exception_error(engine, operation, nullptr);
        }
        std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
        return set_exception_error(engine, operation, nullptr);
    }
}

template <typename T, typename Fn>
T guard_value(
    const T fallback,
    Fn&& fn
) noexcept {
    try {
        return fn();
    } catch (...) {
        return fallback;
    }
}

template <typename Engine, typename T, typename Fn>
T guard_engine_value(
    Engine* engine,
    const T fallback,
    Fn&& fn
) noexcept {
    try {
        if (engine == nullptr) {
            return fn();
        }
        std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
        return fn();
    } catch (...) {
        return fallback;
    }
}

template <typename Fn>
std::uint64_t guard_callback_dispatch(
    frameflow_engine* engine,
    const char* operation,
    Fn&& fn
) noexcept {
    try {
        return fn();
    } catch (const std::exception& error) {
        if (engine != nullptr) {
            std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
            (void)set_exception_error(engine, operation, error.what());
            return 0u;
        }
        (void)set_exception_error(engine, operation, error.what());
        return 0u;
    } catch (...) {
        if (engine != nullptr) {
            std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
            (void)set_exception_error(engine, operation, nullptr);
            return 0u;
        }
        (void)set_exception_error(engine, operation, nullptr);
        return 0u;
    }
}

template <typename Fn>
void guard_void(Fn&& fn) noexcept {
    try {
        fn();
    } catch (...) {
    }
}

frameflow_result initialize_with_surface_target(
    frameflow_engine* engine,
    const frameflow_native_surface_desc& surface,
    const frameflow_options& options,
    const std::uintptr_t legacy_surface_handle
) {
    if (!engine->callbacks.has_any()) {
        return set_error(
            engine,
            FRAMEFLOW_RESULT_INVALID_STATE,
            "callbacks or event sink must be registered before initialize",
            true
        );
    }
    if (engine->lifecycle_state == FRAMEFLOW_LIFECYCLE_DISPOSED) {
        return set_error(engine, FRAMEFLOW_RESULT_INVALID_STATE, "engine is disposed", false);
    }
    if (engine->lifecycle_state != FRAMEFLOW_LIFECYCLE_UNINITIALIZED) {
        return set_error(
            engine,
            FRAMEFLOW_RESULT_INVALID_STATE,
            "initialize is only allowed from UNINITIALIZED",
            true
        );
    }

    remember_surface_target(engine, surface, legacy_surface_handle);
    apply_options(engine, options);
    engine->lifecycle_state = FRAMEFLOW_LIFECYCLE_INITIALIZING;
    if (const auto runtime_error = start_native_surface_runtime(engine, surface); runtime_error.has_value()) {
        engine->lifecycle_state = FRAMEFLOW_LIFECYCLE_FAILED;
        return set_error(engine, FRAMEFLOW_RESULT_INTERNAL_ERROR, *runtime_error, false);
    }
    engine->lifecycle_state = FRAMEFLOW_LIFECYCLE_READY;
    clear_error(engine);
    refresh_offscreen_frame(engine);
    queue_ready_event(engine);
    refresh_diagnostics(engine);
    return FRAMEFLOW_RESULT_OK;
}

frameflow_result ensure_can_mutate_active_engine(frameflow_engine* engine) {
    if (engine == nullptr) {
        return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
    }
    if (engine->lifecycle_state == FRAMEFLOW_LIFECYCLE_DISPOSED) {
        return set_error(engine, FRAMEFLOW_RESULT_INVALID_STATE, "engine is disposed", false);
    }
    if (sync_native_surface_runtime_failure(engine)) {
        return FRAMEFLOW_RESULT_INTERNAL_ERROR;
    }
    if (!is_active_state(engine->lifecycle_state)) {
        return set_error(
            engine,
            FRAMEFLOW_RESULT_INVALID_STATE,
            "engine is not initialized",
            true
        );
    }
    return FRAMEFLOW_RESULT_OK;
}

frameflow_result dispose_engine_state(
    frameflow_engine* engine,
    const bool update_diagnostics
) {
    if (engine == nullptr) {
        return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
    }
    if (engine->lifecycle_state == FRAMEFLOW_LIFECYCLE_DISPOSED) {
        clear_error(engine);
        if (update_diagnostics) {
            refresh_diagnostics(engine);
        }
        return FRAMEFLOW_RESULT_OK;
    }

    engine->lifecycle_state = FRAMEFLOW_LIFECYCLE_DISPOSED;
    stop_native_surface_runtime(engine);
    const std::vector<frameflow::GeoPointAggregate> empty_points{};
    engine->engine.set_points(empty_points);
    engine->engine.clear_selection();
    engine->engine.set_filter(frameflow::GlobeFilter{});
    engine->has_surface_target = false;
    engine->surface_backend = FRAMEFLOW_SURFACE_BACKEND_AUTO;
    engine->surface_bounds = frameflow_surface_bounds{};
    engine->surface_visible = true;
    engine->x11_display = nullptr;
    engine->x11_parent_window = 0u;
    engine->win32_parent_hwnd = nullptr;
    engine->legacy_surface_handle = 0u;
    engine->presentation_kind = FRAMEFLOW_PRESENTATION_KIND_NONE;
    engine->width = 0;
    engine->height = 0;
    engine->scale_factor = 1.0;
    engine->runtime_camera_state.reset();
    engine->render_backend = "none";
    engine->render_backend_diag.clear();
    engine->native_runtime_failure_reported = false;
#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
    engine->cesium_offscreen_renderer_attempted = false;
    engine->cesium_offscreen_renderer.reset();
#endif
    clear_offscreen_frame(engine);
    engine->offscreen_frame.generation = 0u;
    clear_callbacks_and_pending_events(engine);
    engine->pending_events_high_watermark = 0u;
    engine->dispatched_event_count = 0u;
    engine->coalesced_camera_event_count = 0u;
    engine->render_callback_count = 0u;
    engine->diagnostics_callback_count = 0u;
    clear_error(engine);
    if (update_diagnostics) {
        refresh_diagnostics(engine);
    }
    return FRAMEFLOW_RESULT_OK;
}

} // namespace

extern "C" {

int frameflow_bridge_abi_version(void) {
    return frameflow::bridge_abi_version;
}

int frameflow_engine_version_major(void) {
    return frameflow::version_major;
}

int frameflow_engine_version_minor(void) {
    return frameflow::version_minor;
}

int frameflow_engine_version_patch(void) {
    return frameflow::version_patch;
}

int frameflow_command_version_major(void) {
    return frameflow::command_version_major;
}

int frameflow_command_version_minor(void) {
    return frameflow::command_version_minor;
}

frameflow_result frameflow_bridge_get_runtime_info(
    frameflow_runtime_info* out_info
) {
    return guard_result(nullptr, "bridge_get_runtime_info", [&]() -> frameflow_result {
        if (out_info == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        fill_runtime_info(out_info);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_bridge_check_compatibility(
    const int32_t required_bridge_abi_version,
    const int32_t required_command_version_major,
    frameflow_runtime_info* out_info
) {
    return guard_result(nullptr, "bridge_check_compatibility", [&]() -> frameflow_result {
        if (out_info == nullptr || required_bridge_abi_version <= 0 || required_command_version_major <= 0) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }

        fill_runtime_info(out_info);
        if (out_info->bridge_abi_version != required_bridge_abi_version ||
            out_info->command_version_major != required_command_version_major) {
            return FRAMEFLOW_RESULT_NOT_SUPPORTED;
        }
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_engine* frameflow_engine_create(void) {
    return guard_value<frameflow_engine*>(nullptr, []() -> frameflow_engine* {
        auto engine = std::make_unique<frameflow_engine>();
        refresh_diagnostics(engine.get());
        return engine.release();
    });
}

void frameflow_engine_destroy(frameflow_engine* engine) {
    guard_void([&]() {
        if (engine != nullptr) {
            std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
            (void)dispose_engine_state(engine, false);
        }
        delete engine;
    });
}

frameflow_result frameflow_engine_set_callbacks(
    frameflow_engine* engine,
    const frameflow_callbacks* callbacks
) {
    return guard_result(engine, "engine_set_callbacks", [&]() -> frameflow_result {
        if (engine == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        if (engine->lifecycle_state == FRAMEFLOW_LIFECYCLE_DISPOSED) {
            return set_error(engine, FRAMEFLOW_RESULT_INVALID_STATE, "engine is disposed", false);
        }
        clear_callbacks_and_pending_events(engine);
        if (callbacks == nullptr) {
            clear_error(engine);
            refresh_diagnostics(engine);
            return FRAMEFLOW_RESULT_OK;
        }
        if (callbacks->on_ready == nullptr &&
            callbacks->on_location_selected == nullptr &&
            callbacks->on_cluster_selected == nullptr &&
            callbacks->on_camera_changed == nullptr &&
            callbacks->on_engine_error == nullptr) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "callbacks must provide at least one handler",
                true
            );
        }

        engine->callbacks.user = callbacks->user;
        engine->callbacks.on_ready = callbacks->on_ready;
        engine->callbacks.on_location_selected = callbacks->on_location_selected;
        engine->callbacks.on_cluster_selected = callbacks->on_cluster_selected;
        engine->callbacks.on_camera_changed = callbacks->on_camera_changed;
        engine->callbacks.on_engine_error = callbacks->on_engine_error;
        clear_error(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

std::uint64_t frameflow_engine_dispatch_pending_callbacks(frameflow_engine* engine) {
    return guard_callback_dispatch(engine, "engine_dispatch_pending_callbacks", [&]() -> std::uint64_t {
        RegisteredCallbacks callbacks;
        std::vector<PendingEvent> pending_events;
        {
            if (engine == nullptr) {
                return 0u;
            }
            std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
            if (!engine->callbacks.has_any()) {
                return 0u;
            }
            drain_native_surface_runtime_events(engine);
            if (engine->pending_events.empty()) {
                return 0u;
            }
            callbacks = engine->callbacks;
            pending_events = std::move(engine->pending_events);
            engine->pending_events.clear();
        }

        const std::uint64_t dispatched = dispatch_pending_callbacks(callbacks, pending_events);
        {
            std::lock_guard<std::recursive_mutex> lock(engine->api_mutex);
            engine->dispatched_event_count += dispatched;
            refresh_diagnostics(engine);
        }
        return dispatched;
    });
}

frameflow_lifecycle_state frameflow_engine_state(const frameflow_engine* engine) {
    return guard_engine_value(
        engine,
        FRAMEFLOW_LIFECYCLE_FAILED,
        [&]() -> frameflow_lifecycle_state {
            if (engine == nullptr) {
                return FRAMEFLOW_LIFECYCLE_FAILED;
            }
            return engine->lifecycle_state;
        }
    );
}

frameflow_result frameflow_engine_initialize(
    frameflow_engine* engine,
    const uintptr_t surface_handle,
    const std::int32_t width,
    const std::int32_t height,
    const double scale_factor,
    const frameflow_options* options
) {
    return guard_result(engine, "engine_initialize", [&]() -> frameflow_result {
        const frameflow_presentation_target presentation{
            .kind = FRAMEFLOW_PRESENTATION_KIND_NATIVE_SURFACE,
            .surface_handle = surface_handle,
            .width = width,
            .height = height,
            .scale_factor = scale_factor,
        };
        return frameflow_engine_initialize_with_presentation(engine, &presentation, options);
    });
}

frameflow_result frameflow_engine_initialize_with_presentation(
    frameflow_engine* engine,
    const frameflow_presentation_target* presentation,
    const frameflow_options* options
) {
    return guard_result(engine, "engine_initialize_with_presentation", [&]() -> frameflow_result {
        if (engine == nullptr || options == nullptr) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "initialize_with_presentation requires a non-null engine and options",
                true
            );
        }
        if (const auto validation_error = validate_presentation_target(presentation); validation_error.has_value()) {
            return set_error(engine, FRAMEFLOW_RESULT_INVALID_ARGUMENT, *validation_error, true);
        }
        frameflow_native_surface_desc surface{};
        surface.backend = presentation->kind == FRAMEFLOW_PRESENTATION_KIND_OFFSCREEN_BITMAP
            ? FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP
            : FRAMEFLOW_SURFACE_BACKEND_AUTO;
        surface.bounds.x = 0;
        surface.bounds.y = 0;
        surface.bounds.width = presentation->width;
        surface.bounds.height = presentation->height;
        surface.bounds.scale_factor = presentation->scale_factor;
        return initialize_with_surface_target(
            engine,
            surface,
            *options,
            presentation->kind == FRAMEFLOW_PRESENTATION_KIND_NATIVE_SURFACE ? presentation->surface_handle : 0u
        );
    });
}

frameflow_result frameflow_engine_initialize_with_native_surface(
    frameflow_engine* engine,
    const frameflow_native_surface_desc* surface,
    const frameflow_options* options
) {
    return guard_result(engine, "engine_initialize_with_native_surface", [&]() -> frameflow_result {
        if (engine == nullptr || options == nullptr) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "initialize_with_native_surface requires a non-null engine and options",
                true
            );
        }
        if (const auto validation_error = validate_native_surface_desc(surface); validation_error.has_value()) {
            return set_error(engine, FRAMEFLOW_RESULT_INVALID_ARGUMENT, *validation_error, true);
        }
        return initialize_with_surface_target(engine, *surface, *options, 0u);
    });
}

frameflow_result frameflow_engine_set_surface_bounds(
    frameflow_engine* engine,
    const frameflow_surface_bounds* bounds
) {
    return guard_result(engine, "engine_set_surface_bounds", [&]() -> frameflow_result {
        if (engine == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        if (const auto validation_error = validate_surface_bounds(bounds, "set_surface_bounds");
            validation_error.has_value()) {
            return set_error(engine, FRAMEFLOW_RESULT_INVALID_ARGUMENT, *validation_error, true);
        }
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }

        engine->surface_bounds = *bounds;
        engine->width = bounds->width;
        engine->height = bounds->height;
        engine->scale_factor = bounds->scale_factor;
        clear_error(engine);
        if (engine->native_surface_runtime) {
            engine->native_surface_runtime->update_bounds(to_core_surface_bounds(*bounds));
            engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
        }
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_set_surface_visible(
    frameflow_engine* engine,
    const uint8_t visible
) {
    return guard_result(engine, "engine_set_surface_visible", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }

        engine->surface_visible = visible != 0u;
        clear_error(engine);
        if (engine->native_surface_runtime) {
            engine->native_surface_runtime->set_visible(engine->surface_visible);
            engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
        }
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_request_frame(
    frameflow_engine* engine,
    const char* /*reason*/
) {
    return guard_result(engine, "engine_request_frame", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }

        clear_error(engine);
        if (has_offscreen_output_target(engine)) {
            refresh_offscreen_frame(engine);
        } else {
            if (engine->native_surface_runtime) {
                engine->native_surface_runtime->request_frame();
                engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
            }
            refresh_diagnostics(engine);
        }
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_resize(
    frameflow_engine* engine,
    const std::int32_t width,
    const std::int32_t height,
    const double scale_factor
) {
    return guard_result(engine, "engine_resize", [&]() -> frameflow_result {
        if (engine == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        if (width <= 0 || height <= 0 || scale_factor <= 0.0) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "resize requires a non-null engine, positive size, and positive scale factor",
                true
            );
        }
        frameflow_surface_bounds bounds = engine->surface_bounds;
        bounds.width = width;
        bounds.height = height;
        bounds.scale_factor = scale_factor;
        return frameflow_engine_set_surface_bounds(engine, &bounds);
    });
}

frameflow_result frameflow_engine_pause(frameflow_engine* engine) {
    return guard_result(engine, "engine_pause", [&]() -> frameflow_result {
        if (engine == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        if (engine->lifecycle_state == FRAMEFLOW_LIFECYCLE_DISPOSED) {
            return set_error(engine, FRAMEFLOW_RESULT_INVALID_STATE, "engine is disposed", false);
        }
        if (engine->lifecycle_state != FRAMEFLOW_LIFECYCLE_READY) {
            return set_error(engine, FRAMEFLOW_RESULT_INVALID_STATE, "pause requires READY state", true);
        }

        engine->lifecycle_state = FRAMEFLOW_LIFECYCLE_PAUSED;
        clear_error(engine);
        if (engine->native_surface_runtime) {
            engine->native_surface_runtime->set_paused(true);
            engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
        }
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_resume(frameflow_engine* engine) {
    return guard_result(engine, "engine_resume", [&]() -> frameflow_result {
        if (engine == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        if (engine->lifecycle_state == FRAMEFLOW_LIFECYCLE_DISPOSED) {
            return set_error(engine, FRAMEFLOW_RESULT_INVALID_STATE, "engine is disposed", false);
        }
        if (engine->lifecycle_state != FRAMEFLOW_LIFECYCLE_PAUSED) {
            return set_error(engine, FRAMEFLOW_RESULT_INVALID_STATE, "resume requires PAUSED state", true);
        }

        engine->lifecycle_state = FRAMEFLOW_LIFECYCLE_READY;
        clear_error(engine);
        if (engine->native_surface_runtime) {
            engine->native_surface_runtime->set_paused(false);
            engine->render_backend_diag = engine->native_surface_runtime->diagnostics_summary();
        }
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_dispose(frameflow_engine* engine) {
    return guard_result(engine, "engine_dispose", [&]() -> frameflow_result {
        return dispose_engine_state(engine, true);
    });
}

frameflow_result frameflow_engine_set_points(
    frameflow_engine* engine,
    const frameflow_point* points,
    const std::uint64_t point_count
) {
    return guard_result(engine, "engine_set_points", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }
        if (points == nullptr && point_count > 0) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "points array is required when point_count is greater than zero",
                true
            );
        }
        if (points != nullptr) {
            if (const auto validation_error = validate_points_input(points, point_count); validation_error.has_value()) {
                return set_error(
                    engine,
                    FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                    *validation_error,
                    true
                );
            }
        }

        const auto copied = copy_points(points, point_count);
        engine->engine.set_points(copied);
        clear_error(engine);
        sync_native_surface_runtime_scene(engine);
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_set_filters(
    frameflow_engine* engine,
    const frameflow_filter* filter
) {
    return guard_result(engine, "engine_set_filters", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }
        if (filter == nullptr) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "filter snapshot is required",
                true
            );
        }

        engine->engine.set_filter(copy_filter(*filter));
        clear_error(engine);
        sync_native_surface_runtime_scene(engine);
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

std::uint64_t frameflow_engine_point_count(const frameflow_engine* engine) {
    return guard_engine_value(engine, std::uint64_t{0u}, [&]() -> std::uint64_t {
        if (engine == nullptr) {
            return 0;
        }
        return static_cast<std::uint64_t>(engine->engine.point_count());
    });
}

frameflow_result frameflow_engine_focus_location(
    frameflow_engine* engine,
    const std::int64_t location_id
) {
    return guard_result(engine, "engine_focus_location", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }
        if (!engine->engine.focus_location(location_id)) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_NOT_FOUND,
                "location_id is not present in the current point snapshot",
                true
            );
        }

        clear_error(engine);
        queue_location_selected_event(engine, location_id, std::nullopt, 0.0, 0.0, "programmatic_focus");
        sync_native_surface_runtime_scene(engine);
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_select_location(
    frameflow_engine* engine,
    const std::int64_t location_id,
    const char* category_code,
    const double screen_x,
    const double screen_y,
    const char* interaction
) {
    return guard_result(engine, "engine_select_location", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }
        if (!engine->engine.focus_location(location_id)) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_NOT_FOUND,
                "location_id is not present in the current point snapshot",
                true
            );
        }

        clear_error(engine);
        queue_location_selected_event(
            engine,
            location_id,
            copy_nullable_string(category_code),
            screen_x,
            screen_y,
            (interaction != nullptr && interaction[0] != '\0') ? std::string(interaction) : std::string("native_selection")
        );
        sync_native_surface_runtime_scene(engine);
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_select_cluster(
    frameflow_engine* engine,
    const std::int64_t cluster_id,
    const std::int32_t point_count,
    const double longitude,
    const double latitude
) {
    return guard_result(engine, "engine_select_cluster", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }
        if (cluster_id <= 0 || point_count <= 1) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "cluster selection requires a positive cluster_id and point_count greater than one",
                true
            );
        }
        if (!has_valid_geo_coordinates(longitude, latitude)) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "cluster selection requires finite longitude/latitude and latitude in [-90, 90]",
                true
            );
        }

        engine->engine.clear_selection();
        clear_error(engine);
        queue_cluster_selected_event(engine, cluster_id, point_count, longitude, latitude);
        sync_native_surface_runtime_scene(engine);
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_report_camera_changed(
    frameflow_engine* engine,
    const double longitude,
    const double latitude,
    const double height_meters,
    const double heading_degrees,
    const double pitch_degrees,
    const double roll_degrees
) {
    return guard_result(engine, "engine_report_camera_changed", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }
        if (!has_valid_geo_coordinates(longitude, latitude) || !std::isfinite(height_meters) ||
            !std::isfinite(heading_degrees) || !std::isfinite(pitch_degrees) || !std::isfinite(roll_degrees) ||
            height_meters <= 0.0) {
            return set_error(
                engine,
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
                "camera state requires finite values, latitude in [-90, 90], and positive height_meters",
                true
            );
        }

        const frameflow_engine::RuntimeCameraState next_camera{
            .longitude = longitude,
            .latitude = latitude,
            .height_meters = height_meters,
            .heading_degrees = heading_degrees,
            .pitch_degrees = pitch_degrees,
            .roll_degrees = roll_degrees,
        };

        const bool changed = runtime_camera_changed(engine->runtime_camera_state, next_camera);
        engine->runtime_camera_state = next_camera;
        clear_error(engine);
        if (changed) {
            queue_camera_changed_event(engine, next_camera);
        }
        sync_native_surface_runtime_scene(engine);
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

std::uint8_t frameflow_engine_has_selected_location_id(const frameflow_engine* engine) {
    return guard_engine_value(engine, static_cast<std::uint8_t>(0u), [&]() -> std::uint8_t {
        if (engine == nullptr) {
            return 0u;
        }
        return engine->engine.selected_location_id().has_value() ? 1u : 0u;
    });
}

frameflow_result frameflow_engine_get_selected_location_id(
    const frameflow_engine* engine,
    std::int64_t* out_location_id
) {
    return guard_result(const_cast<frameflow_engine*>(engine), "engine_get_selected_location_id", [&]() -> frameflow_result {
        if (engine == nullptr || out_location_id == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        if (!engine->engine.selected_location_id().has_value()) {
            return FRAMEFLOW_RESULT_NOT_FOUND;
        }

        *out_location_id = *engine->engine.selected_location_id();
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_get_location_screen_position(
    const frameflow_engine* engine,
    const std::int64_t location_id,
    frameflow_screen_position* out_position
) {
    return guard_result(
        const_cast<frameflow_engine*>(engine),
        "engine_get_location_screen_position",
        [&]() -> frameflow_result {
            if (engine == nullptr || out_position == nullptr || location_id <= 0) {
                return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
            }

            out_position->x = 0.0;
            out_position->y = 0.0;
            out_position->visible = 0u;

#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
            if (engine->cesium_offscreen_renderer) {
                const auto position = engine->cesium_offscreen_renderer->screen_position_for_location(location_id);
                if (position.has_value()) {
                    out_position->x = position->x;
                    out_position->y = position->y;
                    out_position->visible = 1u;
                }
            }
#endif

            return FRAMEFLOW_RESULT_OK;
        }
    );
}

frameflow_result frameflow_engine_clear_selection(frameflow_engine* engine) {
    return guard_result(engine, "engine_clear_selection", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }

        engine->engine.clear_selection();
        clear_error(engine);
        sync_native_surface_runtime_scene(engine);
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

std::uint8_t frameflow_engine_has_latest_frame(const frameflow_engine* engine) {
    return guard_engine_value(engine, static_cast<std::uint8_t>(0u), [&]() -> std::uint8_t {
        return has_latest_offscreen_frame(engine) ? 1u : 0u;
    });
}

std::uint64_t frameflow_engine_get_frame_generation(const frameflow_engine* engine) {
    return guard_engine_value(engine, std::uint64_t{0u}, [&]() -> std::uint64_t {
        if (engine == nullptr || !has_latest_offscreen_frame(engine)) {
            return 0u;
        }
        return engine->offscreen_frame.generation;
    });
}

std::uint64_t frameflow_engine_get_render_callback_count(const frameflow_engine* engine) {
    return guard_engine_value(engine, std::uint64_t{0u}, [&]() -> std::uint64_t {
        return engine != nullptr ? engine->render_callback_count : 0u;
    });
}

std::uint64_t frameflow_engine_get_diagnostics_callback_count(const frameflow_engine* engine) {
    return guard_engine_value(engine, std::uint64_t{0u}, [&]() -> std::uint64_t {
        return engine != nullptr ? engine->diagnostics_callback_count : 0u;
    });
}

frameflow_result frameflow_engine_render_latest_frame(frameflow_engine* engine) {
    return guard_result(engine, "engine_render_latest_frame", [&]() -> frameflow_result {
        const auto state_check = ensure_can_mutate_active_engine(engine);
        if (state_check != FRAMEFLOW_RESULT_OK) {
            return state_check;
        }

        clear_error(engine);
        refresh_offscreen_frame(engine);
        refresh_diagnostics(engine);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_get_latest_frame_info(
    const frameflow_engine* engine,
    frameflow_frame_info* out_info
) {
    return guard_result(const_cast<frameflow_engine*>(engine), "engine_get_latest_frame_info", [&]() -> frameflow_result {
        if (engine == nullptr || out_info == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        if (!has_latest_offscreen_frame(engine)) {
            return FRAMEFLOW_RESULT_NOT_FOUND;
        }

        out_info->width = engine->width;
        out_info->height = engine->height;
        out_info->stride_bytes = engine->offscreen_frame.stride_bytes;
        out_info->generation = engine->offscreen_frame.generation;
        out_info->pixel_format = FRAMEFLOW_FRAME_PIXEL_FORMAT_RGBA8;
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_copy_latest_frame(
    const frameflow_engine* engine,
    std::uint8_t* out_pixels,
    const std::uint64_t out_pixel_capacity
) {
    return guard_result(const_cast<frameflow_engine*>(engine), "engine_copy_latest_frame", [&]() -> frameflow_result {
        if (engine == nullptr || out_pixels == nullptr) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }
        if (!has_latest_offscreen_frame(engine)) {
            return FRAMEFLOW_RESULT_NOT_FOUND;
        }

        const auto required_bytes = offscreen_required_bytes(*engine);
        if (required_bytes > out_pixel_capacity) {
            return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
        }

        std::memcpy(out_pixels, engine->offscreen_frame.rgba_pixels.data(), required_bytes);
        return FRAMEFLOW_RESULT_OK;
    });
}

frameflow_result frameflow_engine_last_error_code(const frameflow_engine* engine) {
    return guard_engine_value(
        engine,
        FRAMEFLOW_RESULT_INTERNAL_ERROR,
        [&]() -> frameflow_result {
            if (engine == nullptr) {
                return FRAMEFLOW_RESULT_INVALID_ARGUMENT;
            }
            return engine->last_error_code;
        }
    );
}

const char* frameflow_engine_last_error_message(const frameflow_engine* engine) {
    return guard_engine_value(engine, "native exception while reading last error", [&]() -> const char* {
        if (engine == nullptr) {
            return "engine is null";
        }
        return engine->last_error_message.c_str();
    });
}

uint8_t frameflow_engine_last_error_recoverable(const frameflow_engine* engine) {
    return guard_engine_value(engine, static_cast<std::uint8_t>(0u), [&]() -> std::uint8_t {
        if (engine == nullptr) {
            return 0u;
        }
        return engine->last_error_recoverable ? 1u : 0u;
    });
}

const char* frameflow_engine_diagnostics_summary(frameflow_engine* engine) {
    return guard_engine_value(engine, "state=FAILED error=native_exception", [&]() -> const char* {
        if (engine == nullptr) {
            return "state=NULL";
        }
        refresh_diagnostics(engine, false);
        return engine->diagnostics_cache.c_str();
    });
}

} // extern "C"
