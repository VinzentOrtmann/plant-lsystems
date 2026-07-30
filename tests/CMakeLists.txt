include(Catch)

# One test binary for the whole suite; per-milestone sources are appended here
# as the L-system, turtle, voxelizer and .vox writer land.
add_executable(plant_tests
    test_build_environment.cpp
    test_lsystem.cpp
    test_turtle.cpp
    test_voxelize.cpp
    test_voxwriter.cpp
    test_grammar_io.cpp
    test_golden.cpp
    test_metrics.cpp
    test_colonize.cpp)

target_link_libraries(plant_tests
    PRIVATE plant::options glm::glm plant::lsystem plant::turtle plant::voxelize plant::export plant::grammar_io plant::colonize
            Catch2::Catch2WithMain)

catch_discover_tests(plant_tests)
