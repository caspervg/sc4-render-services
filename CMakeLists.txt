cmake_minimum_required(VERSION 4.0)

# Force 32-bit build for SimCity 4 compatibility BEFORE project() call
# Set the platform regardless of current setting to ensure 32-bit
set(CMAKE_GENERATOR_PLATFORM "Win32")
message(STATUS "Forcing 32-bit build for SC4 compatibility")
#
## Force release runtime BEFORE project() call - this is critical
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_CXX_FLAGS_DEBUG "/MD /Zi /Ob0 /Od")
set(CMAKE_C_FLAGS_DEBUG "/MD /Zi /Ob0 /Od")

# Additional 32-bit enforcement
if(WIN32)
    set(CMAKE_SIZEOF_VOID_P 4)
    set(CMAKE_C_SIZEOF_DATA_PTR 4)
    set(CMAKE_CXX_SIZEOF_DATA_PTR 4)
endif()

project(SC4ImguiBackend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform specific settings
if(WIN32)
    add_definitions(-DWIN32 -D_WINDOWS -DUNICODE -D_UNICODE)
endif()

# MSVC specific settings
if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

    # Force release runtime for all configurations
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

    # Override debug flags to use release runtime
    set(CMAKE_CXX_FLAGS_DEBUG "/MD /Zi /Ob0 /Od /RTC1")
    set(CMAKE_C_FLAGS_DEBUG "/MD /Zi /Ob0 /Od /RTC1")

    # Enable debugging in release builds
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
endif()

add_subdirectory(vendor/spdlog)

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
target_compile_definitions(imgui PUBLIC IMGUI_USE_BGRA_PACKED_COLOR)

if(WIN32)
    target_link_libraries(imgui PRIVATE user32 gdi32 ddraw dxguid)
endif()

# GZCOM-DLL (vendor/gzcom-dll has sources, build local static lib)
set(GZCOM_DIR ${CMAKE_SOURCE_DIR}/vendor/gzcom-dll/gzcom-dll)

# Collect all public headers and sources
file(GLOB_RECURSE GZCOM_HEADERS
        ${GZCOM_DIR}/include/*.h
        ${GZCOM_DIR}/include/*.hpp
)
file(GLOB_RECURSE GZCOM_SOURCES
        ${GZCOM_DIR}/src/*.cpp
)

# Exclude vendor app entry and example directors from gzcom sources
list(REMOVE_ITEM GZCOM_SOURCES
        ${GZCOM_DIR}/src/main.cpp
        ${GZCOM_DIR}/src/cGZExampleDllDirector.cpp
        ${GZCOM_DIR}/src/cGZExtraCheatsDllDirector.cpp
        ${GZCOM_DIR}/src/cGZExtraExtraCheatsDllDirector.cpp
        ${GZCOM_DIR}/src/cGZCityHallUpgradeDllDirector.cpp
)

# Build gzcom2 as a static library to match existing linkage
add_library(gzcom2 STATIC ${GZCOM_SOURCES} ${GZCOM_HEADERS})

# Expose include path to dependents
target_include_directories(gzcom2 PUBLIC
        ${GZCOM_DIR}/include
)

# Include directories
include_directories(
        ${CMAKE_SOURCE_DIR}/src
)

# ImGui service DLL
set(IMGUI_SERVICE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/service/ImGuiService.cpp
        ${CMAKE_SOURCE_DIR}/src/service/ImGuiServiceDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
        ${CMAKE_SOURCE_DIR}/src/DX7InterfaceHook.cpp
)

add_library(SC4ImGuiService SHARED ${IMGUI_SERVICE_SOURCES} SC4ImGuiService.def)

target_include_directories(SC4ImGuiService PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
        ${IMGUI_DIR}
)

target_link_libraries(SC4ImGuiService PRIVATE
        imgui
        gzcom2
        spdlog
)
target_compile_definitions(SC4ImGuiService PRIVATE IMGUI_DLL_IMPORT)

if(WIN32)
    target_link_libraries(SC4ImGuiService PRIVATE kernel32 user32 Version ddraw dxguid)
endif()

set_target_properties(SC4ImGuiService PROPERTIES
        OUTPUT_NAME "SC4ImGuiService"
        SUFFIX ".dll"
        PREFIX ""
)

if(MSVC)
    set_target_properties(SC4ImGuiService PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()

# ImGui sample DLL
set(IMGUI_SAMPLE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSample SHARED ${IMGUI_SAMPLE_SOURCES} SC4ImGuiSample.def)

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

if(MSVC)
    set_target_properties(SC4ImGuiSample PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()

# ImGui city-only sample DLL
set(IMGUI_SAMPLE_CITY_SOURCES
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleCityDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSampleCity SHARED ${IMGUI_SAMPLE_CITY_SOURCES} SC4ImGuiSampleCity.def)

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

if(MSVC)
    set_target_properties(SC4ImGuiSampleCity PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()

# ImGui demo sample DLL
set(IMGUI_SAMPLE_DEMO_SOURCES
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleDemoDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSampleDemo SHARED ${IMGUI_SAMPLE_DEMO_SOURCES} SC4ImGuiSampleDemo.def)

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

if(MSVC)
    set_target_properties(SC4ImGuiSampleDemo PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()

# Automatic deployment to SC4 directories
if(WIN32)
    # Try to find SC4 installation directory
    set(SC4_INSTALL_PATHS
            "C:/Program Files (x86)/SimCity 4 Deluxe Edition"
            "C:/Program Files (x86)/Maxis/SimCity 4 Deluxe"
            "C:/Program Files/Maxis/SimCity 4 Deluxe"
            "C:/Program Files (x86)/Steam/steamapps/common/SimCity 4 Deluxe"
            "C:/Program Files/Steam/steamapps/common/SimCity 4 Deluxe"
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

        add_custom_command(TARGET SC4ImGuiService POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiService>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiService.dll to Plugins folder"
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

        message(STATUS "Deployment targets configured:")
        message(STATUS "  - imgui.dll -> ${SC4_INSTALL_DIR}/Apps/")
        message(STATUS "  - SC4ImGuiService.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSampleCity.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSampleDemo.dll -> ${SC4_PLUGINS_DIR}/")
    else()
        message(WARNING "SC4 installation not found. Manual deployment required.")
    endif()
endif()

# Install targets
