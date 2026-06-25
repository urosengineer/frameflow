#include "frameflow/c/bridge.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct SmokeOptions {
    int width = 1280;
    int height = 720;
    int auto_exit_ms = 900;
    int cycles = 2;
    bool simulate_tab_switch = true;
};

struct CallbackTracker {
    std::size_t ready_count = 0;
    std::size_t location_count = 0;
    std::size_t cluster_count = 0;
    std::size_t camera_count = 0;
    std::size_t error_count = 0;
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

struct WindowSession {
    Display* display = nullptr;
    ::Window handle = 0;
    Atom delete_atom = 0;
    int width = 0;
    int height = 0;

    ~WindowSession() {
        if (display != nullptr && handle != 0) {
            XDestroyWindow(display, handle);
            XFlush(display);
        }
    }
};

frameflow_surface_bounds child_bounds_for_window(const WindowSession& window) {
    return frameflow_surface_bounds{
        .x = 12,
        .y = 12,
        .width = std::max(1, window.width - 24),
        .height = std::max(1, window.height - 24),
        .scale_factor = 1.0,
    };
}

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

void on_location_selected(void* user, const frameflow_location_selection_event* /*event*/) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    tracker->location_count += 1;
}

void on_cluster_selected(void* user, const frameflow_cluster_selection_event* /*event*/) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    tracker->cluster_count += 1;
}

