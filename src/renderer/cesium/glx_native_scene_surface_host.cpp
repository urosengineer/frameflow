#include "renderer/cesium/glx_native_scene_surface_host.hpp"

#include <CesiumGeospatial/Cartographic.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <utility>

namespace frameflow::renderer::cesium {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegreesToRadians = kPi / 180.0;
constexpr double kRadiansToDegrees = 180.0 / kPi;
constexpr double kWgs84RadiusMeters = 6'378'137.0;
constexpr double kHorizontalFieldOfViewDegrees = 55.0;
constexpr double kDefaultMinCameraHeightMeters = 50.0;
constexpr double kAbsoluteMinCameraHeightMeters = 5.0;
constexpr double kMaxCameraHeightMeters = 28'000'000.0;
constexpr double kCameraZoomInMultiplier = 0.70;
constexpr double kCameraZoomOutMultiplier = 1.0 / kCameraZoomInMultiplier;
constexpr double kMaxPanLongitudeSpanDegrees = 300.0;
constexpr double kMaxPanLatitudeSpanDegrees = 165.0;
constexpr int kDragStartThresholdPixels = 5;
constexpr const char* kMinCameraHeightMetersEnv = "FRAMEFLOW_MIN_CAMERA_HEIGHT_METERS";

double normalize_heading_degrees(double value) {
    while (value <= -180.0) {
        value += 360.0;
    }
    while (value > 180.0) {
        value -= 360.0;
    }
    return value;
}

double min_camera_height_meters() {
    const char* value = std::getenv(kMinCameraHeightMetersEnv);
    if (value == nullptr || value[0] == '\0') {
        return kDefaultMinCameraHeightMeters;
    }

    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || !std::isfinite(parsed)) {
        return kDefaultMinCameraHeightMeters;
    }
    return std::clamp(parsed, kAbsoluteMinCameraHeightMeters, kMaxCameraHeightMeters - 1.0);
}

RenderedGlobeScene::CameraState to_scene_camera_state(const FrameflowCameraState& camera) {
    return RenderedGlobeScene::CameraState{
        .longitude = camera.longitude,
        .latitude = camera.latitude,
        .height_meters = camera.height_meters,
        .heading_degrees = camera.heading_degrees,
        .pitch_degrees = camera.pitch_degrees,
        .roll_degrees = camera.roll_degrees,
    };
}

CesiumGeospatial::Cartographic initial_cartographic(const FrameflowSceneSnapshot* snapshot = nullptr) {
    const FrameflowCameraState camera = snapshot != nullptr && snapshot->camera.has_value()
        ? *snapshot->camera
        : FrameflowCameraState{};
    return CesiumGeospatial::Cartographic::fromDegrees(
        camera.longitude,
        camera.latitude,
        camera.height_meters
    );
}

double meters_to_degrees(const double meters) {
    return (meters / kWgs84RadiusMeters) * kRadiansToDegrees;
}

double horizontal_field_of_view_radians() {
    return kHorizontalFieldOfViewDegrees * kDegreesToRadians;
}

double vertical_field_of_view_radians(const int width, const int height) {
    const double aspect_ratio = static_cast<double>(std::max(1, width)) /
        static_cast<double>(std::max(1, height));
    return 2.0 * std::atan(
        std::tan(horizontal_field_of_view_radians() * 0.5) /
        std::max(0.0001, aspect_ratio)
    );
}

double ground_span_meters(const double camera_height_meters, const double field_of_view_radians) {
    return 2.0 * std::max(1.0, camera_height_meters) * std::tan(field_of_view_radians * 0.5);
}

double pan_longitude_span_degrees(
    const RenderedGlobeScene::CameraState& camera
) {
    const double latitude_radians = camera.latitude * kDegreesToRadians;
    const double latitude_scale = std::max(0.2, std::abs(std::cos(latitude_radians)));
    const double span = meters_to_degrees(
        ground_span_meters(camera.height_meters, horizontal_field_of_view_radians())
    ) / latitude_scale;
    return std::clamp(span, 0.0, kMaxPanLongitudeSpanDegrees);
}

