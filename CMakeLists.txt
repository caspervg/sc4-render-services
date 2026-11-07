cmake_minimum_required(VERSION 3.20)

# Enable new MSVC runtime library selection policy
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# True when built on its own, false when a parent project pulls this in with add_subdirectory.
# PROJECT_IS_TOP_LEVEL needs CMake 3.21 and is only set after project().
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SC4RS_IS_TOP_LEVEL ON)
else()
    set(SC4RS_IS_TOP_LEVEL OFF)
endif()

# Force 32-bit build for SimCity 4 compatibility BEFORE project() call.
# A parent project picks its own generator and platform.
if(SC4RS_IS_TOP_LEVEL)
    set(CMAKE_GENERATOR_PLATFORM "Win32")
    message(STATUS "Forcing 32-bit build for SC4 compatibility")

    # Additional 32-bit enforcement
    if(WIN32)
        set(CMAKE_SIZEOF_VOID_P 4)
        set(CMAKE_C_SIZEOF_DATA_PTR 4)
        set(CMAKE_CXX_SIZEOF_DATA_PTR 4)
    endif()
endif()

project(SC4RenderServices VERSION 0.0.1 LANGUAGES CXX)

set(SC4RS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

option(SC4RS_BUILD_SAMPLES "Build the sample plugin DLLs" OFF)

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

# Reuse a parent's spdlog and gzcom-dll when it already has them, instead of adding duplicates.
if(NOT TARGET spdlog)
    if(TARGET spdlog::spdlog)
        add_library(spdlog ALIAS spdlog::spdlog)
    else()
        add_subdirectory(vendor/spdlog)
    endif()
endif()
if(NOT TARGET gzcom_dll AND NOT TARGET gzcom2)
    add_subdirectory(vendor/gzcom-dll)

    # Keep CRT runtime consistent across all targets even if a subproject sets /MT(/d).
    if(MSVC)
        target_compile_options(gzcom_dll PRIVATE
                "$<$<CONFIG:Debug>:/MDd>"
                "$<$<CONFIG:Release>:/MD>"
                "$<$<CONFIG:RelWithDebInfo>:/MD>"
                "$<$<CONFIG:MinSizeRel>:/MD>"
        )
    endif()
endif()

set(MINI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mINI/src")
if (NOT EXISTS "${MINI_INCLUDE_DIR}/mini/ini.h")
    message(
        FATAL_ERROR
        "Missing vendor/mINI headers. Initialize submodules with: "
        "'git submodule update --init --recursive'."
    )
endif ()

# ImGui library: upstream docking branch (vendor/imgui has no CMakeLists)
set(IMGUI_DIR ${SC4RS_ROOT}/vendor/imgui)

set(IMGUI_CORE_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui.h
        ${IMGUI_DIR}/imgui_internal.h
        ${SC4RS_ROOT}/src/public/ImGuiUserConfig.h
)

# Win32 platform backend from upstream
set(IMGUI_BACKENDS
        ${IMGUI_DIR}/backends/imgui_impl_win32.h
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

# ImGui is compiled into SC4RenderServices.dll and exported from it. This target only carries
# what every module that includes imgui.h needs to agree on: the headers and the config.
add_library(imgui INTERFACE)
target_include_directories(imgui INTERFACE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${SC4RS_ROOT}/src/public
)
target_compile_definitions(imgui INTERFACE IMGUI_USER_CONFIG="ImGuiUserConfig.h")

# Links a plugin against the ImGui that SC4RenderServices.dll exports. The import is delay-loaded,
# so load order does not matter: the game may load the plugin first, and the import resolves on
# the first ImGui call, which a plugin makes only once it holds the ImGui service.
# Also works from a project that adds this repository with add_subdirectory; pass EXCLUDE_FROM_ALL
# there to keep the sample DLLs out of the parent's build.
function(sc4rs_use_imgui target)
    target_link_libraries(${target} PRIVATE SC4RenderServices imgui delayimp)
    target_compile_definitions(${target} PRIVATE IMGUI_DLL_IMPORT)
    target_link_options(${target} PRIVATE "/DELAYLOAD:SC4RenderServices.dll")
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/public/SC4RenderServicesDelayLoad.cpp)
endfunction()

# Preserve existing link name in this project while using the vendor CMake target.
if(NOT TARGET gzcom2)
    add_library(gzcom2 ALIAS gzcom_dll)
endif()

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

# Polygon clipping and offsetting (Clipper2) and triangulation (earcut), for terrain paint.
set(CLIPPER2_DIR ${SC4RS_ROOT}/vendor/Clipper2/CPP/Clipper2Lib)
if (NOT EXISTS "${CLIPPER2_DIR}/include/clipper2/clipper.h" OR NOT EXISTS "${SC4RS_ROOT}/vendor/earcut/include/mapbox/earcut.hpp")
    message(
        FATAL_ERROR
        "Missing vendor/Clipper2 or vendor/earcut. Initialize submodules with: "
        "'git submodule update --init --recursive'."
    )
endif ()

# The terrain paint geometry has no game dependencies, so the tests can build it on its own.
add_library(sc4rs_terrain_paint STATIC
        ${SC4RS_ROOT}/src/service/decal/TerrainPaintMesh.cpp
        ${CLIPPER2_DIR}/src/clipper.engine.cpp
        ${CLIPPER2_DIR}/src/clipper.offset.cpp
        ${CLIPPER2_DIR}/src/clipper.rectclip.cpp
)
target_include_directories(sc4rs_terrain_paint PUBLIC
        ${SC4RS_ROOT}/src
        ${CLIPPER2_DIR}/include
        ${SC4RS_ROOT}/vendor/earcut/include
)

option(SC4RS_BUILD_TESTS "Build the offline tests" ${SC4RS_IS_TOP_LEVEL})
if(SC4RS_BUILD_TESTS)
    add_executable(TerrainPaintMeshTests ${SC4RS_ROOT}/tests/TerrainPaintMeshTests.cpp)
    target_link_libraries(TerrainPaintMeshTests PRIVATE sc4rs_terrain_paint)
endif()

# Combined ImGui + S3D Camera services DLL (641-gated)
set(CUSTOM_SERVICES_SOURCES
        ${SC4RS_ROOT}/src/service/ImGuiService.cpp
        ${SC4RS_ROOT}/src/service/S3DCameraService.cpp
        ${SC4RS_ROOT}/src/service/DrawService.cpp
        ${SC4RS_ROOT}/src/service/WorldDrawService.cpp
        ${SC4RS_ROOT}/src/service/decal/ClippedTerrainDecalRenderer.cpp
        ${SC4RS_ROOT}/src/service/decal/RelativeCallPatch.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalRegistry.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalService.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalSidecarCodec.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainPaintCodec.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalSymbols.cpp
        ${SC4RS_ROOT}/src/service/decal/TerrainDecalHook.cpp
        ${SC4RS_ROOT}/src/service/RenderServicesDirector.cpp
        ${SC4RS_ROOT}/src/service/GraphicsDriverIdentification.cpp
        ${SC4RS_ROOT}/src/utils/VersionDetection.cpp
        ${SC4RS_ROOT}/src/utils/Logger.cpp
        ${SC4RS_ROOT}/src/utils/Settings.cpp
        ${SC4RS_ROOT}/src/utils/UpdateCheck.cpp
        ${SC4RS_ROOT}/src/DX7InterfaceHook.cpp
        ${SC4RS_ROOT}/src/backends/imgui_impl_gzgdriver.cpp
)

add_library(SC4RenderServices SHARED
        ${CUSTOM_SERVICES_SOURCES}
        ${IMGUI_CORE_SOURCES}
        ${IMGUI_BACKENDS}
        ${SC4_COMMON_DIRECTOR_DEF}
        ${SC4_RENDER_SERVICES_VERSION_RESOURCE}
)

target_include_directories(SC4RenderServices PRIVATE
        ${SC4RS_ROOT}/src
        ${SC4RS_ROOT}/src/backends
        ${SC4RS_ROOT}/src/service
        ${IMGUI_DIR}
        ${MINI_INCLUDE_DIR}
)

target_link_libraries(SC4RenderServices PRIVATE
        imgui
        gzcom2
        spdlog
        sc4rs_terrain_paint
)
target_compile_definitions(SC4RenderServices PRIVATE
        IMGUI_DLL_EXPORT
        SC4RS_PRODUCT_VERSION_STR="${SC4RS_PRODUCT_VERSION_STR}"
)

if(WIN32)
    target_link_libraries(SC4RenderServices PRIVATE kernel32 user32 gdi32 Version ddraw dxguid winmm wininet)
endif()

set_target_properties(SC4RenderServices PROPERTIES
        OUTPUT_NAME "SC4RenderServices"
        SUFFIX ".dll"
        PREFIX ""
)



# Automatic deployment to SC4 directories (a parent project handles its own deployment)
if(WIN32 AND SC4RS_IS_TOP_LEVEL AND NOT DEFINED ENV{CI})
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

        message(STATUS "Deployment targets configured:")
        message(STATUS "  - SC4RenderServices.dll -> ${SC4_PLUGINS_DIR}/")
    else()
        message(WARNING "SC4 installation not found. Manual deployment required.")
    endif()
endif()

if(SC4RS_BUILD_SAMPLES)
    include(${SC4RS_ROOT}/cmake/Samples.cmake)
endif()

add_custom_target(SC4RenderServicesAllTargets DEPENDS SC4RenderServices ${SC4RS_SAMPLE_TARGETS})

# Install targets