void on_camera_changed(void* user, const frameflow_camera_state* /*event*/) {
    auto* tracker = static_cast<CallbackTracker*>(user);
    tracker->camera_count += 1;
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

SmokeOptions parse_args(int argc, char** argv) {
    SmokeOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            std::cout
                << "Usage: frameflow_linux_surface_smoke [--width=N] [--height=N] "
                << "[--auto-exit-ms=N] [--cycles=N] [--no-simulate-tab-switch]\n";
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
        if (argument == "--no-simulate-tab-switch") {
            options.simulate_tab_switch = false;
            continue;
        }
        fail("Unknown argument: " + argument);
    }

    if (options.width <= 0 || options.height <= 0 || options.auto_exit_ms <= 0 || options.cycles <= 0) {
        fail("width, height, auto-exit-ms, and cycles must be positive");
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

WindowSession create_window(Display* display, int screen, const SmokeOptions& options, int cycle_index) {
    WindowSession session;
    session.display = display;
    session.width = options.width;
    session.height = options.height;

    const auto root = RootWindow(display, screen);
    session.handle = XCreateSimpleWindow(
        display,
        root,
        0,
        0,
        static_cast<unsigned int>(session.width),
        static_cast<unsigned int>(session.height),
        1u,
        BlackPixel(display, screen),
        WhitePixel(display, screen)
    );
    if (session.handle == 0) {
        fail("XCreateSimpleWindow failed");
    }

    XStoreName(display, session.handle, ("Frameflow Linux Surface Smoke cycle " + std::to_string(cycle_index)).c_str());
    XSelectInput(display, session.handle, StructureNotifyMask | FocusChangeMask | KeyPressMask | ExposureMask);
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

void run_cycle(Display* display, int screen, const SmokeOptions& options, int cycle_index) {
    WindowSession window = create_window(display, screen, options, cycle_index);
    wait_for_map(window);

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
        .tile_cache_path = "/tmp/frameflow-linux-surface-smoke",
        .max_tile_cache_bytes = 64u * 1024u * 1024u,
        .log_level = "debug",
    };
    const auto child_bounds = child_bounds_for_window(window);
    frameflow_native_surface_desc surface_desc{};
    surface_desc.backend = FRAMEFLOW_SURFACE_BACKEND_LINUX_X11_CHILD;
    surface_desc.bounds = child_bounds;
    surface_desc.platform.x11.display = display;
    surface_desc.platform.x11.parent_window = static_cast<std::uint64_t>(window.handle);
    require_ok(
        frameflow_engine_initialize_with_native_surface(
            engine.handle,
            &surface_desc,
            &init_options
        ),
        engine.handle,
        "initialize_with_native_surface"
    );

    if (frameflow_engine_dispatch_pending_callbacks(engine.handle) != 1u || tracker.ready_count != 1u) {
        fail("expected one ready callback after initialize");
    }
    require_ok(
        frameflow_engine_request_frame(engine.handle, "startup"),
        engine.handle,
        "request_frame startup"
    );

    const char* top_categories[] = {"POLITICS"};
    const frameflow_point point{
        .location_id = 601,
        .label = "Belgrade",
        .kind = FRAMEFLOW_LOCATION_KIND_CITY,
        .country_code = "RS",
        .latitude = 44.7866,
        .longitude = 20.4489,
        .story_count = 8,
        .latest_story_epoch_millis = 1'776'335'400'000LL,
        .top_categories = top_categories,
        .top_category_count = 1u,
        .style_key = "category-politics",
    };
    require_ok(frameflow_engine_set_points(engine.handle, &point, 1u), engine.handle, "set_points");
    require_ok(frameflow_engine_focus_location(engine.handle, 601), engine.handle, "focus_location");
    if (frameflow_engine_dispatch_pending_callbacks(engine.handle) != 1u || tracker.location_count != 1u) {
        fail("expected one location callback after programmatic focus");
    }

    if (options.simulate_tab_switch) {
        require_ok(frameflow_engine_pause(engine.handle), engine.handle, "pause");
        if (frameflow_engine_state(engine.handle) != FRAMEFLOW_LIFECYCLE_PAUSED) {
            fail("pause should move the engine into PAUSED state");
        }
        require_ok(frameflow_engine_resume(engine.handle), engine.handle, "resume");
        if (frameflow_engine_state(engine.handle) != FRAMEFLOW_LIFECYCLE_READY) {
            fail("resume should move the engine back into READY state");
        }
    }

    bool running = true;
    bool resize_requested = false;
    int last_width = window.width;
    int last_height = window.height;
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
                    const frameflow_surface_bounds resized_child_bounds{
                        .x = 12,
                        .y = 12,
                        .width = std::max(1, last_width - 24),
                        .height = std::max(1, last_height - 24),
                        .scale_factor = 1.0,
                    };
                    require_ok(
                        frameflow_engine_set_surface_bounds(
                            engine.handle,
                            &resized_child_bounds
                        ),
                        engine.handle,
                        "set_surface_bounds"
                    );
                    require_ok(
                        frameflow_engine_request_frame(engine.handle, "resize"),
                        engine.handle,
                        "request_frame resize"
                    );
                }
                continue;
            }
            if (event.type == FocusOut && frameflow_engine_state(engine.handle) == FRAMEFLOW_LIFECYCLE_READY) {
                require_ok(frameflow_engine_pause(engine.handle), engine.handle, "focus pause");
                continue;
            }
            if (event.type == FocusIn && frameflow_engine_state(engine.handle) == FRAMEFLOW_LIFECYCLE_PAUSED) {
                require_ok(frameflow_engine_resume(engine.handle), engine.handle, "focus resume");
                continue;
            }
            if (event.type == KeyPress) {
                const auto key = XkbKeycodeToKeysym(display, static_cast<KeyCode>(event.xkey.keycode), 0, 0);
                if (key == XK_Escape || key == XK_q) {
                    running = false;
                } else if (key == XK_f) {
                    require_ok(frameflow_engine_focus_location(engine.handle, 601), engine.handle, "key focus");
                }
            }
        }

        frameflow_engine_dispatch_pending_callbacks(engine.handle);
        require_ok(
            frameflow_engine_request_frame(engine.handle, "tick"),
            engine.handle,
            "request_frame tick"
        );
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::string diagnostics = copy_cstr(frameflow_engine_diagnostics_summary(engine.handle));
    if (diagnostics.find("width=" + std::to_string(std::max(1, last_width - 24))) == std::string::npos) {
        fail("diagnostics should include the latest resized child width");
    }
    if (diagnostics.find("height=" + std::to_string(std::max(1, last_height - 24))) == std::string::npos) {
        fail("diagnostics should include the latest resized child height");
    }
    if (diagnostics.find("surface_backend=LINUX_X11_CHILD") == std::string::npos) {
        fail("diagnostics should expose the Linux X11 child backend");
    }
    if (diagnostics.find("surface_host_backend=glx-native-scene-host") == std::string::npos &&
        diagnostics.find("surface_host_backend=linux-x11-smoke-host") == std::string::npos &&
        diagnostics.find("surface_host_backend=null-surface-host") == std::string::npos) {
        fail("diagnostics should expose the native surface host backend");
    }
    if (diagnostics.find("render_thread_frame_requests=") == std::string::npos) {
        fail("diagnostics should expose render-thread frame request counters");
    }
    if (tracker.error_count != 0u) {
        fail("unexpected engine error callback: " + tracker.last_error_message);
    }

    std::cout << "[cycle " << cycle_index << "] diagnostics: " << diagnostics << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const SmokeOptions options = parse_args(argc, argv);
        DisplaySession display = open_display();
        for (int cycle_index = 1; cycle_index <= options.cycles; ++cycle_index) {
            run_cycle(display.display, display.screen, options, cycle_index);
        }
        std::cout << "Linux surface smoke completed successfully.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frameflow_linux_surface_smoke failed: " << error.what() << '\n';
        return 1;
    }
}
