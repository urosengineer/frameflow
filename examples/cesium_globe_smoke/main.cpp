#include "frameflow/c/bridge.h"
#include "renderer/cesium/rendered_globe_scene.hpp"

#include <CesiumGeospatial/Cartographic.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct SmokeOptions {
    int width = 1280;
    int height = 720;
    int auto_exit_ms = 900;
    int cycles = 2;
    int dense_grid_size = 0;
};

struct CallbackTracker {
    std::size_t ready_count = 0;
    std::size_t location_count = 0;
    std::size_t cluster_count = 0;
    std::size_t camera_count = 0;
    std::size_t error_count = 0;
    std::optional<std::int64_t> selected_location_id;
    std::int64_t last_location_id = 0;
    std::string last_location_interaction;
    std::string last_location_category_code;
    double last_location_screen_x = 0.0;
    double last_location_screen_y = 0.0;
    std::int64_t last_cluster_id = 0;
    std::int32_t last_cluster_point_count = 0;
    double last_cluster_longitude = 0.0;
    double last_cluster_latitude = 0.0;
    double last_camera_longitude = 0.0;
    double last_camera_latitude = 0.0;
    double last_camera_height_meters = 0.0;
    std::string last_error_message;
};

struct DisplaySession {
    Display* display = nullptr;
    int screen = 0;

    ~DisplaySession() {
        if (display != nullptr) {
            XCloseDisplay(display);
        }
    }
};

struct VisualInfoSession {
    XVisualInfo* info = nullptr;

    ~VisualInfoSession() {
        if (info != nullptr) {
            XFree(info);
        }
    }
};

struct WindowSession {
    Display* display = nullptr;
    ::Window handle = 0;
    Colormap colormap = 0;
    Atom delete_atom = 0;
    int width = 0;
    int height = 0;

    ~WindowSession() {
        if (display != nullptr && handle != 0) {
            XDestroyWindow(display, handle);
            XFlush(display);
        }
        if (display != nullptr && colormap != 0) {
            XFreeColormap(display, colormap);
        }
    }
};

struct GlxContextSession {
    Display* display = nullptr;
    GLXContext context = nullptr;
    ::Window window = 0;

    ~GlxContextSession() {
        if (display != nullptr && context != nullptr) {
            glXMakeCurrent(display, None, nullptr);
            glXDestroyContext(display, context);
        }
    }
};

struct EngineSession {
    frameflow_engine* handle = nullptr;

    ~EngineSession() {
        if (handle != nullptr) {
            frameflow_engine_dispose(handle);
            frameflow_engine_destroy(handle);
        }
    }
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string copy_cstr(const char* value) {
    return value == nullptr ? std::string{} : std::string(value);
}

void on_ready(void* user, const frameflow_ready_event* /*event*/) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    tracker->ready_count += 1;
}

void on_location_selected(void* user, const frameflow_location_selection_event* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    tracker->location_count += 1;
    if (event != nullptr) {
        tracker->selected_location_id = event->location_id;
        tracker->last_location_id = event->location_id;
        tracker->last_location_interaction = copy_cstr(event->interaction);
        tracker->last_location_category_code = copy_cstr(event->category_code);
        tracker->last_location_screen_x = event->screen_x;
        tracker->last_location_screen_y = event->screen_y;
    }
}

void on_cluster_selected(void* user, const frameflow_cluster_selection_event* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    tracker->cluster_count += 1;
    tracker->selected_location_id.reset();
    if (event != nullptr) {
        tracker->last_cluster_id = event->cluster_id;
        tracker->last_cluster_point_count = event->point_count;
        tracker->last_cluster_longitude = event->longitude;
        tracker->last_cluster_latitude = event->latitude;
    }
}

void on_camera_changed(void* user, const frameflow_camera_state* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    tracker->camera_count += 1;
    if (event != nullptr) {
        tracker->last_camera_longitude = event->longitude;
        tracker->last_camera_latitude = event->latitude;
        tracker->last_camera_height_meters = event->height_meters;
    }
}

