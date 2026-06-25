#include "renderer/cesium/cesium_dependency_smoke.hpp"

#include <CesiumGeospatial/Cartographic.h>

#include <sstream>

namespace frameflow::renderer::cesium {

std::string dependency_smoke_summary() {
    const auto belgrade = CesiumGeospatial::Cartographic::fromDegrees(20.4489, 44.7866, 117.0);

    std::ostringstream out;
    out << "cesium_dependency_smoke"
        << " longitude_radians=" << belgrade.longitude
        << " latitude_radians=" << belgrade.latitude
        << " height_meters=" << belgrade.height;
    return out.str();
}

} // namespace frameflow::renderer::cesium
