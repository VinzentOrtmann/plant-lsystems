#include "lsystem/LSystem.h"
#include "lsystem/Presets.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace plant::lsystem;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

GrammarSource grammar(std::string axiom,
                      std::vector<std::string> rules,
                      std::map<std::string, float> constants = {}) {
    GrammarSource source;
    source.name = "test";
    source.axiom = std::move(axiom);
    source.rules = std::move(rules);
    source.constants = std::move(constants);
    return source;
}

std::string expandToString(const GrammarSource& source, int iterations, std::uint32_t seed = 0) {
    return toString(LSystem::compile(source).expand(iterations, seed));
}

bool bracketsBalanced(const Word& word) {
    int depth = 0;
    for (const Module& module : word) {
        if (module.symbol == '[') {
            ++depth;
        } else if (module.symbol == ']') {
            if (--depth < 0) {
                return false;
            }
        }
    }
    return depth == 0;
}

std::size_t countSymbol(const Word& word, Symbol symbol) {
    return static_cast<std::size_t>(
        std::count_if(word.begin(), word.end(),
                      [symbol](const Module& m) { return m.symbol == symbol; }));
}

}  // namespace

// ---------------------------------------------------------------------------
// Rewriting
// ---------------------------------------------------------------------------

TEST_CASE("non-parametric rewriting reproduces Lindenmayer's algae", "[lsystem]") {
    // The original 1968 example. Word lengths follow the Fibonacci sequence,
    // which makes a wrong answer obvious rather than merely different.
    const GrammarSource algae = grammar("A", {"A -> A B", "B -> A"});

    CHECK(expandToString(algae, 0) == "A");
    CHECK(expandToString(algae, 1) == "AB");
    CHECK(expandToString(algae, 2) == "ABA");
    CHECK(expandToString(algae, 3) == "ABAAB");
    CHECK(expandToString(algae, 4) == "ABAABABA");
    CHECK(expandToString(algae, 5) == "ABAABABAABAAB");

    const Word word = LSystem::compile(algae).expand(10);
    CHECK(word.size() == 144);
}

TEST_CASE("rewriting is parallel, not sequential", "[lsystem]") {
    // Applied sequentially, the B produced from A would immediately be rewritten
    // again within the same step and the result would be "AAA" rather than "BA".
    CHECK(expandToString(grammar("AB", {"A -> B", "B -> A"}), 1) == "BA");
    CHECK(expandToString(grammar("AB", {"A -> B", "B -> A"}), 2) == "AB");
}

TEST_CASE("modules without a matching production pass through unchanged", "[lsystem]") {
    // Turtle symbols carry no productions of their own; they have to survive
    // every iteration untouched.
    CHECK(expandToString(grammar("F(1)[+X]", {"F(l) -> F(l*0.5)"}), 1) == "F(0.5)[+X]");
}

TEST_CASE("arity is part of the match", "[lsystem]") {
    // A(x) must not fire on a bare A: the successor would read a parameter
    // that does not exist.
    CHECK(expandToString(grammar("A", {"A(x) -> F(x)"}), 3) == "A");
    CHECK(expandToString(grammar("A(2)", {"A(x) -> F(x)"}), 1) == "F(2)");
    CHECK(expandToString(grammar("A(2)", {"A(x,y) -> F(x)"}), 1) == "A(2)");
}

// ---------------------------------------------------------------------------
// Parametric expressions
// ---------------------------------------------------------------------------

TEST_CASE("successor arguments are evaluated against the matched parameters", "[lsystem][expr]") {
    const GrammarSource source = grammar("A(2)", {"A(x) -> F(x*2+1) A(x/2)"});

    CHECK(expandToString(source, 1) == "F(5)A(1)");
    CHECK(expandToString(source, 2) == "F(5)F(3)A(0.5)");
    CHECK(expandToString(source, 3) == "F(5)F(3)F(2)A(0.25)");
}

TEST_CASE("conditions select between productions", "[lsystem][expr]") {
    const GrammarSource source = grammar("A(1)", {
        "A(x) : x > 0.3  -> F(x) A(x*0.5)",
        "A(x) : x <= 0.3 -> F(x)",
    });

    CHECK(expandToString(source, 1) == "F(1)A(0.5)");
    CHECK(expandToString(source, 2) == "F(1)F(0.5)A(0.25)");
    CHECK(expandToString(source, 3) == "F(1)F(0.5)F(0.25)");
    // Terminal: once every bud has become an F the word stops changing.
    CHECK(expandToString(source, 9) == "F(1)F(0.5)F(0.25)");
}