void on_error(void* user, const frameflow_error_event* event) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    tracker->error_count += 1;
    tracker->last_error_message = event == nullptr ? std::string{} : copy_cstr(event->message);
}

void require_ok(
    const frameflow_result result,
    frameflow_engine* engine,
    const std::string& operation
) {
    if (result == FRAMEFLOW_RESULT_OK) {
        return;
    }
    const std::string native_message = engine == nullptr
        ? std::string{}
        : copy_cstr(frameflow_engine_last_error_message(engine));
    fail(operation + " failed with code " + std::to_string(result) +
         (native_message.empty() ? std::string{} : ": " + native_message));
}

void report_scene_camera(
    frameflow_engine* engine,
    const frameflow::renderer::cesium::RenderedGlobeScene& scene
) {
    const auto camera = scene.camera_state();
    require_ok(
        frameflow_engine_report_camera_changed(
            engine,
            camera.longitude,
            camera.latitude,
            camera.height_meters,
            camera.heading_degrees,
            camera.pitch_degrees,
            camera.roll_degrees
        ),
        engine,
        "report_camera_changed"
    );
}

void dispatch_scene_hit(
    frameflow_engine* engine,
    frameflow::renderer::cesium::RenderedGlobeScene& scene,
    const double screen_x,
    const double screen_y
) {
    const auto hit = scene.hit_test(screen_x, screen_y);
    if (!hit.has_value()) {
        return;
    }

    if (hit->kind == frameflow::renderer::cesium::RenderedGlobeScene::InteractionHit::Kind::Location) {
        require_ok(
            frameflow_engine_select_location(
                engine,
                hit->target_id,
                hit->primary_category.has_value() ? hit->primary_category->c_str() : nullptr,
                screen_x,
                screen_y,
                "click"
            ),
            engine,
            "select_location"
        );
        return;
    }

    require_ok(
        frameflow_engine_select_cluster(
            engine,
            hit->target_id,
            hit->point_count,
            hit->longitude,
            hit->latitude
        ),
        engine,
        "select_cluster"
    );
}

