set(FRAMEFLOW_WITH_CESIUM OFF CACHE BOOL "Enable Cesium Native-backed Frameflow adapter code")
set(
    FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cesium-native"
    CACHE PATH
    "Pinned cesium-native source checkout used when FRAMEFLOW_WITH_CESIUM=ON"
)
set(
    FRAMEFLOW_CESIUM_NATIVE_VERSION
    "0.59.0"
    CACHE STRING
    "Expected cesium-native project version for the current Frameflow integration"
)
set(
    FRAMEFLOW_CESIUM_NATIVE_TAG
    "v0.59.0"
    CACHE STRING
    "Expected cesium-native git tag for the current Frameflow integration"
)
set(
    FRAMEFLOW_CESIUM_NATIVE_COMMIT
    "f7e8eabe99a7466882f4dc902a51e02f24992243"
    CACHE STRING
    "Expected cesium-native git commit for the current Frameflow integration"
)
set(
    FRAMEFLOW_CESIUM_VCPKG_COMMIT
    "afc0a2e01ae104a2474216a2df0e8d78516fd5af"
    CACHE STRING
    "Pinned vcpkg commit used by cesium-native's current ezvcpkg bootstrap"
)
set(FRAMEFLOW_CESIUM_USE_EZVCPKG ON CACHE BOOL "Allow cesium-native to bootstrap its pinned vcpkg dependencies")

function(frameflow_extract_cesium_project_version source_dir out_var)
    file(READ "${source_dir}/CMakeLists.txt" _frameflow_cesium_cmakelists)
    string(
        REGEX MATCH
        "project\\([^)]*VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)"
        _frameflow_cesium_version_match
        "${_frameflow_cesium_cmakelists}"
    )
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "Failed to extract cesium-native VERSION from ${source_dir}/CMakeLists.txt")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(frameflow_verify_cesium_git_checkout source_dir out_tag_var out_commit_var)
    find_package(Git QUIET)
    if(NOT Git_FOUND)
        message(
            FATAL_ERROR
            "FRAMEFLOW_WITH_CESIUM=ON requires Git so Frameflow can verify the exact pinned "
            "cesium-native checkout tag (${FRAMEFLOW_CESIUM_NATIVE_TAG}) and commit "
            "(${FRAMEFLOW_CESIUM_NATIVE_COMMIT})."
        )
    endif()
    if(NOT EXISTS "${source_dir}/.git")
        message(
            FATAL_ERROR
            "FRAMEFLOW_WITH_CESIUM=ON requires ${source_dir} to be a git checkout so Frameflow "
            "can verify the exact pinned cesium-native tag and commit."
        )
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" describe --tags --exact-match
        RESULT_VARIABLE _frameflow_cesium_tag_result
        OUTPUT_VARIABLE _frameflow_cesium_discovered_tag
        ERROR_VARIABLE _frameflow_cesium_tag_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _frameflow_cesium_tag_result EQUAL 0)
        message(
            FATAL_ERROR
            "Failed to verify cesium-native exact tag in ${source_dir}. Expected "
            "${FRAMEFLOW_CESIUM_NATIVE_TAG}. Git said: ${_frameflow_cesium_tag_error}"
        )
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
        RESULT_VARIABLE _frameflow_cesium_commit_result
        OUTPUT_VARIABLE _frameflow_cesium_discovered_commit
        ERROR_VARIABLE _frameflow_cesium_commit_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _frameflow_cesium_commit_result EQUAL 0)
        message(
            FATAL_ERROR
            "Failed to verify cesium-native commit in ${source_dir}. Expected "
            "${FRAMEFLOW_CESIUM_NATIVE_COMMIT}. Git said: ${_frameflow_cesium_commit_error}"
        )
    endif()

    set(${out_tag_var} "${_frameflow_cesium_discovered_tag}" PARENT_SCOPE)
    set(${out_commit_var} "${_frameflow_cesium_discovered_commit}" PARENT_SCOPE)
endfunction()