TEST_CASE("constants are usable in axioms, conditions and successors", "[lsystem][expr]") {
    const GrammarSource source = grammar("A(start)",
                                         {"A(x) : x > limit -> +(angle) A(x-1)"},
                                         {{"start", 3.0f}, {"limit", 1.0f}, {"angle", 25.0f}});

    CHECK(expandToString(source, 1) == "+(25)A(2)");
    CHECK(expandToString(source, 2) == "+(25)+(25)A(1)");
    // x is no longer > limit, so the bud stops.
    CHECK(expandToString(source, 5) == "+(25)+(25)A(1)");
}

TEST_CASE("expression operators honour precedence and grouping", "[lsystem][expr]") {
    CHECK(expandToString(grammar("A", {"A -> F(1+2*3)"}), 1) == "F(7)");
    CHECK(expandToString(grammar("A", {"A -> F((1+2)*3)"}), 1) == "F(9)");
    CHECK(expandToString(grammar("A", {"A -> F(-2+5)"}), 1) == "F(3)");
    CHECK(expandToString(grammar("A", {"A -> F(10-2-3)"}), 1) == "F(5)");
    CHECK(expandToString(grammar("A", {"A -> F(8/4/2)"}), 1) == "F(1)");
}

TEST_CASE("conditions combine comparisons with boolean operators", "[lsystem][expr]") {
    const std::vector<std::string> rules = {"A(x,y) : x > 1 && y < 5 -> F(1)"};

    CHECK(expandToString(grammar("A(2,4)", rules), 1) == "F(1)");
    CHECK(expandToString(grammar("A(0,4)", rules), 1) == "A(0,4)");
    CHECK(expandToString(grammar("A(2,9)", rules), 1) == "A(2,9)");

    // && short-circuits, so the guarded division is never evaluated.
    CHECK(expandToString(grammar("A(0)", {"A(w) : w != 0 && 1/w > 2 -> F(1)"}), 1) == "A(0)");
}

TEST_CASE("built-in functions are available, with trigonometry in degrees", "[lsystem][expr]") {
    CHECK(expandToString(grammar("A", {"A -> F(max(2,3)) F(min(2,3)) F(sqrt(16)) F(abs(-3))"}), 1) ==
          "F(3)F(2)F(4)F(3)");
    CHECK(expandToString(grammar("A", {"A -> F(pow(2,10)) F(floor(2.7)) F(clamp(9,0,5))"}), 1) ==
          "F(1024)F(2)F(5)");

    // Angles elsewhere in the grammar are degrees; the trig functions match.
    const Word word = LSystem::compile(grammar("A", {"A -> F(sin(90)) F(cos(180))"})).expand(1);
    REQUIRE(word.size() == 2);
    CHECK_THAT(word[0].params.at(0), WithinAbs(1.0, 1e-6));
    CHECK_THAT(word[1].params.at(0), WithinAbs(-1.0, 1e-6));
}

// ---------------------------------------------------------------------------
// Stochastic behaviour
// ---------------------------------------------------------------------------

TEST_CASE("a seed reproduces exactly one plant", "[lsystem][random]") {
    const GrammarSource source = grammar("A(1)", {
        "A(x) : x > 0.2 -> F(x) [ +(20) A(x*0.6) ] : 1",
        "A(x) : x > 0.2 -> F(x) [ -(20) A(x*0.6) ] : 1",
        "A(x) : x <= 0.2 -> F(x)",
    });
    const LSystem system = LSystem::compile(source);

    CHECK(toString(system.expand(6, 12345)) == toString(system.expand(6, 12345)));

    // ...and different seeds have to actually reach different plants, otherwise
    // the RNG is not being consulted at all.
    const std::string reference = toString(system.expand(6, 0));
    bool anyDifferent = false;
    for (std::uint32_t seed = 1; seed <= 20 && !anyDifferent; ++seed) {
        anyDifferent = toString(system.expand(6, seed)) != reference;
    }
    CHECK(anyDifferent);
}

TEST_CASE("stepping incrementally matches expanding from scratch", "[lsystem][random]") {
    // What lets the viewer cache generations and scrub through them: expand(n)
    // is step() applied n times off one generator, so extending a cache by one
    // generation must give exactly what a fresh expand(n) would. If step() ever
    // drew from the generator differently -- an extra draw, a different order --
    // playback would silently diverge from the seed it claims to show.
    const LSystem system = LSystem::compile(presets::bush());
    const std::uint32_t seed = 4242;

    std::mt19937 rng(seed);
    Word incremental = system.axiom();
    CHECK(toString(incremental) == toString(system.expand(0, seed)));

    for (int generation = 1; generation <= 12; ++generation) {
        CAPTURE(generation);
        incremental = system.step(incremental, rng);
        CHECK(toString(incremental) == toString(system.expand(generation, seed)));
    }
}

