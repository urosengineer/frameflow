#include "frameflow/c/bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::abort();
}

void expect(const bool condition, const char* message) {
    if (!condition) {
        fail(message);
    }
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const char* value)
        : name_(name) {
        if (const char* current_value = std::getenv(name); current_value != nullptr) {
            previous_value_ = current_value;
        }
        setenv(name_.c_str(), value, 1);
    }

    ~ScopedEnvironmentVariable() {
        if (previous_value_.has_value()) {
            setenv(name_.c_str(), previous_value_->c_str(), 1);
            return;
        }
        unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> previous_value_;
};

struct CallbackTracker {
    frameflow_engine* engine_for_callback{};
    std::uint64_t ready_count{};
    std::uint64_t location_count{};
    std::uint64_t cluster_count{};
    std::uint64_t camera_count{};
    std::uint64_t error_count{};

    frameflow_ready_event last_ready{};
    frameflow_location_selection_event last_location{};
    frameflow_cluster_selection_event last_cluster{};
    frameflow_camera_state last_camera{};
    frameflow_error_event last_error{};

    std::string last_location_interaction;
    std::string last_location_category_code;
    std::string last_error_message;
};

void on_ready(void* user, const frameflow_ready_event* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    if (tracker == nullptr || event == nullptr) {
        fail("ready callback received invalid payload");
    }
    tracker->ready_count += 1u;
    tracker->last_ready = *event;
}

void on_ready_throwing(void*, const frameflow_ready_event*) {
    throw std::runtime_error("ready callback failure");
}

void on_ready_clears_callbacks(void* user, const frameflow_ready_event* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    if (tracker == nullptr || event == nullptr || tracker->engine_for_callback == nullptr) {
        fail("ready callback received invalid reentrant payload");
    }
    tracker->ready_count += 1u;
    tracker->last_ready = *event;
    if (frameflow_engine_set_callbacks(tracker->engine_for_callback, nullptr) != FRAMEFLOW_RESULT_OK) {
        fail("ready callback should be able to clear callbacks reentrantly");
    }
}

void on_location_selected(void* user, const frameflow_location_selection_event* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    if (tracker == nullptr || event == nullptr) {
        fail("location callback received invalid payload");
    }
    tracker->location_count += 1u;
    tracker->last_location = *event;
    tracker->last_location_interaction = event->interaction != nullptr ? event->interaction : "";
    tracker->last_location_category_code = event->category_code != nullptr ? event->category_code : "";
}

void on_cluster_selected(void* user, const frameflow_cluster_selection_event* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    if (tracker == nullptr || event == nullptr) {
        fail("cluster callback received invalid payload");
    }
    tracker->cluster_count += 1u;
    tracker->last_cluster = *event;
}

void on_camera_changed(void* user, const frameflow_camera_state* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    if (tracker == nullptr || event == nullptr) {
        fail("camera callback received invalid payload");
    }
    tracker->camera_count += 1u;
    tracker->last_camera = *event;
}

void on_engine_error(void* user, const frameflow_error_event* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    if (tracker == nullptr || event == nullptr) {
        fail("error callback received invalid payload");
    }
    tracker->error_count += 1u;
    tracker->last_error = *event;
    tracker->last_error_message = event->message != nullptr ? event->message : "";
}

frameflow_callbacks make_callbacks(CallbackTracker* tracker) {
    frameflow_callbacks callbacks{};
    callbacks.user = tracker;
    callbacks.on_ready = on_ready;
    callbacks.on_location_selected = on_location_selected;
    callbacks.on_cluster_selected = on_cluster_selected;
    callbacks.on_camera_changed = on_camera_changed;
    callbacks.on_engine_error = on_engine_error;
    return callbacks;
}

frameflow_options default_options() {
    frameflow_options options{};
    options.theme = "dark";
    options.tile_cache_path = "/tmp/frameflow-cache";
    options.max_tile_cache_bytes = 1024 * 1024;
    options.log_level = "info";
    return options;
}

frameflow_surface_bounds surface_bounds(
    const std::int32_t width,
    const std::int32_t height,
    const double scale_factor,
    const std::int32_t x = 0,
    const std::int32_t y = 0
) {
    frameflow_surface_bounds bounds{};
    bounds.x = x;
    bounds.y = y;
    bounds.width = width;
    bounds.height = height;
    bounds.scale_factor = scale_factor;
    return bounds;
}

frameflow_native_surface_desc linux_x11_child_surface_target(
    void* display,
    const std::uint64_t parent_window,
    const std::int32_t width,
    const std::int32_t height,
    const double scale_factor
) {
    frameflow_native_surface_desc target{};
    target.backend = FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD;
    target.bounds = surface_bounds(width, height, scale_factor);
    target.platform.x11.display = display;
    target.platform.x11.parent_window = parent_window;
    return target;
}

frameflow_native_surface_desc windows_hwnd_child_surface_target(
    void* parent_hwnd,
    const std::int32_t width,
    const std::int32_t height,
    const double scale_factor
) {
    frameflow_native_surface_desc target{};
    target.backend = FRAMEFLOW_SURFACE_BACKEND_WINDOWS_HWND_CHILD;
    target.bounds = surface_bounds(width, height, scale_factor);
    target.platform.win32.parent_hwnd = parent_hwnd;
    return target;
}

frameflow_native_surface_desc offscreen_bitmap_surface_target(
    const std::int32_t width,
    const std::int32_t height,
    const double scale_factor
) {
    frameflow_native_surface_desc target{};
    target.backend = FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP;
    target.bounds = surface_bounds(width, height, scale_factor);
    return target;
}

frameflow_presentation_target legacy_native_surface_target(
    const std::uintptr_t surface_handle,
    const std::int32_t width,
    const std::int32_t height,
    const double scale_factor
) {
    frameflow_presentation_target target{};
    target.kind = FRAMEFLOW_PRESENTATION_KIND_NATIVE_SURFACE;
    target.surface_handle = surface_handle;
    target.width = width;
    target.height = height;
    target.scale_factor = scale_factor;
    return target;
}

frameflow_point point(const std::int64_t location_id, const char* label) {
    static const char* kTopCategories[] = {"POLITICS", "CRIME"};
    frameflow_point point{};
    point.location_id = location_id;
    point.label = label;
    point.kind = FRAMEFLOW_LOCATION_KIND_CITY;
    point.country_code = "RS";
    point.latitude = 44.7866;
    point.longitude = 20.4489;
    point.story_count = 3;
    point.latest_story_epoch_millis = 1776335400000LL;
    point.top_categories = kTopCategories;
    point.top_category_count = 2u;
    point.style_key = "category-politics";
    return point;
}

std::array<std::uint8_t, 4u> rgba_at(
    const std::vector<std::uint8_t>& pixels,
    const frameflow_frame_info& frame_info,
    const int x,
    const int y
) {
    const auto offset = (static_cast<std::size_t>(y) * frame_info.stride_bytes) + (static_cast<std::size_t>(x) * 4u);
    return {pixels[offset + 0u], pixels[offset + 1u], pixels[offset + 2u], pixels[offset + 3u]};
}