std::vector<frameflow_point> build_points(const SmokeOptions& options) {
    static const char* belgrade_categories[] = {"POLITICS", "BREAKING"};
    static const char* novi_sad_categories[] = {"SPORT"};
    static const char* nis_categories[] = {"ECONOMY"};
    static const char* sarajevo_categories[] = {"CULTURE"};
    static const char* dense_grid_categories[] = {"POLITICS"};

    std::vector<frameflow_point> points;
    points.reserve(static_cast<std::size_t>(4 + (options.dense_grid_size * options.dense_grid_size)));
    points.push_back(frameflow_point{
        .location_id = 601,
        .label = "Belgrade",
        .kind = FRAMEFLOW_LOCATION_KIND_CITY,
        .country_code = "RS",
        .latitude = 44.7866,
        .longitude = 20.4489,
        .story_count = 8,
        .latest_story_epoch_millis = 1'776'335'400'000LL,
        .top_categories = belgrade_categories,
        .top_category_count = 2u,
        .style_key = "category-politics",
    });
    points.push_back(frameflow_point{
        .location_id = 602,
        .label = "Novi Sad",
        .kind = FRAMEFLOW_LOCATION_KIND_CITY,
        .country_code = "RS",
        .latitude = 45.2671,
        .longitude = 19.8335,
        .story_count = 5,
        .latest_story_epoch_millis = 1'776'333'000'000LL,
        .top_categories = novi_sad_categories,
        .top_category_count = 1u,
        .style_key = "category-sport",
    });
    points.push_back(frameflow_point{
        .location_id = 603,
        .label = "Nis",
        .kind = FRAMEFLOW_LOCATION_KIND_CITY,
        .country_code = "RS",
        .latitude = 43.3209,
        .longitude = 21.8958,
        .story_count = 3,
        .latest_story_epoch_millis = 1'776'328'200'000LL,
        .top_categories = nis_categories,
        .top_category_count = 1u,
        .style_key = "category-economy",
    });
    points.push_back(frameflow_point{
        .location_id = 604,
        .label = "Sarajevo",
        .kind = FRAMEFLOW_LOCATION_KIND_CITY,
        .country_code = "BA",
        .latitude = 43.8563,
        .longitude = 18.4131,
        .story_count = 6,
        .latest_story_epoch_millis = 1'776'330'600'000LL,
        .top_categories = sarajevo_categories,
        .top_category_count = 1u,
        .style_key = "category-culture",
    });

    if (options.dense_grid_size <= 0) {
        return points;
    }

    constexpr double grid_origin_latitude = 44.55;
    constexpr double grid_origin_longitude = 19.95;
    constexpr double grid_spacing_degrees = 0.08;
    for (int row = 0; row < options.dense_grid_size; ++row) {
        for (int column = 0; column < options.dense_grid_size; ++column) {
            const int index = (row * options.dense_grid_size) + column;
            points.push_back(frameflow_point{
                .location_id = 7000 + index,
                .label = "Dense Grid",
                .kind = FRAMEFLOW_LOCATION_KIND_CITY,
                .country_code = "RS",
                .latitude = grid_origin_latitude + (static_cast<double>(row) * grid_spacing_degrees),
                .longitude = grid_origin_longitude + (static_cast<double>(column) * grid_spacing_degrees),
                .story_count = 1 + ((index % 5) + 1),
                .latest_story_epoch_millis = 1'776'320'000'000LL + (static_cast<std::int64_t>(index) * 60'000LL),
                .top_categories = dense_grid_categories,
                .top_category_count = 1u,
                .style_key = "category-politics",
            });
        }
    }

    return points;
}

SmokeOptions parse_args(int argc, char** argv) {
    SmokeOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            std::cout
                << "Usage: frameflow_cesium_globe_smoke [--width=N] [--height=N] "
                << "[--auto-exit-ms=N] [--cycles=N] [--dense-grid-size=N]\n";
            std::exit(0);
        }
        if (argument.rfind("--width=", 0) == 0) {
            options.width = std::stoi(argument.substr(std::strlen("--width=")));
            continue;
        }
        if (argument.rfind("--height=", 0) == 0) {
            options.height = std::stoi(argument.substr(std::strlen("--height=")));
            continue;
        }
        if (argument.rfind("--auto-exit-ms=", 0) == 0) {
            options.auto_exit_ms = std::stoi(argument.substr(std::strlen("--auto-exit-ms=")));
            continue;
        }
        if (argument.rfind("--cycles=", 0) == 0) {
            options.cycles = std::stoi(argument.substr(std::strlen("--cycles=")));
            continue;
        }
        if (argument.rfind("--dense-grid-size=", 0) == 0) {
            options.dense_grid_size = std::stoi(argument.substr(std::strlen("--dense-grid-size=")));
            continue;
        }
        fail("Unknown argument: " + argument);
    }
    if (options.width <= 0 || options.height <= 0 || options.auto_exit_ms <= 0 || options.cycles <= 0 ||
        options.dense_grid_size < 0) {
        fail("width, height, auto-exit-ms, and cycles must be positive, and dense-grid-size must be zero or greater");
    }
    return options;
}

DisplaySession open_display() {
    DisplaySession session;
    session.display = XOpenDisplay(nullptr);
    if (session.display == nullptr) {
        fail("XOpenDisplay failed. Set DISPLAY or run under X11/Xvfb.");
    }
    session.screen = DefaultScreen(session.display);
    return session;
}

VisualInfoSession choose_visual(Display* display, int screen) {
    int attributes[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, None};
    VisualInfoSession visual;
    visual.info = glXChooseVisual(display, screen, attributes);
    if (visual.info == nullptr) {
        fail("glXChooseVisual failed");
    }
    return visual;
}

