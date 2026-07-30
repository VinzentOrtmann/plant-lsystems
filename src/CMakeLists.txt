# Each pipeline stage is its own library so the tests can link the pure
# computational stages (lsystem/turtle/voxelize/export) without dragging in a
# window or an OpenGL context.
#
# ------------------------------------------------------------------ lsystem
# Deliberately free of dependencies -- not even GLM. Rewriting is pure symbol
# manipulation, so the tests run without any graphics stack.
add_library(plant_lsystem STATIC
    lsystem/LSystem.cpp)
add_library(plant::lsystem ALIAS plant_lsystem)

target_include_directories(plant_lsystem PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(plant_lsystem PRIVATE plant::options)

# Grammar JSON, kept out of plant_lsystem on purpose: rewriting stays free of
# dependencies, and its tests build without nlohmann/json.
add_library(plant_grammar_io STATIC
    lsystem/GrammarIO.cpp)
add_library(plant::grammar_io ALIAS plant_grammar_io)

target_include_directories(plant_grammar_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(plant_grammar_io
    PUBLIC  plant::lsystem
    PRIVATE plant::options nlohmann_json::nlohmann_json)

# -------------------------------------------------------------------- turtle
add_library(plant_turtle STATIC
    turtle/Turtle.cpp)
add_library(plant::turtle ALIAS plant_turtle)

target_include_directories(plant_turtle PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(plant_turtle
    PUBLIC  plant::lsystem glm::glm
    PRIVATE plant::options)

# ------------------------------------------------------------------ colonize
# A second, unrelated way to grow a tree. It knows nothing about L-systems and
# depends on plant::turtle only for the Skeleton type, which is the pipeline's
# shared intermediate representation rather than a turtle detail.
add_library(plant_colonize STATIC
    colonize/Colonize.cpp)
add_library(plant::colonize ALIAS plant_colonize)

target_include_directories(plant_colonize PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(plant_colonize
    PUBLIC  plant::turtle glm::glm
    PRIVATE plant::options)

# ------------------------------------------------------------------ voxelize
add_library(plant_voxelize STATIC
    voxelize/VoxelGrid.cpp
    voxelize/Rasterizer.cpp
    voxelize/Metrics.cpp)
add_library(plant::voxelize ALIAS plant_voxelize)

target_include_directories(plant_voxelize PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(plant_voxelize
    PUBLIC  plant::turtle glm::glm
    PRIVATE plant::options)

# -------------------------------------------------------------------- export
add_library(plant_export STATIC
    export/VoxWriter.cpp)
add_library(plant::export ALIAS plant_export)

target_include_directories(plant_export PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(plant_export
    PUBLIC  plant::voxelize
    PRIVATE plant::options)

# ------------------------------------------------------------------- viewer
add_library(plant_viewer STATIC
    viewer/Renderer.cpp
    viewer/UI.cpp)
add_library(plant::viewer ALIAS plant_viewer)

target_include_directories(plant_viewer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(plant_viewer
    PUBLIC  glad_gl_core_33 glfw glm::glm
    PRIVATE plant::options imgui::imgui stb::stb)

# ---------------------------------------------------------------- executable
add_executable(plant-gen main.cpp)
target_link_libraries(plant-gen
    PRIVATE plant::options plant::viewer plant::turtle plant::voxelize plant::export plant::grammar_io plant::colonize
            imgui::imgui)

# GLFW pulls in the Win32 console subsystem by default, which is what we want
# during development: the app logs to stdout.
set_target_properties(plant-gen PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