bool diagnostics_contains_with_retry(
    frameflow_engine* engine,
    const std::string& needle,
    const std::chrono::milliseconds timeout = std::chrono::milliseconds(500)
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const char* diagnostics = frameflow_engine_diagnostics_summary(engine);
        if (diagnostics != nullptr && std::strstr(diagnostics, needle.c_str()) != nullptr) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void lifecycle_starts_uninitialized_with_version_surface() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");
    expect(frameflow_bridge_abi_version() == 2, "bridge ABI version should be 2");
    expect(frameflow_engine_version_major() == 0, "engine major version should be 0");
    expect(frameflow_engine_version_minor() == 1, "engine minor version should be 1");
    expect(frameflow_engine_version_patch() == 0, "engine patch version should be 0");
    expect(frameflow_command_version_major() == 2, "command major version should be 2");
    expect(frameflow_command_version_minor() == 8, "command minor version should be 8");
    expect(
        frameflow_engine_state(engine) == FRAMEFLOW_LIFECYCLE_UNINITIALIZED,
        "engine should start in UNINITIALIZED state"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "state=UNINITIALIZED") != nullptr,
        "diagnostics should expose the initial state"
    );
    frameflow_engine_destroy(engine);
}

void runtime_info_and_compatibility_helpers_fail_fast() {
    frameflow_runtime_info runtime_info{};
    expect(
        frameflow_bridge_get_runtime_info(&runtime_info) == FRAMEFLOW_RESULT_OK,
        "runtime info helper should succeed"
    );
    expect(runtime_info.bridge_abi_version == 2, "runtime info should expose bridge ABI version");
    expect(runtime_info.engine_version_major == 0, "runtime info should expose engine major version");
    expect(runtime_info.engine_version_minor == 1, "runtime info should expose engine minor version");
    expect(runtime_info.engine_version_patch == 0, "runtime info should expose engine patch version");
    expect(runtime_info.command_version_major == 2, "runtime info should expose command major version");
    expect(runtime_info.command_version_minor == 8, "runtime info should expose command minor version");

    frameflow_runtime_info compatibility_info{};
    expect(
        frameflow_bridge_check_compatibility(2, 2, &compatibility_info) ==
            FRAMEFLOW_RESULT_OK,
        "compatibility helper should accept the current ABI and command major versions"
    );
    expect(compatibility_info.bridge_abi_version == 2, "compatibility helper should fill runtime info");
    expect(
        frameflow_bridge_check_compatibility(1, 2, &compatibility_info) ==
            FRAMEFLOW_RESULT_NOT_SUPPORTED,
        "compatibility helper should fail fast on ABI mismatch"
    );
    expect(
        frameflow_bridge_check_compatibility(2, 1, &compatibility_info) ==
            FRAMEFLOW_RESULT_NOT_SUPPORTED,
        "compatibility helper should fail fast on command major mismatch"
    );
    expect(
        frameflow_bridge_check_compatibility(2, 0, &compatibility_info) ==
            FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "compatibility helper should reject invalid required versions"
    );
    expect(
        frameflow_bridge_get_runtime_info(nullptr) == FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "runtime info helper should reject null output buffers"
    );
}

void initialize_requires_callback_sink_and_dispatches_ready_event() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    const auto options = default_options();
    expect(
        frameflow_engine_initialize(engine, 1u, 1280, 720, 1.5, &options) ==
            FRAMEFLOW_RESULT_INVALID_STATE,
        "initialize should require callbacks or event sink registration"
    );
    expect(
        frameflow_engine_last_error_code(engine) == FRAMEFLOW_RESULT_INVALID_STATE,
        "missing callback sink should set INVALID_STATE"
    );

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should accept a valid callback table"
    );

    expect(
        frameflow_engine_initialize(engine, 1u, 1280, 720, 1.5, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should succeed with callbacks registered"
    );
    expect(
        frameflow_engine_state(engine) == FRAMEFLOW_LIFECYCLE_READY,
        "initialize should transition the engine to READY"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "initialize should queue exactly one ready event"
    );
    expect(tracker.ready_count == 1u, "ready callback should be delivered once");
    expect(tracker.last_ready.width == 1280, "ready callback should include width");
    expect(tracker.last_ready.height == 720, "ready callback should include height");
    expect(tracker.last_ready.scale_factor == 1.5, "ready callback should include scale factor");
    expect(tracker.last_ready.bridge_abi_version == 2, "ready callback should include ABI version");
    expect(tracker.last_ready.engine_version_major == 0, "ready callback should include engine major version");
    expect(tracker.last_ready.engine_version_minor == 1, "ready callback should include engine minor version");
    expect(tracker.last_ready.engine_version_patch == 0, "ready callback should include engine patch version");
    expect(tracker.last_ready.command_version_major == 2, "ready callback should include command major version");
    expect(tracker.last_ready.command_version_minor == 8, "ready callback should include command minor version");

    frameflow_engine_destroy(engine);
}

void callback_exceptions_are_captured_at_c_api_boundary() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    auto callbacks = make_callbacks(&tracker);
    callbacks.on_ready = on_ready_throwing;
    const auto options = default_options();

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should accept a throwing host callback for boundary testing"
    );
    expect(
        frameflow_engine_initialize(engine, 1u, 1280, 720, 1.5, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should queue ready event before callback dispatch"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 0u,
        "throwing host callback should be contained by the C API boundary"
    );
    expect(tracker.ready_count == 0u, "throwing ready callback should not be counted as dispatched");
    expect(
        frameflow_engine_last_error_code(engine) == FRAMEFLOW_RESULT_INTERNAL_ERROR,
        "callback exception should record an internal bridge error"
    );
    expect(
        std::strstr(frameflow_engine_last_error_message(engine), "engine_dispatch_pending_callbacks") != nullptr,
        "callback exception should include the dispatch operation name"
    );
    expect(
        std::strstr(frameflow_engine_last_error_message(engine), "ready callback failure") != nullptr,
        "callback exception should preserve the exception detail"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "last_error_code=5") != nullptr,
        "diagnostics should expose the captured callback exception"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "captured callback exception should queue one error callback"
    );
    expect(tracker.error_count == 1u, "error callback should observe the captured callback exception");
    expect(
        tracker.last_error.code == FRAMEFLOW_RESULT_INTERNAL_ERROR,
        "error callback should expose internal error code"
    );
    expect(
        tracker.last_error_message.find("ready callback failure") != std::string::npos,
        "error callback should preserve callback exception detail"
    );

    frameflow_engine_destroy(engine);
}

