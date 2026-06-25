#pragma once

#include <stdint.h>

#if defined(_WIN32)
#  if defined(FRAMEFLOW_BUILDING_DLL)
#    define FRAMEFLOW_API __declspec(dllexport)
#  else
#    define FRAMEFLOW_API __declspec(dllimport)
#  endif
#else
#  define FRAMEFLOW_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct frameflow_engine frameflow_engine;

typedef enum frameflow_result {
    FRAMEFLOW_RESULT_OK = 0,
    FRAMEFLOW_RESULT_INVALID_ARGUMENT = 1,
    FRAMEFLOW_RESULT_INVALID_STATE = 2,
    FRAMEFLOW_RESULT_NOT_FOUND = 3,
    FRAMEFLOW_RESULT_NOT_SUPPORTED = 4,
    FRAMEFLOW_RESULT_INTERNAL_ERROR = 5
} frameflow_result;

typedef enum frameflow_lifecycle_state {
    FRAMEFLOW_LIFECYCLE_UNINITIALIZED = 0,
    FRAMEFLOW_LIFECYCLE_INITIALIZING = 1,
    FRAMEFLOW_LIFECYCLE_READY = 2,
    FRAMEFLOW_LIFECYCLE_PAUSED = 3,
    FRAMEFLOW_LIFECYCLE_FAILED = 4,
    FRAMEFLOW_LIFECYCLE_DISPOSED = 5
} frameflow_lifecycle_state;

typedef enum frameflow_location_kind {
    FRAMEFLOW_LOCATION_KIND_UNKNOWN = 0,
    FRAMEFLOW_LOCATION_KIND_POINT = 1,
    FRAMEFLOW_LOCATION_KIND_CITY = 2,
    FRAMEFLOW_LOCATION_KIND_REGION = 3,
    FRAMEFLOW_LOCATION_KIND_COUNTRY = 4
} frameflow_location_kind;

/*
 * Caller-owned point snapshot input.
 *
 * All string pointers and top_categories entries must stay valid until
 * frameflow_engine_set_points(...) returns. The bridge copies them before
 * returning.
 *
 * Validation rules:
 * - location_id must be positive
 * - latitude and longitude must be finite
 * - latitude must be in [-90, 90]
 * - story_count and latest_story_epoch_millis must be non-negative
 * - top_category_count must not exceed 8
 * - top_categories must be non-null when top_category_count is greater than 0
 * - every provided top_categories entry must be non-null and non-empty
 *
 * Longitude is intentionally not range-limited; callers may provide wrapped
 * finite longitudes and let the renderer normalize projection where needed.
 */
typedef struct frameflow_point {
    int64_t location_id;
    const char* label;
    frameflow_location_kind kind;
    const char* country_code;
    double latitude;
    double longitude;
    int32_t story_count;
    int64_t latest_story_epoch_millis;
    const char* const* top_categories;
    uint64_t top_category_count;
    const char* style_key;
} frameflow_point;

typedef struct frameflow_filter {
    const char* query;
    const char* category_code;
    int64_t location_id;
    uint8_t has_location_id;
    const char* country_code;
    int64_t from_epoch_millis;
    uint8_t has_from_epoch_millis;
    int64_t to_epoch_millis;
    uint8_t has_to_epoch_millis;
} frameflow_filter;

typedef struct frameflow_options {
    const char* theme;
    const char* tile_cache_path;
    uint64_t max_tile_cache_bytes;
    const char* log_level;
} frameflow_options;

typedef enum frameflow_surface_backend {
    FRAMEFLOW_SURFACE_BACKEND_AUTO = 0,
    FRAMEFLOW_SURFACE_BACKEND_OFFSCREEN_BITMAP = 1,
    FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD = 2,
    FRAMEFLOW_SURFACE_BACKEND_WINDOWS_HWND_CHILD = 3
} frameflow_surface_backend;

typedef struct frameflow_surface_bounds {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    double scale_factor;
} frameflow_surface_bounds;

typedef struct frameflow_x11_surface_desc {
    void* display;
    uint64_t parent_window;
} frameflow_x11_surface_desc;