WindowSession create_window(
    Display* display,
    int screen,
    XVisualInfo* visual_info,
    const SmokeOptions& options,
    int cycle_index
) {
    WindowSession session;
    session.display = display;
    session.width = options.width;
    session.height = options.height;

    XSetWindowAttributes attributes{};
    attributes.colormap = XCreateColormap(display, RootWindow(display, screen), visual_info->visual, AllocNone);
    attributes.event_mask = StructureNotifyMask | FocusChangeMask | KeyPressMask | ExposureMask | ButtonPressMask;
    attributes.border_pixel = 0;

    session.colormap = attributes.colormap;
    session.handle = XCreateWindow(
        display,
        RootWindow(display, screen),
        0,
        0,
        static_cast<unsigned int>(session.width),
        static_cast<unsigned int>(session.height),
        0,
        visual_info->depth,
        InputOutput,
        visual_info->visual,
        CWBorderPixel | CWColormap | CWEventMask,
        &attributes
    );
    if (session.handle == 0) {
        fail("XCreateWindow failed");
    }

    XStoreName(display, session.handle, ("Frameflow Cesium Globe Smoke cycle " + std::to_string(cycle_index)).c_str());
    session.delete_atom = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, session.handle, &session.delete_atom, 1);
    XMapRaised(display, session.handle);
    XFlush(display);
    return session;
}

