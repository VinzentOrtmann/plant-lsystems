# All third-party code is fetched at configure time. Nothing is vendored in-tree.
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# Every dependency below is pinned to a tag, so re-running `git fetch` on each
# reconfigure can only ever produce the commit already checked out. Skipping it
# also avoids a failure mode seen on Windows, where a virus scanner still holds
# the freshly cloned tree and the update step dies removing it.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# ---------------------------------------------------------------- GLM (math)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
    SYSTEM)

# ---------------------------------------------------- GLFW (window + context)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
    SYSTEM)

# ------------------------------------------------------------ nlohmann/json
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
    SYSTEM)

# -------------------------------------------------------------------- Catch2
if(PLANT_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.9.1
        GIT_SHALLOW    TRUE
        SYSTEM)
endif()

# ---------------------------------------------------------------- Dear ImGui
# ImGui ships no build system; the sources are fetched and compiled by us
# below into a static `imgui` target (core + GLFW/OpenGL3 backends).
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9
    GIT_SHALLOW    TRUE)

# ------------------------------------------------------------------ stb
# Screenshot PNG encoding. stb publishes no tags at all, only a moving master,
# so this is pinned to a commit -- less readable than a version, but the only
# reproducible choice. Shallow cloning is off because a bare SHA is not a ref.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4)

FetchContent_MakeAvailable(glm glfw nlohmann_json imgui stb)

# stb is header-only with the implementation behind a define; the single
# translation unit that provides it is generated rather than checked in.
set(stb_impl "${CMAKE_BINARY_DIR}/stb_image_write_impl.c")
file(WRITE "${stb_impl}"
     "#define STB_IMAGE_WRITE_IMPLEMENTATION\n#include <stb_image_write.h>\n")
add_library(stb STATIC "${stb_impl}")
target_include_directories(stb SYSTEM PUBLIC "${stb_SOURCE_DIR}")
add_library(stb::stb ALIAS stb)
if(PLANT_BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
    # Provides catch_discover_tests(), which registers each TEST_CASE with CTest.
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# glad has its own generator step; kept in a separate module for readability.
include(Glad)

# ------------------------------------------------- Dear ImGui build target
add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
target_include_directories(imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui PUBLIC glfw)
target_compile_features(imgui PUBLIC cxx_std_17)
add_library(imgui::imgui ALIAS imgui)
