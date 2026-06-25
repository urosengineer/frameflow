#include "renderer/cesium/cesium_dependency_smoke.hpp"

#include <iostream>

int main() {
    std::cout << frameflow::renderer::cesium::dependency_smoke_summary() << '\n';
    return 0;
}