void callback_dispatch_uses_copied_batch_and_callback_snapshot() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    tracker.engine_for_callback = engine;
    auto callbacks = make_callbacks(&tracker);
    callbacks.on_ready = on_ready_clears_callbacks;
    const auto options = default_options();

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should accept reentrant callback test table"
    );
    expect(
        frameflow_engine_initialize(engine, 1u, 1280, 720, 1.5, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should queue ready event"
    );
    const frameflow_point points[]{point(601, "Belgrade")};
    expect(
        frameflow_engine_set_points(engine, points, 1) == FRAMEFLOW_RESULT_OK,
        "set_points should accept snapshot before copied-batch dispatch"
    );
    expect(
        frameflow_engine_focus_location(engine, 601) == FRAMEFLOW_RESULT_OK,
        "focus_location should queue location event before copied-batch dispatch"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 2u,
        "dispatch should deliver copied ready and location events even if callbacks are cleared reentrantly"
    );
    expect(tracker.ready_count == 1u, "ready callback should fire once");
    expect(tracker.location_count == 1u, "location callback from copied batch should still fire");
    expect(
        tracker.last_location.location_id == 601,
        "copied location event should preserve selected location id"
    );

    expect(
        frameflow_engine_focus_location(engine, 601) == FRAMEFLOW_RESULT_OK,
        "focus_location should still mutate state after callbacks were cleared"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 0u,
        "cleared callback registration should prevent future callback delivery"
    );

    frameflow_engine_destroy(engine);
}

void initialize_with_native_surface_supports_linux_x11_descriptor_and_lifecycle_helpers() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();
    const auto surface = linux_x11_child_surface_target(reinterpret_cast<void*>(0x22u), 13u, 1024, 768, 1.25);

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed before native surface initialize"
    );
    expect(
        frameflow_engine_initialize_with_native_surface(engine, &surface, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize_with_native_surface should accept Linux X11 child descriptors"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "native surface initialize should emit one ready event"
    );
    expect(tracker.ready_count == 1u, "ready callback should fire for native surface initialize");
    expect(tracker.last_ready.width == 1024, "ready callback should use surface width");
    expect(tracker.last_ready.height == 768, "ready callback should use surface height");
    expect(tracker.last_ready.scale_factor == 1.25, "ready callback should use surface scale");
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "surface_backend=LINUX_X11_CHILD") != nullptr,
        "diagnostics should expose Linux X11 child surface backend"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "x11_display=present") != nullptr,
        "diagnostics should expose X11 display presence"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "x11_parent_window=present") != nullptr,
        "diagnostics should expose X11 parent window presence"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "x11_parent_window=13") == nullptr,
        "diagnostics should not expose the raw X11 parent window"
    );
    expect(
        diagnostics_contains_with_retry(engine, "surface_host_backend=null-surface-host"),
        "diagnostics should expose the Phase 1 null surface host backend"
    );
    expect(
        diagnostics_contains_with_retry(engine, "render_thread_running=true"),
        "diagnostics should expose an active render thread"
    );
    expect(
        diagnostics_contains_with_retry(engine, "render_thread_mode=IDLE"),
        "diagnostics should expose the render thread idle mode after start"
    );
    expect(
        frameflow_engine_set_surface_visible(engine, 0u) == FRAMEFLOW_RESULT_OK,
        "set_surface_visible should accept hidden state"
    );
    expect(
        frameflow_engine_set_surface_visible(engine, 0u) == FRAMEFLOW_RESULT_OK,
        "set_surface_visible should be idempotent"
    );
    expect(
        diagnostics_contains_with_retry(engine, "surface_visible=false"),
        "diagnostics should expose hidden native surface visibility"
    );
    const auto resized_bounds = surface_bounds(800, 600, 2.0, 11, 17);
    expect(
        frameflow_engine_set_surface_bounds(engine, &resized_bounds) == FRAMEFLOW_RESULT_OK,
        "set_surface_bounds should accept updated bounds"
    );
    expect(
        frameflow_engine_set_surface_bounds(engine, &resized_bounds) == FRAMEFLOW_RESULT_OK,
        "set_surface_bounds should be idempotent for repeated bounds"
    );
    expect(
        frameflow_engine_set_surface_visible(engine, 1u) == FRAMEFLOW_RESULT_OK,
        "set_surface_visible should accept showing the native surface again"
    );
    expect(
        frameflow_engine_request_frame(engine, "smoke") == FRAMEFLOW_RESULT_OK,
        "request_frame should be a safe no-op before native backend implementation"
    );
    expect(
        diagnostics_contains_with_retry(engine, "render_thread_present_count=1"),
        "request_frame should drive one coalesced null-host present on the render thread"
    );
    expect(
        frameflow_engine_pause(engine) == FRAMEFLOW_RESULT_OK,
        "pause should succeed for native surface runtime"
    );
    expect(
        diagnostics_contains_with_retry(engine, "render_thread_mode=PAUSED"),
        "diagnostics should expose paused render-thread state"
    );
    expect(
        frameflow_engine_resume(engine) == FRAMEFLOW_RESULT_OK,
        "resume should succeed for native surface runtime"
    );
    expect(
        diagnostics_contains_with_retry(engine, "render_thread_mode=IDLE"),
        "diagnostics should expose resumed render-thread state"
    );
    const std::string diagnostics = frameflow_engine_diagnostics_summary(engine);
    expect(diagnostics.find("surface_visible=true") != std::string::npos, "diagnostics should expose visible state");
    expect(diagnostics.find("surface_x=11") != std::string::npos, "diagnostics should expose bounds x");
    expect(diagnostics.find("surface_y=17") != std::string::npos, "diagnostics should expose bounds y");
    expect(diagnostics.find("width=800") != std::string::npos, "diagnostics should expose resized width");
    expect(diagnostics.find("height=600") != std::string::npos, "diagnostics should expose resized height");
    expect(diagnostics.find("scale=2") != std::string::npos, "diagnostics should expose resized scale");
    expect(
        diagnostics.find("render_thread_command_high_watermark=") != std::string::npos,
        "diagnostics should expose render-thread command queue pressure"
    );

    frameflow_engine_destroy(engine);
}

void initialize_with_presentation_remains_a_legacy_native_surface_wrapper() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();
    const auto presentation = legacy_native_surface_target(13u, 1024, 768, 1.25);

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed before legacy presentation initialize"
    );
    expect(
        frameflow_engine_initialize_with_presentation(engine, &presentation, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize_with_presentation should remain available as a compatibility wrapper"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "legacy presentation initialize should still emit one ready event"
    );
    expect(tracker.ready_count == 1u, "legacy presentation wrapper should still emit ready");
    const std::string diagnostics = frameflow_engine_diagnostics_summary(engine);
    expect(diagnostics.find("surface_backend=AUTO") != std::string::npos, "legacy wrapper should map to AUTO backend");
    expect(
        diagnostics.find("legacy_surface_handle=present") != std::string::npos,
        "legacy wrapper should expose native surface handle presence"
    );
    expect(
        diagnostics.find("legacy_surface_handle=13") == std::string::npos,
        "legacy wrapper should not expose the raw native surface handle"
    );

    frameflow_engine_destroy(engine);
}

void initialize_with_native_surface_supports_windows_hwnd_descriptor() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();
    const auto surface = windows_hwnd_child_surface_target(reinterpret_cast<void*>(0x44u), 640, 360, 1.0);

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed before Windows HWND initialize"
    );
    expect(
        frameflow_engine_initialize_with_native_surface(engine, &surface, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize_with_native_surface should accept Windows HWND child descriptors"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "Windows HWND initialize should still emit one ready event"
    );
    const std::string diagnostics = frameflow_engine_diagnostics_summary(engine);
    expect(
        diagnostics.find("surface_backend=WINDOWS_HWND_CHILD") != std::string::npos,
        "diagnostics should expose Windows HWND child surface backend"
    );
    expect(
        diagnostics.find("win32_parent_hwnd=present") != std::string::npos,
        "diagnostics should expose HWND presence"
    );
    expect(
        diagnostics.find("win32_parent_hwnd=0x44") == std::string::npos,
        "diagnostics should not expose the raw HWND pointer"
    );

    frameflow_engine_destroy(engine);
}

