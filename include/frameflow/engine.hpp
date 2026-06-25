#pragma once

#include "frameflow/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frameflow {

class Engine {
public:
    Engine() = default;

    void set_points(std::span<const GeoPointAggregate> points);
    void set_filter(GlobeFilter filter);
    bool focus_location(std::int64_t location_id);
    void clear_selection();

    [[nodiscard]] std::size_t point_count() const noexcept;
    [[nodiscard]] std::span<const GeoPointAggregate> points() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> selected_location_id() const noexcept;
    [[nodiscard]] const GlobeFilter& filter() const noexcept;
    [[nodiscard]] std::string diagnostics_summary() const;

private:
    std::vector<GeoPointAggregate> points_;
    GlobeFilter filter_;
    std::optional<std::int64_t> selected_location_id_;
};

} // namespace frameflow
