#pragma once

#include "frameflow/types.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frameflow {

enum class FrameflowSurfaceBackend {
    Auto,
    OffscreenBitmap,
    LinuxX11Child,
    WindowsHwndChild,
};

enum class FrameflowPresentationMode {
    NativeSurface,
    OffscreenBitmap,
};

enum class FrameflowRenderLoopMode {
    Starting,
    Idle,
    Paused,
    Failed,
    Stopped,
};

struct FrameflowSurfaceBounds {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t width{};
    std::int32_t height{};
    double scale_factor{1.0};
};

struct FrameflowX11SurfaceDesc {
    void* display{};
    std::uint64_t parent_window{};
};

struct FrameflowWin32SurfaceDesc {
    void* parent_hwnd{};
};

struct FrameflowNativeSurfaceDesc {
    FrameflowSurfaceBackend backend{FrameflowSurfaceBackend::Auto};
    FrameflowSurfaceBounds bounds{};
    std::optional<FrameflowX11SurfaceDesc> x11;
    std::optional<FrameflowWin32SurfaceDesc> win32;

    [[nodiscard]] FrameflowPresentationMode presentation_mode() const noexcept {
        return backend == FrameflowSurfaceBackend::OffscreenBitmap
            ? FrameflowPresentationMode::OffscreenBitmap
            : FrameflowPresentationMode::NativeSurface;
    }
};

struct FrameflowCameraState {
    double longitude{15.0};
    double latitude{20.0};
    double height_meters{8'500'000.0};
    double heading_degrees{0.0};
    double pitch_degrees{-35.0};
    double roll_degrees{0.0};
};

struct FrameflowSceneSnapshot {
    std::vector<GeoPointAggregate> points;
    std::optional<std::int64_t> selected_location_id;
    std::optional<std::int64_t> focus_location_id;
    std::optional<FrameflowCameraState> camera;
};

struct FrameflowRenderStats {
    FrameflowRenderLoopMode loop_mode{FrameflowRenderLoopMode::Stopped};
    bool worker_running{false};
    bool surface_created{false};
    bool visible{true};
    bool paused{false};
    std::uint64_t thread_start_count{0u};
    std::uint64_t thread_stop_count{0u};
    std::uint64_t wake_count{0u};
    std::uint64_t frame_request_count{0u};
    std::uint64_t create_count{0u};
    std::uint64_t resize_count{0u};
    std::uint64_t visibility_change_count{0u};
    std::uint64_t scene_update_count{0u};
    std::uint64_t make_current_count{0u};
    std::uint64_t present_count{0u};
    std::uint64_t destroy_count{0u};
    std::uint64_t command_high_watermark{0u};
    FrameflowSurfaceBounds active_bounds{};
    std::string surface_host_backend{"none"};
    std::string surface_host_diag{"unset"};
    std::string last_status{"not-started"};
    std::string failure_message;
};

struct FrameflowSurfaceEvent {
    enum class Type {
        LocationSelected,
        ClusterSelected,
        SelectionCleared,
        CameraChanged
    };

    Type type{Type::LocationSelected};
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
};

class FrameflowSurfaceHost {
public:
    virtual ~FrameflowSurfaceHost() = default;

