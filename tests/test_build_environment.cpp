// Milestone 1 has no domain logic to test yet. This checks the two things the
// rest of the suite will assume: that the test binary is compiled as C++20 and
// that GLM is on the include path with the conventions we rely on
// (right-handed, radians-only angles).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

TEST_CASE("toolchain provides C++20", "[build]") {
    STATIC_REQUIRE(__cplusplus >= 202002L);
}

TEST_CASE("glm rotates in radians about an arbitrary axis", "[build][glm]") {
    // The turtle interpreter leans on exactly this: rotate a heading 90 degrees
    // about the world up axis.
    const glm::vec3 heading(1.0f, 0.0f, 0.0f);
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 turned = glm::vec3(rot * glm::vec4(heading, 0.0f));

    using Catch::Matchers::WithinAbs;
    REQUIRE_THAT(turned.x, WithinAbs(0.0, 1e-5));
    REQUIRE_THAT(turned.y, WithinAbs(0.0, 1e-5));
    REQUIRE_THAT(turned.z, WithinAbs(-1.0, 1e-5));
}
