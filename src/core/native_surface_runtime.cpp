#include "native_surface_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <sstream>
#include <utility>

#if defined(FRAMEFLOW_HAS_X11_SURFACE_HOST)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace frameflow {

namespace {

constexpr std::uint64_t kMaxCoalescedFrameRequests = 4u;

#if defined(FRAMEFLOW_HAS_X11_SURFACE_HOST)
class FrameflowLinuxX11SmokeSurfaceHost final : public FrameflowSurfaceHost {
public:
    explicit FrameflowLinuxX11SmokeSurfaceHost(std::uint64_t parent_window)
        : parent_window_(static_cast<::Window>(parent_window)) {}

    [[nodiscard]] std::optional<std::string> create(const FrameflowNativeSurfaceDesc& desc) override {
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            return "XOpenDisplay failed for linux X11 smoke surface host";
        }
        screen_ = DefaultScreen(display_);
        bounds_ = desc.bounds;
        visible_ = true;

        XColor allocated{};
        XColor exact{};
        const auto colormap = DefaultColormap(display_, screen_);
        background_pixel_ = XAllocNamedColor(display_, colormap, "midnight blue", &allocated, &exact) != 0
            ? allocated.pixel
            : BlackPixel(display_, screen_);
        alternate_background_pixel_ = XAllocNamedColor(display_, colormap, "dark slate blue", &allocated, &exact) != 0
            ? allocated.pixel
            : WhitePixel(display_, screen_);
        foreground_pixel_ = XAllocNamedColor(display_, colormap, "white", &allocated, &exact) != 0
            ? allocated.pixel
            : WhitePixel(display_, screen_);

        child_window_ = XCreateSimpleWindow(
            display_,
            parent_window_,
            bounds_.x,
            bounds_.y,
            static_cast<unsigned int>(std::max(1, bounds_.width)),
            static_cast<unsigned int>(std::max(1, bounds_.height)),
            0u,
            foreground_pixel_,
            background_pixel_
        );
        if (child_window_ == 0u) {
            close_display();
            return "XCreateSimpleWindow failed for linux X11 smoke surface host";
        }

