#pragma once

#include "core/native_surface_runtime.hpp"
#include "renderer/cesium/rendered_globe_scene.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frameflow::renderer::cesium {

class GlxNativeSceneSurfaceHost final : public FrameflowSurfaceHost {
public:
    explicit GlxNativeSceneSurfaceHost(std::uint64_t parent_window);
    ~GlxNativeSceneSurfaceHost() override;

    GlxNativeSceneSurfaceHost(const GlxNativeSceneSurfaceHost&) = delete;
    GlxNativeSceneSurfaceHost& operator=(const GlxNativeSceneSurfaceHost&) = delete;

    [[nodiscard]] std::optional<std::string> create(const FrameflowNativeSurfaceDesc& desc) override;
    [[nodiscard]] std::optional<std::string> resize(const FrameflowSurfaceBounds& bounds) override;
    [[nodiscard]] std::optional<std::string> set_visible(bool visible) override;
    [[nodiscard]] std::optional<std::string> update_scene(const FrameflowSceneSnapshot& snapshot) override;
    [[nodiscard]] std::optional<std::string> make_current() override;
    [[nodiscard]] std::optional<std::string> swap_buffers() override;
    [[nodiscard]] std::vector<FrameflowSurfaceEvent> drain_events() override;
    [[nodiscard]] std::optional<std::string> destroy() override;
    [[nodiscard]] const char* backend_name() const noexcept override;
    [[nodiscard]] std::string diagnostics_summary() const override;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace frameflow::renderer::cesium