typedef struct frameflow_win32_surface_desc {
    void* parent_hwnd;
} frameflow_win32_surface_desc;

typedef union frameflow_native_surface_platform_desc {
    frameflow_x11_surface_desc x11;
    frameflow_win32_surface_desc win32;
} frameflow_native_surface_platform_desc;

/*
 * Platform-neutral surface descriptor used by the production initialization
 * contract.
 *
 * `OFFSCREEN_BITMAP` uses only `bounds`.
 * `LINUX_X11_CHILD` requires a non-null `platform.x11.display` and non-zero
 * `platform.x11.parent_window`.
 * `WINDOWS_HWND_CHILD` requires a non-null `platform.win32.parent_hwnd`.
 * `AUTO` is reserved for compatibility wrappers and future host-selected
 * backends.
 */
typedef struct frameflow_native_surface_desc {
    frameflow_surface_backend backend;
    frameflow_surface_bounds bounds;
    frameflow_native_surface_platform_desc platform;
} frameflow_native_surface_desc;

typedef enum frameflow_presentation_kind {
    FRAMEFLOW_PRESENTATION_KIND_NONE = 0,
    FRAMEFLOW_PRESENTATION_KIND_NATIVE_SURFACE = 1,
    FRAMEFLOW_PRESENTATION_KIND_OFFSCREEN_BITMAP = 2
} frameflow_presentation_kind;

/*
 * Legacy compatibility initialization contract.
 *
 * New hosts should use `frameflow_native_surface_desc` through
 * `frameflow_engine_initialize_with_native_surface(...)`.
 * This wrapper remains temporarily so older raw-handle integrations can keep
 * working while the platform-neutral descriptor rolls out.
 */
typedef struct frameflow_presentation_target {
    frameflow_presentation_kind kind;
    uintptr_t surface_handle;
    int32_t width;
    int32_t height;
    double scale_factor;
} frameflow_presentation_target;

typedef enum frameflow_frame_pixel_format {
    FRAMEFLOW_FRAME_PIXEL_FORMAT_NONE = 0,
    FRAMEFLOW_FRAME_PIXEL_FORMAT_RGBA8 = 1
} frameflow_frame_pixel_format;

typedef struct frameflow_frame_info {
    int32_t width;
    int32_t height;
    uint32_t stride_bytes;
    uint64_t generation;
    frameflow_frame_pixel_format pixel_format;
} frameflow_frame_info;

typedef struct frameflow_runtime_info {
    int32_t bridge_abi_version;
    int32_t engine_version_major;
    int32_t engine_version_minor;
    int32_t engine_version_patch;
    int32_t command_version_major;
    int32_t command_version_minor;
} frameflow_runtime_info;

typedef struct frameflow_ready_event {
    int32_t width;
    int32_t height;
    double scale_factor;
    int32_t bridge_abi_version;
    int32_t engine_version_major;
    int32_t engine_version_minor;
    int32_t engine_version_patch;
    int32_t command_version_major;
    int32_t command_version_minor;
} frameflow_ready_event;

typedef struct frameflow_location_selection_event {
    int64_t location_id;
    const char* category_code;
    double screen_x;
    double screen_y;
    const char* interaction;
} frameflow_location_selection_event;

typedef struct frameflow_cluster_selection_event {
    int64_t cluster_id;
    int32_t point_count;
    double longitude;
    double latitude;
} frameflow_cluster_selection_event;

typedef struct frameflow_camera_state {
    double longitude;
    double latitude;
    double height_meters;
    double heading_degrees;
    double pitch_degrees;
    double roll_degrees;
} frameflow_camera_state;

typedef struct frameflow_screen_position {
    double x;
    double y;
    uint8_t visible;
} frameflow_screen_position;

typedef struct frameflow_error_event {
    frameflow_result code;
    const char* message;
    uint8_t recoverable;
    int32_t bridge_abi_version;
    int32_t engine_version_major;
    int32_t engine_version_minor;
    int32_t engine_version_patch;
    int32_t command_version_major;
    int32_t command_version_minor;
} frameflow_error_event;