void selection_and_resize_preserve_engine_state_with_explicit_dispatch() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed"
    );
    expect(
        frameflow_engine_initialize(engine, 1u, 1280, 720, 1.5, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should succeed with valid arguments"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "ready event should be dispatched once"
    );

    const frameflow_point points[]{point(601, "Belgrade")};
    expect(
        frameflow_engine_set_points(engine, points, 1) == FRAMEFLOW_RESULT_OK,
        "set_points should accept a snapshot in READY state"
    );
    expect(frameflow_engine_point_count(engine) == 1, "point_count should reflect the snapshot");
    expect(
        frameflow_engine_focus_location(engine, 601) == FRAMEFLOW_RESULT_OK,
        "focus_location should accept a known location"
    );
    expect(
        frameflow_engine_has_selected_location_id(engine) == 1u,
        "selection presence should be exposed explicitly"
    );
    std::int64_t selected_location_id = 0;
    expect(
        frameflow_engine_get_selected_location_id(engine, &selected_location_id) ==
            FRAMEFLOW_RESULT_OK,
        "selected location getter should return the active location id"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "focus_location should queue one selection event"
    );
    expect(tracker.location_count == 1u, "location callback should be delivered once");
    expect(selected_location_id == 601, "selected location getter should match the focused point");
    expect(tracker.last_location.location_id == 601, "location callback should include selected location id");
    expect(
        tracker.last_location_interaction == "programmatic_focus",
        "selection interaction should describe host-triggered focus"
    );
    frameflow_screen_position projected_position{};
    expect(
        frameflow_engine_get_location_screen_position(engine, 601, &projected_position) ==
            FRAMEFLOW_RESULT_OK,
        "screen projection query should be non-fatal for known locations"
    );
    expect(
        projected_position.visible == 0u,
        "legacy surface projection query should report not visible without an offscreen projector"
    );
    expect(
        frameflow_engine_get_location_screen_position(engine, 0, &projected_position) ==
            FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "screen projection query should reject invalid location ids"
    );
    expect(
        frameflow_engine_get_location_screen_position(engine, 601, nullptr) ==
            FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "screen projection query should require an output struct"
    );

    frameflow_filter filter{};
    filter.query = "election";
    filter.category_code = "POLITICS";
    filter.location_id = 601;
    filter.has_location_id = 1u;
    filter.country_code = "RS";
    expect(
        frameflow_engine_set_filters(engine, &filter) == FRAMEFLOW_RESULT_OK,
        "set_filters should accept a filter snapshot"
    );

    expect(
        frameflow_engine_resize(engine, 1920, 1080, 2.0) == FRAMEFLOW_RESULT_OK,
        "resize should succeed in READY state"
    );
    expect(
        frameflow_engine_state(engine) == FRAMEFLOW_LIFECYCLE_READY,
        "resize should not recreate or leave READY state"
    );
    expect(frameflow_engine_point_count(engine) == 1, "resize should preserve point data");
    expect(
        frameflow_engine_has_selected_location_id(engine) == 1u,
        "resize should preserve selection presence"
    );
    selected_location_id = 0;
    expect(
        frameflow_engine_get_selected_location_id(engine, &selected_location_id) ==
            FRAMEFLOW_RESULT_OK,
        "selected location getter should still work after resize"
    );
    expect(selected_location_id == 601, "resize should preserve the selected location id");
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 0u,
        "resize and filter updates should not emit callbacks yet"
    );

    const char* diagnostics = frameflow_engine_diagnostics_summary(engine);
    expect(std::strstr(diagnostics, "width=1920") != nullptr, "diagnostics should include resized width");
    expect(std::strstr(diagnostics, "height=1080") != nullptr, "diagnostics should include resized height");
    expect(
        std::strstr(diagnostics, "filter_category=POLITICS") != nullptr,
        "diagnostics should include the active category filter"
    );
    expect(
        std::strstr(diagnostics, "filter_location_id=601") != nullptr,
        "diagnostics should include the active location filter"
    );
    expect(
        std::strstr(diagnostics, "first_style_key=category-politics") != nullptr,
        "diagnostics should expose copied style metadata from the point snapshot"
    );
    expect(
        std::strstr(diagnostics, "first_top_category=POLITICS") != nullptr,
        "diagnostics should expose copied top-category metadata from the point snapshot"
    );

    frameflow_engine_destroy(engine);
}

void native_selection_preserves_click_metadata() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed"
    );
    expect(
        frameflow_engine_initialize(engine, 3u, 1280, 720, 1.0, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should succeed"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "ready event should be dispatched once"
    );

    const frameflow_point points[]{point(601, "Belgrade")};
    expect(
        frameflow_engine_set_points(engine, points, 1) == FRAMEFLOW_RESULT_OK,
        "set_points should accept a ready snapshot"
    );
    expect(
        frameflow_engine_select_location(engine, 601, "POLITICS", 1200.0, 440.0, "click") ==
            FRAMEFLOW_RESULT_OK,
        "select_location should accept known points with click metadata"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "select_location should queue one selection event"
    );
    expect(tracker.location_count == 1u, "location callback should be delivered once");
    expect(tracker.last_location.location_id == 601, "selection callback should include the selected location id");
    expect(tracker.last_location_category_code == "POLITICS", "selection callback should preserve category metadata");
    expect(tracker.last_location.screen_x == 1200.0, "selection callback should preserve screen x");
    expect(tracker.last_location.screen_y == 440.0, "selection callback should preserve screen y");
    expect(tracker.last_location_interaction == "click", "selection callback should preserve interaction name");

    std::int64_t selected_location_id = 0;
    expect(
        frameflow_engine_get_selected_location_id(engine, &selected_location_id) ==
            FRAMEFLOW_RESULT_OK,
        "selected location getter should work after select_location"
    );
    expect(selected_location_id == 601, "selected location getter should reflect select_location");

    expect(
        frameflow_engine_select_location(engine, 404, nullptr, 10.0, 20.0, "click") ==
            FRAMEFLOW_RESULT_NOT_FOUND,
        "select_location should reject unknown points"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "unknown native selection should queue one error event"
    );
    expect(tracker.error_count == 1u, "error callback should observe unknown native selection");

    frameflow_engine_destroy(engine);
}

