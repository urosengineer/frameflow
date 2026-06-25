# Third-Party Notices

This file summarizes third-party components and services that Frameflow can use
directly or indirectly. It is informational and does not replace the applicable
upstream license files.

## Frameflow

Frameflow source code is licensed under the Apache License, Version 2.0. See
`LICENSE`.

## Optional Cesium Native Integration

Frameflow's default build does not require Cesium Native. Cesium-backed targets
are enabled only when `FRAMEFLOW_WITH_CESIUM=ON`.

Cesium Native:

- Project: Cesium Native
- Upstream: https://github.com/CesiumGS/cesium-native
- License: Apache License, Version 2.0

When Cesium Native is enabled, users are responsible for obtaining a compatible
source checkout and complying with Cesium Native's own `LICENSE` and
`ThirdParty.json`, including licenses for dependencies fetched or built through
Cesium's dependency tooling.

This source distribution does not vendor `third_party/cesium-native`. If a
downstream distribution vendors Cesium Native, it must include the corresponding
upstream notices.

## System Graphics Dependencies

Some optional Linux examples and native-surface paths use system-provided X11,
OpenGL, and GLX libraries when available. These libraries are not vendored by
Frameflow. Their licensing and redistribution terms are governed by the
libraries and operating system packages installed on the user's machine.

## Optional Imagery Providers

Frameflow can use external map imagery through Cesium Native raster overlays
when the user provides runtime credentials.

Supported provider integrations include:

- Google Maps, configured with `FRAMEFLOW_GOOGLE_MAPS_API_KEY`
- MapTiler raster maps, configured with `FRAMEFLOW_MAPTILER_API_KEY`
- Stadia raster maps, configured with `FRAMEFLOW_STADIA_API_KEY`

Frameflow does not store or distribute provider API keys. Users are responsible
for provider terms, quota, billing, attribution, and any operational limits such
as HTTP 429 responses.

## Cartography And Overlays

Frameflow can render configured basemap imagery and host-provided cartographic
overlays. Boundary and label interpretation is data-driven; the renderer
displays supplied imagery and overlay data and does not hardcode political
boundary semantics.

Projects that enable host-owned boundary or label overlays are responsible for
their own cartographic data licensing, conversion pipeline, and policy.