/*
 * Callback payload string pointers are valid only during the callback
 * invocation. Hosts must copy any string values they need to retain after the
 * callback returns.
 */
typedef void (*frameflow_ready_callback)(
    void* user,
    const frameflow_ready_event* event
);

typedef void (*frameflow_location_selection_callback)(
    void* user,
    const frameflow_location_selection_event* event
);

typedef void (*frameflow_cluster_selection_callback)(
    void* user,
    const frameflow_cluster_selection_event* event
);

typedef void (*frameflow_camera_changed_callback)(
    void* user,
    const frameflow_camera_state* event
);

typedef void (*frameflow_error_callback)(
    void* user,
    const frameflow_error_event* event
);

typedef struct frameflow_callbacks {
    void* user;
    frameflow_ready_callback on_ready;
    frameflow_location_selection_callback on_location_selected;
    frameflow_cluster_selection_callback on_cluster_selected;
    frameflow_camera_changed_callback on_camera_changed;
    frameflow_error_callback on_engine_error;
} frameflow_callbacks;

/*
 * Threading contract:
 *
 * - Public engine operations, except final handle destruction, are serialized
 *   per engine handle.
 * - The host owns handle lifetime; `frameflow_engine_destroy(...)` must not
 *   race with any other API call for the same handle.
 * - `frameflow_engine_dispatch_pending_callbacks(...)` copies the pending
 *   event batch and callback table, releases the engine lock, then invokes host
 *   callbacks.
 * - Callback payload pointers are valid only during the callback invocation.
 * - Host callbacks may call non-destroy engine APIs, but must not call
 *   `frameflow_engine_destroy(...)` for the same handle from inside a
 *   callback.
 */