TEST_CASE("grammars without alternatives ignore the seed entirely", "[lsystem][random]") {
    // The two productions have complementary conditions, so only one can ever
    // match and no random draw happens.
    const LSystem system = LSystem::compile(grammar("A(1)", {
        "A(x) : x > 0.3  -> F(x) A(x*0.5)",
        "A(x) : x <= 0.3 -> F(x)",
    }));

    CHECK(toString(system.expand(5, 0)) == toString(system.expand(5, 999999)));
}

TEST_CASE("production weights bias the draw", "[lsystem][random]") {
    // One draw per iteration, so a 2000-iteration run is 2000 samples of a
    // 9:1 split. Expected 1800 X with a standard deviation of about 13; the
    // bounds below are far enough out that this cannot flake.
    const LSystem system = LSystem::compile(grammar("A", {"A -> X A : 9", "A -> Y A : 1"}));
    const Word word = system.expand(2000, 4242);

    REQUIRE(word.size() == 2001);
    const std::size_t xs = countSymbol(word, 'X');
    const std::size_t ys = countSymbol(word, 'Y');
    REQUIRE(xs + ys == 2000);
    CHECK(xs > 1600);
    CHECK(xs < 1950);
}

TEST_CASE("random() jitters a parameter without breaking reproducibility", "[lsystem][random]") {
    // Weighted productions vary structure; random() varies the numbers, which is
    // what stops every branch at a given depth from being identical.
    const GrammarSource source = grammar("A(1)", {"A(x) -> F(x*random(0.5,1.5)) A(x)"});
    const LSystem system = LSystem::compile(source);

    CHECK(toString(system.expand(6, 77)) == toString(system.expand(6, 77)));
    CHECK(toString(system.expand(6, 77)) != toString(system.expand(6, 78)));

    // Successive draws differ, so this is a fresh sample per module rather than
    // one value reused everywhere.
    const Word word = system.expand(5, 3);
    std::vector<float> lengths;
    for (const Module& module : word) {
        if (module.symbol == 'F') {
            lengths.push_back(module.params.at(0));
        }
    }
    REQUIRE(lengths.size() == 5);
    CHECK(std::adjacent_find(lengths.begin(), lengths.end()) == lengths.end());
    for (const float length : lengths) {
        CHECK(length >= 0.5f);
        CHECK(length <= 1.5f);
    }
}

TEST_CASE("random() draws from the same generator as the production weights",
          "[lsystem][random]") {
    // Two grammars that differ only in an unused random() call must not agree:
    // if they did, the draws would be coming from somewhere else and the seed
    // would no longer determine the whole plant.
    const LSystem plain = LSystem::compile(grammar("A(1)", {
        "A(x) -> F(x) [ +(20) A(x*0.7) ] : 1",
        "A(x) -> F(x) [ -(20) A(x*0.7) ] : 1",
    }));
    const LSystem jittered = LSystem::compile(grammar("A(1)", {
        "A(x) -> F(x*random(1,1)) [ +(20) A(x*0.7) ] : 1",
        "A(x) -> F(x*random(1,1)) [ -(20) A(x*0.7) ] : 1",
    }));

    // random(1,1) returns 1 regardless, so the words would match if the extra
    // draw did not shift the shared stream.
    CHECK(toString(plain.expand(4, 5)) != toString(jittered.expand(4, 5)));
}

TEST_CASE("random() is usable in a condition", "[lsystem][random]") {
    // An alternative to production weights: roll a die in the guard.
    const LSystem system = LSystem::compile(grammar("A", {
        "A : random(0,1) < 0.5 -> X A",
        "A -> Y A",
    }));
    const Word word = system.expand(400, 11);

    const std::size_t xs = countSymbol(word, 'X');
    const std::size_t ys = countSymbol(word, 'Y');
    CHECK(xs + ys == 400);
    // Both branches fire; the exact split is not the point.
    CHECK(xs > 50);
    CHECK(ys > 50);
}