void wait_for_map(WindowSession& window) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        while (XPending(window.display) > 0) {
            XEvent event{};
            XNextEvent(window.display, &event);
            if (event.type == MapNotify && event.xmap.window == window.handle) {
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

GlxContextSession create_context(Display* display, XVisualInfo* visual_info, ::Window window) {
    GlxContextSession session;
    session.display = display;
    session.window = window;
    session.context = glXCreateContext(display, visual_info, nullptr, True);
    if (session.context == nullptr) {
        fail("glXCreateContext failed");
    }
    if (!glXMakeCurrent(display, window, session.context)) {
        fail("glXMakeCurrent failed");
    }
    return session;
}

void run_cycle(Display* display, int screen, const SmokeOptions& options, int cycle_index) {
    VisualInfoSession visual = choose_visual(display, screen);
    WindowSession window = create_window(display, screen, visual.info, options, cycle_index);
    wait_for_map(window);
    GlxContextSession context = create_context(display, visual.info, window.handle);

    CallbackTracker tracker{};
    const frameflow_callbacks callbacks{
        .user = &tracker,
        .on_ready = on_ready,
        .on_location_selected = on_location_selected,
        .on_cluster_selected = on_cluster_selected,
        .on_camera_changed = on_camera_changed,
        .on_engine_error = on_error,
    };

    EngineSession engine{.handle = frameflow_engine_create()};
    if (engine.handle == nullptr) {
        fail("frameflow_engine_create returned null");
    }
    require_ok(frameflow_engine_set_callbacks(engine.handle, &callbacks), engine.handle, "set_callbacks");

    const frameflow_options init_options{
        .theme = "dark",
        .tile_cache_path = "/tmp/frameflow-cesium-globe-smoke",
        .max_tile_cache_bytes = 64u * 1024u * 1024u,
        .log_level = "debug",
    };
    require_ok(
        frameflow_engine_initialize(
            engine.handle,
            static_cast<std::uintptr_t>(window.handle),
            window.width,
            window.height,
            1.0,
            &init_options
        ),
        engine.handle,
        "initialize"
    );
    if (frameflow_engine_dispatch_pending_callbacks(engine.handle) != 1u || tracker.ready_count != 1u) {
        fail("expected one ready callback after initialize");
    }

    const auto points = build_points(options);
    const auto expected_point_count = points.size();
    require_ok(
        frameflow_engine_set_points(
            engine.handle,
            points.data(),
            static_cast<std::uint64_t>(expected_point_count)
        ),
        engine.handle,
        "set_points"
    );
    require_ok(frameflow_engine_focus_location(engine.handle, 601), engine.handle, "focus_location");
    if (frameflow_engine_dispatch_pending_callbacks(engine.handle) != 1u || tracker.location_count != 1u) {
        fail("expected one location callback after programmatic focus");
    }

    frameflow::renderer::cesium::RenderedGlobeScene scene(
        CesiumGeospatial::Cartographic::fromDegrees(20.4489, 44.7866, 2'500'000.0)
    );
    scene.set_points(points.data(), static_cast<std::uint64_t>(expected_point_count));
    scene.set_selected_location(tracker.selected_location_id);
    scene.resize(window.width, window.height);
    report_scene_camera(engine.handle, scene);
    if (frameflow_engine_dispatch_pending_callbacks(engine.handle) != 1u || tracker.camera_count != 1u) {
        fail("expected one initial camera callback after reporting scene camera");
    }

    bool running = true;
    bool native_location_click_triggered = false;
    bool native_cluster_click_triggered = false;
    bool camera_motion_triggered = false;
    bool resize_requested = false;
    int last_width = window.width;
    int last_height = window.height;
    std::optional<std::int64_t> native_clicked_location_id;
    std::string native_clicked_category_code;
    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline = started_at + std::chrono::milliseconds(options.auto_exit_ms);

    while (running && std::chrono::steady_clock::now() < deadline) {
        if (!resize_requested &&
            std::chrono::steady_clock::now() - started_at > std::chrono::milliseconds(options.auto_exit_ms / 3)) {
            XResizeWindow(
                display,
                window.handle,
                static_cast<unsigned int>(window.width + 160),
                static_cast<unsigned int>(window.height + 90)
            );
            XFlush(display);
            resize_requested = true;
        }

        while (XPending(display) > 0) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == window.delete_atom) {
                running = false;
                continue;
            }
            if (event.type == ConfigureNotify && event.xconfigure.window == window.handle) {
                if (event.xconfigure.width != last_width || event.xconfigure.height != last_height) {
                    last_width = event.xconfigure.width;
                    last_height = event.xconfigure.height;
                    require_ok(
                        frameflow_engine_resize(engine.handle, last_width, last_height, 1.0),
                        engine.handle,
                        "resize"
                    );
                    scene.resize(last_width, last_height);
                }
                continue;
            }
            if (event.type == FocusOut && frameflow_engine_state(engine.handle) == FRAMEFLOW_LIFECYCLE_READY) {
                require_ok(frameflow_engine_pause(engine.handle), engine.handle, "focus pause");
                scene.pause();
                continue;
            }
            if (event.type == FocusIn && frameflow_engine_state(engine.handle) == FRAMEFLOW_LIFECYCLE_PAUSED) {
                require_ok(frameflow_engine_resume(engine.handle), engine.handle, "focus resume");
                scene.resume();
                continue;
            }
            if (event.type == KeyPress) {
                const auto key = XkbKeycodeToKeysym(display, static_cast<KeyCode>(event.xkey.keycode), 0, 0);
                if (key == XK_Escape || key == XK_q) {
                    running = false;
                } else if (key == XK_f) {
                    require_ok(frameflow_engine_focus_location(engine.handle, 601), engine.handle, "key focus");
                } else if (key == XK_c) {
                    if (const auto target = scene.first_cluster_screen_position(); target.has_value()) {
                        dispatch_scene_hit(engine.handle, scene, target->x, target->y);
                    }
                } else if (key == XK_Left) {
                    if (scene.adjust_camera(-0.8, 0.0, 0.0)) {
                        report_scene_camera(engine.handle, scene);
                    }
                } else if (key == XK_Right) {
                    if (scene.adjust_camera(0.8, 0.0, 0.0)) {
                        report_scene_camera(engine.handle, scene);
                    }
                } else if (key == XK_Up) {
                    if (scene.adjust_camera(0.0, 0.8, 0.0)) {
                        report_scene_camera(engine.handle, scene);
                    }
                } else if (key == XK_Down) {
                    if (scene.adjust_camera(0.0, -0.8, 0.0)) {
                        report_scene_camera(engine.handle, scene);
                    }
                } else if (key == XK_plus || key == XK_equal) {
                    if (scene.adjust_camera(0.0, 0.0, -300000.0)) {
                        report_scene_camera(engine.handle, scene);
                    }
                } else if (key == XK_minus || key == XK_underscore) {
                    if (scene.adjust_camera(0.0, 0.0, 300000.0)) {
                        report_scene_camera(engine.handle, scene);
                    }
                }
                continue;
            }
            if (event.type == ButtonPress) {
                if (event.xbutton.button == Button1) {
                    dispatch_scene_hit(
                        engine.handle,
                        scene,
                        static_cast<double>(event.xbutton.x),
                        static_cast<double>(event.xbutton.y)
                    );
                } else if (event.xbutton.button == Button3) {
                    require_ok(frameflow_engine_clear_selection(engine.handle), engine.handle, "clear_selection");
                    scene.set_selected_location(std::nullopt);
                }
            }
        }

        if (!native_location_click_triggered &&
            std::chrono::steady_clock::now() - started_at > std::chrono::milliseconds(options.auto_exit_ms / 2)) {
            const auto target = scene.first_location_screen_position();
            if (!target.has_value()) {
                fail("expected at least one visible standalone location screen position");
            }
            const auto hit = scene.hit_test(target->x, target->y);
            if (!hit.has_value() ||
                hit->kind != frameflow::renderer::cesium::RenderedGlobeScene::InteractionHit::Kind::Location ||
                hit->target_id <= 0) {
                fail("scene hit-test should resolve a visible standalone location");
            }
            native_clicked_location_id = hit->target_id;
            native_clicked_category_code = hit->primary_category.value_or("");
            dispatch_scene_hit(engine.handle, scene, target->x, target->y);
            native_location_click_triggered = true;
        }

        if (!camera_motion_triggered &&
            std::chrono::steady_clock::now() - started_at > std::chrono::milliseconds(options.auto_exit_ms / 3)) {
            if (!scene.adjust_camera(0.3, -0.1, -100000.0)) {
                fail("expected first scripted camera motion step to update the scene camera");
            }
            report_scene_camera(engine.handle, scene);
            if (!scene.adjust_camera(0.3, -0.15, -100000.0)) {
                fail("expected second scripted camera motion step to update the scene camera");
            }
            report_scene_camera(engine.handle, scene);
            if (!scene.adjust_camera(0.3, -0.15, -150000.0)) {
                fail("expected final scripted camera motion step to update the scene camera");
            }
            report_scene_camera(engine.handle, scene);
            camera_motion_triggered = true;
        }

        if (!native_cluster_click_triggered &&
            std::chrono::steady_clock::now() - started_at > std::chrono::milliseconds((options.auto_exit_ms * 2) / 3)) {
            const auto target = scene.first_cluster_screen_position();
            if (!target.has_value()) {
                fail("expected at least one visible cluster screen position");
            }
            const auto hit = scene.hit_test(target->x, target->y);
            if (!hit.has_value() ||
                hit->kind != frameflow::renderer::cesium::RenderedGlobeScene::InteractionHit::Kind::Cluster ||
                hit->point_count < 2) {
                fail("scene hit-test should resolve a visible cluster");
            }
            dispatch_scene_hit(engine.handle, scene, target->x, target->y);
            native_cluster_click_triggered = true;
        }

        scene.render_frame();
        glXSwapBuffers(display, window.handle);
        frameflow_engine_dispatch_pending_callbacks(engine.handle);
        scene.set_selected_location(tracker.selected_location_id);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    const std::string engine_diagnostics = copy_cstr(frameflow_engine_diagnostics_summary(engine.handle));
    const std::string render_diagnostics = scene.diagnostics_summary();
    if (engine_diagnostics.find("core_diag=[points=" + std::to_string(expected_point_count)) == std::string::npos) {
        fail("engine diagnostics should report the expected point snapshot size");
    }
    if (engine_diagnostics.find("width=" + std::to_string(last_width)) == std::string::npos) {
        fail("engine diagnostics should include the latest resized width");
    }
    if (engine_diagnostics.find("height=" + std::to_string(last_height)) == std::string::npos) {
        fail("engine diagnostics should include the latest resized height");
    }
    if (render_diagnostics.find("frames_rendered=0") != std::string::npos) {
        fail("render diagnostics should report at least one rendered frame");
    }
    if (render_diagnostics.find("points=" + std::to_string(expected_point_count)) == std::string::npos) {
        fail("render diagnostics should report the expected render snapshot size");
    }
    if (render_diagnostics.find("visible_points=0") != std::string::npos) {
        fail("render diagnostics should report at least one visible geo point");
    }
    if (render_diagnostics.find("clusters=0") != std::string::npos) {
        fail("render diagnostics should report at least one visible cluster");
    }
    if (render_diagnostics.find("selected_location_id=none") == std::string::npos) {
        fail("render diagnostics should clear location highlight after cluster selection");
    }
    if (tracker.location_count < 2u) {
        fail("expected both programmatic focus and native click selection callbacks");
    }
    if (tracker.cluster_count < 1u) {
        fail("expected at least one native cluster selection callback");
    }
    if (tracker.camera_count < 2u) {
        fail("expected initial and moved camera callbacks");
    }
    if (tracker.last_location_interaction != "click") {
        fail("last selection callback should describe click interaction");
    }
    if (tracker.last_location_category_code != native_clicked_category_code) {
        fail("last selection callback should preserve the picked point category");
    }
    if (tracker.last_location_screen_x <= 0.0 || tracker.last_location_screen_y <= 0.0) {
        fail("last selection callback should include positive screen coordinates");
    }
    if (!native_clicked_location_id.has_value()) {
        fail("smoke should remember which standalone location was click-selected");
    }
    if (tracker.last_location_id != *native_clicked_location_id) {
        fail("last location callback should match the click-selected standalone location");
    }
    if (tracker.last_cluster_id <= 0) {
        fail("cluster callback should preserve a positive cluster id");
    }
    if (tracker.last_cluster_point_count < 2) {
        fail("cluster callback should preserve aggregated point count");
    }
    if (tracker.last_cluster_longitude == 0.0 || tracker.last_cluster_latitude == 0.0) {
        fail("cluster callback should preserve centroid coordinates");
    }
    if (tracker.last_camera_longitude == 0.0 || tracker.last_camera_height_meters <= 0.0) {
        fail("camera callback should preserve runtime camera coordinates");
    }
    if (tracker.selected_location_id.has_value()) {
        fail("tracker should clear selected location after cluster selection");
    }
    if (engine_diagnostics.find("selected_location_id=none") == std::string::npos) {
        fail("engine diagnostics should clear selected location after cluster selection");
    }
    if (engine_diagnostics.find("camera_longitude_degrees=") == std::string::npos) {
        fail("engine diagnostics should include the latest runtime camera snapshot");
    }
    if (engine_diagnostics.find("coalesced_camera_events=0") != std::string::npos) {
        fail("engine diagnostics should report coalesced camera events after scripted burst motion");
    }
    if (tracker.error_count != 0u) {
        fail("unexpected engine error callback: " + tracker.last_error_message);
    }

    std::cout << "[cycle " << cycle_index << "] engine: " << engine_diagnostics << '\n';
    std::cout << "[cycle " << cycle_index << "] render: " << render_diagnostics << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const SmokeOptions options = parse_args(argc, argv);
        DisplaySession display = open_display();
        for (int cycle_index = 1; cycle_index <= options.cycles; ++cycle_index) {
            run_cycle(display.display, display.screen, options, cycle_index);
        }
        std::cout << "Cesium geo point smoke completed successfully.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frameflow_cesium_globe_smoke failed: " << error.what() << '\n';
        return 1;
    }
}
