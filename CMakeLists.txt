cmake_minimum_required(VERSION 3.24)

project(
    Frameflow
    VERSION 0.1.0
    DESCRIPTION "Native rendering runtime for geographic discovery surfaces"
    LANGUAGES CXX
)

option(FRAMEFLOW_BUILD_EXAMPLES "Build Frameflow example binaries" ON)
option(FRAMEFLOW_BUILD_TESTS "Build Frameflow tests" ON)
option(FRAMEFLOW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(FRAMEFLOW_BUILD_LINUX_SURFACE_SMOKE "Build the Linux X11 surface smoke example when X11 is available" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FrameflowCompilerWarnings)
include(FrameflowCesium)
find_package(X11 QUIET)

add_library(frameflow_core STATIC
    src/core/engine.cpp
    src/core/native_surface_runtime.cpp
    src/renderer/cartography/cartography_dataset.cpp
)

target_include_directories(frameflow_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(frameflow_core PUBLIC cxx_std_20)
frameflow_apply_compiler_warnings(frameflow_core)
if(X11_FOUND)
    target_include_directories(frameflow_core PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(frameflow_core PUBLIC ${X11_LIBRARIES})
    target_compile_definitions(frameflow_core PRIVATE FRAMEFLOW_HAS_X11_SURFACE_HOST=1)
endif()

set_target_properties(frameflow_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

add_library(frameflow SHARED
    src/bridge/bridge_events.cpp
    src/bridge/bridge_model.cpp
    src/bridge/c_api.cpp
    src/bridge/software_globe_renderer.cpp
)

target_link_libraries(frameflow
    PRIVATE
        frameflow_core
)

target_compile_features(frameflow PRIVATE cxx_std_20)
target_include_directories(frameflow
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
frameflow_apply_compiler_warnings(frameflow)

set_target_properties(frameflow PROPERTIES
    OUTPUT_NAME "frameflow"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

frameflow_enable_cesium()

if(FRAMEFLOW_WITH_CESIUM)
    add_library(frameflow_renderer_cesium_adapter STATIC
        src/renderer/cesium/cesium_dependency_smoke.cpp
    )
    target_link_libraries(frameflow_renderer_cesium_adapter
        PUBLIC
            CesiumGeospatial
            CesiumUtility
    )
    target_compile_features(frameflow_renderer_cesium_adapter PRIVATE cxx_std_20)
    target_include_directories(frameflow_renderer_cesium_adapter
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    frameflow_apply_compiler_warnings(frameflow_renderer_cesium_adapter)
    set_target_properties(frameflow_renderer_cesium_adapter PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    set(FRAMEFLOW_CESIUM_GL_SUPPORT_AVAILABLE OFF)
    if(UNIX AND NOT APPLE)
        find_package(OpenGL QUIET)
        find_package(X11 QUIET)
        if(OpenGL_FOUND AND X11_FOUND)
            add_library(frameflow_renderer_cesium_gl_support STATIC
                src/renderer/cesium/rendered_globe_scene.cpp
                src/renderer/cesium/glx_offscreen_scene_host.cpp
                src/renderer/cesium/glx_native_scene_surface_host.cpp
            )
            target_link_libraries(frameflow_renderer_cesium_gl_support
                PUBLIC
                    frameflow_core
                    frameflow_renderer_cesium_adapter
                    Cesium3DTilesSelection
                    CesiumRasterOverlays
                    CesiumAsync
                    CesiumCurl
                    CesiumGltf
                    CesiumGltfReader
                    OpenGL::GL
                    ${X11_LIBRARIES}
            )
            target_compile_features(frameflow_renderer_cesium_gl_support PRIVATE cxx_std_20)
            target_include_directories(frameflow_renderer_cesium_gl_support
                PRIVATE
                    "${CMAKE_CURRENT_SOURCE_DIR}/include"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src"
                    ${X11_INCLUDE_DIR}
            )
            frameflow_apply_compiler_warnings(frameflow_renderer_cesium_gl_support)
            set_target_properties(frameflow_renderer_cesium_gl_support PROPERTIES
                POSITION_INDEPENDENT_CODE ON
            )

            target_link_libraries(frameflow
                PRIVATE
                    frameflow_renderer_cesium_gl_support
            )
            target_include_directories(frameflow
                PRIVATE
                    "${CMAKE_CURRENT_SOURCE_DIR}/src"
                    ${X11_INCLUDE_DIR}
            )
            target_compile_definitions(frameflow PRIVATE FRAMEFLOW_HAS_CESIUM_OFFSCREEN_SUPPORT=1)
            set(FRAMEFLOW_CESIUM_GL_SUPPORT_AVAILABLE ON)
        endif()
    endif()
endif()

if(FRAMEFLOW_BUILD_EXAMPLES)
    add_executable(frameflow_smoke
        examples/smoke/main.cpp
    )
    target_link_libraries(frameflow_smoke PRIVATE frameflow_core)
    target_compile_features(frameflow_smoke PRIVATE cxx_std_20)
    frameflow_apply_compiler_warnings(frameflow_smoke)

    if(FRAMEFLOW_WITH_CESIUM)
        add_executable(frameflow_cesium_dependency_smoke
            examples/cesium_dependency_smoke/main.cpp
        )
        target_link_libraries(frameflow_cesium_dependency_smoke
            PRIVATE
                frameflow_renderer_cesium_adapter
        )
        target_compile_features(frameflow_cesium_dependency_smoke PRIVATE cxx_std_20)
        target_include_directories(frameflow_cesium_dependency_smoke
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        frameflow_apply_compiler_warnings(frameflow_cesium_dependency_smoke)

        if(FRAMEFLOW_CESIUM_GL_SUPPORT_AVAILABLE)
            add_executable(frameflow_cesium_globe_smoke
                examples/cesium_globe_smoke/main.cpp
            )
            target_link_libraries(frameflow_cesium_globe_smoke
                PRIVATE
                    frameflow
                    frameflow_renderer_cesium_gl_support
            )
            target_include_directories(frameflow_cesium_globe_smoke
                PRIVATE
                    "${CMAKE_CURRENT_SOURCE_DIR}/src"
                    ${X11_INCLUDE_DIR}
            )
            target_compile_features(frameflow_cesium_globe_smoke PRIVATE cxx_std_20)
            frameflow_apply_compiler_warnings(frameflow_cesium_globe_smoke)
        elseif(UNIX AND NOT APPLE)
            message(
                STATUS
                "Skipping frameflow_cesium_globe_smoke because OpenGL or X11 dependencies were not found"
            )
        endif()
    endif()

    if(UNIX AND NOT APPLE AND FRAMEFLOW_BUILD_LINUX_SURFACE_SMOKE)
        find_package(X11 QUIET)
        if(X11_FOUND)
            add_executable(frameflow_linux_surface_smoke
                examples/linux_surface_smoke/main.cpp
            )
            target_link_libraries(frameflow_linux_surface_smoke
                PRIVATE
                    frameflow
                    ${X11_LIBRARIES}
            )
            target_include_directories(frameflow_linux_surface_smoke
                PRIVATE
                    ${X11_INCLUDE_DIR}
            )
            target_compile_features(frameflow_linux_surface_smoke PRIVATE cxx_std_20)
            frameflow_apply_compiler_warnings(frameflow_linux_surface_smoke)
        else()
            message(STATUS "Skipping frameflow_linux_surface_smoke because X11 was not found")
        endif()
    endif()
endif()

if(FRAMEFLOW_BUILD_TESTS)
    enable_testing()

    add_executable(frameflow_core_tests
        tests/core_tests.cpp
    )
    target_link_libraries(frameflow_core_tests PRIVATE frameflow_core)
    target_include_directories(frameflow_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(frameflow_core_tests PRIVATE cxx_std_20)
    frameflow_apply_compiler_warnings(frameflow_core_tests)

    add_executable(frameflow_bridge_tests
        tests/bridge_tests.cpp
    )
    target_link_libraries(frameflow_bridge_tests PRIVATE frameflow)
    target_compile_features(frameflow_bridge_tests PRIVATE cxx_std_20)
    frameflow_apply_compiler_warnings(frameflow_bridge_tests)

    add_test(NAME frameflow_core_tests COMMAND frameflow_core_tests)
    add_test(NAME frameflow_bridge_tests COMMAND frameflow_bridge_tests)
    if(FRAMEFLOW_WITH_CESIUM AND FRAMEFLOW_CESIUM_GL_SUPPORT_AVAILABLE)
        add_executable(frameflow_cesium_scene_tests
            tests/cesium_scene_tests.cpp
        )
        target_link_libraries(frameflow_cesium_scene_tests PRIVATE frameflow_renderer_cesium_gl_support)
        target_include_directories(frameflow_cesium_scene_tests
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}/include"
                "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_compile_features(frameflow_cesium_scene_tests PRIVATE cxx_std_20)
        frameflow_apply_compiler_warnings(frameflow_cesium_scene_tests)
        add_test(NAME frameflow_cesium_scene_tests COMMAND frameflow_cesium_scene_tests)
    endif()
    if(FRAMEFLOW_WITH_CESIUM AND TARGET frameflow_cesium_dependency_smoke)
        add_test(NAME frameflow_cesium_dependency_smoke COMMAND frameflow_cesium_dependency_smoke)
    endif()
endif()