TEST_CASE("random() is refused in the axiom", "[lsystem][random][errors]") {
    // The axiom is evaluated once at compile time, before a seed exists, so a
    // draw there would be frozen across every seed -- silently, if allowed.
    CHECK_THROWS_AS(LSystem::compile(grammar("A(random(1,2))", {"A(x) -> F(x)"})), ParseError);
    CHECK_NOTHROW(LSystem::compile(grammar("A(1)", {"A(x) -> F(random(1,2))"})));
}

// ---------------------------------------------------------------------------
// Context-sensitive productions
// ---------------------------------------------------------------------------

TEST_CASE("a production can require a left, right or two-sided context", "[lsystem][context]") {
    CHECK(expandToString(grammar("ABC", {"A < B -> X"}), 1) == "AXC");
    CHECK(expandToString(grammar("ABC", {"B > C -> X"}), 1) == "AXC");
    CHECK(expandToString(grammar("ABC", {"A < B > C -> X"}), 1) == "AXC");

    // The same rules must not fire where the context is absent.
    CHECK(expandToString(grammar("ZBC", {"A < B > C -> X"}), 1) == "ZBC");
    CHECK(expandToString(grammar("ABZ", {"A < B > C -> X"}), 1) == "ABZ");
    CHECK(expandToString(grammar("B", {"A < B -> X"}), 1) == "B");
}

TEST_CASE("context may span several modules", "[lsystem][context]") {
    CHECK(expandToString(grammar("ABCD", {"A B < C > D -> X"}), 1) == "ABXD");
    CHECK(expandToString(grammar("ZBCD", {"A B < C > D -> X"}), 1) == "ZBCD");
}

TEST_CASE("context modules bind parameters the successor can read", "[lsystem][context]") {
    // The parameter vector spans left context, predecessor and right context in
    // that order, so an expression sees all three.
    CHECK(expandToString(grammar("A(2)B(3)C(4)", {"A(x) < B(y) > C(z) -> F(x*100+y*10+z)"}), 1) ==
          "A(2)F(234)C(4)");
}

TEST_CASE("arity applies to context modules too", "[lsystem][context]") {
    CHECK(expandToString(grammar("A(1)B", {"A(x) < B -> X"}), 1) == "A(1)X");
    CHECK(expandToString(grammar("AB", {"A(x) < B -> X"}), 1) == "AB");
}

TEST_CASE("left context is the ancestor chain, not the preceding characters",
          "[lsystem][context]") {
    // In A[X]B the bracketed X is a *sibling* of B, not its parent: walking
    // back must hop the whole subtree and land on A.
    CHECK(expandToString(grammar("A[X]B", {"A < B -> Y"}), 1) == "A[X]Y");
    CHECK(expandToString(grammar("A[X]B", {"X < B -> Y"}), 1) == "A[X]B");

    // Inside a branch, the parent is the module before the '['.
    CHECK(expandToString(grammar("A[B]", {"A < B -> Y"}), 1) == "A[Y]");

    // Nested siblings are hopped just the same.
    CHECK(expandToString(grammar("A[X][Z]B", {"A < B -> Y"}), 1) == "A[X][Z]Y");
    CHECK(expandToString(grammar("A[X[Q]]B", {"A < B -> Y"}), 1) == "A[X[Q]]Y");
}

TEST_CASE("right context follows any path of descendants", "[lsystem][context]") {
    // A's children in A[B]C are both B (down the branch) and C (along the axis).
    CHECK(expandToString(grammar("A[B]C", {"A > B -> Y"}), 1) == "Y[B]C");
    CHECK(expandToString(grammar("A[B]C", {"A > C -> Y"}), 1) == "Y[B]C");
    // ...but not a module that is no descendant at all.
    CHECK(expandToString(grammar("A[B]C", {"A > Z -> Y"}), 1) == "A[B]C");

    // A right context must not escape upwards out of its branch: B ends the
    // branch it lives in, so C is not below it.
    CHECK(expandToString(grammar("A[B]C", {"B > C -> Y"}), 1) == "A[B]C");
}

TEST_CASE("a failed branch is backtracked before the next one is tried", "[lsystem][context]") {
    // Both branches hang off A. The first matches P and then dies at the ']';
    // the search has to unwind and find the full path down the second.
    CHECK(expandToString(grammar("A[P][P Q]", {"A > P Q -> Y"}), 1) == "Y[P][PQ]");

    // Neither branch completes the path.
    CHECK(expandToString(grammar("A[P][P Z]", {"A > P Q -> Y"}), 1) == "A[P][PZ]");
}