    [[nodiscard]] virtual std::optional<std::string> create(const FrameflowNativeSurfaceDesc& desc) = 0;
    [[nodiscard]] virtual std::optional<std::string> resize(const FrameflowSurfaceBounds& bounds) = 0;
    [[nodiscard]] virtual std::optional<std::string> set_visible(bool visible) = 0;
    [[nodiscard]] virtual std::optional<std::string> update_scene(const FrameflowSceneSnapshot& snapshot) {
        (void)snapshot;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<std::string> make_current() = 0;
    [[nodiscard]] virtual std::optional<std::string> swap_buffers() = 0;
    [[nodiscard]] virtual std::vector<FrameflowSurfaceEvent> drain_events() { return {}; }
    [[nodiscard]] virtual std::optional<std::string> destroy() = 0;
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
    [[nodiscard]] virtual std::string diagnostics_summary() const = 0;
};

class FrameflowNullSurfaceHost final : public FrameflowSurfaceHost {
public:
    [[nodiscard]] std::optional<std::string> create(const FrameflowNativeSurfaceDesc& desc) override;
    [[nodiscard]] std::optional<std::string> resize(const FrameflowSurfaceBounds& bounds) override;
    [[nodiscard]] std::optional<std::string> set_visible(bool visible) override;
    [[nodiscard]] std::optional<std::string> make_current() override;
    [[nodiscard]] std::optional<std::string> swap_buffers() override;
    [[nodiscard]] std::optional<std::string> destroy() override;
    [[nodiscard]] const char* backend_name() const noexcept override;
    [[nodiscard]] std::string diagnostics_summary() const override;

private:
    bool created_{false};
    bool visible_{true};
    FrameflowSurfaceBounds bounds_{};
};

struct FrameflowQueuedCommands {
    bool stop_requested{false};
    std::optional<FrameflowSurfaceBounds> bounds;
    std::optional<bool> visible;
    std::optional<bool> paused;
    std::optional<FrameflowSceneSnapshot> scene_snapshot;
    std::uint64_t frame_requests{0u};
    std::uint64_t command_high_watermark{0u};

    [[nodiscard]] bool has_work() const noexcept {
        return stop_requested ||
            bounds.has_value() ||
            visible.has_value() ||
            paused.has_value() ||
            scene_snapshot.has_value() ||
            frame_requests > 0u;
    }
};

class FrameflowCommandQueue {
public:
    void enqueue_bounds(FrameflowSurfaceBounds bounds);
    void enqueue_visibility(bool visible);
    void enqueue_paused(bool paused);
    void enqueue_scene_snapshot(FrameflowSceneSnapshot snapshot);
    void enqueue_frame_request();
    void enqueue_stop();

    [[nodiscard]] std::optional<FrameflowQueuedCommands> drain_ready();
    [[nodiscard]] FrameflowQueuedCommands wait_and_drain();

private:
    template <typename Mutation>
    void enqueue_mutation(Mutation&& mutation);

    [[nodiscard]] FrameflowQueuedCommands drain_locked();

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<FrameflowSurfaceBounds> pending_bounds_;
    std::optional<bool> pending_visible_;
    std::optional<bool> pending_paused_;
    std::optional<FrameflowSceneSnapshot> pending_scene_snapshot_;
    std::uint64_t pending_frame_requests_{0u};
    bool stop_requested_{false};
    std::uint64_t pending_command_count_{0u};
    std::uint64_t command_high_watermark_{0u};
};

class FrameflowRenderThread {
public:
    FrameflowRenderThread(
        FrameflowNativeSurfaceDesc desc,
        std::unique_ptr<FrameflowSurfaceHost> surface_host
    );
    ~FrameflowRenderThread();

    FrameflowRenderThread(const FrameflowRenderThread&) = delete;
    FrameflowRenderThread& operator=(const FrameflowRenderThread&) = delete;

    [[nodiscard]] bool start(std::string* error_message);
    void update_bounds(FrameflowSurfaceBounds bounds);
    void set_visible(bool visible);
    void set_paused(bool paused);
    void update_scene_snapshot(FrameflowSceneSnapshot snapshot);
    void request_frame();
    [[nodiscard]] std::vector<FrameflowSurfaceEvent> drain_surface_events();
    void stop();

    [[nodiscard]] FrameflowRenderStats stats_snapshot() const;
    [[nodiscard]] std::string diagnostics_summary() const;

private:
    void run();
    void set_failed_state(std::string message);

    FrameflowNativeSurfaceDesc desc_;
    std::unique_ptr<FrameflowSurfaceHost> surface_host_;
    FrameflowCommandQueue command_queue_;

    mutable std::mutex stats_mutex_;
    FrameflowRenderStats stats_;

    std::mutex start_mutex_;
    std::condition_variable start_condition_;
    bool start_completed_{false};
    bool start_succeeded_{false};
    std::string start_error_message_;

    std::thread worker_;
    bool stop_called_{false};
    mutable std::mutex surface_events_mutex_;
    std::vector<FrameflowSurfaceEvent> pending_surface_events_;
};

[[nodiscard]] const char* frameflow_render_loop_mode_name(FrameflowRenderLoopMode mode) noexcept;

} // namespace frameflow