        XSelectInput(display_, child_window_, ExposureMask | StructureNotifyMask);
        gc_ = XCreateGC(display_, child_window_, 0u, nullptr);
        if (gc_ == nullptr) {
            destroy_window();
            close_display();
            return "XCreateGC failed for linux X11 smoke surface host";
        }
        XMapRaised(display_, child_window_);
        XFlush(display_);
        created_ = true;
        last_status_ = "created";
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> resize(const FrameflowSurfaceBounds& bounds) override {
        if (!created_) {
            return "linux X11 smoke surface host must be created before resize";
        }
        bounds_ = bounds;
        XMoveResizeWindow(
            display_,
            child_window_,
            bounds_.x,
            bounds_.y,
            static_cast<unsigned int>(std::max(1, bounds_.width)),
            static_cast<unsigned int>(std::max(1, bounds_.height))
        );
        XFlush(display_);
        last_status_ = "resized";
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> set_visible(const bool visible) override {
        if (!created_) {
            return "linux X11 smoke surface host must be created before set_visible";
        }
        visible_ = visible;
        if (visible_) {
            XMapRaised(display_, child_window_);
        } else {
            XUnmapWindow(display_, child_window_);
        }
        XFlush(display_);
        last_status_ = visible_ ? "visible" : "hidden";
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> make_current() override {
        if (!created_) {
            return "linux X11 smoke surface host must be created before make_current";
        }
        last_status_ = "current";
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> swap_buffers() override {
        if (!created_) {
            return "linux X11 smoke surface host must be created before swap_buffers";
        }
        frame_counter_ += 1u;
        const unsigned long clear_pixel = (frame_counter_ % 2u == 0u)
            ? background_pixel_
            : alternate_background_pixel_;
        XSetForeground(display_, gc_, clear_pixel);
        XFillRectangle(
            display_,
            child_window_,
            gc_,
            0,
            0,
            static_cast<unsigned int>(std::max(1, bounds_.width)),
            static_cast<unsigned int>(std::max(1, bounds_.height))
        );
        XSetForeground(display_, gc_, foreground_pixel_);
        XDrawRectangle(
            display_,
            child_window_,
            gc_,
            2,
            2,
            static_cast<unsigned int>(std::max(1, bounds_.width - 5)),
            static_cast<unsigned int>(std::max(1, bounds_.height - 5))
        );
        char label[192];
        std::snprintf(
            label,
            sizeof(label),
            "Frameflow Linux Smoke frame=%llu size=%dx%d",
            static_cast<unsigned long long>(frame_counter_),
            bounds_.width,
            bounds_.height
        );
        XDrawString(display_, child_window_, gc_, 16, 28, label, static_cast<int>(std::strlen(label)));
        XFlush(display_);
        last_status_ = "presented";
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> destroy() override {
        destroy_window();
        close_display();
        visible_ = true;
        created_ = false;
        last_status_ = "destroyed";
        return std::nullopt;
    }

    [[nodiscard]] const char* backend_name() const noexcept override {
        return "linux-x11-smoke-host";
    }

    [[nodiscard]] std::string diagnostics_summary() const override {
        std::ostringstream out;
        out << "created=" << (created_ ? "true" : "false")
            << " visible=" << (visible_ ? "true" : "false")
            << " parent_window=" << (parent_window_ != 0u ? "present" : "none")
            << " child_window=" << (child_window_ != 0u ? "present" : "none")
            << " frame_counter=" << frame_counter_
            << " bounds=" << bounds_.x << "," << bounds_.y << "," << bounds_.width << "x" << bounds_.height
            << " status=" << last_status_;
        return out.str();
    }

private:
    void destroy_window() {
        if (display_ != nullptr && gc_ != nullptr && child_window_ != 0u) {
            XFreeGC(display_, gc_);
            gc_ = nullptr;
        }
        if (display_ != nullptr && child_window_ != 0u) {
            XDestroyWindow(display_, child_window_);
            XFlush(display_);
            child_window_ = 0u;
        }
    }

    void close_display() {
        if (display_ != nullptr) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
        screen_ = 0;
    }

    Display* display_{nullptr};
    int screen_{0};
    ::Window parent_window_{0u};
    ::Window child_window_{0u};
    GC gc_{nullptr};
    unsigned long background_pixel_{0u};
    unsigned long alternate_background_pixel_{0u};
    unsigned long foreground_pixel_{0u};
    bool created_{false};
    bool visible_{true};
    std::uint64_t frame_counter_{0u};
    FrameflowSurfaceBounds bounds_{};
    std::string last_status_{"idle"};
};
#endif

std::unique_ptr<FrameflowSurfaceHost> create_surface_host_for_desc(
    const FrameflowNativeSurfaceDesc& desc,
    std::string* selection_reason
) {
#if defined(FRAMEFLOW_HAS_X11_SURFACE_HOST)
    if (desc.backend == FrameflowSurfaceBackend::LinuxX11Child &&
        desc.x11.has_value() &&
        desc.x11->display != nullptr &&
        desc.x11->parent_window > 4096u &&
        reinterpret_cast<std::uintptr_t>(desc.x11->display) > 4096u) {
        const char* display_env = std::getenv("DISPLAY");
        if (display_env != nullptr && display_env[0] != '\0') {
            if (selection_reason != nullptr) {
                *selection_reason = "selected real linux X11 smoke surface host";
            }
            return std::make_unique<FrameflowLinuxX11SmokeSurfaceHost>(desc.x11->parent_window);
        }
        if (selection_reason != nullptr) {
            *selection_reason = "DISPLAY is unavailable; falling back to null surface host";
        }
    } else if (desc.backend == FrameflowSurfaceBackend::LinuxX11Child && selection_reason != nullptr) {
        *selection_reason = "descriptor does not expose plausible X11 handles; falling back to null surface host";
    }
#else
    (void)desc;
#endif
    if (selection_reason != nullptr && selection_reason->empty()) {
        *selection_reason = "selected null surface host";
    }
    return std::make_unique<FrameflowNullSurfaceHost>();
}

} // namespace

const char* frameflow_render_loop_mode_name(const FrameflowRenderLoopMode mode) noexcept {
    switch (mode) {
        case FrameflowRenderLoopMode::Starting:
            return "STARTING";
        case FrameflowRenderLoopMode::Idle:
            return "IDLE";
        case FrameflowRenderLoopMode::Paused:
            return "PAUSED";
        case FrameflowRenderLoopMode::Failed:
            return "FAILED";
        case FrameflowRenderLoopMode::Stopped:
        default:
            return "STOPPED";
    }
}

std::optional<std::string> FrameflowNullSurfaceHost::create(const FrameflowNativeSurfaceDesc& desc) {
    created_ = true;
    visible_ = true;
    bounds_ = desc.bounds;
    return std::nullopt;
}

std::optional<std::string> FrameflowNullSurfaceHost::resize(const FrameflowSurfaceBounds& bounds) {
    if (!created_) {
        return "null surface host must be created before resize";
    }
    bounds_ = bounds;
    return std::nullopt;
}

std::optional<std::string> FrameflowNullSurfaceHost::set_visible(const bool visible) {
    if (!created_) {
        return "null surface host must be created before set_visible";
    }
    visible_ = visible;
    return std::nullopt;
}

std::optional<std::string> FrameflowNullSurfaceHost::make_current() {
    if (!created_) {
        return "null surface host must be created before make_current";
    }
    return std::nullopt;
}

std::optional<std::string> FrameflowNullSurfaceHost::swap_buffers() {
    if (!created_) {
        return "null surface host must be created before swap_buffers";
    }
    return std::nullopt;
}

std::optional<std::string> FrameflowNullSurfaceHost::destroy() {
    created_ = false;
    return std::nullopt;
}

const char* FrameflowNullSurfaceHost::backend_name() const noexcept {
    return "null-surface-host";
}

std::string FrameflowNullSurfaceHost::diagnostics_summary() const {
    std::ostringstream out;
    out << "created=" << (created_ ? "true" : "false")
        << " visible=" << (visible_ ? "true" : "false")
        << " bounds=" << bounds_.x << "," << bounds_.y << "," << bounds_.width << "x" << bounds_.height;
    return out.str();
}

template <typename Mutation>
void FrameflowCommandQueue::enqueue_mutation(Mutation&& mutation) {
    {
        std::lock_guard lock(mutex_);
        mutation();
        command_high_watermark_ = std::max(command_high_watermark_, pending_command_count_);
    }
    condition_.notify_one();
}

void FrameflowCommandQueue::enqueue_bounds(const FrameflowSurfaceBounds bounds) {
    enqueue_mutation([this, bounds]() {
        if (!pending_bounds_.has_value()) {
            pending_command_count_ += 1u;
        }
        pending_bounds_ = bounds;
    });
}

void FrameflowCommandQueue::enqueue_visibility(const bool visible) {
    enqueue_mutation([this, visible]() {
        if (!pending_visible_.has_value()) {
            pending_command_count_ += 1u;
        }
        pending_visible_ = visible;
    });
}

void FrameflowCommandQueue::enqueue_paused(const bool paused) {
    enqueue_mutation([this, paused]() {
        if (!pending_paused_.has_value()) {
            pending_command_count_ += 1u;
        }
        pending_paused_ = paused;
    });
}

void FrameflowCommandQueue::enqueue_scene_snapshot(FrameflowSceneSnapshot snapshot) {
    enqueue_mutation([this, snapshot = std::move(snapshot)]() mutable {
        if (!pending_scene_snapshot_.has_value()) {
            pending_command_count_ += 1u;
        }
        pending_scene_snapshot_ = std::move(snapshot);
    });
}

void FrameflowCommandQueue::enqueue_frame_request() {
    enqueue_mutation([this]() {
        if (pending_frame_requests_ == 0u) {
            pending_command_count_ += 1u;
        }
        pending_frame_requests_ = std::min(
            pending_frame_requests_ + 1u,
            kMaxCoalescedFrameRequests
        );
    });
}

void FrameflowCommandQueue::enqueue_stop() {
    enqueue_mutation([this]() {
        if (!stop_requested_) {
            pending_command_count_ += 1u;
        }
        stop_requested_ = true;
    });
}

FrameflowQueuedCommands FrameflowCommandQueue::drain_locked() {
    FrameflowQueuedCommands commands;
    commands.stop_requested = stop_requested_;
    commands.bounds = pending_bounds_;
    commands.visible = pending_visible_;
    commands.paused = pending_paused_;
    commands.scene_snapshot = std::move(pending_scene_snapshot_);
    commands.frame_requests = pending_frame_requests_;
    commands.command_high_watermark = command_high_watermark_;

    stop_requested_ = false;
    pending_bounds_.reset();
    pending_visible_.reset();
    pending_paused_.reset();
    pending_scene_snapshot_.reset();
    pending_frame_requests_ = 0u;
    pending_command_count_ = 0u;
    return commands;
}

std::optional<FrameflowQueuedCommands> FrameflowCommandQueue::drain_ready() {
    std::lock_guard lock(mutex_);
    if (!stop_requested_ &&
        !pending_bounds_.has_value() &&
        !pending_visible_.has_value() &&
        !pending_paused_.has_value() &&
        !pending_scene_snapshot_.has_value() &&
        pending_frame_requests_ == 0u) {
        return std::nullopt;
    }
    return drain_locked();
}

FrameflowQueuedCommands FrameflowCommandQueue::wait_and_drain() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this]() {
        return stop_requested_ ||
            pending_bounds_.has_value() ||
            pending_visible_.has_value() ||
            pending_paused_.has_value() ||
            pending_scene_snapshot_.has_value() ||
            pending_frame_requests_ > 0u;
    });
    return drain_locked();
}

FrameflowRenderThread::FrameflowRenderThread(
    FrameflowNativeSurfaceDesc desc,
    std::unique_ptr<FrameflowSurfaceHost> surface_host
) : desc_(std::move(desc)), surface_host_(std::move(surface_host)) {
    std::string selection_reason;
    if (surface_host_ == nullptr) {
        surface_host_ = create_surface_host_for_desc(desc_, &selection_reason);
    } else {
        selection_reason = "selected injected surface host";
    }
    stats_.active_bounds = desc_.bounds;
    stats_.surface_host_backend = surface_host_ != nullptr ? surface_host_->backend_name() : "none";
    stats_.surface_host_diag = surface_host_ != nullptr ? surface_host_->diagnostics_summary() : "missing";
    stats_.visible = true;
    stats_.last_status = selection_reason;
}

FrameflowRenderThread::~FrameflowRenderThread() {
    stop();
}

bool FrameflowRenderThread::start(std::string* error_message) {
    if (worker_.joinable()) {
        if (error_message != nullptr) {
            *error_message = "render thread already started";
        }
        return false;
    }

    {
        std::lock_guard lock(start_mutex_);
        start_completed_ = false;
        start_succeeded_ = false;
        start_error_message_.clear();
    }

    worker_ = std::thread([this]() { run(); });

    std::unique_lock lock(start_mutex_);
    start_condition_.wait(lock, [this]() { return start_completed_; });
    if (!start_succeeded_ && error_message != nullptr) {
        *error_message = start_error_message_;
    }
    return start_succeeded_;
}

void FrameflowRenderThread::update_bounds(const FrameflowSurfaceBounds bounds) {
    command_queue_.enqueue_bounds(bounds);
}

void FrameflowRenderThread::set_visible(const bool visible) {
    command_queue_.enqueue_visibility(visible);
}

void FrameflowRenderThread::set_paused(const bool paused) {
    command_queue_.enqueue_paused(paused);
}

void FrameflowRenderThread::update_scene_snapshot(FrameflowSceneSnapshot snapshot) {
    command_queue_.enqueue_scene_snapshot(std::move(snapshot));
}

void FrameflowRenderThread::request_frame() {
    command_queue_.enqueue_frame_request();
}

std::vector<FrameflowSurfaceEvent> FrameflowRenderThread::drain_surface_events() {
    std::lock_guard lock(surface_events_mutex_);
    auto events = std::move(pending_surface_events_);
    pending_surface_events_.clear();
    return events;
}

void FrameflowRenderThread::stop() {
    if (stop_called_) {
        if (worker_.joinable()) {
            worker_.join();
        }
        return;
    }
    stop_called_ = true;
    command_queue_.enqueue_stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}

FrameflowRenderStats FrameflowRenderThread::stats_snapshot() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

std::string FrameflowRenderThread::diagnostics_summary() const {
    const auto stats = stats_snapshot();
    std::ostringstream out;
    out << "mode=" << frameflow_render_loop_mode_name(stats.loop_mode)
        << " worker_running=" << (stats.worker_running ? "true" : "false")
        << " surface_created=" << (stats.surface_created ? "true" : "false")
        << " visible=" << (stats.visible ? "true" : "false")
        << " paused=" << (stats.paused ? "true" : "false")
        << " backend=" << stats.surface_host_backend
        << " active_bounds=" << stats.active_bounds.x
        << "," << stats.active_bounds.y
        << "," << stats.active_bounds.width
        << "x" << stats.active_bounds.height
        << "@" << stats.active_bounds.scale_factor
        << " thread_start_count=" << stats.thread_start_count
        << " thread_stop_count=" << stats.thread_stop_count
        << " wake_count=" << stats.wake_count
        << " frame_request_count=" << stats.frame_request_count
        << " create_count=" << stats.create_count
        << " resize_count=" << stats.resize_count
        << " visibility_change_count=" << stats.visibility_change_count
        << " scene_update_count=" << stats.scene_update_count
        << " make_current_count=" << stats.make_current_count
        << " present_count=" << stats.present_count
        << " destroy_count=" << stats.destroy_count
        << " command_high_watermark=" << stats.command_high_watermark
        << " host_diag=[" << stats.surface_host_diag << "]"
        << " status=" << stats.last_status;
    if (!stats.failure_message.empty()) {
        out << " failure_message=" << stats.failure_message;
    }
    return out.str();
}

void FrameflowRenderThread::set_failed_state(std::string message) {
    const std::string persisted_message = std::move(message);
    {
        std::lock_guard stats_lock(stats_mutex_);
        stats_.worker_running = false;
        stats_.surface_created = false;
        stats_.loop_mode = FrameflowRenderLoopMode::Failed;
        stats_.last_status = persisted_message;
        stats_.failure_message = persisted_message;
        if (surface_host_ != nullptr) {
            stats_.surface_host_diag = surface_host_->diagnostics_summary();
        }
    }
    {
        std::lock_guard start_lock(start_mutex_);
        start_completed_ = true;
        start_succeeded_ = false;
        start_error_message_ = persisted_message;
    }
    start_condition_.notify_all();
}

void FrameflowRenderThread::run() {
    {
        std::lock_guard lock(stats_mutex_);
        stats_.worker_running = true;
        stats_.thread_start_count += 1u;
        stats_.loop_mode = FrameflowRenderLoopMode::Starting;
        stats_.last_status = "starting";
    }

    if (surface_host_ == nullptr) {
        set_failed_state("render thread requires a surface host");
        return;
    }
    if (const auto create_error = surface_host_->create(desc_); create_error.has_value()) {
        set_failed_state(*create_error);
        return;
    }

    {
        std::lock_guard stats_lock(stats_mutex_);
        stats_.surface_created = true;
        stats_.create_count += 1u;
        stats_.loop_mode = FrameflowRenderLoopMode::Idle;
        stats_.surface_host_diag = surface_host_->diagnostics_summary();
        stats_.last_status = "ready";
    }
    {
        std::lock_guard start_lock(start_mutex_);
        start_completed_ = true;
        start_succeeded_ = true;
        start_error_message_.clear();
    }
    start_condition_.notify_all();

    while (true) {
        const auto commands = command_queue_.wait_and_drain();
        {
            std::lock_guard lock(stats_mutex_);
            stats_.wake_count += 1u;
            stats_.command_high_watermark = std::max(
                stats_.command_high_watermark,
                commands.command_high_watermark
            );
        }
        if (commands.stop_requested) {
            const auto stats_before_stop = stats_snapshot();
            if (stats_before_stop.surface_created && stats_before_stop.visible) {
                if (const auto visibility_error = surface_host_->set_visible(false); visibility_error.has_value()) {
                    set_failed_state(*visibility_error);
                } else {
                    std::lock_guard lock(stats_mutex_);
                    stats_.visible = false;
                    stats_.visibility_change_count += 1u;
                    stats_.surface_host_diag = surface_host_->diagnostics_summary();
                    stats_.last_status = "hidden-before-stop";
                }
            }
            break;
        }

        if (commands.bounds.has_value()) {
            if (const auto resize_error = surface_host_->resize(*commands.bounds); resize_error.has_value()) {
                set_failed_state(*resize_error);
                break;
            }
            std::lock_guard lock(stats_mutex_);
            stats_.active_bounds = *commands.bounds;
            stats_.resize_count += 1u;
            stats_.surface_host_diag = surface_host_->diagnostics_summary();
            stats_.last_status = "bounds-updated";
        }

        if (commands.visible.has_value()) {
            if (const auto visibility_error = surface_host_->set_visible(*commands.visible); visibility_error.has_value()) {
                set_failed_state(*visibility_error);
                break;
            }
            std::lock_guard lock(stats_mutex_);
            stats_.visible = *commands.visible;
            stats_.visibility_change_count += 1u;
            stats_.surface_host_diag = surface_host_->diagnostics_summary();
            stats_.last_status = *commands.visible ? "visible" : "hidden";
        }

        if (commands.paused.has_value()) {
            std::lock_guard lock(stats_mutex_);
            stats_.paused = *commands.paused;
            stats_.loop_mode = stats_.paused ? FrameflowRenderLoopMode::Paused : FrameflowRenderLoopMode::Idle;
            stats_.last_status = stats_.paused ? "paused" : "resumed";
        }

        if (commands.scene_snapshot.has_value()) {
            if (const auto scene_error = surface_host_->update_scene(*commands.scene_snapshot); scene_error.has_value()) {
                set_failed_state(*scene_error);
                break;
            }
            std::lock_guard lock(stats_mutex_);
            stats_.scene_update_count += 1u;
            stats_.surface_host_diag = surface_host_->diagnostics_summary();
            stats_.last_status = "scene-updated";
        }

        if (commands.frame_requests > 0u) {
            std::lock_guard lock(stats_mutex_);
            stats_.frame_request_count += commands.frame_requests;
        }

        auto stats_before_present = stats_snapshot();
        std::uint64_t pending_presents = commands.frame_requests;
        while (pending_presents > 0u &&
            stats_before_present.surface_created &&
            stats_before_present.visible &&
            !stats_before_present.paused) {
            if (const auto make_current_error = surface_host_->make_current(); make_current_error.has_value()) {
                set_failed_state(*make_current_error);
                break;
            }
            if (const auto swap_error = surface_host_->swap_buffers(); swap_error.has_value()) {
                set_failed_state(*swap_error);
                break;
            }

            bool had_surface_events = false;
            if (auto surface_events = surface_host_->drain_events(); !surface_events.empty()) {
                had_surface_events = true;
                std::lock_guard events_lock(surface_events_mutex_);
                pending_surface_events_.insert(
                    pending_surface_events_.end(),
                    std::make_move_iterator(surface_events.begin()),
                    std::make_move_iterator(surface_events.end())
                );
            }

            {
                std::lock_guard lock(stats_mutex_);
                stats_.make_current_count += 1u;
                stats_.present_count += 1u;
                stats_.loop_mode = FrameflowRenderLoopMode::Idle;
                stats_.surface_host_diag = surface_host_->diagnostics_summary();
                stats_.last_status = "presented";
            }

            pending_presents -= 1u;
            if (had_surface_events) {
                pending_presents = std::max<std::uint64_t>(pending_presents, 1u);
            }
            stats_before_present = stats_snapshot();
        }
    }

    if (const auto destroy_error = surface_host_->destroy(); destroy_error.has_value()) {
        std::lock_guard lock(stats_mutex_);
        stats_.last_status = *destroy_error;
        stats_.failure_message = *destroy_error;
        stats_.loop_mode = FrameflowRenderLoopMode::Failed;
    } else {
        std::lock_guard lock(stats_mutex_);
        if (stats_.loop_mode != FrameflowRenderLoopMode::Failed) {
            stats_.last_status = "stopped";
            stats_.loop_mode = FrameflowRenderLoopMode::Stopped;
        }
        stats_.surface_host_diag = surface_host_->diagnostics_summary();
    }

    {
        std::lock_guard lock(stats_mutex_);
        stats_.surface_created = false;
        stats_.worker_running = false;
        stats_.destroy_count += 1u;
        stats_.thread_stop_count += 1u;
    }
}

} // namespace frameflow