TEST_CASE("a branch belongs to the module it follows", "[lsystem][context]") {
    // In A[P]C[P Q] the second branch hangs off C, not A, so there is no
    // A -> P -> Q path even though "P Q" appears to A's right in the string.
    CHECK(expandToString(grammar("A[P]C[P Q]", {"A > P Q -> Y"}), 1) == "A[P]C[PQ]");
    CHECK(expandToString(grammar("A[P]C[P Q]", {"C > P Q -> Y"}), 1) == "A[P]Y[PQ]");
}

TEST_CASE("a signal travels up a stem one module per step", "[lsystem][context]") {
    // The canonical use of context sensitivity: S is a signal that moves into
    // whichever F sits above it, one step at a time.
    const GrammarSource source = grammar("SFFFF", {"S -> F", "S < F -> S"});

    CHECK(expandToString(source, 0) == "SFFFF");
    CHECK(expandToString(source, 1) == "FSFFF");
    CHECK(expandToString(source, 2) == "FFSFF");
    CHECK(expandToString(source, 3) == "FFFSF");
    CHECK(expandToString(source, 4) == "FFFFS");
    // Nothing above it left to move into.
    CHECK(expandToString(source, 5) == "FFFFF");
}

TEST_CASE("a signal splits at a fork", "[lsystem][context]") {
    // Both the branch and the continuing axis are children of the signalling
    // module, so the signal has to enter both. This is the whole reason context
    // is matched in the tree rather than in the string.
    const GrammarSource source = grammar("SF[FF]F", {"S -> F", "S < F -> S"});

    CHECK(expandToString(source, 1) == "FS[FF]F");
    CHECK(expandToString(source, 2) == "FF[SF]S");
    CHECK(expandToString(source, 3) == "FF[FS]F");
}

TEST_CASE("context is read from the word as it stood at the start of the step",
          "[lsystem][context]") {
    // Rewriting is parallel. If context were read from the output being built,
    // the A produced here would immediately satisfy the second rule and the
    // whole word would collapse in one step.
    CHECK(expandToString(grammar("BB", {"B -> A", "A < B -> X"}), 1) == "AA");
}

TEST_CASE("context-free productions still work alongside context-sensitive ones",
          "[lsystem][context]") {
    const GrammarSource source = grammar("ABA", {"A -> C", "A < B -> X"});
    CHECK(expandToString(source, 1) == "CXC");
}