void native_cluster_selection_clears_location_and_dispatches_cluster_event() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed"
    );
    expect(
        frameflow_engine_initialize(engine, 7u, 1280, 720, 1.0, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should succeed"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "ready event should be dispatched once"
    );

    const frameflow_point points[]{point(601, "Belgrade"), point(602, "Novi Sad")};
    expect(
        frameflow_engine_set_points(engine, points, 2) == FRAMEFLOW_RESULT_OK,
        "set_points should accept a ready snapshot"
    );
    expect(
        frameflow_engine_focus_location(engine, 601) == FRAMEFLOW_RESULT_OK,
        "focus_location should succeed before cluster selection"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "focus_location should queue one location event"
    );
    expect(
        frameflow_engine_has_selected_location_id(engine) == 1u,
        "location selection should be active before cluster selection"
    );

    expect(
        frameflow_engine_select_cluster(engine, 7701, 2, 20.1412, 45.02685) ==
            FRAMEFLOW_RESULT_OK,
        "select_cluster should accept aggregated cluster metadata"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "select_cluster should queue one cluster event"
    );
    expect(tracker.cluster_count == 1u, "cluster callback should be delivered once");
    expect(tracker.last_cluster.cluster_id == 7701, "cluster callback should preserve cluster id");
    expect(tracker.last_cluster.point_count == 2, "cluster callback should preserve point count");
    expect(tracker.last_cluster.longitude == 20.1412, "cluster callback should preserve longitude");
    expect(tracker.last_cluster.latitude == 45.02685, "cluster callback should preserve latitude");
    expect(
        frameflow_engine_has_selected_location_id(engine) == 0u,
        "cluster selection should clear the active location selection"
    );

    std::int64_t selected_location_id = 0;
    expect(
        frameflow_engine_get_selected_location_id(engine, &selected_location_id) ==
            FRAMEFLOW_RESULT_NOT_FOUND,
        "cluster selection should leave no location selected"
    );

    expect(
        frameflow_engine_select_cluster(engine, 0, 1, 20.0, 44.0) ==
            FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "select_cluster should reject invalid aggregated metadata"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "invalid cluster selection should queue one error event"
    );
    expect(tracker.error_count == 1u, "error callback should observe invalid cluster selection");
    expect(
        frameflow_engine_select_cluster(engine, 7702, 2, 20.0, 120.0) ==
            FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "select_cluster should reject latitude values outside the valid geographic range"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "invalid cluster latitude should queue one error event"
    );
    expect(tracker.error_count == 2u, "error callback should observe invalid cluster latitude");
    expect(
        frameflow_engine_select_cluster(engine, 7703, 2, std::numeric_limits<double>::quiet_NaN(), 44.0) ==
            FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "select_cluster should reject non-finite coordinates"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "non-finite cluster coordinates should queue one error event"
    );
    expect(tracker.error_count == 3u, "error callback should observe non-finite cluster coordinates");

    frameflow_engine_destroy(engine);
}

void runtime_camera_state_emits_distinct_callbacks_and_updates_diagnostics() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed"
    );
    expect(
        frameflow_engine_initialize(engine, 11u, 1280, 720, 1.0, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should succeed"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "ready event should be dispatched once"
    );

    expect(
        frameflow_engine_report_camera_changed(engine, 20.4489, 44.7866, 2500000.0, 0.0, -45.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "report_camera_changed should accept a finite runtime camera snapshot"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "first camera snapshot should queue one callback"
    );
    expect(tracker.camera_count == 1u, "camera callback should be delivered once");
    expect(tracker.last_camera.longitude == 20.4489, "camera callback should preserve longitude");
    expect(tracker.last_camera.latitude == 44.7866, "camera callback should preserve latitude");
    expect(tracker.last_camera.height_meters == 2500000.0, "camera callback should preserve height");
    expect(tracker.last_camera.pitch_degrees == -45.0, "camera callback should preserve pitch");

    expect(
        frameflow_engine_report_camera_changed(engine, 20.4489, 44.7866, 2500000.0, 0.0, -45.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "report_camera_changed should accept duplicate runtime snapshots"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 0u,
        "duplicate camera snapshot should not queue another callback"
    );

    expect(
        frameflow_engine_report_camera_changed(engine, 20.8, 44.9, 2200000.0, 5.0, -40.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "report_camera_changed should accept the first changed camera state in a pending burst"
    );
    expect(
        frameflow_engine_report_camera_changed(engine, 20.9, 44.95, 2000000.0, 10.0, -35.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "report_camera_changed should accept another changed camera state before dispatch"
    );
    expect(
        frameflow_engine_report_camera_changed(engine, 21.0, 45.0, 1800000.0, 15.0, -30.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "report_camera_changed should accept the final changed camera state before dispatch"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "coalesced camera burst should dispatch only the final camera snapshot"
    );
    expect(tracker.camera_count == 2u, "camera callback should be delivered for changed state");
    expect(tracker.last_camera.longitude == 21.0, "camera callback should preserve the final coalesced longitude");
    expect(tracker.last_camera.heading_degrees == 15.0, "camera callback should preserve the final coalesced heading");
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "camera_longitude_degrees=21") != nullptr,
        "diagnostics should include the latest runtime camera longitude"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "coalesced_camera_events=2") != nullptr,
        "diagnostics should report coalesced camera event count"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "pending_events_high_watermark=1") != nullptr,
        "diagnostics should report the bounded pending event high-water mark"
    );

    expect(
        frameflow_engine_report_camera_changed(engine, 10.0, 120.0, 1000.0, 0.0, 0.0, 0.0) ==
            FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "report_camera_changed should reject invalid latitude values"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "invalid camera state should queue one error callback"
    );
    expect(tracker.error_count == 1u, "error callback should observe invalid camera state");

    frameflow_engine_destroy(engine);
}

void invalid_usage_dispatches_error_events_and_dispose_clears_callbacks() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();
    const frameflow_point points[]{point(10, "Novi Sad")};

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed"
    );
    expect(
        frameflow_engine_set_points(engine, points, 1) == FRAMEFLOW_RESULT_INVALID_STATE,
        "set_points should fail before initialize"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "pre-initialize state failure should queue one error event"
    );
    expect(tracker.error_count == 1u, "error callback should observe the pre-initialize failure");
    expect(tracker.last_error.code == FRAMEFLOW_RESULT_INVALID_STATE, "error callback should include result code");
    expect(tracker.last_error.recoverable == 1u, "pre-initialize failure should be recoverable");

    expect(
        frameflow_engine_initialize(engine, 0u, 640, 480, 1.0, &options) ==
            FRAMEFLOW_RESULT_INVALID_ARGUMENT,
        "initialize should reject a missing surface handle"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "invalid initialize should queue one error event"
    );
    expect(tracker.error_count == 2u, "error callback should observe initialize argument failure");
    expect(tracker.last_error.code == FRAMEFLOW_RESULT_INVALID_ARGUMENT, "invalid initialize should map to INVALID_ARGUMENT");
    expect(tracker.last_error.recoverable == 1u, "invalid initialize should be recoverable");
    expect(tracker.last_error.bridge_abi_version == 2, "error callback should include ABI version");
    expect(tracker.last_error.engine_version_major == 0, "error callback should include engine major version");
    expect(tracker.last_error.command_version_major == 2, "error callback should include command major version");
    expect(tracker.last_error.command_version_minor == 8, "error callback should include command minor version");
    expect(
        tracker.last_error_message.find("surface handle") != std::string::npos,
        "error message should mention the missing surface handle"
    );

    expect(
        frameflow_engine_initialize(engine, 5u, 640, 480, 1.0, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should succeed with a valid surface handle"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "valid initialize should still emit ready"
    );
    expect(
        frameflow_engine_set_points(engine, points, 1) == FRAMEFLOW_RESULT_OK,
        "set_points should succeed after initialize"
    );
    expect(
        frameflow_engine_focus_location(engine, 10) == FRAMEFLOW_RESULT_OK,
        "focus_location should succeed for a known location before dispose"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "valid focus should dispatch one selection event"
    );
    expect(
        frameflow_engine_focus_location(engine, 404) == FRAMEFLOW_RESULT_NOT_FOUND,
        "focus_location should return NOT_FOUND for unknown locations"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "unknown focus should queue one error event"
    );
    expect(tracker.error_count == 3u, "error callback should observe lookup failures");
    expect(tracker.last_error.code == FRAMEFLOW_RESULT_NOT_FOUND, "lookup failure should map to NOT_FOUND");
    expect(tracker.last_error.recoverable == 1u, "lookup failure should be recoverable");

    expect(frameflow_engine_pause(engine) == FRAMEFLOW_RESULT_OK, "pause should succeed");
    expect(
        frameflow_engine_dispose(engine) == FRAMEFLOW_RESULT_OK,
        "dispose should succeed"
    );
    expect(
        frameflow_engine_dispose(engine) == FRAMEFLOW_RESULT_OK,
        "dispose should be idempotent"
    );
    expect(
        frameflow_engine_state(engine) == FRAMEFLOW_LIFECYCLE_DISPOSED,
        "dispose should leave the engine in DISPOSED"
    );
    expect(frameflow_engine_point_count(engine) == 0u, "dispose should clear point data");
    expect(
        frameflow_engine_has_selected_location_id(engine) == 0u,
        "dispose should clear selected location state"
    );
    std::int64_t disposed_selected_location_id = 0;
    expect(
        frameflow_engine_get_selected_location_id(engine, &disposed_selected_location_id) ==
            FRAMEFLOW_RESULT_NOT_FOUND,
        "dispose should leave no selected location to read back"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "points=0") != nullptr,
        "dispose diagnostics should not report stale point state"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "selected_location_id=none") != nullptr,
        "dispose diagnostics should not report stale selection state"
    );

    frameflow_filter filter{};
    filter.category_code = "CRIME";
    expect(
        frameflow_engine_set_filters(engine, &filter) == FRAMEFLOW_RESULT_INVALID_STATE,
        "post-dispose commands should fail with INVALID_STATE"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 0u,
        "dispose should clear callbacks and pending events"
    );
    expect(tracker.error_count == 3u, "no new callbacks should fire after dispose");
    expect(
        frameflow_engine_last_error_code(engine) == FRAMEFLOW_RESULT_INVALID_STATE,
        "last_error_code should still reflect the post-dispose command failure"
    );
    expect(
        frameflow_engine_last_error_recoverable(engine) == 0u,
        "post-dispose failures should not be recoverable on the same handle"
    );

    frameflow_engine_destroy(engine);
}

