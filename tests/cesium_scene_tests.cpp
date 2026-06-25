#include "renderer/cesium/rendered_globe_scene.hpp"

#include <CesiumGeospatial/Cartographic.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

using frameflow::renderer::cesium::RenderedGlobeScene;

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::abort();
}

void expect(const bool condition, const char* message) {
    if (!condition) {
        fail(message);
    }
}

void hud_hit_test_resolves_expected_actions() {
    unsetenv("FRAMEFLOW_GOOGLE_MAPS_API_KEY");

    RenderedGlobeScene scene(
        CesiumGeospatial::Cartographic::fromDegrees(20.4489, 44.7866, 8'500'000.0)
    );
    scene.resize(1280, 720);

    const auto zoom_in = scene.hud_action_screen_position(RenderedGlobeScene::HudAction::ZoomIn);
    const auto zoom_out = scene.hud_action_screen_position(RenderedGlobeScene::HudAction::ZoomOut);
    const auto reset = scene.hud_action_screen_position(RenderedGlobeScene::HudAction::ResetCamera);
    const auto compass = scene.hud_action_screen_position(RenderedGlobeScene::HudAction::ResetOrientation);

    expect(zoom_in.has_value(), "zoom-in HUD position should exist");
    expect(zoom_out.has_value(), "zoom-out HUD position should exist");
    expect(reset.has_value(), "reset HUD position should exist");
    expect(compass.has_value(), "compass HUD position should exist");
    expect(zoom_in->x > 1100.0, "HUD controls should sit in the top-right corner");
    expect(zoom_in->y < zoom_out->y && zoom_out->y < reset->y, "HUD controls should stack vertically");
    expect(compass->y > reset->y, "compass should sit below the camera controls");

    const auto zoom_in_hit = scene.hud_hit_test(zoom_in->x, zoom_in->y);
    const auto zoom_out_hit = scene.hud_hit_test(zoom_out->x, zoom_out->y);
    const auto reset_hit = scene.hud_hit_test(reset->x, reset->y);
    const auto compass_hit = scene.hud_hit_test(compass->x, compass->y);

    expect(zoom_in_hit.has_value() && *zoom_in_hit == RenderedGlobeScene::HudAction::ZoomIn, "zoom-in hit-test should resolve zoom-in");
    expect(zoom_out_hit.has_value() && *zoom_out_hit == RenderedGlobeScene::HudAction::ZoomOut, "zoom-out hit-test should resolve zoom-out");
    expect(reset_hit.has_value() && *reset_hit == RenderedGlobeScene::HudAction::ResetCamera, "reset hit-test should resolve reset");
    expect(compass_hit.has_value() && *compass_hit == RenderedGlobeScene::HudAction::ResetOrientation, "compass hit-test should resolve reset-orientation");
    expect(!scene.hud_hit_test(32.0, 32.0).has_value(), "non-HUD coordinates should not resolve a HUD action");
}

