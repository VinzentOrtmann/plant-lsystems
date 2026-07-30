#include "voxelize/Rasterizer.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace plant::voxelize {

bool insideRoundCone(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b, float radiusA,
                     float radiusB) {
    // The swept solid is the union over t in [0,1] of the sphere centred at
    // c(t) = a + t*(b - a) with radius r(t) = radiusA + t*(radiusB - radiusA).
    // The point is inside when some t satisfies |p - c(t)|^2 - r(t)^2 <= 0.
    // Expanding that leaves a quadratic in t:
    //
    //     f(t) = A*t^2 - 2*B*t + C
    //     A = |d|^2 - dr^2,  B = dot(pa, d) + radiusA*dr,  C = |pa|^2 - radiusA^2
    //
    // so the test reduces to minimising a parabola over [0,1]. Note this is the
    // true union of spheres, not the cheaper "project onto the axis, compare
    // against the interpolated radius" test -- that one under-fills a tapered
    // segment, because the nearest point on the axis is not the sphere that
    // reaches furthest.
    const glm::vec3 axis = b - a;
    const glm::vec3 relative = point - a;
    const float deltaRadius = radiusB - radiusA;

    const float A = glm::dot(axis, axis) - deltaRadius * deltaRadius;
    const float B = glm::dot(relative, axis) + radiusA * deltaRadius;
    const float C = glm::dot(relative, relative) - radiusA * radiusA;

    if (A <= 0.0f) {
        // One end sphere swallows the other: the union is just the bigger one.
        const glm::vec3 toEnd = point - b;
        return C <= 0.0f || glm::dot(toEnd, toEnd) <= radiusB * radiusB;
    }

    const float t = std::clamp(B / A, 0.0f, 1.0f);
    return A * t * t - 2.0f * B * t + C <= 0.0f;
}

glm::vec3 closestPointOnTriangle(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b,
                                 const glm::vec3& c) {
    // Voronoi-region test: the nearest point lies in one of seven regions --
    // the three vertices, the three edges, or the face interior. Checking them
    // in this order lets each case fall out of a couple of dot products.
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;

    const glm::vec3 ap = point - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }

    const glm::vec3 bp = point - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return a + ab * (d1 / (d1 - d3));
    }

    const glm::vec3 cp = point - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }

    const float denominator = va + vb + vc;
    if (!(std::fabs(denominator) > 0.0f)) {
        return a;  // degenerate triangle: every point of it is as near as any
    }
    const float inverse = 1.0f / denominator;
    return a + ab * (vb * inverse) + ac * (vc * inverse);
}

float voxelSizeForResolution(const turtle::Skeleton& skeleton, int longestAxisVoxels) {
    if (skeleton.empty() || longestAxisVoxels <= 0) {
        return 1.0f;
    }
    const glm::vec3 extent = skeleton.size();
    const float longest = std::max({extent.x, extent.y, extent.z});
    if (!(longest > 0.0f)) {
        return 1.0f;
    }
    return longest / static_cast<float>(longestAxisVoxels);
}

namespace {

/// Bark at the thick end of the range, foliage green at the thin end.
///
/// Keyed on thickness rather than branch depth, because depth means different
/// things to different generators: the turtle's is bracket nesting, while space
/// colonization increments it at every non-first child, so it saturates almost
/// immediately and paints even a limb off the trunk as a twig. Thickness is the
/// same physical quantity either way.
///
/// Logarithmic, because radii span more than an order of magnitude: mapped
/// linearly, everything but the trunk collapses into the thinnest slot.
ColorIndex colorForRadius(float radius, float thinnest, float thickest,
                          const RasterizerConfig& config) {
    const int count = std::max(1, config.colorCount);
    if (!(thickest > thinnest) || !(radius > 0.0f)) {
        return config.firstColor;
    }
    const float span = std::log(thickest / thinnest);
    const float fraction =
        span > 1e-6f ? std::clamp(std::log(thickest / radius) / span, 0.0f, 1.0f) : 0.0f;
    const int step =
        std::clamp(static_cast<int>(fraction * static_cast<float>(count)), 0, count - 1);
    return static_cast<ColorIndex>(config.firstColor + step);
}

}  // namespace

