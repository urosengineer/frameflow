#include "frameflow/engine.hpp"

#include <iostream>
#include <vector>

int main() {
    frameflow::Engine engine;
    frameflow::GeoPointAggregate belgrade;
    belgrade.location_id = 601;
    belgrade.label = "Belgrade";
    belgrade.kind = frameflow::LocationKind::City;
    belgrade.country_code = "RS";
    belgrade.latitude = 44.7866;
    belgrade.longitude = 20.4489;
    belgrade.story_count = 8;

    const std::vector<frameflow::GeoPointAggregate> points{belgrade};

    engine.set_points(points);
    const bool focused = engine.focus_location(601);

    std::cout << engine.diagnostics_summary() << '\n';
    return focused ? 0 : 1;
}
