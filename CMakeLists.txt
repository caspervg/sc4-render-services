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

add_subdirectory(vendor/spdlog)
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
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/d3d7imgui/ImGui)

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
        ${CMAKE_SOURCE_DIR}/src
)

set(SC4_COMMON_DIRECTOR_DEF ${CMAKE_SOURCE_DIR}/SC4Director.def)

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
            ${CMAKE_SOURCE_DIR}/cmake/SC4RenderServicesVersion.rc.in
            ${CMAKE_CURRENT_BINARY_DIR}/SC4RenderServicesVersion.rc
            @ONLY
    )
    set(SC4_RENDER_SERVICES_VERSION_RESOURCE ${CMAKE_CURRENT_BINARY_DIR}/SC4RenderServicesVersion.rc)
endif()

# Combined ImGui + S3D Camera services DLL (641-gated)
set(CUSTOM_SERVICES_SOURCES
        ${CMAKE_SOURCE_DIR}/src/service/ImGuiService.cpp
        ${CMAKE_SOURCE_DIR}/src/service/S3DCameraService.cpp
        ${CMAKE_SOURCE_DIR}/src/service/DrawService.cpp
        ${CMAKE_SOURCE_DIR}/src/service/RenderServicesDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/VersionDetection.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Settings.cpp
        ${CMAKE_SOURCE_DIR}/src/DX7InterfaceHook.cpp
)

add_library(SC4RenderServices SHARED
        ${CUSTOM_SERVICES_SOURCES}
        ${SC4_COMMON_DIRECTOR_DEF}
        ${SC4_RENDER_SERVICES_VERSION_RESOURCE}
)

target_include_directories(SC4RenderServices PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
        ${IMGUI_DIR}
        ${MINI_INCLUDE_DIR}
)

target_link_libraries(SC4RenderServices PRIVATE
        imgui
        gzcom2
        spdlog
)
target_compile_definitions(SC4RenderServices PRIVATE IMGUI_DLL_IMPORT)

if(WIN32)
    target_link_libraries(SC4RenderServices PRIVATE kernel32 user32 Version ddraw dxguid)
endif()

set_target_properties(SC4RenderServices PROPERTIES
        OUTPUT_NAME "SC4RenderServices"
        SUFFIX ".dll"
        PREFIX ""
)


# ImGui sample DLL
set(IMGUI_SAMPLE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSample SHARED ${IMGUI_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4ImGuiSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleCityDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSampleCity SHARED ${IMGUI_SAMPLE_CITY_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4ImGuiSampleCity PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleDemoDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSampleDemo SHARED ${IMGUI_SAMPLE_DEMO_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4ImGuiSampleDemo PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiTextureSample.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiTextureSample SHARED ${IMGUI_TEXTURE_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4ImGuiTextureSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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
        ${CMAKE_SOURCE_DIR}/src/sample/WorldProjectionSampleDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/DX7InterfaceHook.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4WorldProjectionSample SHARED ${WORLD_PROJECTION_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4WorldProjectionSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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
        ${CMAKE_SOURCE_DIR}/src/sample/S3DCameraDebugSampleDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4S3DCameraDebugSample SHARED ${S3D_CAMERA_DEBUG_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4S3DCameraDebugSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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
        ${CMAKE_SOURCE_DIR}/src/sample/OverlayManagerSampleDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4OverlayManagerSample SHARED ${OVERLAY_MANAGER_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4OverlayManagerSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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
        ${CMAKE_SOURCE_DIR}/src/sample/DrawServiceSampleDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4DrawServiceSample SHARED ${DRAW_SERVICE_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4DrawServiceSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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
        ${CMAKE_SOURCE_DIR}/src/sample/road-decal/RoadDecalSampleDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/sample/road-decal/RoadDecalData.cpp
        ${CMAKE_SOURCE_DIR}/src/sample/road-decal/RoadDecalInputControl.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4RoadDecalSample SHARED ${ROAD_DECAL_SAMPLE_SOURCES} ${SC4_COMMON_DIRECTOR_DEF})

target_include_directories(SC4RoadDecalSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
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


add_custom_target(SC4RenderServicesAllTargets
        DEPENDS imgui SC4RenderServices SC4ImGuiSample SC4ImGuiSampleCity SC4ImGuiSampleDemo SC4ImGuiTextureSample SC4WorldProjectionSample SC4S3DCameraDebugSample SC4OverlayManagerSample SC4DrawServiceSample SC4RoadDecalSample
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
                -DSRC=${CMAKE_SOURCE_DIR}/SC4RenderServices.ini
                -DDST=${SC4_PLUGINS_DIR}/SC4RenderServices.ini
                -P ${CMAKE_SOURCE_DIR}/cmake/CopyIfNotExists.cmake
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
    else()
        message(WARNING "SC4 installation not found. Manual deployment required.")
    endif()
endif()

# Install targets