void hud_actions_adjust_and_reset_camera() {
    unsetenv("FRAMEFLOW_GOOGLE_MAPS_API_KEY");

    RenderedGlobeScene scene(
        CesiumGeospatial::Cartographic::fromDegrees(20.4489, 44.7866, 8'500'000.0)
    );
    scene.resize(1280, 720);

    const auto initial = scene.camera_state();
    expect(scene.activate_hud_action(RenderedGlobeScene::HudAction::ZoomIn), "zoom-in HUD action should change the camera");
    const auto after_zoom_in = scene.camera_state();
    expect(after_zoom_in.height_meters < initial.height_meters, "zoom-in should reduce camera height");

    expect(scene.activate_hud_action(RenderedGlobeScene::HudAction::ZoomOut), "zoom-out HUD action should change the camera");
    const auto after_zoom_out = scene.camera_state();
    expect(after_zoom_out.height_meters > after_zoom_in.height_meters, "zoom-out should increase camera height");

    for (int iteration = 0; iteration < 64; ++iteration) {
        scene.activate_hud_action(RenderedGlobeScene::HudAction::ZoomIn);
    }
    const auto deep_zoom = scene.camera_state();
    expect(deep_zoom.height_meters <= 1'500.0 + 1e-6, "zoom-in should now reach low city/street-level camera heights");

    expect(scene.adjust_camera(3.5, -1.75, -250'000.0), "camera adjustment should update the scene");
    const auto moved = scene.camera_state();
    expect(
        std::abs(moved.longitude - initial.longitude) > 1e-6 ||
            std::abs(moved.latitude - initial.latitude) > 1e-6 ||
            std::abs(moved.height_meters - initial.height_meters) > 1.0,
        "camera motion should move away from the home camera"
    );

    expect(scene.activate_hud_action(RenderedGlobeScene::HudAction::ResetCamera), "reset HUD action should restore the home camera");
    const auto reset = scene.camera_state();
    expect(std::abs(reset.longitude - initial.longitude) <= 1e-9, "reset should restore home longitude");
    expect(std::abs(reset.latitude - initial.latitude) <= 1e-9, "reset should restore home latitude");
    expect(std::abs(reset.height_meters - initial.height_meters) <= 1e-6, "reset should restore home height");
    expect(std::abs(reset.heading_degrees - initial.heading_degrees) <= 1e-9, "reset should restore home heading");
}

void compass_reset_restores_heading_and_changes_projection() {
    unsetenv("FRAMEFLOW_GOOGLE_MAPS_API_KEY");

    static const char* categories[] = {"POLITICS"};
    frameflow_point point{};
    point.location_id = 7001;
    point.label = "East";
    point.kind = FRAMEFLOW_LOCATION_KIND_CITY;
    point.country_code = "RS";
    point.latitude = 44.7866;
    point.longitude = 30.0;
    point.story_count = 1;
    point.top_categories = categories;
    point.top_category_count = 1u;

    RenderedGlobeScene scene(
        CesiumGeospatial::Cartographic::fromDegrees(20.4489, 44.7866, 8'500'000.0)
    );
    scene.resize(1280, 720);
    scene.set_points(&point, 1u);

    const auto north_up = scene.screen_position_for_location(7001);
    expect(north_up.has_value(), "test point should be visible in north-up mode");

    auto rotated = scene.camera_state();
    rotated.heading_degrees = 90.0;
    scene.set_camera_state(rotated);
    const auto east_up = scene.screen_position_for_location(7001);
    expect(east_up.has_value(), "test point should stay visible after heading change");
    expect(
        std::abs(east_up->x - north_up->x) > 1.0 || std::abs(east_up->y - north_up->y) > 1.0,
        "changing heading should rotate the projected point positions"
    );

    expect(scene.activate_hud_action(RenderedGlobeScene::HudAction::ResetOrientation), "compass action should reset orientation");
    const auto reset = scene.camera_state();
    expect(std::abs(reset.heading_degrees) <= 1e-9, "compass reset should restore north-up heading");
}

void diagnostics_report_boundary_overlay_status() {
    unsetenv("FRAMEFLOW_GOOGLE_MAPS_API_KEY");
    unsetenv("FRAMEFLOW_BOUNDARY_OVERLAY_PATH");

    RenderedGlobeScene scene(
        CesiumGeospatial::Cartographic::fromDegrees(20.4489, 44.7866, 8'500'000.0)
    );

    const auto diagnostics = scene.diagnostics_summary();
    expect(
        diagnostics.find("cartography=[source=unavailable") != std::string::npos,
        "scene diagnostics should report missing optional boundary overlay"
    );
    expect(
        diagnostics.find("missing_env=FRAMEFLOW_BOUNDARY_OVERLAY_PATH") != std::string::npos,
        "scene diagnostics should explain how to provide optional boundary overlay"
    );
}

void country_boundaries_project_when_runtime_dataset_is_available() {
    unsetenv("FRAMEFLOW_GOOGLE_MAPS_API_KEY");
    const auto override_path =
        std::filesystem::temp_directory_path() / "frameflow-boundary-overlay-cesium-scene.txt";
    {
        std::ofstream output(override_path);
        output
            << "frameflow-boundary-overlay-v1\n"
            << "meta\tdataset_id=runtime-scene-test-v1\tlocale=en\n"
            << "boundary\tcountry\ttest-region\tTest Region\tTR\t120\t18.0,47.0;23.0,47.0;23.0,42.0;18.0,42.0;18.0,47.0\n"
            << "label\tcountry\ttest-region\tTest Region\tTR\t20.4\t44.8\t120\t1500\t28000000\n";
    }
    setenv("FRAMEFLOW_BOUNDARY_OVERLAY_PATH", override_path.c_str(), 1);

    RenderedGlobeScene scene(
        CesiumGeospatial::Cartographic::fromDegrees(20.4489, 44.7866, 8'500'000.0)
    );
    unsetenv("FRAMEFLOW_BOUNDARY_OVERLAY_PATH");
    std::filesystem::remove(override_path);
    scene.resize(1280, 720);

    const auto regional_stats = scene.cartography_render_stats();
    expect(
        regional_stats.visible_country_boundaries >= 1u,
        "regional camera should project at least one runtime country boundary"
    );
    expect(
        regional_stats.visible_country_boundary_segments >= 1u,
        "regional camera should project visible boundary segments"
    );

    auto remote_camera = scene.camera_state();
    remote_camera.longitude = -145.0;
    remote_camera.latitude = -18.0;
    remote_camera.height_meters = 12'000.0;
    scene.set_camera_state(remote_camera);

    const auto remote_stats = scene.cartography_render_stats();
    expect(
        remote_stats.visible_country_boundaries == 0u,
        "remote open-ocean camera should not project nearby country boundaries"
    );
    expect(
        remote_stats.visible_country_boundary_segments == 0u,
        "remote open-ocean camera should not project nearby boundary segments"
    );
}

} // namespace

int main() {
    hud_hit_test_resolves_expected_actions();
    hud_actions_adjust_and_reset_camera();
    compass_reset_restores_heading_and_changes_projection();
    diagnostics_report_boundary_overlay_status();
    country_boundaries_project_when_runtime_dataset_is_available();
    return 0;
}
