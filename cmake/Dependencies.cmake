# All third-party code is fetched at configure time. Nothing is vendored in-tree.
#
# Every dependency comes down as a release archive pinned by SHA-256 rather than
# as a git clone. A hash pins a source tree at least as firmly as a tag, the
# download is a fraction of the size, and it sidesteps a failure that made this
# project unbuildable on Windows for a while: FetchContent periodically decides
# to re-populate a dependency, and if anything still holds the git working tree
# -- a scanner, an orphaned git process on .git/shallow.lock -- the removal
# fails and the configure wedges until the directory is deleted by hand. An
# extracted archive has no working tree to hold.
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- GLM (math)
FetchContent_Declare(glm
    URL      https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz
    URL_HASH SHA256=6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001
    SYSTEM)

# ---------------------------------------------------- GLFW (window + context)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    URL      https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
    SYSTEM)

# ------------------------------------------------------------ nlohmann/json
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    URL      https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
    URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
    SYSTEM)

# -------------------------------------------------------------------- Catch2
if(PLANT_BUILD_TESTS)
    FetchContent_Declare(Catch2
        URL      https://github.com/catchorg/Catch2/archive/refs/tags/v3.9.1.tar.gz
        URL_HASH SHA256=a215c2a723bd7483efd236dc86066842a389cb4e344c61119c978acdf24d39be
        SYSTEM)
endif()

# ---------------------------------------------------------------- Dear ImGui
# ImGui ships no build system; the sources are fetched and compiled by us
# below into a static `imgui` target (core + GLFW/OpenGL3 backends).
FetchContent_Declare(imgui
    URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz
    URL_HASH SHA256=af97ed649182c39314320514a672b82008ab462b9293fe23d37b30bfa5d05519)

# ------------------------------------------------------------------ stb
# Screenshot PNG encoding. stb publishes no tags at all, only a moving master,
# so this is pinned to a commit archive -- less readable than a version, and
# the only reproducible choice available.
FetchContent_Declare(stb
    URL      https://github.com/nothings/stb/archive/31c1ad37456438565541f4919958214b6e762fb4.tar.gz
    URL_HASH SHA256=e4e3bba9c572a4a4148373a914d88ea0f0d11de8cc2c66739926e7eca0223319)

FetchContent_MakeAvailable(glm glfw nlohmann_json imgui stb)
if(PLANT_BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
    # Provides catch_discover_tests(), which registers each TEST_CASE with CTest.
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# stb is header-only with the implementation behind a define; the single
# translation unit that provides it is generated rather than checked in.
set(stb_impl "${CMAKE_BINARY_DIR}/stb_image_write_impl.c")
file(WRITE "${stb_impl}"
     "#define STB_IMAGE_WRITE_IMPLEMENTATION\n#include <stb_image_write.h>\n")
add_library(stb STATIC "${stb_impl}")
target_include_directories(stb SYSTEM PUBLIC "${stb_SOURCE_DIR}")
add_library(stb::stb ALIAS stb)

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