double pan_latitude_span_degrees(
    const RenderedGlobeScene::CameraState& camera,
    const FrameflowSurfaceBounds& bounds
) {
    const double span = meters_to_degrees(
        ground_span_meters(camera.height_meters, vertical_field_of_view_radians(bounds.width, bounds.height))
    );
    return std::clamp(span, 0.0, kMaxPanLatitudeSpanDegrees);
}

} // namespace

class GlxNativeSceneSurfaceHost::Impl {
public:
    explicit Impl(const std::uint64_t parent_window)
        : parent_window(static_cast<::Window>(parent_window)) {}

    ~Impl() {
        destroy();
    }

    std::optional<std::string> create(const FrameflowNativeSurfaceDesc& desc) {
        display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            return "XOpenDisplay failed for GLX native scene surface host";
        }

        screen = DefaultScreen(display);
        bounds = desc.bounds;
        visible = true;

        int attributes[] = {
            GLX_RGBA,
            GLX_DOUBLEBUFFER,
            GLX_DEPTH_SIZE,
            24,
            GLX_STENCIL_SIZE,
            8,
            None
        };
        visual_info = glXChooseVisual(display, screen, attributes);
        if (visual_info == nullptr) {
            int fallback_attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None};
            visual_info = glXChooseVisual(display, screen, fallback_attributes);
        }
        if (visual_info == nullptr) {
            return "glXChooseVisual failed for GLX native scene surface host";
        }

        colormap = XCreateColormap(display, parent_window, visual_info->visual, AllocNone);
        if (colormap == 0u) {
            return "XCreateColormap failed for GLX native scene surface host";
        }

        XSetWindowAttributes window_attributes{};
        window_attributes.colormap = colormap;
        window_attributes.background_pixel = BlackPixel(display, screen);
        window_attributes.border_pixel = 0u;
        window_attributes.event_mask =
            ExposureMask |
            StructureNotifyMask |
            ButtonPressMask |
            ButtonReleaseMask |
            ButtonMotionMask |
            PointerMotionMask;

        child_window = XCreateWindow(
            display,
            parent_window,
            bounds.x,
            bounds.y,
            static_cast<unsigned int>(std::max(1, bounds.width)),
            static_cast<unsigned int>(std::max(1, bounds.height)),
            0u,
            visual_info->depth,
            InputOutput,
            visual_info->visual,
            CWColormap | CWBackPixel | CWBorderPixel | CWEventMask,
            &window_attributes
        );
        if (child_window == 0u) {
            return "XCreateWindow failed for GLX native scene surface host";
        }

        context = glXCreateContext(display, visual_info, nullptr, True);
        if (context == nullptr) {
            return "glXCreateContext failed for GLX native scene surface host";
        }
        if (!glXMakeCurrent(display, child_window, context)) {
            return "glXMakeCurrent failed for GLX native scene surface host";
        }

        scene = std::make_unique<RenderedGlobeScene>(initial_cartographic());
        scene->resize(std::max(1, bounds.width), std::max(1, bounds.height));

        XMapRaised(display, child_window);
        XSync(display, False);
        created = true;
        status = "created";
        return std::nullopt;
    }

    std::optional<std::string> resize(const FrameflowSurfaceBounds& next_bounds) {
        if (!created) {
            return "GLX native scene surface host must be created before resize";
        }
        bounds = next_bounds;
        XMoveResizeWindow(
            display,
            child_window,
            bounds.x,
            bounds.y,
            static_cast<unsigned int>(std::max(1, bounds.width)),
            static_cast<unsigned int>(std::max(1, bounds.height))
        );
        XSync(display, False);
        if (scene) {
            scene->resize(std::max(1, bounds.width), std::max(1, bounds.height));
        }
        status = "resized";
        return std::nullopt;
    }

    std::optional<std::string> set_visible(const bool next_visible) {
        if (!created) {
            return "GLX native scene surface host must be created before set_visible";
        }
        visible = next_visible;
        if (visible) {
            XMapRaised(display, child_window);
        } else {
            XUnmapWindow(display, child_window);
        }
        XSync(display, False);
        status = visible ? "visible" : "hidden";
        return std::nullopt;
    }

    std::optional<std::string> update_scene(const FrameflowSceneSnapshot& snapshot) {
        if (!created || !scene) {
            return "GLX native scene surface host must be created before update_scene";
        }
        scene->set_points(snapshot.points);
        scene->set_selected_location(snapshot.selected_location_id);
        scene->set_focus_location(snapshot.focus_location_id);
        if (snapshot.camera.has_value()) {
            scene->set_camera_state(to_scene_camera_state(*snapshot.camera));
        }
        status = "scene-updated";
        return std::nullopt;
    }

    std::optional<std::string> make_current() {
        if (!created || display == nullptr || context == nullptr || child_window == 0u) {
            return "GLX native scene surface host must be created before make_current";
        }
        if (!glXMakeCurrent(display, child_window, context)) {
            return "glXMakeCurrent failed for GLX native scene surface host";
        }
        status = "current";
        return std::nullopt;
    }

    std::optional<std::string> swap_buffers() {
        if (!created || scene == nullptr) {
            return "GLX native scene surface host must be created before swap_buffers";
        }
        process_pending_x11_events();
        scene->render_frame();
        glXSwapBuffers(display, child_window);
        frame_counter += 1u;
        status = "presented";
        return std::nullopt;
    }

    std::vector<FrameflowSurfaceEvent> drain_events() {
        auto events = std::move(pending_events);
        pending_events.clear();
        return events;
    }

    std::optional<std::string> destroy() {
        scene.reset();
        if (display != nullptr && context != nullptr) {
            glXMakeCurrent(display, None, nullptr);
        }
        if (display != nullptr && child_window != 0u) {
            XUnmapWindow(display, child_window);
            XSync(display, False);
        }
        if (display != nullptr && context != nullptr) {
            glXDestroyContext(display, context);
            context = nullptr;
        }
        if (display != nullptr && child_window != 0u) {
            XDestroyWindow(display, child_window);
            XSync(display, False);
            child_window = 0u;
        }
        if (display != nullptr && colormap != 0u) {
            XFreeColormap(display, colormap);
            colormap = 0u;
        }
        if (visual_info != nullptr) {
            XFree(visual_info);
            visual_info = nullptr;
        }
        if (display != nullptr) {
            XCloseDisplay(display);
            display = nullptr;
        }
        created = false;
        visible = true;
        status = "destroyed";
        return std::nullopt;
    }

    std::string diagnostics_summary() const {
        std::ostringstream out;
        out << "created=" << (created ? "true" : "false")
            << " visible=" << (visible ? "true" : "false")
            << " dragging=" << (dragging ? "true" : "false")
            << " parent_window=" << (parent_window != 0u ? "present" : "none")
            << " child_window=" << (child_window != 0u ? "present" : "none")
            << " frame_counter=" << frame_counter
            << " input_events=" << input_event_count
            << " bounds=" << bounds.x << "," << bounds.y << "," << bounds.width << "x" << bounds.height
            << " status=" << status;
        if (scene) {
            out << " scene=[" << scene->diagnostics_summary() << "]";
        }
        return out.str();
    }

    void process_pending_x11_events() {
        if (!created || display == nullptr || child_window == 0u || scene == nullptr) {
            return;
        }

        int guard = 0;
        while (XPending(display) > 0 && guard < 512) {
            guard += 1;
            XEvent event{};
            XNextEvent(display, &event);
            if (event.xany.window != child_window) {
                continue;
            }
            input_event_count += 1u;

            switch (event.type) {
                case ConfigureNotify:
                    bounds.width = std::max(1, event.xconfigure.width);
                    bounds.height = std::max(1, event.xconfigure.height);
                    scene->resize(bounds.width, bounds.height);
                    break;

                case ButtonPress:
                    handle_button_press(event.xbutton);
                    break;

                case ButtonRelease:
                    handle_button_release(event.xbutton);
                    break;

                case MotionNotify:
                    handle_motion(event.xmotion);
                    break;

                default:
                    break;
            }
        }
    }

    void handle_button_press(const XButtonEvent& event) {
        if (event.button == Button1) {
            if (const auto hud_action = scene->hud_hit_test(static_cast<double>(event.x), static_cast<double>(event.y));
                hud_action.has_value()) {
                if (*hud_action == RenderedGlobeScene::HudAction::ResetOrientation) {
                    compass_pressed = true;
                    compass_dragging = false;
                    press_compass_x = event.x;
                    press_compass_y = event.y;
                    last_pointer_x = event.x;
                    last_pointer_y = event.y;
                    pointer_pressed = false;
                    dragging = false;
                    return;
                }
                pointer_pressed = false;
                dragging = false;
                last_pointer_x = event.x;
                last_pointer_y = event.y;
                if (scene->activate_hud_action(*hud_action)) {
                    queue_camera_changed_event();
                }
                return;
            }
            pointer_pressed = true;
            dragging = false;
            press_pointer_x = event.x;
            press_pointer_y = event.y;
            last_pointer_x = event.x;
            last_pointer_y = event.y;
            return;
        }

        if (event.button == Button3) {
            orientation_pressed = true;
            orienting = false;
            press_orientation_x = event.x;
            press_orientation_y = event.y;
            last_pointer_x = event.x;
            last_pointer_y = event.y;
            return;
        }

        if (event.button != Button4 && event.button != Button5) {
            return;
        }

        const auto camera = scene->camera_state();
        const double multiplier = event.button == Button4 ? kCameraZoomInMultiplier : kCameraZoomOutMultiplier;
        const double next_height = std::clamp(
            camera.height_meters * multiplier,
            min_camera_height_meters(),
            kMaxCameraHeightMeters
        );
        if (std::abs(next_height - camera.height_meters) <= 1.0) {
            return;
        }
        if (scene->adjust_camera(0.0, 0.0, next_height - camera.height_meters)) {
            queue_camera_changed_event();
        }
    }

    void handle_button_release(const XButtonEvent& event) {
        if (event.button == Button3) {
            orientation_pressed = false;
            orienting = false;
            return;
        }

        if (event.button != Button1) {
            return;
        }

        if (compass_pressed || compass_dragging) {
            const bool was_compass_dragging = compass_dragging;
            compass_pressed = false;
            compass_dragging = false;
            if (!was_compass_dragging && scene->activate_hud_action(RenderedGlobeScene::HudAction::ResetOrientation)) {
                queue_camera_changed_event();
            }
            return;
        }

        if (!pointer_pressed && !dragging) {
            return;
        }

        const bool was_dragging = dragging;
        pointer_pressed = false;
        dragging = false;
        if (was_dragging) {
            return;
        }

        const auto hit = scene->hit_test(static_cast<double>(event.x), static_cast<double>(event.y));
        if (!hit.has_value()) {
            scene->set_selected_location(std::nullopt);
            FrameflowSurfaceEvent surface_event;
            surface_event.type = FrameflowSurfaceEvent::Type::SelectionCleared;
            pending_events.push_back(std::move(surface_event));
            return;
        }

        if (hit->kind == RenderedGlobeScene::InteractionHit::Kind::Location) {
            if (scene->selected_location_id().has_value() && *scene->selected_location_id() == hit->target_id) {
                scene->set_selected_location(std::nullopt);
                FrameflowSurfaceEvent surface_event;
                surface_event.type = FrameflowSurfaceEvent::Type::SelectionCleared;
                pending_events.push_back(std::move(surface_event));
                return;
            }

            scene->set_selected_location(hit->target_id);
            FrameflowSurfaceEvent surface_event;
            surface_event.type = FrameflowSurfaceEvent::Type::LocationSelected;
            surface_event.location_id = hit->target_id;
            surface_event.category_code = hit->primary_category;
            surface_event.screen_x = static_cast<double>(event.x);
            surface_event.screen_y = static_cast<double>(event.y);
            surface_event.interaction = "click";
            pending_events.push_back(std::move(surface_event));
            return;
        }

        scene->set_selected_location(std::nullopt);
        FrameflowSurfaceEvent surface_event;
        surface_event.type = FrameflowSurfaceEvent::Type::ClusterSelected;
        surface_event.cluster_id = hit->target_id;
        surface_event.point_count = hit->point_count;
        surface_event.longitude = hit->longitude;
        surface_event.latitude = hit->latitude;
        pending_events.push_back(std::move(surface_event));
    }

    void handle_motion(const XMotionEvent& event) {
        if (!pointer_pressed && !dragging && !orientation_pressed && !orienting && !compass_pressed && !compass_dragging) {
            return;
        }

        const int delta_x = event.x - last_pointer_x;
        const int delta_y = event.y - last_pointer_y;
        if (compass_pressed || compass_dragging) {
            if (!compass_dragging) {
                const int press_delta_x = event.x - press_compass_x;
                const int press_delta_y = event.y - press_compass_y;
                if ((press_delta_x * press_delta_x) + (press_delta_y * press_delta_y) <
                    (kDragStartThresholdPixels * kDragStartThresholdPixels)) {
                    return;
                }
                compass_dragging = true;
            }
            last_pointer_x = event.x;
            last_pointer_y = event.y;
            update_heading_from_compass_pointer(static_cast<double>(event.x), static_cast<double>(event.y));
            return;
        }
        if (orientation_pressed || orienting) {
            if (!orienting) {
                const int press_delta_x = event.x - press_orientation_x;
                const int press_delta_y = event.y - press_orientation_y;
                if ((press_delta_x * press_delta_x) + (press_delta_y * press_delta_y) <
                    (kDragStartThresholdPixels * kDragStartThresholdPixels)) {
                    return;
                }
                orienting = true;
            }
            last_pointer_x = event.x;
            last_pointer_y = event.y;
            if (delta_x == 0) {
                return;
            }
            const double heading_degrees = -(
                static_cast<double>(delta_x) /
                static_cast<double>(std::max(1, bounds.width))
            ) * 180.0;
            if (scene->adjust_orientation(heading_degrees, 0.0, 0.0)) {
                queue_camera_changed_event();
            }
            return;
        }

        if (!dragging) {
            const int press_delta_x = event.x - press_pointer_x;
            const int press_delta_y = event.y - press_pointer_y;
            if ((press_delta_x * press_delta_x) + (press_delta_y * press_delta_y) <
                (kDragStartThresholdPixels * kDragStartThresholdPixels)) {
                return;
            }
            dragging = true;
        }
        last_pointer_x = event.x;
        last_pointer_y = event.y;
        if (delta_x == 0 && delta_y == 0) {
            return;
        }

        const auto camera = scene->camera_state();
        const double longitude_degrees = -(
            static_cast<double>(delta_x) /
            static_cast<double>(std::max(1, bounds.width))
        ) * pan_longitude_span_degrees(camera);
        const double latitude_degrees = (
            static_cast<double>(delta_y) /
            static_cast<double>(std::max(1, bounds.height))
        ) * pan_latitude_span_degrees(camera, bounds);
        if (scene->adjust_camera(longitude_degrees, latitude_degrees, 0.0)) {
            queue_camera_changed_event();
        }
    }

    void queue_camera_changed_event() {
        if (!scene) {
            return;
        }
        const auto camera = scene->camera_state();
        FrameflowSurfaceEvent event;
        event.type = FrameflowSurfaceEvent::Type::CameraChanged;
        event.longitude = camera.longitude;
        event.latitude = camera.latitude;
        event.height_meters = camera.height_meters;
        event.heading_degrees = camera.heading_degrees;
        event.pitch_degrees = camera.pitch_degrees;
        event.roll_degrees = camera.roll_degrees;
        pending_events.push_back(std::move(event));
    }

    void update_heading_from_compass_pointer(const double pointer_x, const double pointer_y) {
        const auto compass_center = scene->hud_action_screen_position(RenderedGlobeScene::HudAction::ResetOrientation);
        if (!compass_center.has_value()) {
            return;
        }
        const double delta_x = pointer_x - compass_center->x;
        const double delta_y = pointer_y - compass_center->y;
        if ((delta_x * delta_x) + (delta_y * delta_y) <= 4.0) {
            return;
        }

        auto camera = scene->camera_state();
        const double next_heading = normalize_heading_degrees(
            -(std::atan2(delta_x, -delta_y) * 180.0 / M_PI)
        );
        if (std::abs(normalize_heading_degrees(next_heading - camera.heading_degrees)) <= 0.25) {
            return;
        }
        camera.heading_degrees = next_heading;
        scene->set_camera_state(camera);
        queue_camera_changed_event();
    }

    Display* display{nullptr};
    int screen{0};
    XVisualInfo* visual_info{nullptr};
    Colormap colormap{0u};
    ::Window parent_window{0u};
    ::Window child_window{0u};
    GLXContext context{nullptr};
    std::unique_ptr<RenderedGlobeScene> scene;
    FrameflowSurfaceBounds bounds{};
    bool created{false};
    bool visible{true};
    bool pointer_pressed{false};
    bool orientation_pressed{false};
    bool compass_pressed{false};
    bool dragging{false};
    bool orienting{false};
    bool compass_dragging{false};
    int press_pointer_x{0};
    int press_pointer_y{0};
    int press_orientation_x{0};
    int press_orientation_y{0};
    int press_compass_x{0};
    int press_compass_y{0};
    int last_pointer_x{0};
    int last_pointer_y{0};
    std::uint64_t frame_counter{0u};
    std::uint64_t input_event_count{0u};
    std::vector<FrameflowSurfaceEvent> pending_events;
    std::string status{"idle"};
};