function(frameflow_enable_cesium)
    if(NOT FRAMEFLOW_WITH_CESIUM)
        return()
    endif()

    if(NOT EXISTS "${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR}/CMakeLists.txt")
        message(
            FATAL_ERROR
            "FRAMEFLOW_WITH_CESIUM=ON requires a cesium-native source checkout at "
            "${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR}. Expected tag ${FRAMEFLOW_CESIUM_NATIVE_TAG}."
        )
    endif()

    frameflow_extract_cesium_project_version(
        "${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR}"
        FRAMEFLOW_CESIUM_DISCOVERED_VERSION
    )
    frameflow_verify_cesium_git_checkout(
        "${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR}"
        FRAMEFLOW_CESIUM_DISCOVERED_TAG
        FRAMEFLOW_CESIUM_DISCOVERED_COMMIT
    )
    if(NOT FRAMEFLOW_CESIUM_DISCOVERED_VERSION STREQUAL FRAMEFLOW_CESIUM_NATIVE_VERSION)
        message(
            FATAL_ERROR
            "cesium-native version mismatch: expected ${FRAMEFLOW_CESIUM_NATIVE_VERSION} "
            "(${FRAMEFLOW_CESIUM_NATIVE_TAG}), found ${FRAMEFLOW_CESIUM_DISCOVERED_VERSION} in "
            "${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR}. Update the pinned settings deliberately if "
            "you are moving Frameflow to a newer Cesium revision."
        )
    endif()
    if(NOT FRAMEFLOW_CESIUM_DISCOVERED_TAG STREQUAL FRAMEFLOW_CESIUM_NATIVE_TAG)
        message(
            FATAL_ERROR
            "cesium-native tag mismatch: expected ${FRAMEFLOW_CESIUM_NATIVE_TAG}, found "
            "${FRAMEFLOW_CESIUM_DISCOVERED_TAG} in ${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR}."
        )
    endif()
    if(NOT FRAMEFLOW_CESIUM_DISCOVERED_COMMIT STREQUAL FRAMEFLOW_CESIUM_NATIVE_COMMIT)
        message(
            FATAL_ERROR
            "cesium-native commit mismatch: expected ${FRAMEFLOW_CESIUM_NATIVE_COMMIT}, found "
            "${FRAMEFLOW_CESIUM_DISCOVERED_COMMIT} in ${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR}."
        )
    endif()

    set(CESIUM_TESTS_ENABLED OFF CACHE BOOL "Disable cesium-native tests in Frameflow builds" FORCE)
    set(CESIUM_INSTALL_HEADERS OFF CACHE BOOL "Disable cesium-native install headers in Frameflow builds" FORCE)
    set(CESIUM_INSTALL_STATIC_LIBS OFF CACHE BOOL "Disable cesium-native static install in Frameflow builds" FORCE)
    set(CESIUM_ENABLE_CLANG_TIDY OFF CACHE BOOL "Disable cesium-native clang-tidy in Frameflow builds" FORCE)
    set(CESIUM_USE_EZVCPKG ${FRAMEFLOW_CESIUM_USE_EZVCPKG} CACHE BOOL "Delegate dependency bootstrap to cesium-native" FORCE)

    message(
        STATUS
        "Enabling Cesium Native from ${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR} "
        "(version ${FRAMEFLOW_CESIUM_DISCOVERED_VERSION}, tag ${FRAMEFLOW_CESIUM_DISCOVERED_TAG}, "
        "commit ${FRAMEFLOW_CESIUM_DISCOVERED_COMMIT})"
    )
    if(FRAMEFLOW_CESIUM_USE_EZVCPKG)
        message(
            STATUS
            "Cesium ezvcpkg bootstrap is enabled. Default cache root is \$HOME/.ezvcpkg unless "
            "EZVCPKG_BASEDIR is provided."
        )
    elseif(NOT CMAKE_TOOLCHAIN_FILE)
        message(
            FATAL_ERROR
            "FRAMEFLOW_WITH_CESIUM=ON with FRAMEFLOW_CESIUM_USE_EZVCPKG=OFF requires "
            "CMAKE_TOOLCHAIN_FILE to point at a compatible vcpkg toolchain."
        )
    endif()

    add_subdirectory(
        "${FRAMEFLOW_CESIUM_NATIVE_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/third_party/cesium-native"
        EXCLUDE_FROM_ALL
    )

    foreach(_frameflow_cesium_pic_target
        CesiumAsync
        CesiumCurl
        CesiumGltf
        CesiumUtility
        CesiumGeometry
        CesiumGeospatial
        CesiumJsonReader
        CesiumJsonWriter
        CesiumVectorData
        CesiumGltfReader
        CesiumGltfContent
        Cesium3DTiles
        Cesium3DTilesReader
        Cesium3DTilesContent
        CesiumRasterOverlays
        CesiumQuantizedMeshTerrain
        Cesium3DTilesSelection
    )
        if(TARGET ${_frameflow_cesium_pic_target})
            set_target_properties(${_frameflow_cesium_pic_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        endif()
    endforeach()
endfunction()
