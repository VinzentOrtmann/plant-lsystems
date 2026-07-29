#pragma once

#include "lsystem/LSystem.h"

#include <string_view>
#include <vector>

/// Hardcoded starter grammars. These are the fallback set the viewer always
/// has available; `assets/presets/` will carry the same shape as JSON so rule
/// sets can be edited without rebuilding.
///
/// Every grammar rewrites down to the turtle alphabet:
///   F(len,width)  draw a segment       [ ]  push/pop the turtle state
///   +(a) -(a)     yaw left/right       &(a) ^(a)  pitch down/up
///   \(a) /(a)     roll left/right
/// The capital letters A and B are non-drawing "buds" that only exist to carry
/// state between iterations; by the last iteration they have all been replaced.
///
/// Lengths are in world units and widths are radii, so `F(1,0.1)` is a segment
/// one unit long and one tenth of a unit thick.
namespace plant::lsystem::presets {

/// Monopodial tree, after the classic construction in The Algorithmic Beauty of
/// Plants. Three buds take turns: A extends the trunk and sheds one lateral per
/// node, while B and C alternate to make each lateral branch fork left and right
/// in turn.
///
/// The structure that matters is the unbracketed bud at the end of every rule.
/// It continues the axis, so the trunk keeps a central leader instead of
/// dissolving into a bundle of equal branches that drifts off to one side. The
/// 137.5 degree roll is the golden angle, which keeps successive laterals from
/// stacking on top of each other -- the same spiral phyllotaxis real trees use.
inline GrammarSource simpleTree() {
    GrammarSource source;
    source.name = "simple-tree";
    source.description = "Monopodial tree with a central leader and golden-angle laterals.";
    source.constants = {
        {"trunkAngle", 40.0f},   // lateral pitched off the trunk
        {"branchAngle", 45.0f},  // fork angle within a lateral
        {"divergence", 137.5f},  // golden angle
        {"extend", 0.9f},        // contraction along an axis
        {"fork", 0.6f},          // contraction into a new branch
        {"taper", 0.707f},       // width contraction, i.e. area halves at a fork
        {"minLength", 0.04f},
    };
    source.axiom = "A(1, 0.08)";
    source.rules = {
        "A(l,w) : l > minLength -> F(l,w)"
        " [ &(trunkAngle) B(l*fork, w*taper) ]"
        " /(divergence) A(l*extend, w*taper)",

        "B(l,w) : l > minLength -> F(l,w)"
        " [ -(branchAngle) $ C(l*fork, w*taper) ]"
        " C(l*extend, w*taper)",

        "C(l,w) : l > minLength -> F(l,w)"
        " [ +(branchAngle) $ B(l*fork, w*taper) ]"
        " B(l*extend, w*taper)",

        // Terminal cases: a spent bud becomes one last segment instead of
        // lingering in the word as a non-drawing symbol.
        "A(l,w) : l <= minLength -> F(l,w)",
        "B(l,w) : l <= minLength -> F(l,w)",
        "C(l,w) : l <= minLength -> F(l,w)",
    };
    return source;
}

/// Fern frond: a rachis that keeps extending while shedding a pair of pinnae at
/// every node, each pinna itself a smaller copy of the same pattern.
///
/// The small roll on the rachis matters more than it looks. Yaw turns about U,
/// so a frond built from +/- alone stays perfectly flat no matter how many
/// nodes it grows -- and so does one rolled by 180 degrees, which only swaps a
/// symmetric pair for itself. Rolling a few degrees per node tilts U off the
/// world axis and lets the frond twist along its length.
inline GrammarSource fern() {
    GrammarSource source;
    source.name = "fern";
    source.description = "Frond with paired pinnae, self-similar down two levels.";
    source.constants = {{"pinnaAngle", 60.0f}, {"leafletAngle", 45.0f}, {"twist", 18.0f}};
    source.axiom = "A(1, 0.05)";
    source.rules = {
        "A(l,w) : l > 0.1 -> F(l,w)"
        " [ +(pinnaAngle) &(12) B(l*0.55, w*0.4) ]"
        " [ -(pinnaAngle) &(12) B(l*0.55, w*0.4) ]"
        " /(twist) A(l*0.88, w*0.9)",
        "A(l,w) : l <= 0.1 -> F(l,w)",

        "B(l,w) : l > 0.04 -> F(l,w)"
        " [ +(leafletAngle) B(l*0.5, w*0.5) ]"
        " [ -(leafletAngle) B(l*0.5, w*0.5) ]"
        " B(l*0.7, w*0.8)",
        "B(l,w) : l <= 0.04 -> F(l,w)",
    };
    return source;
}

/// Stochastic bush, varying in both of the ways an L-system can.
///
/// Weighted productions vary the *structure*: three competing rules for the
/// same bud, picked by the seed. Weights are relative, not probabilities, so
/// 3/2/1 means the forked shape is chosen half the time. random() then varies
/// the *numbers* within whichever rule fired. Structure alone still leaves every
/// branch at a given depth geometrically identical, which reads as artificial
/// however good the branching is.
inline GrammarSource bush() {
    GrammarSource source;
    source.name = "bush";
    source.description = "Stochastic shrub; the seed picks the habit and jitters the geometry.";
    source.constants = {{"a", 28.0f}, {"divergence", 137.5f}};
    source.axiom = "A(1, 0.08)";
    source.rules = {
        // Fork into a roughly symmetric pair.
        "A(l,w) : l > 0.1 -> F(l,w)"
        " [ +(a*random(0.7,1.4)) A(l*random(0.65,0.85), w*0.7) ] /(divergence)"
        " [ -(a*random(0.7,1.4)) A(l*random(0.65,0.85), w*0.7) ] : 3",
        // Nod over, then throw one branch back upwards.
        "A(l,w) : l > 0.1 -> F(l,w)"
        " [ &(a*random(1.1,1.7)) A(l*0.7, w*0.7) ] /(divergence)"
        " [ ^(a*random(0.8,1.2)) A(l*0.8, w*0.75) ] : 2",
        // Simply extend, which is what produces the occasional long bare shoot.
        "A(l,w) : l > 0.1 -> F(l*random(0.9,1.3), w) /(divergence) A(l*0.9, w*0.9) : 1",

        "A(l,w) : l <= 0.1 -> F(l,w)",
    };
    return source;
}

/// Context-sensitive development, after the signal-driven models in chapter 3
/// of The Algorithmic Beauty of Plants.
///
/// An apex keeps adding internodes at the top while a signal `S` climbs the
/// stem from the base, one internode per step, dropping a lateral bud as it
/// passes. The rule that moves it,
///
///     S < F(l,w) -> F(l,w) [ ... ] S
///
/// fires only on the internode directly above the signal, which is what makes
/// this a genuinely context-sensitive grammar rather than a parametric one.
///
/// The payoff is the age gradient: a bud dropped early has had many more steps
/// to develop than one dropped near the top, so the plant is broad at the
/// bottom and tapers to a point without any rule saying so.
inline GrammarSource signalStem() {
    GrammarSource source;
    source.name = "signal";
    source.description = "Context-sensitive: a signal climbs the stem, leaving laterals to age.";
    source.constants = {
        {"lateralAngle", 50.0f},
        {"forkAngle", 40.0f},
        {"divergence", 137.5f},
        {"internode", 0.35f},
        {"stemWidth", 0.045f},
    };
    source.axiom = "S F(internode, stemWidth) A(internode, stemWidth)";
    source.rules = {
        // The apex extends the stem, staying just ahead of the signal.
        "A(l,w) : l > 0.12 -> F(l,w) A(l*0.96, w*0.96)",
        "A(l,w) : l <= 0.12 -> F(l,w)",

        // The signal advances into the internode above it and leaves a bud.
        "S < F(l,w) -> F(l,w) [ +(lateralAngle) L(l*0.8, w*0.6) ] /(divergence) S",
        // ...and the old signal is consumed. An empty successor deletes a module.
        "S -> ",

        // Buds then develop on their own, for as many steps as they have left.
        // The slow 0.88 contraction is deliberate: it takes a bud a dozen steps
        // to finish, so how long ago the signal passed still shows.
        "L(l,w) : l > 0.05 -> F(l,w) [ +(forkAngle) L(l*0.5, w*0.5) ] L(l*0.88, w*0.9)",
        "L(l,w) : l <= 0.05 -> F(l,w)",
    };
    return source;
}

/// Leafy shoot: the same bracketed branching as the others, but every twig ends
/// in a filled polygon rather than a bare stub.
///
/// The leaf is traced by walking the turtle around its outline and recording a
/// vertex with `.` at each corner. Starting from the base and heading along the
/// twig, `+(45) f(s)` reaches the left corner, then two `-(90)` turns walk on to
/// the tip and back down the right side, closing a diamond. Because it is the
/// ordinary turtle doing the walking, the leaf inherits the twig's orientation
/// for free -- leaves face whichever way their branch grew.
inline GrammarSource leafyShoot() {
    GrammarSource source;
    source.name = "leafy";
    source.description = "Branching shoot whose twigs end in filled polygon leaves.";
    source.constants = {
        {"branchAngle", 32.0f},
        {"divergence", 137.5f},
        {"leafSize", 0.16f},
        {"minLength", 0.16f},
    };
    source.axiom = "A(1, 0.05)";
    source.rules = {
        "A(l,w) : l > minLength -> F(l,w)"
        " [ +(branchAngle*random(0.8,1.2)) A(l*0.62, w*0.7) ] /(divergence)"
        " [ -(branchAngle*random(0.8,1.2)) A(l*0.62, w*0.7) ] /(divergence)"
        " A(l*0.78, w*0.85)",

        // A spent bud becomes one last twig carrying a leaf.
        "A(l,w) : l <= minLength -> F(l,w) [ L(leafSize) ]",

        // The outline itself: base, left corner, tip, right corner.
        "L(s) -> { . +(45) f(s) . -(90) f(s) . -(90) f(s) . }",
    };
    return source;
}

inline std::vector<GrammarSource> all() {
    return {simpleTree(), fern(), bush(), signalStem(), leafyShoot()};
}

/// Returns nullptr if no preset carries that name.
inline const GrammarSource* find(std::string_view name) {
    static const std::vector<GrammarSource> presets = all();
    for (const GrammarSource& preset : presets) {
        if (preset.name == name) {
            return &preset;
        }
    }
    return nullptr;
}

}  // namespace plant::lsystem::presets