GlxNativeSceneSurfaceHost::GlxNativeSceneSurfaceHost(const std::uint64_t parent_window)
    : impl_(std::make_unique<Impl>(parent_window)) {}

GlxNativeSceneSurfaceHost::~GlxNativeSceneSurfaceHost() = default;

std::optional<std::string> GlxNativeSceneSurfaceHost::create(const FrameflowNativeSurfaceDesc& desc) {
    return impl_->create(desc);
}

std::optional<std::string> GlxNativeSceneSurfaceHost::resize(const FrameflowSurfaceBounds& bounds) {
    return impl_->resize(bounds);
}

std::optional<std::string> GlxNativeSceneSurfaceHost::set_visible(const bool visible) {
    return impl_->set_visible(visible);
}

std::optional<std::string> GlxNativeSceneSurfaceHost::update_scene(const FrameflowSceneSnapshot& snapshot) {
    return impl_->update_scene(snapshot);
}

std::optional<std::string> GlxNativeSceneSurfaceHost::make_current() {
    return impl_->make_current();
}

std::optional<std::string> GlxNativeSceneSurfaceHost::swap_buffers() {
    return impl_->swap_buffers();
}

std::vector<FrameflowSurfaceEvent> GlxNativeSceneSurfaceHost::drain_events() {
    return impl_->drain_events();
}

std::optional<std::string> GlxNativeSceneSurfaceHost::destroy() {
    return impl_->destroy();
}

const char* GlxNativeSceneSurfaceHost::backend_name() const noexcept {
    return "glx-native-scene-host";
}

std::string GlxNativeSceneSurfaceHost::diagnostics_summary() const {
    return impl_->diagnostics_summary();
}

} // namespace frameflow::renderer::cesium