VoxelGrid voxelize(const turtle::Skeleton& skeleton, const RasterizerConfig& config) {
    if (!(config.voxelSize > 0.0f)) {
        throw VoxelizeError("voxel size must be positive");
    }
    if (skeleton.empty()) {
        return VoxelGrid({0, 0, 0}, glm::vec3(0.0f), config.voxelSize);
    }

    const float minRadius = config.minRadiusVoxels * config.voxelSize;
    const float halfThickness = 0.5f * config.polygonThicknessVoxels * config.voxelSize;

    // Sized from the radii actually rasterized, not from Skeleton::bounds: the
    // minimum-radius floor inflates twigs thinner than a voxel, and a grid built
    // from the skeleton's own bounds would clip that growth against its walls.
    // Polygon slabs bulge past their vertices by half their thickness for the
    // same reason.
    glm::vec3 solidMin(std::numeric_limits<float>::max());
    glm::vec3 solidMax(std::numeric_limits<float>::lowest());
    // Range the colour ramp is stretched across.
    float thinnest = std::numeric_limits<float>::max();
    float thickest = 0.0f;
    for (const turtle::Segment& segment : skeleton.segments) {
        const float radiusA = std::max(segment.startRadius, minRadius);
        const float radiusB = std::max(segment.endRadius, minRadius);
        thinnest = std::min(thinnest, segment.startRadius);
        thickest = std::max(thickest, segment.startRadius);
        solidMin = glm::min(solidMin, glm::min(segment.start - radiusA, segment.end - radiusB));
        solidMax = glm::max(solidMax, glm::max(segment.start + radiusA, segment.end + radiusB));
    }
    for (const turtle::Polygon& polygon : skeleton.polygons) {
        for (const glm::vec3& vertex : polygon.vertices) {
            solidMin = glm::min(solidMin, vertex - halfThickness);
            solidMax = glm::max(solidMax, vertex + halfThickness);
        }
    }

    // Plus one voxel of padding on every side, so a branch grazing the box does
    // not get clipped by float rounding on the way into index space.
    const glm::vec3 origin = solidMin - glm::vec3(config.voxelSize);
    const glm::vec3 span = (solidMax + glm::vec3(config.voxelSize)) - origin;

    glm::ivec3 dimensions;
    for (int axis = 0; axis < 3; ++axis) {
        dimensions[axis] = std::max(1, static_cast<int>(std::ceil(span[axis] / config.voxelSize)));
        if (dimensions[axis] > config.maxDimension) {
            throw VoxelizeError("voxel size " + std::to_string(config.voxelSize) + " needs a " +
                                std::to_string(dimensions[axis]) + " voxel axis, past the limit of " +
                                std::to_string(config.maxDimension) +
                                "; raise maxDimension or use voxelSizeForResolution()");
        }
    }

    VoxelGrid grid(dimensions, origin, config.voxelSize);

    for (const turtle::Segment& segment : skeleton.segments) {
        const float radiusA = std::max(segment.startRadius, minRadius);
        const float radiusB = std::max(segment.endRadius, minRadius);
        const ColorIndex color = colorForRadius(segment.startRadius, thinnest, thickest, config);

        // Index-space bounding box of the swept solid.
        const glm::vec3 lo = glm::min(segment.start - radiusA, segment.end - radiusB);
        const glm::vec3 hi = glm::max(segment.start + radiusA, segment.end + radiusB);
        const glm::ivec3 first = glm::max(
            glm::ivec3(glm::floor((lo - origin) / config.voxelSize)), glm::ivec3(0));
        const glm::ivec3 last = glm::min(
            glm::ivec3(glm::ceil((hi - origin) / config.voxelSize)), dimensions - 1);

        for (int z = first.z; z <= last.z; ++z) {
            for (int y = first.y; y <= last.y; ++y) {
                for (int x = first.x; x <= last.x; ++x) {
                    const glm::ivec3 position(x, y, z);
                    if (insideRoundCone(grid.voxelCenter(position), segment.start, segment.end,
                                        radiusA, radiusB)) {
                        grid.set(position, color);
                    }
                }
            }
        }
    }

    // Polygons are fanned into triangles from their first vertex, which is
    // exact for the convex outlines grammars actually trace.
    for (const turtle::Polygon& polygon : skeleton.polygons) {
        for (std::size_t corner = 1; corner + 1 < polygon.vertices.size(); ++corner) {
            const glm::vec3& a = polygon.vertices[0];
            const glm::vec3& b = polygon.vertices[corner];
            const glm::vec3& c = polygon.vertices[corner + 1];

            const glm::vec3 lo = glm::min(a, glm::min(b, c)) - halfThickness;
            const glm::vec3 hi = glm::max(a, glm::max(b, c)) + halfThickness;
            const glm::ivec3 first =
                glm::max(glm::ivec3(glm::floor((lo - origin) / config.voxelSize)), glm::ivec3(0));
            const glm::ivec3 last =
                glm::min(glm::ivec3(glm::ceil((hi - origin) / config.voxelSize)), dimensions - 1);

            for (int z = first.z; z <= last.z; ++z) {
                for (int y = first.y; y <= last.y; ++y) {
                    for (int x = first.x; x <= last.x; ++x) {
                        const glm::ivec3 position(x, y, z);
                        const glm::vec3 centre = grid.voxelCenter(position);
                        const glm::vec3 nearest = closestPointOnTriangle(centre, a, b, c);
                        if (glm::dot(centre - nearest, centre - nearest) <=
                            halfThickness * halfThickness) {
                            grid.set(position, config.polygonColor);
                        }
                    }
                }
            }
        }
    }

    return grid;
}

}  // namespace plant::voxelize
