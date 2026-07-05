cmake_minimum_required(VERSION 3.20)

# Enable new MSVC runtime library selection policy
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Force 32-bit build for SimCity 4 compatibility BEFORE project() call
# Set the platform regardless of current setting to ensure 32-bit
set(CMAKE_GENERATOR_PLATFORM "Win32")
message(STATUS "Forcing 32-bit build for SC4 compatibility")

# Additional 32-bit enforcement
if(WIN32)
    set(CMAKE_SIZEOF_VOID_P 4)
    set(CMAKE_C_SIZEOF_DATA_PTR 4)
    set(CMAKE_CXX_SIZEOF_DATA_PTR 4)
endif()

project(SC4RenderServices VERSION 0.0.1 LANGUAGES CXX)

set(SC4RS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform specific settings
if(WIN32)
    add_definitions(-DWIN32 -D_WINDOWS -DUNICODE -D_UNICODE)
endif()

# MSVC specific settings
if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

    # For Visual Studio multi-config generator, set the runtime library policy
    # This ensures consistent runtime linking across all targets and dependencies
    # CMP0091 allows us to use CMAKE_MSVC_RUNTIME_LIBRARY variable

    # Determine runtime library based on CMAKE_BUILD_TYPE (for single-config) or default
    if(CMAKE_BUILD_TYPE)
        if(CMAKE_BUILD_TYPE MATCHES "Debug")
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
        else()
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        endif()
    else()
        # For Visual Studio (multi-config), default to Release runtime, but generator will override per-config
        # The CMAKE_CONFIGURATION_TYPES will be handled by the generator
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()

    # Enable debugging in release builds
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
endif()

if(NOT TARGET spdlog)
    add_subdirectory(vendor/spdlog)
endif()
add_subdirectory(vendor/gzcom-dll)

set(MINI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mINI/src")
if (NOT EXISTS "${MINI_INCLUDE_DIR}/mini/ini.h")
    message(
        FATAL_ERROR
        "Missing vendor/mINI headers. Initialize submodules with: "
        "'git submodule update --init --recursive'."
    )
endif ()

# Keep CRT runtime consistent across all targets even if a subproject sets /MT(/d).
if(MSVC)
    target_compile_options(gzcom_dll PRIVATE
            "$<$<CONFIG:Debug>:/MDd>"
            "$<$<CONFIG:Release>:/MD>"
            "$<$<CONFIG:RelWithDebInfo>:/MD>"
            "$<$<CONFIG:MinSizeRel>:/MD>"
    )
endif()

# ImGui library (vendor/imguid3d7 has no CMakeLists)
set(IMGUI_DIR ${SC4RS_ROOT}/vendor/d3d7imgui/ImGui)

# Add all core ImGui .cpp and .h files
file(GLOB IMGUI_CORE_SOURCES
        ${IMGUI_DIR}/*.cpp
        ${IMGUI_DIR}/*.h
)

# Explicitly add the Win32 and DX7 backend files
set(IMGUI_BACKENDS
        ${IMGUI_DIR}/imgui_impl_win32.h
        ${IMGUI_DIR}/imgui_impl_win32.cpp
        ${IMGUI_DIR}/imgui_impl_dx7.h
        ${IMGUI_DIR}/imgui_impl_dx7.cpp
)

add_library(imgui SHARED ${IMGUI_CORE_SOURCES} ${IMGUI_BACKENDS})

target_include_directories(imgui PUBLIC
        ${IMGUI_DIR}
)
target_compile_definitions(imgui PRIVATE IMGUI_DLL_EXPORT)

if(WIN32)
    target_link_libraries(imgui PRIVATE user32 gdi32 ddraw dxguid)
endif()

# Preserve existing link name in this project while using the vendor CMake target.
add_library(gzcom2 ALIAS gzcom_dll)

# Include directories
include_directories(
        ${SC4RS_ROOT}/src
)

set(SC4_COMMON_DIRECTOR_DEF ${SC4RS_ROOT}/SC4Director.def)

set(SC4RS_VERSION_INPUT "${PROJECT_VERSION}")
if(DEFINED SC4RS_RELEASE_VERSION AND NOT SC4RS_RELEASE_VERSION STREQUAL "")
    set(SC4RS_VERSION_INPUT "${SC4RS_RELEASE_VERSION}")
endif()
string(REGEX REPLACE "^v" "" SC4RS_PRODUCT_VERSION_STR "${SC4RS_VERSION_INPUT}")

if(SC4RS_PRODUCT_VERSION_STR MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(\\.([0-9]+))?([-.].*)?$")
    set(SC4RS_FILE_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(SC4RS_FILE_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(SC4RS_FILE_VERSION_PATCH "${CMAKE_MATCH_3}")
    if(CMAKE_MATCH_5)
        set(SC4RS_FILE_VERSION_BUILD "${CMAKE_MATCH_5}")
    else()
        set(SC4RS_FILE_VERSION_BUILD 0)
    endif()
else()
    message(FATAL_ERROR "SC4RS_RELEASE_VERSION must look like v1.2.3 or v1.2.3.4 (got '${SC4RS_VERSION_INPUT}')")
endif()

set(SC4RS_FILE_VERSION_COMMAS
        "${SC4RS_FILE_VERSION_MAJOR},${SC4RS_FILE_VERSION_MINOR},${SC4RS_FILE_VERSION_PATCH},${SC4RS_FILE_VERSION_BUILD}")
set(SC4RS_FILE_VERSION_DOTS
        "${SC4RS_FILE_VERSION_MAJOR}.${SC4RS_FILE_VERSION_MINOR}.${SC4RS_FILE_VERSION_PATCH}.${SC4RS_FILE_VERSION_BUILD}")

set(SC4_RENDER_SERVICES_VERSION_RESOURCE "")
if(WIN32)
    configure_file(
            ${SC4RS_ROOT}/cmake/SC4RenderServicesVersion.rc.in
            ${CMAKE_CURRENT_BINARY_DIR}/SC4RenderServicesVersion.rc
            @ONLY
    )
    set(SC4_RENDER_SERVICES_VERSION_RESOURCE ${CMAKE_CURRENT_BINARY_DIR}/SC4RenderServicesVersion.rc)
endif()

# Combined ImGui + S3D Camera services DLL (641-gated)
set(CUSTOM_SERVICES_SOURCES
        ${SC4RS_ROOT}/src/service/ImGuiService.cpp
        ${SC4RS_ROOT}/src/service/S3DCameraService.cpp
        ${SC4RS_ROOT}/src/service/DrawService.cpp
        ${SC4RS_ROOT}/src/service/decal/ClippedTerrainDecalRenderer.cpp
        ${SC4RS_ROOT}/src/service/decal/RelativeCallPatch.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalRegistry.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalService.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalSidecarCodec.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalSymbols.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalHook.cpp
        ${SC4RS_ROOT}/src/service/RenderServicesDirector.cpp
        ${SC4RS_ROOT}/src/utils/VersionDetection.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
        ${SC4RS_ROOT}/src/utils/Settings.cpp
        ${SC4RS_ROOT}/src/DX7InterfaceHook.cpp
)

add_library(SC4RenderServices SHARED
        ${CUSTOM_SERVICES_SOURCES}
        ${SC4_COMMON_DIRECTOR_DEF}
        ${SC4_RENDER_SERVICES_VERSION_RESOURCE}
)

target_include_directories(SC4RenderServices PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
        ${MINI_INCLUDE_DIR}
)

target_link_libraries(SC4RenderServices PRIVATE
        imgui
        gzcom2
        spdlog
)
target_compile_definitions(SC4RenderServices PRIVATE
        IMGUI_DLL_IMPORT
        SC4RS_PRODUCT_VERSION_STR="${SC4RS_PRODUCT_VERSION_STR}"
)

if(WIN32)
    target_link_libraries(SC4RenderServices PRIVATE kernel32 user32 Version ddraw dxguid winmm)
endif()

set_target_properties(SC4RenderServices PROPERTIES
        OUTPUT_NAME "SC4RenderServices"
        SUFFIX ".dll"
        PREFIX ""
)


# ImGui sample DLL
set(IMGUI_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/ImGuiSampleDirector.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSample SHARED ${IMGUI_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4ImGuiSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4ImGuiSample PRIVATE
        imgui
        gzcom2
        spdlog
)
target_compile_definitions(SC4ImGuiSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4ImGuiSample PROPERTIES
        OUTPUT_NAME "SC4ImGuiSample"
        SUFFIX ".dll"
        PREFIX ""
)


# ImGui city-only sample DLL
set(IMGUI_SAMPLE_CITY_SOURCES
        ${SC4RS_ROOT}/src/sample/ImGuiSampleCityDirector.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSampleCity SHARED ${IMGUI_SAMPLE_CITY_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4ImGuiSampleCity PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4ImGuiSampleCity PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4ImGuiSampleCity PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4ImGuiSampleCity PROPERTIES
        OUTPUT_NAME "SC4ImGuiSampleCity"
        SUFFIX ".dll"
        PREFIX ""
)


# ImGui demo sample DLL
set(IMGUI_SAMPLE_DEMO_SOURCES
        ${SC4RS_ROOT}/src/sample/ImGuiSampleDemoDirector.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSampleDemo SHARED ${IMGUI_SAMPLE_DEMO_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4ImGuiSampleDemo PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4ImGuiSampleDemo PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4ImGuiSampleDemo PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4ImGuiSampleDemo PROPERTIES
        OUTPUT_NAME "SC4ImGuiSampleDemo"
        SUFFIX ".dll"
        PREFIX ""
)


# ImGui texture sample DLL
set(IMGUI_TEXTURE_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/ImGuiTextureSample.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4ImGuiTextureSample SHARED ${IMGUI_TEXTURE_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4ImGuiTextureSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4ImGuiTextureSample PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4ImGuiTextureSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4ImGuiTextureSample PROPERTIES
        OUTPUT_NAME "SC4ImGuiTextureSample"
        SUFFIX ".dll"
        PREFIX ""
)


# World Projection sample DLL
set(WORLD_PROJECTION_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/WorldProjectionSampleDirector.cpp
        ${SC4RS_ROOT}/src/DX7InterfaceHook.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4WorldProjectionSample SHARED ${WORLD_PROJECTION_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4WorldProjectionSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4WorldProjectionSample PRIVATE
        imgui
        gzcom2
        spdlog
)

if(WIN32)
    target_link_libraries(SC4WorldProjectionSample PRIVATE gdiplus)
endif()

target_compile_definitions(SC4WorldProjectionSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4WorldProjectionSample PROPERTIES
        OUTPUT_NAME "SC4WorldProjectionSample"
        SUFFIX ".dll"
        PREFIX ""
)

# S3D Camera debug sample DLL
set(S3D_CAMERA_DEBUG_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/S3DCameraDebugSampleDirector.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4S3DCameraDebugSample SHARED ${S3D_CAMERA_DEBUG_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4S3DCameraDebugSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4S3DCameraDebugSample PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4S3DCameraDebugSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4S3DCameraDebugSample PROPERTIES
        OUTPUT_NAME "SC4S3DCameraDebugSample"
        SUFFIX ".dll"
        PREFIX ""
)

# Overlay Manager sample DLL
set(OVERLAY_MANAGER_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/OverlayManagerSampleDirector.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4OverlayManagerSample SHARED ${OVERLAY_MANAGER_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4OverlayManagerSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4OverlayManagerSample PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4OverlayManagerSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4OverlayManagerSample PROPERTIES
        OUTPUT_NAME "SC4OverlayManagerSample"
        SUFFIX ".dll"
        PREFIX ""
)

# Draw Service sample DLL
set(DRAW_SERVICE_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/DrawServiceSampleDirector.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4DrawServiceSample SHARED ${DRAW_SERVICE_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4DrawServiceSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4DrawServiceSample PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4DrawServiceSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4DrawServiceSample PROPERTIES
        OUTPUT_NAME "SC4DrawServiceSample"
        SUFFIX ".dll"
        PREFIX ""
)

# Road Decal sample DLL
set(ROAD_DECAL_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/road-decal/RoadDecalSampleDirector.cpp
        ${SC4RS_ROOT}/src/sample/road-decal/RoadDecalData.cpp
        ${SC4RS_ROOT}/src/sample/road-decal/RoadDecalInputControl.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4RoadDecalSample SHARED ${ROAD_DECAL_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4RoadDecalSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4RoadDecalSample PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4RoadDecalSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4RoadDecalSample PROPERTIES
        OUTPUT_NAME "SC4RoadDecalSample"
        SUFFIX ".dll"
        PREFIX ""
)

# Terrain Decal sample DLL
set(TERRAIN_DECAL_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/TerrainDecalSampleDirector.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4TerrainDecalSample SHARED ${TERRAIN_DECAL_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4TerrainDecalSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4TerrainDecalSample PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4TerrainDecalSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4TerrainDecalSample PROPERTIES
        OUTPUT_NAME "SC4TerrainDecalSample"
        SUFFIX ".dll"
        PREFIX ""
)

# Camera View Input sample DLL
set(CAMERA_VIEW_INPUT_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/camera-view-input/CameraViewInputSampleDirector.cpp
        ${SC4RS_ROOT}/src/sample/camera-view-input/CameraViewInputControl.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4CameraViewInputSample SHARED ${CAMERA_VIEW_INPUT_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4CameraViewInputSample PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4CameraViewInputSample PRIVATE
        imgui
        gzcom2
        spdlog
)

target_compile_definitions(SC4CameraViewInputSample PRIVATE IMGUI_DLL_IMPORT)

set_target_properties(SC4CameraViewInputSample PROPERTIES
        OUTPUT_NAME "SC4CameraViewInputSample"
        SUFFIX ".dll"
        PREFIX ""
)


# Date Jumper sample DLL (Ctrl+F9 to fast-forward to the next seasonal date)
set(DATE_JUMPER_SAMPLE_SOURCES
        ${SC4RS_ROOT}/src/sample/date-jumper/DateJumperDirector.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4DateJumperSample SHARED ${DATE_JUMPER_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4DateJumperSample PRIVATE
        ${SC4RS_ROOT}/src
)

target_link_libraries(SC4DateJumperSample PRIVATE
        gzcom2
        spdlog
)

set_target_properties(SC4DateJumperSample PROPERTIES
        OUTPUT_NAME "SC4DateJumperSample"
        SUFFIX ".dll"
        PREFIX ""
)


# Fixed-size pool growth crash fix (guards the GZ pool `grow` call site against
# block-ring saturation; see docs/sc4-fixed-pool-growth-crash.md)
set(POOL_GROWTH_FIX_SOURCES
        ${SC4RS_ROOT}/src/sample/pool-growth-fix/PoolGrowthFixDirector.cpp
        ${SC4RS_ROOT}/src/service/decal/RelativeCallPatch.cpp
        ${SC4RS_ROOT}/src/utils/VersionDetection.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4FixedPoolGrowthFixSample SHARED ${POOL_GROWTH_FIX_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4FixedPoolGrowthFixSample PRIVATE
        ${SC4RS_ROOT}/src
)

target_link_libraries(SC4FixedPoolGrowthFixSample PRIVATE
        gzcom2
        spdlog
)

if(WIN32)
    target_link_libraries(SC4FixedPoolGrowthFixSample PRIVATE Version)
endif()

set_target_properties(SC4FixedPoolGrowthFixSample PROPERTIES
        OUTPUT_NAME "SC4FixedPoolGrowthFixSample"
        SUFFIX ".dll"
        PREFIX ""
)

# Terrain diagonal near-cliff triangulation fix. This patches the terrain
# redisplay pass after cliff texture classification and only changes quad
# triangulation flags for non-cliff cells.
set(TERRAIN_DIAGONAL_FIX_SOURCES
        ${SC4RS_ROOT}/src/sample/terrain-diagonal-fix/TerrainDiagonalFixDirector.cpp
        ${SC4RS_ROOT}/src/service/decal/RelativeCallPatch.cpp
        ${SC4RS_ROOT}/src/utils/VersionDetection.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4TerrainDiagonalFixSample SHARED ${TERRAIN_DIAGONAL_FIX_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4TerrainDiagonalFixSample PRIVATE
        ${SC4RS_ROOT}/src
        ${MINI_INCLUDE_DIR}
)

target_link_libraries(SC4TerrainDiagonalFixSample PRIVATE
        gzcom2
        spdlog
)

if(WIN32)
    target_link_libraries(SC4TerrainDiagonalFixSample PRIVATE Version)
endif()

set_target_properties(SC4TerrainDiagonalFixSample PROPERTIES
        OUTPUT_NAME "SC4TerrainDiagonalFixSample"
        SUFFIX ".dll"
        PREFIX ""
)

# Power-pole cable customization. Lets a pole prop exemplar declare custom cable attach points
# per connection direction via new exemplar properties, instead of being limited to the engine's
# two baked 4-point tables. See docs/sc4-powerline-tool-re.md for the underlying RE.
set(POWER_POLE_CUSTOMIZATION_SOURCES
        ${SC4RS_ROOT}/src/sample/power-pole-customization/PowerPoleCustomizationDirector.cpp
        ${SC4RS_ROOT}/src/service/decal/RelativeCallPatch.cpp
        ${SC4RS_ROOT}/src/utils/VersionDetection.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
)

add_library(SC4PowerPoleCustomizationSample SHARED ${POWER_POLE_CUSTOMIZATION_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4PowerPoleCustomizationSample PRIVATE
        ${SC4RS_ROOT}/src
        ${IMGUI_DIR}
        ${MINI_INCLUDE_DIR}
)

target_link_libraries(SC4PowerPoleCustomizationSample PRIVATE
        gzcom2
        spdlog
        imgui
)

# The status overlay is optional: imgui.dll is DELAY-LOADED so this DLL still loads (and every pole
# feature + the Tab key hook keep working) when SC4RenderServices / imgui.dll is not installed. No
# ImGui:: symbol is touched unless the ImGui service was acquired, so the delay-load never resolves
# in that case. IMGUI_DLL_IMPORT matches the shared imgui build the samples use.
target_compile_definitions(SC4PowerPoleCustomizationSample PRIVATE IMGUI_DLL_IMPORT)

if(WIN32)
    target_link_libraries(SC4PowerPoleCustomizationSample PRIVATE Version delayimp)
    target_link_options(SC4PowerPoleCustomizationSample PRIVATE /DELAYLOAD:imgui.dll)
endif()

set_target_properties(SC4PowerPoleCustomizationSample PROPERTIES
        OUTPUT_NAME "SC4PowerPoleCustomizationSample"
        SUFFIX ".dll"
        PREFIX ""
)


add_custom_target(SC4RenderServicesAllTargets
        DEPENDS imgui SC4RenderServices SC4ImGuiSample SC4ImGuiSampleCity SC4ImGuiSampleDemo SC4ImGuiTextureSample SC4WorldProjectionSample SC4S3DCameraDebugSample SC4OverlayManagerSample SC4DrawServiceSample SC4RoadDecalSample SC4TerrainDecalSample SC4CameraViewInputSample SC4DateJumperSample SC4FixedPoolGrowthFixSample SC4TerrainDiagonalFixSample SC4PowerPoleCustomizationSample
)

# Automatic deployment to SC4 directories
if(WIN32 AND NOT DEFINED ENV{CI})
    # Try to find SC4 installation directory
    set(SC4_INSTALL_PATHS
            "C:/Program Files (x86)/SimCity 4 Deluxe Edition"
            "C:/Program Files (x86)/Maxis/SimCity 4 Deluxe"
            "C:/Program Files/Maxis/SimCity 4 Deluxe"
            "C:/Program Files (x86)/Steam/steamapps/common/SimCity 4 Deluxe"
            "C:/Program Files/Steam/steamapps/common/SimCity 4 Deluxe"
            "C:/Program Files (x86)/GOG Galaxy/Games/SimCity 4 Deluxe Edition"
            "C:/GOG Games/SimCity 4 Deluxe Edition"
    )

    foreach(SC4_PATH ${SC4_INSTALL_PATHS})
        if(EXISTS "${SC4_PATH}/Apps/SimCity 4.exe")
            set(SC4_INSTALL_DIR "${SC4_PATH}")
            break()
        endif()
    endforeach()

    # Get user's Documents folder
    set(USERPROFILE_DIR "$ENV{USERPROFILE}")
    set(SC4_PLUGINS_DIR "${USERPROFILE_DIR}/Documents/SimCity 4/Plugins")

    if(SC4_INSTALL_DIR)
        message(STATUS "Found SC4 installation at: ${SC4_INSTALL_DIR}")

        add_custom_command(TARGET imgui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_INSTALL_DIR}/Apps"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:imgui>
                "${SC4_INSTALL_DIR}/Apps/"
                COMMENT "Deploying imgui.dll to Apps folder"
        )

        add_custom_command(TARGET SC4RenderServices POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4RenderServices>
                "${SC4_PLUGINS_DIR}/"
                COMMAND ${CMAKE_COMMAND}
                -DSRC=${SC4RS_ROOT}/SC4RenderServices.ini
                -DDST=${SC4_PLUGINS_DIR}/SC4RenderServices.ini
                -P ${SC4RS_ROOT}/cmake/CopyIfNotExists.cmake
                COMMENT "Deploying SC4RenderServices.dll to Plugins folder (INI only if missing)"
        )

        add_custom_command(TARGET SC4ImGuiSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4ImGuiSampleCity POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiSampleCity>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiSampleCity.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4ImGuiSampleDemo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiSampleDemo>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiSampleDemo.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4ImGuiTextureSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiTextureSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiTextureSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4WorldProjectionSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4WorldProjectionSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4WorldProjectionSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4DrawServiceSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4DrawServiceSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4DrawServiceSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4RoadDecalSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4RoadDecalSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4RoadDecalSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4TerrainDecalSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4TerrainDecalSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4TerrainDecalSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4CameraViewInputSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4CameraViewInputSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4CameraViewInputSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4DateJumperSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4DateJumperSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4DateJumperSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4FixedPoolGrowthFixSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4FixedPoolGrowthFixSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4FixedPoolGrowthFixSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4TerrainDiagonalFixSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4TerrainDiagonalFixSample>
                "${SC4_PLUGINS_DIR}/"
                COMMAND ${CMAKE_COMMAND}
                -DSRC=${SC4RS_ROOT}/SC4TerrainDiagonalFix.ini
                -DDST=${SC4_PLUGINS_DIR}/SC4TerrainDiagonalFix.ini
                -P ${SC4RS_ROOT}/cmake/CopyIfNotExists.cmake
                COMMENT "Deploying SC4TerrainDiagonalFixSample.dll to Plugins folder (INI only if missing)"
        )

        add_custom_command(TARGET SC4PowerPoleCustomizationSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4PowerPoleCustomizationSample>
                "${SC4_PLUGINS_DIR}/"
                COMMAND ${CMAKE_COMMAND}
                -DSRC=${SC4RS_ROOT}/SC4PowerPoleCustomization.ini
                -DDST=${SC4_PLUGINS_DIR}/SC4PowerPoleCustomization.ini
                -P ${SC4RS_ROOT}/cmake/CopyIfNotExists.cmake
                COMMENT "Deploying SC4PowerPoleCustomizationSample.dll to Plugins folder (INI only if missing)"
        )

        message(STATUS "Deployment targets configured:")
        message(STATUS "  - imgui.dll -> ${SC4_INSTALL_DIR}/Apps/")
        message(STATUS "  - SC4RenderServices.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSampleCity.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSampleDemo.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiTextureSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4WorldProjectionSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4DrawServiceSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4RoadDecalSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4TerrainDecalSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4CameraViewInputSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4FixedPoolGrowthFixSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4TerrainDiagonalFixSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4PowerPoleCustomizationSample.dll -> ${SC4_PLUGINS_DIR}/")
    else()
        message(WARNING "SC4 installation not found. Manual deployment required.")
    endif()
endif()

# Install targets