void destroy_without_dispose_cleans_active_engine_state() {
    frameflow_engine_destroy(nullptr);

    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();
    const auto surface = offscreen_bitmap_surface_target(640, 360, 1.0);
    const frameflow_point points[]{point(44, "Belgrade")};

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed before destroy-without-dispose scenario"
    );
    expect(
        frameflow_engine_initialize_with_native_surface(engine, &surface, &options) ==
            FRAMEFLOW_RESULT_OK,
        "offscreen initialize should succeed before destroy-without-dispose scenario"
    );
    expect(
        frameflow_engine_set_points(engine, points, 1) == FRAMEFLOW_RESULT_OK,
        "set_points should succeed before destroy-without-dispose scenario"
    );
    expect(
        frameflow_engine_focus_location(engine, 44) == FRAMEFLOW_RESULT_OK,
        "focus_location should queue a pending callback before destroy"
    );
    expect(tracker.ready_count == 0u, "callbacks should remain pending before explicit dispatch");
    expect(tracker.location_count == 0u, "selection callback should remain pending before explicit dispatch");

    frameflow_engine_destroy(engine);

    expect(tracker.ready_count == 0u, "destroy should not dispatch pending ready callbacks");
    expect(tracker.location_count == 0u, "destroy should not dispatch pending selection callbacks");
    expect(tracker.error_count == 0u, "destroy should not dispatch error callbacks");
}