FRAMEFLOW_API int frameflow_bridge_abi_version(void);
FRAMEFLOW_API int frameflow_engine_version_major(void);
FRAMEFLOW_API int frameflow_engine_version_minor(void);
FRAMEFLOW_API int frameflow_engine_version_patch(void);
FRAMEFLOW_API int frameflow_command_version_major(void);
FRAMEFLOW_API int frameflow_command_version_minor(void);
FRAMEFLOW_API frameflow_result frameflow_bridge_get_runtime_info(
    frameflow_runtime_info* out_info
);
FRAMEFLOW_API frameflow_result frameflow_bridge_check_compatibility(
    int32_t required_bridge_abi_version,
    int32_t required_command_version_major,
    frameflow_runtime_info* out_info
);
FRAMEFLOW_API frameflow_engine* frameflow_engine_create(void);
FRAMEFLOW_API void frameflow_engine_destroy(frameflow_engine* engine);
FRAMEFLOW_API frameflow_result frameflow_engine_set_callbacks(
    frameflow_engine* engine,
    const frameflow_callbacks* callbacks
);
FRAMEFLOW_API uint64_t frameflow_engine_dispatch_pending_callbacks(
    frameflow_engine* engine
);
FRAMEFLOW_API frameflow_lifecycle_state frameflow_engine_state(
    const frameflow_engine* engine
);
FRAMEFLOW_API frameflow_result frameflow_engine_initialize_with_presentation(
    frameflow_engine* engine,
    const frameflow_presentation_target* presentation,
    const frameflow_options* options
);
FRAMEFLOW_API frameflow_result frameflow_engine_initialize_with_native_surface(
    frameflow_engine* engine,
    const frameflow_native_surface_desc* surface,
    const frameflow_options* options
);
FRAMEFLOW_API frameflow_result frameflow_engine_initialize(
    frameflow_engine* engine,
    uintptr_t surface_handle,
    int32_t width,
    int32_t height,
    double scale_factor,
    const frameflow_options* options
);
FRAMEFLOW_API frameflow_result frameflow_engine_set_surface_bounds(
    frameflow_engine* engine,
    const frameflow_surface_bounds* bounds
);
FRAMEFLOW_API frameflow_result frameflow_engine_set_surface_visible(
    frameflow_engine* engine,
    uint8_t visible
);
FRAMEFLOW_API frameflow_result frameflow_engine_request_frame(
    frameflow_engine* engine,
    const char* reason
);
FRAMEFLOW_API frameflow_result frameflow_engine_resize(
    frameflow_engine* engine,
    int32_t width,
    int32_t height,
    double scale_factor
);
FRAMEFLOW_API frameflow_result frameflow_engine_pause(
    frameflow_engine* engine
);
FRAMEFLOW_API frameflow_result frameflow_engine_resume(
    frameflow_engine* engine
);
FRAMEFLOW_API frameflow_result frameflow_engine_dispose(
    frameflow_engine* engine
);
FRAMEFLOW_API frameflow_result frameflow_engine_set_points(
    frameflow_engine* engine,
    const frameflow_point* points,
    uint64_t point_count
);
FRAMEFLOW_API frameflow_result frameflow_engine_set_filters(
    frameflow_engine* engine,
    const frameflow_filter* filter
);
FRAMEFLOW_API uint64_t frameflow_engine_point_count(const frameflow_engine* engine);
FRAMEFLOW_API frameflow_result frameflow_engine_focus_location(
    frameflow_engine* engine,
    int64_t location_id
);
FRAMEFLOW_API frameflow_result frameflow_engine_select_location(
    frameflow_engine* engine,
    int64_t location_id,
    const char* category_code,
    double screen_x,
    double screen_y,
    const char* interaction
);
FRAMEFLOW_API frameflow_result frameflow_engine_select_cluster(
    frameflow_engine* engine,
    int64_t cluster_id,
    int32_t point_count,
    double longitude,
    double latitude
);
FRAMEFLOW_API frameflow_result frameflow_engine_report_camera_changed(
    frameflow_engine* engine,
    double longitude,
    double latitude,
    double height_meters,
    double heading_degrees,
    double pitch_degrees,
    double roll_degrees
);
FRAMEFLOW_API uint8_t frameflow_engine_has_selected_location_id(
    const frameflow_engine* engine
);
FRAMEFLOW_API frameflow_result frameflow_engine_get_selected_location_id(
    const frameflow_engine* engine,
    int64_t* out_location_id
);
FRAMEFLOW_API frameflow_result frameflow_engine_get_location_screen_position(
    const frameflow_engine* engine,
    int64_t location_id,
    frameflow_screen_position* out_position
);
FRAMEFLOW_API frameflow_result frameflow_engine_clear_selection(
    frameflow_engine* engine
);
FRAMEFLOW_API frameflow_result frameflow_engine_render_latest_frame(
    frameflow_engine* engine
);
FRAMEFLOW_API uint8_t frameflow_engine_has_latest_frame(
    const frameflow_engine* engine
);
FRAMEFLOW_API uint64_t frameflow_engine_get_frame_generation(
    const frameflow_engine* engine
);
FRAMEFLOW_API uint64_t frameflow_engine_get_render_callback_count(
    const frameflow_engine* engine
);
FRAMEFLOW_API uint64_t frameflow_engine_get_diagnostics_callback_count(
    const frameflow_engine* engine
);
FRAMEFLOW_API frameflow_result frameflow_engine_get_latest_frame_info(
    const frameflow_engine* engine,
    frameflow_frame_info* out_info
);
FRAMEFLOW_API frameflow_result frameflow_engine_copy_latest_frame(
    const frameflow_engine* engine,
    uint8_t* out_pixels,
    uint64_t out_pixel_capacity
);
FRAMEFLOW_API frameflow_result frameflow_engine_last_error_code(
    const frameflow_engine* engine
);
FRAMEFLOW_API const char* frameflow_engine_last_error_message(
    const frameflow_engine* engine
);
FRAMEFLOW_API uint8_t frameflow_engine_last_error_recoverable(
    const frameflow_engine* engine
);
FRAMEFLOW_API const char* frameflow_engine_diagnostics_summary(
    frameflow_engine* engine
);

#ifdef __cplusplus
}
#endif
