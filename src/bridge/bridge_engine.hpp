#pragma once

#include "core/native_surface_runtime.hpp"
#include "frameflow/c/bridge.h"
#include "frameflow/engine.hpp"

#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
#include "renderer/cesium/glx_offscreen_scene_host.hpp"
#endif

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct RegisteredCallbacks {
    void* user{};
    frameflow_ready_callback on_ready{};
    frameflow_location_selection_callback on_location_selected{};
    frameflow_cluster_selection_callback on_cluster_selected{};
    frameflow_camera_changed_callback on_camera_changed{};
    frameflow_error_callback on_engine_error{};

    [[nodiscard]] bool has_any() const noexcept {
        return on_ready != nullptr ||
            on_location_selected != nullptr ||
            on_cluster_selected != nullptr ||
            on_camera_changed != nullptr ||
            on_engine_error != nullptr;
    }

    void clear() noexcept {
        user = nullptr;
        on_ready = nullptr;
        on_location_selected = nullptr;
        on_cluster_selected = nullptr;
        on_camera_changed = nullptr;
        on_engine_error = nullptr;
    }
};

struct PendingEvent {
    enum class Type {
        Ready,
        LocationSelected,
        ClusterSelected,
        CameraChanged,
        Error
    };

    Type type{Type::Ready};

    std::int32_t width{};
    std::int32_t height{};
    double scale_factor{};

    std::int64_t location_id{};
    std::optional<std::string> category_code;
    double screen_x{};
    double screen_y{};
    std::string interaction;

    std::int64_t cluster_id{};
    std::int32_t point_count{};
    double longitude{};
    double latitude{};

    double height_meters{};
    double heading_degrees{};
    double pitch_degrees{};
    double roll_degrees{};

    frameflow_result error_code{FRAMEFLOW_RESULT_OK};
    std::string error_message;
    bool error_recoverable{};
};

struct frameflow_engine {
    struct OffscreenFrame {
        std::vector<std::uint8_t> rgba_pixels;
        std::uint32_t stride_bytes{0u};
        std::uint64_t generation{0u};
    };

    struct RuntimeCameraState {
        double longitude{};
        double latitude{};
        double height_meters{};
        double heading_degrees{};
        double pitch_degrees{};
        double roll_degrees{};
    };

    mutable std::recursive_mutex api_mutex;
    frameflow::Engine engine;
    frameflow_lifecycle_state lifecycle_state{FRAMEFLOW_LIFECYCLE_UNINITIALIZED};
    bool has_surface_target{false};
    frameflow_surface_backend surface_backend{FRAMEFLOW_SURFACE_BACKEND_AUTO};
    frameflow_surface_bounds surface_bounds{};
    bool surface_visible{true};
    void* x11_display{};
    std::uint64_t x11_parent_window{};
    void* win32_parent_hwnd{};
    std::uintptr_t legacy_surface_handle{};
    frameflow_presentation_kind presentation_kind{FRAMEFLOW_PRESENTATION_KIND_NONE};
    std::int32_t width{};
    std::int32_t height{};
    double scale_factor{1.0};
    std::string theme{"dark"};
    std::string tile_cache_path;
    std::uint64_t max_tile_cache_bytes{};
    std::string log_level{"info"};
    frameflow_result last_error_code{FRAMEFLOW_RESULT_OK};
    std::string last_error_message;
    bool last_error_recoverable{false};
    std::string diagnostics_cache;
    RegisteredCallbacks callbacks;
    std::vector<PendingEvent> pending_events;
    std::optional<RuntimeCameraState> runtime_camera_state;
    std::uint64_t pending_events_high_watermark{0u};
    std::uint64_t dispatched_event_count{0u};
    std::uint64_t coalesced_camera_event_count{0u};
    std::uint64_t render_callback_count{0u};
    std::uint64_t diagnostics_callback_count{0u};
    OffscreenFrame offscreen_frame;
    std::string render_backend{"software-offscreen"};
    std::string render_backend_diag;
    bool native_runtime_failure_reported{false};
    std::unique_ptr<frameflow::FrameflowRenderThread> native_surface_runtime;
#ifdef FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT
    bool cesium_offscreen_renderer_attempted{false};
    std::unique_ptr<frameflow::renderer::cesium::GlxOffscreenSceneHost> cesium_offscreen_renderer;
#endif
};
