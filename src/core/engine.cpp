#include "frameflow/engine.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace frameflow {

void Engine::set_points(std::span<const GeoPointAggregate> points) {
    points_.assign(points.begin(), points.end());

    if (selected_location_id_.has_value()) {
        const auto selected_id = *selected_location_id_;
        const auto still_present = std::any_of(points_.begin(), points_.end(), [selected_id](const auto& point) {
            return point.location_id == selected_id;
        });
        if (!still_present) {
            selected_location_id_.reset();
        }
    }
}

void Engine::set_filter(GlobeFilter filter) {
    filter_ = std::move(filter);
}

bool Engine::focus_location(std::int64_t location_id) {
    const auto found = std::any_of(points_.begin(), points_.end(), [location_id](const auto& point) {
        return point.location_id == location_id;
    });
    if (!found) {
        return false;
    }
    selected_location_id_ = location_id;
    return true;
}

void Engine::clear_selection() {
    selected_location_id_.reset();
}

std::size_t Engine::point_count() const noexcept {
    return points_.size();
}

std::span<const GeoPointAggregate> Engine::points() const noexcept {
    return points_;
}

std::optional<std::int64_t> Engine::selected_location_id() const noexcept {
    return selected_location_id_;
}

const GlobeFilter& Engine::filter() const noexcept {
    return filter_;
}

std::string Engine::diagnostics_summary() const {
    std::ostringstream out;
    out << "points=" << points_.size();
    if (selected_location_id_.has_value()) {
        out << " selected_location_id=" << *selected_location_id_;
    } else {
        out << " selected_location_id=none";
    }

    std::size_t styled_points = 0;
    std::size_t total_top_categories = 0;
    std::optional<std::string> first_style_key;
    std::optional<std::string> first_top_category;
    for (const auto& point : points_) {
        total_top_categories += point.top_categories.size();
        if (point.style_key.has_value()) {
            styled_points += 1;
            if (!first_style_key.has_value()) {
                first_style_key = point.style_key;
            }
        }
        if (!first_top_category.has_value() && !point.top_categories.empty()) {
            first_top_category = point.top_categories.front();
        }
    }

    out << " styled_points=" << styled_points
        << " total_top_categories=" << total_top_categories;
    if (first_style_key.has_value()) {
        out << " first_style_key=" << *first_style_key;
    } else {
        out << " first_style_key=none";
    }
    if (first_top_category.has_value()) {
        out << " first_top_category=" << *first_top_category;
    } else {
        out << " first_top_category=none";
    }
    return out.str();
}

} // namespace frameflow