TEST_CASE("malformed context is rejected", "[lsystem][context][errors]") {
    // Exactly one module is rewritten.
    CHECK_THROWS_AS(LSystem::compile(grammar("AB", {"A B -> X"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("AB", {"A < B C > D -> X"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("AB", {"A < > B -> X"})), ParseError);
    // Brackets are structure, not modules.
    CHECK_THROWS_AS(LSystem::compile(grammar("AB", {"[ < B -> X"})), ParseError);
    // A name cannot be bound twice across the three parts.
    CHECK_THROWS_AS(LSystem::compile(grammar("A(1)B(2)", {"A(x) < B(x) -> F(x)"})), ParseError);
}

TEST_CASE("a comparison in a condition is not mistaken for a context marker",
          "[lsystem][context][errors]") {
    // '<' and '>' are reserved on the left of '->', but the condition is split
    // off first, so comparisons there are safe.
    CHECK(expandToString(grammar("A(5)", {"A(x) : x > 3 -> F(x)"}), 1) == "F(5)");
    CHECK(expandToString(grammar("A(5)", {"A(x) : x < 3 -> F(x)"}), 1) == "A(5)");
    CHECK(expandToString(grammar("B(1)A(5)", {"B(y) < A(x) : x > 3 -> F(x+y)"}), 1) == "B(1)F(6)");
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST_CASE("malformed grammars are rejected at compile time", "[lsystem][errors]") {
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A F(1)"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A -> F(z)"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A(x) -> F(x"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A(1) -> F(1)"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A(x,x) -> F(x)"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A -> F(nope(1))"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A -> F(max(1))"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A -> F(1) : 0"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A -> F(1) : -2"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A -> F(1) : often"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("A", {"A : -> F(1)"})), ParseError);
    CHECK_THROWS_AS(LSystem::compile(grammar("", {"A -> F(1)"})), ParseError);
}

TEST_CASE("parse errors name the offending rule", "[lsystem][errors]") {
    // A grammar is a list of near-identical strings; an error that does not say
    // which one it came from is close to useless.
    try {
        LSystem::compile(grammar("A", {"A -> F(1)", "A -> F(bogus)"}));
        FAIL("expected a ParseError");
    } catch (const ParseError& error) {
        const std::string message = error.what();
        CHECK_THAT(message, ContainsSubstring("bogus"));
        CHECK_THAT(message, ContainsSubstring("A -> F(bogus)"));
    }
}

TEST_CASE("runaway grammars hit the module budget instead of exhausting memory", "[lsystem][errors]") {
    LSystem system = LSystem::compile(grammar("A", {"A -> A A"}));
    system.setMaxModules(1000);

    CHECK_NOTHROW(system.expand(9));  // 512 modules
    CHECK_THROWS_AS(system.expand(20), ExpansionLimitExceeded);
}

TEST_CASE("dividing by a parameter that reached zero is reported, not silently infinite",
          "[lsystem][errors]") {
    const LSystem system = LSystem::compile(grammar("A(0)", {"A(x) -> F(1/x)"}));
    CHECK_THROWS_AS(system.expand(1), EvaluationError);
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

TEST_CASE("every preset compiles and grows a drawable word", "[lsystem][presets]") {
    for (const GrammarSource& source : presets::all()) {
        CAPTURE(source.name);

        const LSystem system = LSystem::compile(source);
        const Word word = system.expand(5, 1234);

        CHECK(word.size() > 10);
        // Something to draw, and a bracket structure the turtle can walk.
        CHECK(countSymbol(word, 'F') > 0);
        CHECK(bracketsBalanced(word));

        // Every F carries the (length, width) pair the turtle will read.
        for (const Module& module : word) {
            if (module.symbol == 'F') {
                REQUIRE(module.params.size() == 2);
                CHECK(module.params[0] > 0.0f);
                CHECK(module.params[1] > 0.0f);
            }
        }

        CHECK(toString(system.expand(4, 7)) == toString(system.expand(4, 7)));
    }
}

TEST_CASE("the signal preset carries exactly one signal, then consumes it",
          "[lsystem][presets][context]") {
    const LSystem system = LSystem::compile(presets::signalStem());

    // One signal climbs the stem...
    for (int iterations = 0; iterations <= 12; ++iterations) {
        CAPTURE(iterations);
        CHECK(countSymbol(system.expand(iterations), 'S') == 1);
    }

    // ...and once the apex has stopped (its 0.96 contraction runs about 27
    // steps) and the signal runs out of stem above it, the rule that advances
    // it stops firing and `S -> ` deletes it for good.
    CHECK(countSymbol(system.expand(40), 'S') == 0);
    CHECK(toString(system.expand(40)) == toString(system.expand(46)));
}

TEST_CASE("preset lookup by name", "[lsystem][presets]") {
    REQUIRE(presets::find("fern") != nullptr);
    CHECK(presets::find("fern")->name == "fern");
    CHECK(presets::find("no-such-plant") == nullptr);
}

TEST_CASE("only the bush preset responds to the seed", "[lsystem][presets]") {
    const LSystem tree = LSystem::compile(presets::simpleTree());
    CHECK(toString(tree.expand(6, 1)) == toString(tree.expand(6, 2)));

    const LSystem shrub = LSystem::compile(presets::bush());
    CHECK(toString(shrub.expand(6, 1)) != toString(shrub.expand(6, 2)));
}

TEST_CASE("the deterministic tree grammar reaches a fixed point", "[lsystem][presets]") {
    // Growth has to stop on its own: the l > minLength condition is the only
    // thing standing between a branching grammar and unbounded expansion. Once
    // settled, every bud has become a segment and further iterations are no-ops.
    //
    // 32 iterations is where the trunk's 0.9 contraction finally drops below
    // minLength; it moves if those constants are retuned.
    const LSystem tree = LSystem::compile(presets::simpleTree());
    const Word settled = tree.expand(32);

    CHECK(countSymbol(settled, 'A') == 0);
    CHECK(countSymbol(settled, 'B') == 0);
    CHECK(countSymbol(settled, 'C') == 0);
    CHECK(countSymbol(settled, 'F') > 1000);
    CHECK(toString(tree.expand(36)) == toString(settled));
}

TEST_CASE("preset growth stays within the default module budget", "[lsystem][presets]") {
    // The conditions in each grammar are what stop growth; if one of them is
    // wrong the word explodes exponentially instead of converging.
    for (const GrammarSource& source : presets::all()) {
        CAPTURE(source.name);
        const LSystem system = LSystem::compile(source);
        CHECK_NOTHROW(system.expand(9, 3));
    }
}