void offscreen_bitmap_surface_backend_produces_copyable_frame_contract() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();
    const auto surface = offscreen_bitmap_surface_target(900, 600, 2.0);

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed before offscreen initialize"
    );
    expect(
        frameflow_engine_initialize_with_native_surface(engine, &surface, &options) ==
            FRAMEFLOW_RESULT_OK,
        "offscreen surface backend should initialize successfully for bitmap readback"
    );
    expect(
        frameflow_engine_state(engine) == FRAMEFLOW_LIFECYCLE_READY,
        "offscreen initialize should transition the engine to READY"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "offscreen initialize should queue one ready event"
    );
    expect(tracker.ready_count == 1u, "offscreen initialize should emit one ready callback");
    expect(frameflow_engine_has_latest_frame(engine) == 1u, "offscreen initialize should expose a latest frame");

    frameflow_frame_info frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &frame_info) == FRAMEFLOW_RESULT_OK,
        "offscreen frame info should be available after initialize"
    );
    expect(frame_info.width == 900, "frame info should expose offscreen width");
    expect(frame_info.height == 600, "frame info should expose offscreen height");
    expect(frame_info.pixel_format == FRAMEFLOW_FRAME_PIXEL_FORMAT_RGBA8, "frame info should expose RGBA8 format");
    expect(frame_info.generation > 0u, "frame info should expose a non-zero generation");
    expect(
        frameflow_engine_get_frame_generation(engine) == frame_info.generation,
        "cheap frame generation getter should match latest frame info"
    );
    const std::uint64_t initial_render_callback_count = frameflow_engine_get_render_callback_count(engine);
    expect(
        initial_render_callback_count == frame_info.generation,
        "render callback count should advance with produced offscreen frames"
    );
    const std::uint64_t initial_diagnostics_callback_count =
        frameflow_engine_get_diagnostics_callback_count(engine);
    expect(
        initial_diagnostics_callback_count > 0u,
        "diagnostics callback count should reflect prior diagnostics refreshes"
    );

    std::vector<std::uint8_t> frame_pixels(frame_info.stride_bytes * static_cast<std::uint64_t>(frame_info.height), 0u);
    expect(
        frameflow_engine_copy_latest_frame(engine, frame_pixels.data(), frame_pixels.size()) ==
            FRAMEFLOW_RESULT_OK,
        "offscreen frame pixels should be copyable into a caller-owned buffer"
    );
    expect(
        std::any_of(frame_pixels.begin(), frame_pixels.end(), [](const std::uint8_t value) { return value != 0u; }),
        "offscreen frame buffer should contain rendered pixel data"
    );
    frameflow_frame_info copied_frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &copied_frame_info) == FRAMEFLOW_RESULT_OK,
        "copying the cached offscreen frame should leave frame info available"
    );
    expect(
        copied_frame_info.generation == frame_info.generation,
        "copy_latest_frame should not render or advance frame generation"
    );
    expect(
        frameflow_engine_get_render_callback_count(engine) == initial_render_callback_count,
        "copy_latest_frame should not advance the render callback count"
    );
    expect(
        frameflow_engine_render_latest_frame(engine) == FRAMEFLOW_RESULT_OK,
        "explicit offscreen render should refresh the cached frame"
    );
    frameflow_frame_info explicitly_rendered_frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &explicitly_rendered_frame_info) ==
            FRAMEFLOW_RESULT_OK,
        "frame info should be available after an explicit offscreen render"
    );
    expect(
        explicitly_rendered_frame_info.generation > copied_frame_info.generation,
        "explicit offscreen render should advance frame generation"
    );
    expect(
        frameflow_engine_get_frame_generation(engine) == explicitly_rendered_frame_info.generation,
        "cheap frame generation getter should track explicit renders"
    );
    expect(
        frameflow_engine_get_render_callback_count(engine) > initial_render_callback_count,
        "explicit render should advance the render callback count"
    );
    const std::uint64_t diagnostics_callback_count_before_summary =
        frameflow_engine_get_diagnostics_callback_count(engine);
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "diagnostics_callback_count=") != nullptr,
        "diagnostics summary should expose the diagnostics callback count"
    );
    expect(
        frameflow_engine_get_diagnostics_callback_count(engine) == diagnostics_callback_count_before_summary,
        "diagnostics_summary getter should not advance diagnostics callback count"
    );
    const auto background_pixel = rgba_at(frame_pixels, frame_info, 8, 8);
    expect(
        background_pixel[3] == 255u,
        "offscreen zero-point frame should contain an opaque clear background when imagery is unavailable"
    );
    expect(
        frameflow_engine_report_camera_changed(engine, 15.0, 20.0, 18'000'000.0, 0.0, -35.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "offscreen camera should accept a wide zero-point view"
    );
    frameflow_frame_info wide_zero_point_frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &wide_zero_point_frame_info) == FRAMEFLOW_RESULT_OK,
        "frame info should remain available after a wide zero-point view"
    );
    std::vector<std::uint8_t> wide_zero_point_frame_pixels(
        wide_zero_point_frame_info.stride_bytes * static_cast<std::uint64_t>(wide_zero_point_frame_info.height),
        0u
    );
    expect(
        frameflow_engine_copy_latest_frame(
            engine,
            wide_zero_point_frame_pixels.data(),
            wide_zero_point_frame_pixels.size()
        ) == FRAMEFLOW_RESULT_OK,
        "wide zero-point frame should be copyable"
    );
    expect(
        frameflow_engine_report_camera_changed(engine, 15.0, 20.0, 7'500'000.0, 0.0, -35.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "offscreen camera should accept a tighter zero-point view"
    );
    frameflow_frame_info tight_zero_point_frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &tight_zero_point_frame_info) == FRAMEFLOW_RESULT_OK,
        "frame info should remain available after a tighter zero-point view"
    );
    std::vector<std::uint8_t> tight_zero_point_frame_pixels(
        tight_zero_point_frame_info.stride_bytes * static_cast<std::uint64_t>(tight_zero_point_frame_info.height),
        0u
    );
    expect(
        frameflow_engine_copy_latest_frame(
            engine,
            tight_zero_point_frame_pixels.data(),
            tight_zero_point_frame_pixels.size()
        ) == FRAMEFLOW_RESULT_OK,
        "tight zero-point frame should be copyable"
    );
    const auto wide_zero_point_background_pixel = rgba_at(
        wide_zero_point_frame_pixels,
        wide_zero_point_frame_info,
        8,
        8
    );
    const auto tight_zero_point_background_pixel = rgba_at(
        tight_zero_point_frame_pixels,
        tight_zero_point_frame_info,
        8,
        8
    );
    expect(
        wide_zero_point_background_pixel[3] == 255u && tight_zero_point_background_pixel[3] == 255u,
        "offscreen zero-point frames should remain opaque when imagery is unavailable"
    );
    expect(
        frameflow_engine_report_camera_changed(engine, 20.4489, 44.7866, 7'500'000.0, 0.0, -35.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "offscreen camera should accept a Europe-centered land view"
    );
    frameflow_frame_info europe_frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &europe_frame_info) == FRAMEFLOW_RESULT_OK,
        "frame info should remain available after Europe land view"
    );
    std::vector<std::uint8_t> europe_frame_pixels(
        europe_frame_info.stride_bytes * static_cast<std::uint64_t>(europe_frame_info.height),
        0u
    );
    expect(
        frameflow_engine_copy_latest_frame(engine, europe_frame_pixels.data(), europe_frame_pixels.size()) ==
            FRAMEFLOW_RESULT_OK,
        "Europe-centered offscreen frame should be copyable"
    );
    const auto europe_center_pixel = rgba_at(
        europe_frame_pixels,
        europe_frame_info,
        europe_frame_info.width / 2,
        europe_frame_info.height / 2
    );
    expect(
        frameflow_engine_report_camera_changed(engine, -30.0, 0.0, 7'500'000.0, 0.0, -35.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "offscreen camera should accept an Atlantic-centered ocean view"
    );
    frameflow_frame_info atlantic_frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &atlantic_frame_info) == FRAMEFLOW_RESULT_OK,
        "frame info should remain available after Atlantic ocean view"
    );
    std::vector<std::uint8_t> atlantic_frame_pixels(
        atlantic_frame_info.stride_bytes * static_cast<std::uint64_t>(atlantic_frame_info.height),
        0u
    );
    expect(
        frameflow_engine_copy_latest_frame(engine, atlantic_frame_pixels.data(), atlantic_frame_pixels.size()) ==
            FRAMEFLOW_RESULT_OK,
        "Atlantic-centered offscreen frame should be copyable"
    );
    const std::string render_diagnostics = frameflow_engine_diagnostics_summary(engine);
    if (render_diagnostics.find("render_backend=cesium-glx-offscreen") != std::string::npos) {
        expect(
            render_diagnostics.find("camera_longitude_degrees=-30") != std::string::npos,
            "Cesium offscreen backend diagnostics should reflect the Atlantic camera snapshot"
        );
    } else {
        expect(
            europe_center_pixel[1] > europe_center_pixel[2],
            "Europe-centered globe should render land-toned pixels at the viewport center"
        );
        const auto atlantic_center_pixel = rgba_at(
            atlantic_frame_pixels,
            atlantic_frame_info,
            atlantic_frame_info.width / 2,
            atlantic_frame_info.height / 2
        );
        expect(
            atlantic_center_pixel[2] > atlantic_center_pixel[1],
            "Atlantic-centered globe should render ocean-toned pixels at the viewport center"
        );
    }

    const frameflow_point points[]{point(901, "Belgrade"), point(902, "Novi Sad")};
    expect(frameflow_engine_set_points(engine, points, 2) == FRAMEFLOW_RESULT_OK, "set_points should work on offscreen target");
    frameflow_frame_info updated_frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &updated_frame_info) == FRAMEFLOW_RESULT_OK,
        "frame info should still be available after point updates"
    );
    expect(updated_frame_info.generation > frame_info.generation, "frame generation should advance after content changes");

    std::vector<std::uint8_t> frame_with_points(
        updated_frame_info.stride_bytes * static_cast<std::uint64_t>(updated_frame_info.height),
        0u
    );
    expect(
        frameflow_engine_copy_latest_frame(engine, frame_with_points.data(), frame_with_points.size()) ==
            FRAMEFLOW_RESULT_OK,
        "updated offscreen frame should remain copyable after point updates"
    );
    expect(
        frameflow_engine_report_camera_changed(engine, 15.0, 20.0, 7'500'000.0, 0.0, -35.0, 0.0) ==
            FRAMEFLOW_RESULT_OK,
        "report_camera_changed should accept a zoomed-in offscreen camera snapshot"
    );
    frameflow_frame_info zoomed_frame_info{};
    expect(
        frameflow_engine_get_latest_frame_info(engine, &zoomed_frame_info) == FRAMEFLOW_RESULT_OK,
        "frame info should still be available after zooming the offscreen camera"
    );
    expect(
        zoomed_frame_info.generation > updated_frame_info.generation,
        "frame generation should advance after offscreen zoom changes"
    );
    std::vector<std::uint8_t> zoomed_frame_pixels(
        zoomed_frame_info.stride_bytes * static_cast<std::uint64_t>(zoomed_frame_info.height),
        0u
    );
    expect(
        frameflow_engine_copy_latest_frame(engine, zoomed_frame_pixels.data(), zoomed_frame_pixels.size()) ==
            FRAMEFLOW_RESULT_OK,
        "zoomed offscreen frame should remain copyable"
    );
    expect(
        !zoomed_frame_pixels.empty(),
        "offscreen zoom changes should keep the latest frame copyable when imagery is unavailable"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "surface_backend=OFFSCREEN_BITMAP") != nullptr,
        "diagnostics should expose the offscreen surface backend"
    );
    expect(
        std::strstr(frameflow_engine_diagnostics_summary(engine), "has_latest_frame=true") != nullptr,
        "diagnostics should report latest frame availability"
    );

    frameflow_engine_destroy(engine);
}

