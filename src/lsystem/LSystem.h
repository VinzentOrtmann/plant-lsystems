#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

/// Parametric, stochastic L-systems.
///
/// A word is a sequence of modules: a symbol plus zero or more numeric
/// parameters, written `F(0.5,0.1)` or just `F`. Productions rewrite one module
/// into a sequence of modules, computing the new parameters with arithmetic
/// expressions over the matched module's parameters:
///
///     A(l,w) : l > 0.12 -> F(l,w) [ +(30) A(l*0.7, w*0.7) ]
///     \_____/  \_______/    \_________________________________/
///     predecessor condition                successor
///
/// Several productions may share a predecessor; one is then drawn at random,
/// weighted by an optional trailing probability:
///
///     A(l) -> F(l) [ +(25) A(l*0.8) ] : 3
///     A(l) -> F(l) A(l*0.9)           : 1
///
/// Rewriting is parallel (every module in the word is replaced at once), which
/// is what makes L-systems model growth rather than sequential execution.
///
/// Expressions may call `random(lo, hi)`, which draws from the same generator
/// the production weights use, so a seed still reproduces exactly one plant.
/// It is rejected in the axiom: that is evaluated once at compile time, before
/// a seed exists, so randomness there would be frozen across every seed.
///
/// Productions may also require context, written `left < strict > right`:
///
///     A(x) < F(l) > B -> F(l*x)
///
/// Context is matched in the *tree* the brackets describe, not in the flat
/// string. Left context is the chain of ancestors, so sibling branches are
/// skipped over; right context is any path of descendants, so it may follow a
/// branch. This is what lets a signal travel up a stem and split at a fork.
namespace plant::lsystem {

using Symbol = char;

/// One symbol with its parameters. `F(0.5,0.1)` is symbol 'F', params {0.5,0.1}.
struct Module {
    Symbol symbol = '\0';
    std::vector<float> params;

    Module() = default;
    explicit Module(Symbol s) : symbol(s) {}
    Module(Symbol s, std::vector<float> p) : symbol(s), params(std::move(p)) {}
};

bool operator==(const Module& a, const Module& b);
bool operator!=(const Module& a, const Module& b);

using Word = std::vector<Module>;

/// Renders a word back into the source syntax, e.g. "F(0.5,0.1)[+(30)A]".
/// Round-trips through the grammar parser and is what the tests assert on.
std::string toString(const Word& word);

/// Arithmetic expression over a production's formal parameters. Defined in
/// LSystem.cpp; grammars only ever hold these by pointer.
class Expr;
using ExprPtr = std::shared_ptr<const Expr>;

/// A module on the right-hand side of a production: its arguments are
/// expressions, evaluated against the matched module's parameters.
struct SuccessorModule {
    Symbol symbol = '\0';
    std::vector<ExprPtr> args;
};

/// One module on the left of a production: a symbol plus names for its
/// parameters. The name count also fixes the arity, so `A(l,w)` never matches
/// a bare `A`.
struct ModulePattern {
    Symbol symbol = '\0';
    std::vector<std::string> formals;
};

struct Production {
    /// Ancestors the module must have, outermost first. Empty for a
    /// context-free production.
    std::vector<ModulePattern> leftContext;
    /// The module actually rewritten.
    ModulePattern predecessor;
    /// Descendants the module must have, nearest first. Empty for a
    /// context-free production.
    std::vector<ModulePattern> rightContext;

    /// Formal names from leftContext, predecessor and rightContext concatenated
    /// in that order. Expressions index into a parameter vector assembled the
    /// same way, so a context module's parameters are readable in the successor.
    std::vector<std::string> formals;

    /// Null means unconditional. Non-zero counts as true.
    ExprPtr condition;
    std::vector<SuccessorModule> successor;
    /// Relative probability among the productions matching the same module.
    /// Always > 0.
    float weight = 1.0f;

    [[nodiscard]] bool hasContext() const {
        return !leftContext.empty() || !rightContext.empty();
    }
};

/// Thrown by LSystem::compile for any malformed grammar.
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Thrown by expand()/step() when a grammar exceeds its module budget.
struct ExpansionLimitExceeded : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Thrown by expand()/step() when an expression cannot be evaluated, e.g. a
/// production that divides by a parameter that reached zero.
struct EvaluationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// A grammar in source form: exactly what a preset file contains, and what
/// LSystem::compile consumes.
struct GrammarSource {
    std::string name;
    std::string description;
    /// Starting word, e.g. "A(1,0.1)". May reference constants.
    std::string axiom;
    /// One production per entry, e.g. "A(l,w) : l > 0.1 -> F(l,w) A(l*0.8,w*0.8) : 2".
    std::vector<std::string> rules;
    /// Named values usable anywhere an expression is allowed. Substituted at
    /// compile time, so changing one requires recompiling the grammar.
    std::map<std::string, float> constants;
};

class LSystem {
public:
    /// Parses and type-checks a grammar. Throws ParseError with the offending
    /// rule quoted; every identifier must resolve to a formal or a constant.
    static LSystem compile(const GrammarSource& source);

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const Word& axiom() const { return axiom_; }
    [[nodiscard]] const std::vector<Production>& productions() const { return productions_; }

    /// Applies every production once, in parallel, across the whole word.
    /// `rng` is consumed only where a module has more than one matching
    /// production, so a grammar without alternatives never touches it.
    [[nodiscard]] Word step(const Word& word, std::mt19937& rng) const;

    /// Rewrites the axiom `iterations` times. Fully determined by
    /// (grammar, iterations, seed). Non-positive iteration counts return the
    /// axiom unchanged.
    [[nodiscard]] Word expand(int iterations, std::uint32_t seed = 0) const;

    /// Upper bound on the word length, guarding against grammars that blow up
    /// exponentially. Exceeding it throws ExpansionLimitExceeded.
    void setMaxModules(std::size_t limit) { maxModules_ = limit; }
    [[nodiscard]] std::size_t maxModules() const { return maxModules_; }

private:
    std::string name_;
    Word axiom_;
    std::vector<Production> productions_;
    std::size_t maxModules_ = 1u << 21;  // ~2M modules
};

}  // namespace plant::lsystem
