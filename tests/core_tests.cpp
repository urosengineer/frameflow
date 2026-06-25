#include "frameflow/engine.hpp"
#include "core/native_surface_runtime.hpp"
#include "renderer/cartography/cartography_dataset.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
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

frameflow::GeoPointAggregate point(std::int64_t location_id, const char* label) {
    frameflow::GeoPointAggregate aggregate;
    aggregate.location_id = location_id;
    aggregate.label = label;
    aggregate.kind = frameflow::LocationKind::City;
    aggregate.latitude = 44.0;
    aggregate.longitude = 20.0;
    aggregate.story_count = 1;
    return aggregate;
}

class RecordingSurfaceHost final : public frameflow::FrameflowSurfaceHost {
public:
    [[nodiscard]] std::optional<std::string> create(const frameflow::FrameflowNativeSurfaceDesc& desc) override {
        std::lock_guard lock(mutex_);
        bounds_ = desc.bounds;
        created_ = true;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> resize(const frameflow::FrameflowSurfaceBounds& bounds) override {
        std::lock_guard lock(mutex_);
        if (next_resize_error_.has_value()) {
            auto error = std::move(*next_resize_error_);
            next_resize_error_.reset();
            return error;
        }
        bounds_ = bounds;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> set_visible(const bool visible) override {
        std::lock_guard lock(mutex_);
        visible_ = visible;
        lifecycle_events_.push_back(visible ? "visible" : "hidden");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> update_scene(const frameflow::FrameflowSceneSnapshot& snapshot) override {
        std::lock_guard lock(mutex_);
        scene_update_count_ += 1u;
        last_scene_snapshot_ = snapshot;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> make_current() override {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> swap_buffers() override {
        std::lock_guard lock(mutex_);
        present_count_ += 1u;
        if (emit_camera_event_on_first_present_ && present_count_ == 1u) {
            frameflow::FrameflowSurfaceEvent event;
            event.type = frameflow::FrameflowSurfaceEvent::Type::CameraChanged;
            event.longitude = 20.5;
            event.latitude = 44.2;
            event.height_meters = 11'000.0;
            pending_events_.push_back(std::move(event));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<frameflow::FrameflowSurfaceEvent> drain_events() override {
        std::lock_guard lock(mutex_);
        auto events = std::move(pending_events_);
        pending_events_.clear();
        return events;
    }

    [[nodiscard]] std::optional<std::string> destroy() override {
        std::lock_guard lock(mutex_);
        lifecycle_events_.push_back("destroyed");
        created_ = false;
        return std::nullopt;
    }

    [[nodiscard]] const char* backend_name() const noexcept override {
        return "recording-surface-host";
    }

    [[nodiscard]] std::string diagnostics_summary() const override {
        std::lock_guard lock(mutex_);
        return "recording";
    }

    void enqueue_event(frameflow::FrameflowSurfaceEvent event) {
        std::lock_guard lock(mutex_);
        pending_events_.push_back(std::move(event));
    }

    [[nodiscard]] std::uint64_t scene_update_count() const {
        std::lock_guard lock(mutex_);
        return scene_update_count_;
    }

    [[nodiscard]] std::optional<frameflow::FrameflowSceneSnapshot> last_scene_snapshot() const {
        std::lock_guard lock(mutex_);
        return last_scene_snapshot_;
    }

    void set_emit_camera_event_on_first_present(const bool enabled) {
        std::lock_guard lock(mutex_);
        emit_camera_event_on_first_present_ = enabled;
    }

    void fail_next_resize(std::string message) {
        std::lock_guard lock(mutex_);
        next_resize_error_ = std::move(message);
    }

    [[nodiscard]] std::uint64_t present_count() const {
        std::lock_guard lock(mutex_);
        return present_count_;
    }

    [[nodiscard]] std::vector<std::string> lifecycle_events() const {
        std::lock_guard lock(mutex_);
        return lifecycle_events_;
    }

private:
    mutable std::mutex mutex_;
    bool created_{false};
    bool visible_{true};
    frameflow::FrameflowSurfaceBounds bounds_{};
    std::uint64_t scene_update_count_{0u};
    std::uint64_t present_count_{0u};
    bool emit_camera_event_on_first_present_{false};
    std::optional<frameflow::FrameflowSceneSnapshot> last_scene_snapshot_;
    std::optional<std::string> next_resize_error_;
    std::vector<std::string> lifecycle_events_;
    std::vector<frameflow::FrameflowSurfaceEvent> pending_events_;
};

void set_points_replaces_dataset() {
    frameflow::Engine engine;
    const std::vector<frameflow::GeoPointAggregate> initial{point(1, "One"), point(2, "Two")};
    engine.set_points(initial);
    expect(engine.point_count() == 2, "initial snapshot should contain two points");

    const std::vector<frameflow::GeoPointAggregate> replacement{point(3, "Three")};
    engine.set_points(replacement);
    expect(engine.point_count() == 1, "replacement snapshot should contain one point");
}

void focus_requires_known_location() {
    frameflow::Engine engine;
    const std::vector<frameflow::GeoPointAggregate> points{point(10, "Belgrade")};
    engine.set_points(points);

    expect(engine.focus_location(10), "known location should be focusable");
    expect(engine.selected_location_id().has_value(), "selection should be present after focus");
    expect(*engine.selected_location_id() == 10, "selection should match focused location");
    expect(!engine.focus_location(99), "unknown location should not be focusable");
    expect(engine.selected_location_id().has_value(), "failed focus should preserve prior selection");
    expect(*engine.selected_location_id() == 10, "failed focus should not replace prior selection");
}

void set_points_clears_stale_selection() {
    frameflow::Engine engine;
    const std::vector<frameflow::GeoPointAggregate> initial{point(10, "Belgrade")};
    engine.set_points(initial);
    expect(engine.focus_location(10), "known location should be focusable");

    const std::vector<frameflow::GeoPointAggregate> replacement{point(11, "Novi Sad")};
    engine.set_points(replacement);
    expect(!engine.selected_location_id().has_value(), "replacing the dataset should clear stale selection");
}

void command_queue_coalesces_latest_surface_snapshots() {
    frameflow::FrameflowCommandQueue queue;

    queue.enqueue_bounds(frameflow::FrameflowSurfaceBounds{
        .x = 0,
        .y = 0,
        .width = 640,
        .height = 480,
        .scale_factor = 1.0,
    });
    queue.enqueue_bounds(frameflow::FrameflowSurfaceBounds{
        .x = 14,
        .y = 22,
        .width = 1280,
        .height = 720,
        .scale_factor = 1.5,
    });
    queue.enqueue_visibility(false);
    queue.enqueue_visibility(true);
    queue.enqueue_paused(true);
    queue.enqueue_paused(false);
    queue.enqueue_frame_request();
    queue.enqueue_frame_request();

    const auto drained = queue.drain_ready();
    expect(drained.has_value(), "drain_ready should expose queued commands");
    expect(drained->bounds.has_value(), "drained commands should include bounds");
    expect(drained->bounds->x == 14, "latest bounds x should win");
    expect(drained->bounds->y == 22, "latest bounds y should win");
    expect(drained->bounds->width == 1280, "latest bounds width should win");
    expect(drained->bounds->height == 720, "latest bounds height should win");
    expect(drained->bounds->scale_factor == 1.5, "latest bounds scale should win");
    expect(drained->visible.has_value() && *drained->visible, "latest visibility should win");
    expect(drained->paused.has_value() && !*drained->paused, "latest paused state should win");
    expect(drained->frame_requests == 2u, "frame requests should accumulate before drain");
    expect(drained->command_high_watermark >= 4u, "command queue should record a meaningful high watermark");
    expect(!queue.drain_ready().has_value(), "draining should clear pending commands");
}

void render_thread_starts_processes_commands_and_stops_cleanly() {
    frameflow::FrameflowNativeSurfaceDesc desc;
    desc.backend = frameflow::FrameflowSurfaceBackend::LinuxX11Child;
    desc.bounds = frameflow::FrameflowSurfaceBounds{
        .x = 0,
        .y = 0,
        .width = 800,
        .height = 600,
        .scale_factor = 1.0,
    };
    desc.x11 = frameflow::FrameflowX11SurfaceDesc{
        .display = reinterpret_cast<void*>(0x33u),
        .parent_window = 77u,
    };

    frameflow::FrameflowRenderThread thread(
        desc,
        std::make_unique<frameflow::FrameflowNullSurfaceHost>()
    );
    std::string start_error;
    expect(thread.start(&start_error), "render thread should start with the null surface host");

    thread.update_bounds(frameflow::FrameflowSurfaceBounds{
        .x = 10,
        .y = 16,
        .width = 1024,
        .height = 768,
        .scale_factor = 2.0,
    });
    thread.set_visible(false);
    thread.set_paused(true);
    thread.request_frame();
    thread.set_visible(true);
    thread.set_paused(false);
    thread.request_frame();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    frameflow::FrameflowRenderStats stats;
    do {
        stats = thread.stats_snapshot();
        if (stats.resize_count >= 1u &&
            stats.visibility_change_count >= 1u &&
            stats.frame_request_count >= 2u &&
            stats.present_count >= 1u &&
            stats.active_bounds.width == 1024 &&
            stats.active_bounds.height == 768 &&
            stats.visible &&
            !stats.paused) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    stats = thread.stats_snapshot();
    expect(stats.worker_running, "render thread should still be running before stop");
    expect(stats.surface_created, "render thread should keep the surface alive while running");
    expect(stats.thread_start_count == 1u, "render thread should start exactly once");
    expect(stats.resize_count >= 1u, "render thread should apply at least one bounds update");
    expect(stats.visibility_change_count >= 1u, "render thread should coalesce visibility changes into at least one apply");
    expect(stats.frame_request_count >= 2u, "render thread should account for frame requests");
    expect(stats.present_count >= 1u, "render thread should present at least one coalesced frame");
    expect(stats.active_bounds.width == 1024, "render thread should retain the latest width");
    expect(stats.active_bounds.height == 768, "render thread should retain the latest height");
    expect(stats.command_high_watermark >= 1u, "render thread should expose command queue pressure");
    expect(stats.loop_mode == frameflow::FrameflowRenderLoopMode::Idle, "render thread should return to IDLE");

    thread.stop();
    thread.stop();

    const auto stopped = thread.stats_snapshot();
    expect(!stopped.worker_running, "render thread stop should join the worker");
    expect(!stopped.surface_created, "render thread stop should destroy the surface");
    expect(stopped.thread_stop_count == 1u, "render thread should stop exactly once");
    expect(stopped.destroy_count == 1u, "render thread should destroy the host exactly once");
    expect(
        stopped.loop_mode == frameflow::FrameflowRenderLoopMode::Stopped,
        "render thread stop should leave the worker in STOPPED mode"
    );
}

void render_thread_applies_scene_snapshot_without_frame_request() {
    auto* host = new RecordingSurfaceHost();

    frameflow::FrameflowNativeSurfaceDesc desc;
    desc.backend = frameflow::FrameflowSurfaceBackend::LinuxX11Child;
    desc.bounds = frameflow::FrameflowSurfaceBounds{
        .x = 0,
        .y = 0,
        .width = 640,
        .height = 480,
        .scale_factor = 1.0,
    };

    frameflow::FrameflowRenderThread thread(
        desc,
        std::unique_ptr<frameflow::FrameflowSurfaceHost>(host)
    );
    std::string start_error;
    expect(thread.start(&start_error), "recording surface host should start");

    frameflow::FrameflowSceneSnapshot snapshot;
    snapshot.points = {point(41, "Belgrade")};
    snapshot.selected_location_id = 41;
    snapshot.camera = frameflow::FrameflowCameraState{
        .longitude = 20.0,
        .latitude = 44.0,
        .height_meters = 15'000.0,
        .heading_degrees = 0.0,
        .pitch_degrees = -35.0,
        .roll_degrees = 0.0,
    };
    thread.update_scene_snapshot(snapshot);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (host->scene_update_count() == 0u && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    expect(host->scene_update_count() >= 1u, "scene snapshot updates should wake the render thread");
    const auto applied_snapshot = host->last_scene_snapshot();
    expect(applied_snapshot.has_value(), "recording host should receive a scene snapshot");
    expect(applied_snapshot->selected_location_id.has_value(), "scene snapshot should carry selected location");
    expect(*applied_snapshot->selected_location_id == 41, "scene snapshot should preserve selected location");
    thread.stop();
}

void render_thread_records_failure_message_when_surface_host_fails() {
    auto* host = new RecordingSurfaceHost();

    frameflow::FrameflowNativeSurfaceDesc desc;
    desc.backend = frameflow::FrameflowSurfaceBackend::LinuxX11Child;
    desc.bounds = frameflow::FrameflowSurfaceBounds{
        .x = 0,
        .y = 0,
        .width = 640,
        .height = 480,
        .scale_factor = 1.0,
    };

    frameflow::FrameflowRenderThread thread(
        desc,
        std::unique_ptr<frameflow::FrameflowSurfaceHost>(host)
    );
    std::string start_error;
    expect(thread.start(&start_error), "recording surface host should start before injected failure");

    host->fail_next_resize("synthetic resize failure");
    thread.update_bounds(frameflow::FrameflowSurfaceBounds{
        .x = 4,
        .y = 8,
        .width = 800,
        .height = 600,
        .scale_factor = 1.0,
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    frameflow::FrameflowRenderStats stats;
    do {
        stats = thread.stats_snapshot();
        if (stats.loop_mode == frameflow::FrameflowRenderLoopMode::Failed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    stats = thread.stats_snapshot();
    expect(
        stats.loop_mode == frameflow::FrameflowRenderLoopMode::Failed,
        "render thread should enter FAILED mode when the surface host fails"
    );
    expect(
        stats.failure_message == "synthetic resize failure",
        "render thread stats should preserve the host failure message"
    );
    expect(
        thread.diagnostics_summary().find("synthetic resize failure") != std::string::npos,
        "render thread diagnostics should include the host failure message"
    );
    thread.stop();
}

void render_thread_hides_surface_before_destroy_on_stop() {
    auto* host = new RecordingSurfaceHost();

    frameflow::FrameflowNativeSurfaceDesc desc;
    desc.backend = frameflow::FrameflowSurfaceBackend::LinuxX11Child;
    desc.bounds = frameflow::FrameflowSurfaceBounds{
        .x = 0,
        .y = 0,
        .width = 640,
        .height = 480,
        .scale_factor = 1.0,
    };

    frameflow::FrameflowRenderThread thread(
        desc,
        std::unique_ptr<frameflow::FrameflowSurfaceHost>(host)
    );
    std::string start_error;
    expect(thread.start(&start_error), "recording surface host should start before stop lifecycle assertion");

    thread.stop();

    const auto events = host->lifecycle_events();
    const auto hidden = std::find(events.begin(), events.end(), "hidden");
    const auto destroyed = std::find(events.begin(), events.end(), "destroyed");
    expect(hidden != events.end(), "render thread stop should hide the surface before destroying it");
    expect(destroyed != events.end(), "render thread stop should destroy the surface");
    expect(hidden < destroyed, "render thread stop should hide before destroy");

    const auto stopped = thread.stats_snapshot();
    expect(!stopped.visible, "render thread stop should leave stats visibility false after hide-before-stop");
}

void render_thread_drains_surface_events_from_host() {
    auto* host = new RecordingSurfaceHost();

    frameflow::FrameflowNativeSurfaceDesc desc;
    desc.backend = frameflow::FrameflowSurfaceBackend::LinuxX11Child;
    desc.bounds = frameflow::FrameflowSurfaceBounds{
        .x = 0,
        .y = 0,
        .width = 640,
        .height = 480,
        .scale_factor = 1.0,
    };

    frameflow::FrameflowRenderThread thread(
        desc,
        std::unique_ptr<frameflow::FrameflowSurfaceHost>(host)
    );
    std::string start_error;
    expect(thread.start(&start_error), "recording surface host should start");

    frameflow::FrameflowSurfaceEvent location_event;
    location_event.type = frameflow::FrameflowSurfaceEvent::Type::LocationSelected;
    location_event.location_id = 77;
    location_event.category_code = std::string("POLITICS");
    location_event.screen_x = 321.0;
    location_event.screen_y = 123.0;
    location_event.interaction = "click";
    host->enqueue_event(std::move(location_event));

    frameflow::FrameflowSurfaceEvent clear_event;
    clear_event.type = frameflow::FrameflowSurfaceEvent::Type::SelectionCleared;
    host->enqueue_event(std::move(clear_event));

    frameflow::FrameflowSurfaceEvent camera_event;
    camera_event.type = frameflow::FrameflowSurfaceEvent::Type::CameraChanged;
    camera_event.longitude = 19.85;
    camera_event.latitude = 44.12;
    camera_event.height_meters = 12'500.0;
    camera_event.pitch_degrees = -35.0;
    host->enqueue_event(std::move(camera_event));

    thread.request_frame();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::vector<frameflow::FrameflowSurfaceEvent> drained_events;
    while (drained_events.empty() && std::chrono::steady_clock::now() < deadline) {
        drained_events = thread.drain_surface_events();
        if (drained_events.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    expect(drained_events.size() == 3u, "render thread should expose drained surface events");
    expect(
        drained_events[0].type == frameflow::FrameflowSurfaceEvent::Type::LocationSelected &&
            drained_events[0].location_id == 77,
        "first drained event should preserve location selection payload"
    );
    expect(
        drained_events[1].type == frameflow::FrameflowSurfaceEvent::Type::SelectionCleared,
        "second drained event should preserve selection clear intent"
    );
    expect(
        drained_events[2].type == frameflow::FrameflowSurfaceEvent::Type::CameraChanged &&
            drained_events[2].height_meters == 12'500.0,
        "third drained event should preserve camera payload"
    );
    thread.stop();
}

void render_thread_repaints_when_surface_input_emits_events() {
    auto* host = new RecordingSurfaceHost();
    host->set_emit_camera_event_on_first_present(true);

    frameflow::FrameflowNativeSurfaceDesc desc;
    desc.backend = frameflow::FrameflowSurfaceBackend::LinuxX11Child;
    desc.bounds = frameflow::FrameflowSurfaceBounds{
        .x = 0,
        .y = 0,
        .width = 640,
        .height = 480,
        .scale_factor = 1.0,
    };

    frameflow::FrameflowRenderThread thread(
        desc,
        std::unique_ptr<frameflow::FrameflowSurfaceHost>(host)
    );
    std::string start_error;
    expect(thread.start(&start_error), "recording surface host should start");

    thread.request_frame();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (host->present_count() < 2u && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    expect(host->present_count() >= 2u, "surface input events should trigger a follow-up present cycle");
    thread.stop();
}

void default_cartography_dataset_is_unavailable_without_runtime_override() {
    unsetenv("FRAMEFLOW_BOUNDARY_OVERLAY_PATH");

    const auto dataset = frameflow::renderer::cartography::CartographyDatasetLoader::load_default();
    expect(!dataset.available(), "boundary overlay should be unavailable by default");
    expect(
        dataset.source == frameflow::renderer::cartography::CartographyDatasetSource::Unavailable,
        "default boundary overlay should not depend on packaged assets"
    );
    expect(dataset.metadata.schema == "frameflow-boundary-overlay-v1", "boundary overlay schema header should be locked");
    expect(
        dataset.diagnostics_summary().find("missing_env=FRAMEFLOW_BOUNDARY_OVERLAY_PATH") != std::string::npos,
        "boundary overlay diagnostics should explain how to provide optional overlay data"
    );
}

void runtime_override_cartography_dataset_wins_when_valid() {
    const auto override_path =
        std::filesystem::temp_directory_path() / "frameflow-boundary-overlay-runtime.txt";
    {
        std::ofstream output(override_path);
        output
            << "frameflow-boundary-overlay-v1\n"
            << "meta\tdataset_id=runtime-override-v1\tlocale=en\n"
            << "boundary\tcountry\tit\tItaly\tIT\t120\t6.6,47.1;18.6,47.1;18.6,36.4;6.6,36.4;6.6,47.1\n"
            << "label\tcountry\tit\tItaly\tIT\t12.5\t42.8\t120\t1500\t28000000\n"
            << "label\tcapital\trome\tRome\tIT\t12.4964\t41.9028\t110\t1500\t4500000\n";
    }

    setenv("FRAMEFLOW_BOUNDARY_OVERLAY_PATH", override_path.c_str(), 1);
    const auto dataset = frameflow::renderer::cartography::CartographyDatasetLoader::load_default();
    unsetenv("FRAMEFLOW_BOUNDARY_OVERLAY_PATH");
    std::filesystem::remove(override_path);

    expect(dataset.available(), "valid runtime boundary overlay should load");
    expect(
        dataset.source == frameflow::renderer::cartography::CartographyDatasetSource::RuntimeOverride,
        "runtime override should be used when explicitly configured"
    );
    expect(dataset.metadata.dataset_id == "runtime-override-v1", "runtime override metadata should be preserved");
    expect(dataset.boundaries.size() == 1u, "runtime override should expose its own boundary count");
    expect(dataset.labels.size() == 2u, "runtime override should expose its own label count");
}

void invalid_runtime_override_is_reported_without_packaged_fallback() {
    const auto override_path =
        std::filesystem::temp_directory_path() / "frameflow-boundary-overlay-invalid.txt";
    {
        std::ofstream output(override_path);
        output << "not-a-valid-cartography-file\n";
    }

    setenv("FRAMEFLOW_BOUNDARY_OVERLAY_PATH", override_path.c_str(), 1);
    const auto dataset = frameflow::renderer::cartography::CartographyDatasetLoader::load_default();
    unsetenv("FRAMEFLOW_BOUNDARY_OVERLAY_PATH");
    std::filesystem::remove(override_path);

    expect(!dataset.available(), "invalid runtime override should not fall back to packaged assets");
    expect(
        dataset.source == frameflow::renderer::cartography::CartographyDatasetSource::Unavailable,
        "invalid runtime override should leave cartography unavailable"
    );
    expect(
        dataset.diagnostics_summary().find("invalid_header_line=1") != std::string::npos,
        "boundary overlay diagnostics should record runtime override parse failure"
    );
}

} // namespace

int main() {
    set_points_replaces_dataset();
    focus_requires_known_location();
    set_points_clears_stale_selection();
    command_queue_coalesces_latest_surface_snapshots();
    render_thread_starts_processes_commands_and_stops_cleanly();
    render_thread_applies_scene_snapshot_without_frame_request();
    render_thread_records_failure_message_when_surface_host_fails();
    render_thread_hides_surface_before_destroy_on_stop();
    render_thread_drains_surface_events_from_host();
    render_thread_repaints_when_surface_input_emits_events();
    default_cartography_dataset_is_unavailable_without_runtime_override();
    runtime_override_cartography_dataset_wins_when_valid();
    invalid_runtime_override_is_reported_without_packaged_fallback();
    return 0;
}