void point_input_validation_and_selection_getters_are_explicit() {
    auto* engine = frameflow_engine_create();
    expect(engine != nullptr, "engine create should return a handle");

    CallbackTracker tracker{};
    const auto callbacks = make_callbacks(&tracker);
    const auto options = default_options();

    expect(
        frameflow_engine_set_callbacks(engine, &callbacks) == FRAMEFLOW_RESULT_OK,
        "set_callbacks should succeed"
    );
    expect(
        frameflow_engine_initialize(engine, 9u, 640, 480, 1.0, &options) ==
            FRAMEFLOW_RESULT_OK,
        "initialize should succeed"
    );
    expect(
        frameflow_engine_dispatch_pending_callbacks(engine) == 1u,
        "ready event should be dispatched once"
    );

    auto expect_invalid_point = [&](frameflow_point invalid_point, const char* message_fragment) {
        expect(
            frameflow_engine_set_points(engine, &invalid_point, 1) ==
                FRAMEFLOW_RESULT_INVALID_ARGUMENT,
            "set_points should reject invalid point metadata"
        );
        expect(
            frameflow_engine_last_error_code(engine) == FRAMEFLOW_RESULT_INVALID_ARGUMENT,
            "invalid point metadata should set INVALID_ARGUMENT"
        );
        expect(
            std::strstr(frameflow_engine_last_error_message(engine), message_fragment) != nullptr,
            "invalid point metadata should explain the rejected field"
        );
    };

    frameflow_point invalid_point = point(777, "Invalid");
    invalid_point.top_categories = nullptr;
    invalid_point.top_category_count = 2u;
    expect_invalid_point(invalid_point, "top_category_count but no top_categories");

    invalid_point = point(0, "Invalid");
    expect_invalid_point(invalid_point, "positive location_id");

    invalid_point = point(777, "Invalid");
    invalid_point.latitude = 91.0;
    expect_invalid_point(invalid_point, "latitude in [-90, 90]");

    invalid_point = point(777, "Invalid");
    invalid_point.longitude = std::numeric_limits<double>::infinity();
    expect_invalid_point(invalid_point, "finite longitude/latitude");

    invalid_point = point(777, "Invalid");
    invalid_point.story_count = -1;
    expect_invalid_point(invalid_point, "non-negative story_count");

    invalid_point = point(777, "Invalid");
    invalid_point.latest_story_epoch_millis = -1;
    expect_invalid_point(invalid_point, "non-negative latest_story_epoch_millis");

    static const char* kTooManyCategories[] = {
        "POLITICS",
        "WORLD",
        "ECONOMY",
        "CRIME",
        "SPORT",
        "TECH",
        "CULTURE",
        "WEATHER",
        "SCIENCE",
    };
    invalid_point = point(777, "Invalid");
    invalid_point.top_categories = kTooManyCategories;
    invalid_point.top_category_count = 9u;
    expect_invalid_point(invalid_point, "maximum top_category_count");

    static const char* kInvalidCategories[] = {"POLITICS", nullptr};
    invalid_point = point(777, "Invalid");
    invalid_point.top_categories = kInvalidCategories;
    invalid_point.top_category_count = 2u;
    expect_invalid_point(invalid_point, "must not be null or empty");

    frameflow_point valid_edge_point = point(778, "Wrapped longitude");
    valid_edge_point.longitude = 540.0;
    valid_edge_point.story_count = 0;
    expect(
        frameflow_engine_set_points(engine, &valid_edge_point, 1) == FRAMEFLOW_RESULT_OK,
        "set_points should accept zero story_count and finite wrapped longitude"
    );

    expect(
        frameflow_engine_has_selected_location_id(engine) == 0u,
        "no selection should be reported without sentinel semantics"
    );
    std::int64_t selected_location_id = 0;
    expect(
        frameflow_engine_get_selected_location_id(engine, &selected_location_id) ==
            FRAMEFLOW_RESULT_NOT_FOUND,
        "selected location getter should return NOT_FOUND when no selection exists"
    );

    frameflow_engine_destroy(engine);
}

} // namespace

int main() {
    lifecycle_starts_uninitialized_with_version_surface();
    runtime_info_and_compatibility_helpers_fail_fast();
    initialize_requires_callback_sink_and_dispatches_ready_event();
    callback_exceptions_are_captured_at_c_api_boundary();
    callback_dispatch_uses_copied_batch_and_callback_snapshot();
    initialize_with_native_surface_supports_linux_x11_descriptor_and_lifecycle_helpers();
    initialize_with_presentation_remains_a_legacy_native_surface_wrapper();
    initialize_with_native_surface_supports_windows_hwnd_descriptor();
    selection_and_resize_preserve_engine_state_with_explicit_dispatch();
    native_selection_preserves_click_metadata();
    native_cluster_selection_clears_location_and_dispatches_cluster_event();
    runtime_camera_state_emits_distinct_callbacks_and_updates_diagnostics();
    invalid_usage_dispatches_error_events_and_dispose_clears_callbacks();
    destroy_without_dispose_cleans_active_engine_state();
    offscreen_bitmap_surface_backend_produces_copyable_frame_contract();
    point_input_validation_and_selection_getters_are_explicit();
    return 0;
}
