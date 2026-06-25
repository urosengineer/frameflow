#pragma once

#include "bridge_engine.hpp"

#include <cstddef>

bool has_offscreen_output_target(const frameflow_engine* engine);

bool has_latest_offscreen_frame(const frameflow_engine* engine);

std::size_t offscreen_required_bytes(const frameflow_engine& engine);

void clear_offscreen_frame(frameflow_engine* engine);

void refresh_offscreen_frame(frameflow_engine* engine);

void sync_native_surface_runtime_scene(frameflow_engine* engine);
