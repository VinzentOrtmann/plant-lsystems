cmake_minimum_required(VERSION 3.25)

project(plant-lsystems
    VERSION 0.1.0
    DESCRIPTION "Procedural plant generator: L-systems -> voxels -> MagicaVoxel .vox"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PLANT_BUILD_TESTS "Build the Catch2 test suite" ON)

if(PROJECT_IS_TOP_LEVEL)
    # Multi-config generators append the configuration name, giving
    # bin/Debug/ and bin/RelWithDebInfo/. Do NOT flatten those into one
    # directory: the configs would overwrite each other, and `ctest -C Debug`
    # would happily run whichever build landed there last.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Compile options shared by first-party targets only (never leaked to deps).
add_library(plant_options INTERFACE)
add_library(plant::options ALIAS plant_options)
target_compile_features(plant_options INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(plant_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(plant_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

include(Dependencies)

add_subdirectory(src)

# Without this, opening the generated .sln selects ALL_BUILD, which cannot run.
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT plant-gen)

if(PLANT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------- packaging
# `cmake --install build/msvc --config RelWithDebInfo --prefix dist` produces a
# runnable folder; `cpack` zips it. The grammars ship alongside the executable
# rather than inside it, so an installed copy is still editable.
install(TARGETS plant-gen RUNTIME DESTINATION .)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/" DESTINATION assets)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION .)

set(CPACK_PACKAGE_NAME "plant-lsystems")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_FILE_NAME "plant-lsystems-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
# A directory, not an installer: this is a thing you unzip and run.
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "plant-lsystems")
include(CPack)

message(STATUS "plant-lsystems ${PROJECT_VERSION} | tests: ${PLANT_BUILD_TESTS}")
