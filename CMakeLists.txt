cmake_minimum_required(VERSION 3.16)

project(TerrainSurfaceToolEditorPlugin)

set(vendor_name Vamps)
set(target_name TerrainSurfaceTool)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Preprocessor definitions
add_definitions(-DUNIGINE_DOUBLE -D_CRT_SECURE_NO_WARNINGS)

# Qt5 setup - auto MOC for Q_OBJECT macros
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

# Set Qt5 path - Use Qt 5.12.3 to match Unigine 2.18
set(CMAKE_PREFIX_PATH "C:/Qt/Qt5.12.3/5.12.3/msvc2017_64")
set(Qt5_DIR "C:/Qt/Qt5.12.3/5.12.3/msvc2017_64/lib/cmake/Qt5")

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)

# Unigine SDK paths - EXACT MATCH TO GANTRYLABELTOOL
set(UNIGINE_SDK_DIR "C:/Users/snare.ext/AppData/Local/unigine/browser/sdks/sim_windows_2.18.0.1_bin")
set(UNIGINE_INCLUDE_DIR "${UNIGINE_SDK_DIR}/include")
set(UNIGINE_LIB_DIR "${UNIGINE_SDK_DIR}/lib")

# Deploy path - EXACT MATCH TO GANTRYLABELTOOL
set(DEPLOY_DIR "C:/Users/snare.ext/Documents/UNIGINE Projects/unigine_project_3/bin/plugins/${vendor_name}/${target_name}")

set(binary_name ${target_name}_editorplugin)
string(APPEND binary_name "_double")
string(APPEND binary_name "_x64")

# Include directories - EXACT MATCH TO GANTRYLABELTOOL
include_directories(
    ${UNIGINE_INCLUDE_DIR}
    ${UNIGINE_INCLUDE_DIR}/editor
    ${CMAKE_CURRENT_SOURCE_DIR}/source
)

# Source files - Single plugin with all terrain logic integrated
set(SOURCES
    source/TerrainSurfaceToolEditorPlugin.cpp
)

set(HEADERS
    source/TerrainSurfaceToolEditorPlugin.h
)

# Create shared library (DLL) - EXACT MATCH TO GANTRYLABELTOOL
add_library(${target_name} SHARED ${SOURCES} ${HEADERS})

# Link libraries - EXACT MATCH TO GANTRYLABELTOOL
target_link_libraries(${target_name}
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    "${UNIGINE_LIB_DIR}/EditorCore_double_x64.lib"
    "${UNIGINE_LIB_DIR}/Unigine_double_x64.lib"
)

# Output directory and plugin filename format - EXACT MATCH TO GANTRYLABELTOOL
set_target_properties(${target_name} PROPERTIES
    OUTPUT_NAME ${binary_name}$<$<CONFIG:Debug>:d>
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DEPLOY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DEPLOY_DIR}"
)
