#include "lsystem/LSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace plant::lsystem {

// ---------------------------------------------------------------------------
// Expression tree
// ---------------------------------------------------------------------------

/// Everything an expression can read while it is being evaluated.
struct EvalContext {
    /// Parameters of the module being rewritten; formals index into this.
    const std::vector<float>& params;
    /// Null while compiling the axiom, where there is no seed yet. random() is
    /// rejected at parse time in that case, so no node ever dereferences it.
    std::mt19937* rng = nullptr;
};

/// Evaluated against the parameters of the module being rewritten. Identifiers
/// are resolved during parsing, so evaluation is a plain tree walk with no
/// name lookup.
class Expr {
public:
    virtual ~Expr() = default;
    virtual float eval(const EvalContext& context) const = 0;
};

namespace {

std::string quoted(std::string_view text) {
    return "'" + std::string(text) + "'";
}

/// Uniform in [0,1). Hand-rolled rather than via std::uniform_real_distribution,
/// whose output is not specified across standard library implementations: a
/// seed has to reproduce the same plant everywhere.
float canonicalRandom(std::mt19937& rng) {
    return static_cast<float>(static_cast<double>(rng()) * (1.0 / 4294967296.0));
}

class Literal final : public Expr {
public:
    explicit Literal(float value) : value_(value) {}
    float eval(const EvalContext&) const override { return value_; }

private:
    float value_;
};

/// Reference to a formal parameter, resolved to an index at parse time.
class ParamRef final : public Expr {
public:
    explicit ParamRef(std::size_t index) : index_(index) {}
    float eval(const EvalContext& context) const override { return context.params[index_]; }

private:
    std::size_t index_;
};

class Negate final : public Expr {
public:
    explicit Negate(ExprPtr operand) : operand_(std::move(operand)) {}
    float eval(const EvalContext& context) const override { return -operand_->eval(context); }

private:
    ExprPtr operand_;
};

class LogicalNot final : public Expr {
public:
    explicit LogicalNot(ExprPtr operand) : operand_(std::move(operand)) {}
    float eval(const EvalContext& context) const override {
        return operand_->eval(context) == 0.0f ? 1.0f : 0.0f;
    }

private:
    ExprPtr operand_;
};

enum class BinOp { Add, Sub, Mul, Div, Less, LessEq, Greater, GreaterEq, Equal, NotEqual, And, Or };

class Binary final : public Expr {
public:
    Binary(BinOp op, ExprPtr lhs, ExprPtr rhs)
        : op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

    float eval(const EvalContext& context) const override {
        // Short-circuit so `w > 0 && l/w > 2` cannot divide by zero.
        if (op_ == BinOp::And) {
            return (lhs_->eval(context) != 0.0f && rhs_->eval(context) != 0.0f) ? 1.0f : 0.0f;
        }
        if (op_ == BinOp::Or) {
            return (lhs_->eval(context) != 0.0f || rhs_->eval(context) != 0.0f) ? 1.0f : 0.0f;
        }

        const float a = lhs_->eval(context);
        const float b = rhs_->eval(context);
        switch (op_) {
            case BinOp::Add: return a + b;
            case BinOp::Sub: return a - b;
            case BinOp::Mul: return a * b;
            case BinOp::Div:
                if (b == 0.0f) {
                    // Letting this produce inf would quietly poison the geometry
                    // several stages downstream; fail where the bug actually is.
                    throw EvaluationError("division by zero while evaluating a production");
                }
                return a / b;
            case BinOp::Less:      return a < b ? 1.0f : 0.0f;
            case BinOp::LessEq:    return a <= b ? 1.0f : 0.0f;
            case BinOp::Greater:   return a > b ? 1.0f : 0.0f;
            case BinOp::GreaterEq: return a >= b ? 1.0f : 0.0f;
            case BinOp::Equal:     return a == b ? 1.0f : 0.0f;
            case BinOp::NotEqual:  return a != b ? 1.0f : 0.0f;
            case BinOp::And:
            case BinOp::Or:        break;  // handled above
        }
        return 0.0f;
    }

private:
    BinOp op_;
    ExprPtr lhs_;
    ExprPtr rhs_;
};

enum class Fn { Sin, Cos, Tan, Sqrt, Abs, Floor, Ceil, Min, Max, Pow, Clamp, Random };

struct FnInfo {
    const char* name;
    Fn fn;
    std::size_t arity;
};

// Trigonometric functions take degrees, matching the angle symbols (+, &, /).
constexpr FnInfo kFunctions[] = {
    {"sin", Fn::Sin, 1},   {"cos", Fn::Cos, 1},     {"tan", Fn::Tan, 1},
    {"sqrt", Fn::Sqrt, 1}, {"abs", Fn::Abs, 1},     {"floor", Fn::Floor, 1},
    {"ceil", Fn::Ceil, 1}, {"min", Fn::Min, 2},     {"max", Fn::Max, 2},
    {"pow", Fn::Pow, 2},   {"clamp", Fn::Clamp, 3}, {"random", Fn::Random, 2},
};

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

class Call final : public Expr {
public:
    Call(Fn fn, std::vector<ExprPtr> args) : fn_(fn), args_(std::move(args)) {}

    float eval(const EvalContext& context) const override {
        const float a = args_[0]->eval(context);
        switch (fn_) {
            case Fn::Sin:   return std::sin(a * kDegToRad);
            case Fn::Cos:   return std::cos(a * kDegToRad);
            case Fn::Tan:   return std::tan(a * kDegToRad);
            case Fn::Sqrt:  return std::sqrt(a);
            case Fn::Abs:   return std::fabs(a);
            case Fn::Floor: return std::floor(a);
            case Fn::Ceil:  return std::ceil(a);
            case Fn::Min:   return std::min(a, args_[1]->eval(context));
            case Fn::Max:   return std::max(a, args_[1]->eval(context));
            case Fn::Pow:   return std::pow(a, args_[1]->eval(context));
            case Fn::Clamp: return std::clamp(a, args_[1]->eval(context), args_[2]->eval(context));
            case Fn::Random: {
                if (context.rng == nullptr) {
                    // Unreachable: the parser refuses random() wherever no
                    // generator will be available.
                    throw EvaluationError("random() used where no generator is available");
                }
                const float high = args_[1]->eval(context);
                return a + canonicalRandom(*context.rng) * (high - a);
            }
        }
        return 0.0f;
    }

private:
    Fn fn_;
    std::vector<ExprPtr> args_;
};

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class Tok { End, Number, Ident, Op, LParen, RParen, Comma };

struct Token {
    Tok kind = Tok::End;
    std::string text;
    float number = 0.0f;
};

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) { advance(); }

    const Token& peek() const { return current_; }

    Token take() {
        Token taken = current_;
        advance();
        return taken;
    }

    bool takeIfOp(std::string_view op) {
        if (current_.kind == Tok::Op && current_.text == op) {
            advance();
            return true;
        }
        return false;
    }

private:
    void advance();

    std::string_view source_;
    std::size_t pos_ = 0;
    Token current_;
};

void Lexer::advance() {
    while (pos_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[pos_])) != 0) {
        ++pos_;
    }
    current_ = Token{};
    if (pos_ >= source_.size()) {
        return;
    }

    const char c = source_[pos_];

    const bool startsNumber =
        std::isdigit(static_cast<unsigned char>(c)) != 0 ||
        (c == '.' && pos_ + 1 < source_.size() &&
         std::isdigit(static_cast<unsigned char>(source_[pos_ + 1])) != 0);
    if (startsNumber) {
        const std::size_t start = pos_;
        while (pos_ < source_.size() &&
               (std::isdigit(static_cast<unsigned char>(source_[pos_])) != 0 || source_[pos_] == '.')) {
            ++pos_;
        }
        if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
            const std::size_t exponent = pos_;
            ++pos_;
            if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_])) != 0) {
                while (pos_ < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_])) != 0) {
                    ++pos_;
                }
            } else {
                pos_ = exponent;  // trailing 'e' was not an exponent after all
            }
        }
        current_.kind = Tok::Number;
        current_.text = std::string(source_.substr(start, pos_ - start));
        current_.number = std::strtof(current_.text.c_str(), nullptr);
        return;
    }

    if (isIdentStart(c)) {
        const std::size_t start = pos_;
        while (pos_ < source_.size() && isIdentChar(source_[pos_])) {
            ++pos_;
        }
        current_.kind = Tok::Ident;
        current_.text = std::string(source_.substr(start, pos_ - start));
        return;
    }

    switch (c) {
        case '(': ++pos_; current_.kind = Tok::LParen; current_.text = "("; return;
        case ')': ++pos_; current_.kind = Tok::RParen; current_.text = ")"; return;
        case ',': ++pos_; current_.kind = Tok::Comma;  current_.text = ","; return;
        default: break;
    }

    if (pos_ + 1 < source_.size()) {
        const std::string_view two = source_.substr(pos_, 2);
        for (const std::string_view op : {"<=", ">=", "==", "!=", "&&", "||"}) {
            if (two == op) {
                pos_ += 2;
                current_.kind = Tok::Op;
                current_.text = std::string(two);
                return;
            }
        }
    }

    if (std::string_view("+-*/<>!").find(c) != std::string_view::npos) {
        ++pos_;
        current_.kind = Tok::Op;
        current_.text = std::string(1, c);
        return;
    }

    throw ParseError("unexpected character " + quoted(std::string(1, c)) + " in expression");
}

// ---------------------------------------------------------------------------
// Expression parser
// ---------------------------------------------------------------------------

/// Recursive descent over the usual precedence ladder. Identifiers are bound
/// eagerly: a formal parameter becomes an index, a constant becomes a literal,
/// and anything else is an error at compile time rather than at growth time.
class ExprParser {
public:
    ExprParser(std::string_view source,
               const std::vector<std::string>& formals,
               const std::map<std::string, float>& constants,
               bool allowRandom)
        : lexer_(source), formals_(formals), constants_(constants), allowRandom_(allowRandom) {}

    ExprPtr parse() {
        ExprPtr expr = parseOr();
        if (lexer_.peek().kind != Tok::End) {
            throw ParseError("trailing " + quoted(lexer_.peek().text) + " in expression");
        }
        return expr;
    }

private:
    ExprPtr parseOr() {
        ExprPtr lhs = parseAnd();
        while (lexer_.takeIfOp("||")) {
            lhs = std::make_shared<Binary>(BinOp::Or, std::move(lhs), parseAnd());
        }
        return lhs;
    }

    ExprPtr parseAnd() {
        ExprPtr lhs = parseComparison();
        while (lexer_.takeIfOp("&&")) {
            lhs = std::make_shared<Binary>(BinOp::And, std::move(lhs), parseComparison());
        }
        return lhs;
    }

    ExprPtr parseComparison() {
        ExprPtr lhs = parseAdditive();
        for (;;) {
            if (lexer_.takeIfOp("<"))       lhs = std::make_shared<Binary>(BinOp::Less, std::move(lhs), parseAdditive());
            else if (lexer_.takeIfOp("<=")) lhs = std::make_shared<Binary>(BinOp::LessEq, std::move(lhs), parseAdditive());
            else if (lexer_.takeIfOp(">"))  lhs = std::make_shared<Binary>(BinOp::Greater, std::move(lhs), parseAdditive());
            else if (lexer_.takeIfOp(">=")) lhs = std::make_shared<Binary>(BinOp::GreaterEq, std::move(lhs), parseAdditive());
            else if (lexer_.takeIfOp("==")) lhs = std::make_shared<Binary>(BinOp::Equal, std::move(lhs), parseAdditive());
            else if (lexer_.takeIfOp("!=")) lhs = std::make_shared<Binary>(BinOp::NotEqual, std::move(lhs), parseAdditive());
            else return lhs;
        }
    }

    ExprPtr parseAdditive() {
        ExprPtr lhs = parseMultiplicative();
        for (;;) {
            if (lexer_.takeIfOp("+"))      lhs = std::make_shared<Binary>(BinOp::Add, std::move(lhs), parseMultiplicative());
            else if (lexer_.takeIfOp("-")) lhs = std::make_shared<Binary>(BinOp::Sub, std::move(lhs), parseMultiplicative());
            else return lhs;
        }
    }

    ExprPtr parseMultiplicative() {
        ExprPtr lhs = parseUnary();
        for (;;) {
            if (lexer_.takeIfOp("*"))      lhs = std::make_shared<Binary>(BinOp::Mul, std::move(lhs), parseUnary());
            else if (lexer_.takeIfOp("/")) lhs = std::make_shared<Binary>(BinOp::Div, std::move(lhs), parseUnary());
            else return lhs;
        }
    }

    ExprPtr parseUnary() {
        if (lexer_.takeIfOp("-")) {
            return std::make_shared<Negate>(parseUnary());
        }
        if (lexer_.takeIfOp("+")) {
            return parseUnary();
        }
        if (lexer_.takeIfOp("!")) {
            return std::make_shared<LogicalNot>(parseUnary());
        }
        return parsePrimary();
    }

    ExprPtr parsePrimary();

    ExprPtr resolveIdentifier(const std::string& name);

    Lexer lexer_;
    const std::vector<std::string>& formals_;
    const std::map<std::string, float>& constants_;
    bool allowRandom_ = true;
};

ExprPtr ExprParser::resolveIdentifier(const std::string& name) {
    const auto formal = std::find(formals_.begin(), formals_.end(), name);
    if (formal != formals_.end()) {
        return std::make_shared<ParamRef>(
            static_cast<std::size_t>(std::distance(formals_.begin(), formal)));
    }
    const auto constant = constants_.find(name);
    if (constant != constants_.end()) {
        return std::make_shared<Literal>(constant->second);
    }
    throw ParseError("unknown identifier " + quoted(name) +
                     " (not a formal parameter of this production, nor a constant)");
}

ExprPtr ExprParser::parsePrimary() {
    const Token token = lexer_.take();
    switch (token.kind) {
        case Tok::Number:
            return std::make_shared<Literal>(token.number);

        case Tok::LParen: {
            ExprPtr inner = parseOr();
            if (lexer_.peek().kind != Tok::RParen) {
                throw ParseError("expected ')' in expression");
            }
            lexer_.take();
            return inner;
        }

        case Tok::Ident: {
            if (lexer_.peek().kind != Tok::LParen) {
                return resolveIdentifier(token.text);
            }
            const auto known = std::find_if(std::begin(kFunctions), std::end(kFunctions),
                                            [&](const FnInfo& f) { return token.text == f.name; });
            if (known == std::end(kFunctions)) {
                throw ParseError("unknown function " + quoted(token.text));
            }
            if (known->fn == Fn::Random && !allowRandom_) {
                // The axiom is evaluated once at compile time, before a seed
                // exists; randomness there would be frozen across every seed.
                throw ParseError("random() cannot be used in the axiom, only in productions");
            }
            lexer_.take();  // '('
            std::vector<ExprPtr> args;
            if (lexer_.peek().kind != Tok::RParen) {
                args.push_back(parseOr());
                while (lexer_.peek().kind == Tok::Comma) {
                    lexer_.take();
                    args.push_back(parseOr());
                }
            }
            if (lexer_.peek().kind != Tok::RParen) {
                throw ParseError("expected ')' closing call to " + quoted(token.text));
            }
            lexer_.take();
            if (args.size() != known->arity) {
                throw ParseError(quoted(token.text) + " takes " + std::to_string(known->arity) +
                                 " argument(s), got " + std::to_string(args.size()));
            }
            return std::make_shared<Call>(known->fn, std::move(args));
        }

        case Tok::End:
            throw ParseError("expression ended unexpectedly");

        default:
            throw ParseError("unexpected " + quoted(token.text) + " in expression");
    }
}

// ---------------------------------------------------------------------------
// Grammar parsing
// ---------------------------------------------------------------------------

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

/// Given text positioned just after '(', returns the contents up to the
/// matching ')' and moves `pos` past it.
std::string_view takeParenGroup(std::string_view text, std::size_t& pos) {
    const std::size_t start = pos;
    int depth = 1;
    while (pos < text.size() && depth > 0) {
        if (text[pos] == '(') {
            ++depth;
        } else if (text[pos] == ')') {
            --depth;
            if (depth == 0) {
                break;
            }
        }
        ++pos;
    }
    if (depth != 0) {
        throw ParseError("unbalanced '(' in " + quoted(text));
    }
    const std::string_view group = text.substr(start, pos - start);
    ++pos;  // consume ')'
    return group;
}

/// Splits an argument list on commas that are not nested inside parentheses.
std::vector<std::string_view> splitArguments(std::string_view text) {
    std::vector<std::string_view> parts;
    if (trim(text).empty()) {
        return parts;
    }
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            --depth;
        } else if (text[i] == ',' && depth == 0) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

bool isSymbolChar(char c) {
    return std::isspace(static_cast<unsigned char>(c)) == 0 && c != '(' && c != ')' && c != ',';
}

/// Parses "F(l,w)[+(30)A(l*r)]" into successor modules. Any non-space character
/// other than parentheses and commas is a legal symbol, which is what lets the
/// turtle alphabet (F + - & ^ \ / [ ]) share the syntax with named modules.
std::vector<SuccessorModule> parseModuleList(std::string_view text,
                                             const std::vector<std::string>& formals,
                                             const std::map<std::string, float>& constants,
                                             bool allowRandom) {
    std::vector<SuccessorModule> modules;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
            ++pos;
            continue;
        }
        if (!isSymbolChar(text[pos])) {
            throw ParseError("unexpected " + quoted(std::string(1, text[pos])) + " in " + quoted(text));
        }

        SuccessorModule module;
        module.symbol = text[pos];
        ++pos;

        std::size_t lookahead = pos;
        while (lookahead < text.size() && std::isspace(static_cast<unsigned char>(text[lookahead])) != 0) {
            ++lookahead;
        }
        if (lookahead < text.size() && text[lookahead] == '(') {
            pos = lookahead + 1;
            const std::string_view group = takeParenGroup(text, pos);
            for (const std::string_view argument : splitArguments(group)) {
                if (trim(argument).empty()) {
                    throw ParseError("empty argument in " + quoted(text));
                }
                module.args.push_back(
                    ExprParser(trim(argument), formals, constants, allowRandom).parse());
            }
        }
        modules.push_back(std::move(module));
    }
    return modules;
}

/// Parses a run of patterns like "A(x) B" into module patterns. Used for both
/// sides of the context and for the strict predecessor.
std::vector<ModulePattern> parsePatternList(std::string_view text) {
    std::vector<ModulePattern> patterns;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
            ++pos;
            continue;
        }
        if (!isSymbolChar(text[pos])) {
            throw ParseError(quoted(std::string(1, text[pos])) + " is not a valid module symbol");
        }
        if (text[pos] == '[' || text[pos] == ']') {
            // Brackets describe the tree the context is matched against; they
            // are structure, not modules, so they cannot appear in a pattern.
            throw ParseError("brackets cannot appear on the left of a production: " + quoted(text));
        }

        ModulePattern pattern;
        pattern.symbol = text[pos];
        ++pos;

        std::size_t lookahead = pos;
        while (lookahead < text.size() &&
               std::isspace(static_cast<unsigned char>(text[lookahead])) != 0) {
            ++lookahead;
        }
        if (lookahead < text.size() && text[lookahead] == '(') {
            pos = lookahead + 1;
            const std::string_view group = takeParenGroup(text, pos);
            for (const std::string_view raw : splitArguments(group)) {
                const std::string_view formal = trim(raw);
                if (formal.empty() || !isIdentStart(formal.front()) ||
                    !std::all_of(formal.begin(), formal.end(), isIdentChar)) {
                    throw ParseError("invalid formal parameter " + quoted(formal) + " in " +
                                     quoted(text));
                }
                pattern.formals.emplace_back(formal);
            }
        }
        patterns.push_back(std::move(pattern));
    }
    return patterns;
}

float parseWeight(std::string_view text) {
    const std::string trimmed(trim(text));
    if (trimmed.empty()) {
        throw ParseError("production weight is empty");
    }
    char* end = nullptr;
    const float weight = std::strtof(trimmed.c_str(), &end);
    if (end != trimmed.c_str() + trimmed.size()) {
        throw ParseError("production weight " + quoted(trimmed) + " is not a number");
    }
    if (!(weight > 0.0f)) {
        throw ParseError("production weight must be positive, got " + trimmed);
    }
    return weight;
}

Production parseProduction(std::string_view rule, const std::map<std::string, float>& constants) {
    const std::size_t arrow = rule.find("->");
    if (arrow == std::string_view::npos) {
        throw ParseError("production is missing '->': " + quoted(rule));
    }

    std::string_view lhs = rule.substr(0, arrow);
    std::string_view rhs = rule.substr(arrow + 2);

    std::string_view conditionText;
    if (const std::size_t colon = lhs.find(':'); colon != std::string_view::npos) {
        conditionText = trim(lhs.substr(colon + 1));
        lhs = lhs.substr(0, colon);
        if (conditionText.empty()) {
            throw ParseError("production has an empty condition: " + quoted(rule));
        }
    }

    std::string_view weightText;
    if (const std::size_t colon = rhs.find(':'); colon != std::string_view::npos) {
        weightText = rhs.substr(colon + 1);
        rhs = rhs.substr(0, colon);
    }

    // Context markers are searched only after the condition has been split off,
    // so the '<' and '>' of a comparison cannot be mistaken for them.
    std::string_view strictText = lhs;
    std::string_view leftText;
    std::string_view rightText;
    if (const std::size_t marker = strictText.find('<'); marker != std::string_view::npos) {
        leftText = strictText.substr(0, marker);
        strictText = strictText.substr(marker + 1);
    }
    if (const std::size_t marker = strictText.find('>'); marker != std::string_view::npos) {
        rightText = strictText.substr(marker + 1);
        strictText = strictText.substr(0, marker);
    }

    Production production;
    production.leftContext = parsePatternList(trim(leftText));
    production.rightContext = parsePatternList(trim(rightText));

    const std::vector<ModulePattern> strict = parsePatternList(trim(strictText));
    if (strict.size() != 1) {
        throw ParseError("a production rewrites exactly one module, got " +
                         std::to_string(strict.size()) + ": " + quoted(rule));
    }
    production.predecessor = strict.front();

    // One flat parameter vector spans all three parts, so a successor can read
    // a context module's parameters as easily as its own.
    const auto appendFormals = [&](const ModulePattern& pattern) {
        for (const std::string& name : pattern.formals) {
            if (std::find(production.formals.begin(), production.formals.end(), name) !=
                production.formals.end()) {
                throw ParseError("duplicate formal parameter " + quoted(name) + " in " +
                                 quoted(rule));
            }
            production.formals.push_back(name);
        }
    };
    for (const ModulePattern& pattern : production.leftContext) {
        appendFormals(pattern);
    }
    appendFormals(production.predecessor);
    for (const ModulePattern& pattern : production.rightContext) {
        appendFormals(pattern);
    }

    if (!conditionText.empty()) {
        production.condition =
            ExprParser(conditionText, production.formals, constants, /*allowRandom=*/true).parse();
    }
    production.successor =
        parseModuleList(trim(rhs), production.formals, constants, /*allowRandom=*/true);
    if (!weightText.empty()) {
        production.weight = parseWeight(weightText);
    }
    return production;
}

// ---------------------------------------------------------------------------
// Context matching
// ---------------------------------------------------------------------------

/// Index of the bracket partnering each '[' and ']', so the walkers can hop
/// over a whole subtree in constant time. Built once per rewriting step.
std::vector<std::size_t> buildBracketTable(const Word& word) {
    std::vector<std::size_t> partner(word.size(), 0);
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (word[i].symbol == '[') {
            open.push_back(i);
        } else if (word[i].symbol == ']' && !open.empty()) {
            partner[open.back()] = i;
            partner[i] = open.back();
            open.pop_back();
        }
    }
    return partner;
}

bool matchesPattern(const Module& module, const ModulePattern& pattern) {
    return module.symbol == pattern.symbol && module.params.size() == pattern.formals.size();
}

/// Walks from `index` towards the root, matching `patterns` against successive
/// ancestors. The patterns read outermost-first, so they are consumed in
/// reverse: the last one must be the immediate parent.
///
/// Scanning backwards, a ']' marks a sibling subtree that was opened and closed
/// before we got here, so it is hopped over entirely; a '[' means we are inside
/// a branch and stepping past it ascends to the parent.
bool matchLeftContext(const Word& word, std::size_t index,
                      const std::vector<ModulePattern>& patterns,
                      const std::vector<std::size_t>& partner, std::vector<float>& bindings) {
    if (patterns.empty()) {
        return true;
    }
    std::size_t remaining = patterns.size();
    std::size_t cursor = index;
    std::vector<float> found;

    while (cursor > 0 && remaining > 0) {
        --cursor;
        const Module& candidate = word[cursor];
        if (candidate.symbol == ']') {
            cursor = partner[cursor];  // hop the sibling subtree; loop decrements past its '['
            continue;
        }
        if (candidate.symbol == '[') {
            continue;  // ascending out of a branch
        }
        --remaining;
        if (!matchesPattern(candidate, patterns[remaining])) {
            return false;
        }
        // Collected nearest-first, so reversed relative to the formal order.
        found.insert(found.begin(), candidate.params.begin(), candidate.params.end());
    }

    if (remaining > 0) {
        return false;  // ran out of ancestors
    }
    bindings.insert(bindings.end(), found.begin(), found.end());
    return true;
}

/// Matches `patterns` against some path of descendants starting at `cursor`.
///
/// A node's children are the first module of each bracketed branch plus the
/// module that continues the axis after them, so this tries each branch in turn
/// and backtracks. That is what lets a right context follow a fork.
bool matchRightContext(const Word& word, std::size_t cursor,
                       const std::vector<ModulePattern>& patterns, std::size_t depth,
                       const std::vector<std::size_t>& partner, std::vector<float>& bindings) {
    if (depth == patterns.size()) {
        return true;
    }
    while (cursor < word.size()) {
        const Module& candidate = word[cursor];
        if (candidate.symbol == '[') {
            const std::size_t restore = bindings.size();
            if (matchRightContext(word, cursor + 1, patterns, depth, partner, bindings)) {
                return true;
            }
            bindings.resize(restore);
            cursor = partner[cursor] + 1;  // that branch failed; try the next child
            continue;
        }
        if (candidate.symbol == ']') {
            return false;  // end of this branch: no children left
        }
        if (!matchesPattern(candidate, patterns[depth])) {
            return false;
        }
        const std::size_t restore = bindings.size();
        bindings.insert(bindings.end(), candidate.params.begin(), candidate.params.end());
        if (matchRightContext(word, cursor + 1, patterns, depth + 1, partner, bindings)) {
            return true;
        }
        bindings.resize(restore);
        return false;
    }
    return false;
}

std::string formatNumber(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
    return buffer;
}

}  // namespace

// ---------------------------------------------------------------------------
// Word helpers
// ---------------------------------------------------------------------------

bool operator==(const Module& a, const Module& b) {
    return a.symbol == b.symbol && a.params == b.params;
}

bool operator!=(const Module& a, const Module& b) {
    return !(a == b);
}

std::string toString(const Word& word) {
    std::string out;
    for (const Module& module : word) {
        out.push_back(module.symbol);
        if (module.params.empty()) {
            continue;
        }
        out.push_back('(');
        for (std::size_t i = 0; i < module.params.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            out += formatNumber(module.params[i]);
        }
        out.push_back(')');
    }
    return out;
}

// ---------------------------------------------------------------------------
// LSystem
// ---------------------------------------------------------------------------

LSystem LSystem::compile(const GrammarSource& source) {
    LSystem system;
    system.name_ = source.name;

    const std::vector<std::string> noFormals;
    const std::vector<SuccessorModule> axiomModules =
        parseModuleList(trim(source.axiom), noFormals, source.constants, /*allowRandom=*/false);
    if (axiomModules.empty()) {
        throw ParseError("grammar " + quoted(source.name) + " has an empty axiom");
    }
    const std::vector<float> noParams;
    const EvalContext axiomContext{noParams, nullptr};
    for (const SuccessorModule& module : axiomModules) {
        Module instance(module.symbol);
        instance.params.reserve(module.args.size());
        for (const ExprPtr& argument : module.args) {
            instance.params.push_back(argument->eval(axiomContext));
        }
        system.axiom_.push_back(std::move(instance));
    }

    system.productions_.reserve(source.rules.size());
    for (const std::string& rule : source.rules) {
        if (trim(rule).empty()) {
            continue;
        }
        try {
            system.productions_.push_back(parseProduction(rule, source.constants));
        } catch (const ParseError& error) {
            throw ParseError(std::string(error.what()) + "\n  in rule: " + rule);
        }
    }
    return system;
}

Word LSystem::step(const Word& word, std::mt19937& rng) const {
    Word out;
    out.reserve(word.size() + word.size() / 2);

    // Only paid for by grammars that actually use context.
    const bool anyContext = std::any_of(productions_.begin(), productions_.end(),
                                        [](const Production& p) { return p.hasContext(); });
    const std::vector<std::size_t> partner =
        anyContext ? buildBracketTable(word) : std::vector<std::size_t>{};

    // Parallel rewriting: context is read from the word as it stands at the
    // start of the step, never from the output being built.
    std::vector<const Production*> candidates;
    std::vector<std::vector<float>> candidateBindings;
    std::vector<float> bindings;

    for (std::size_t index = 0; index < word.size(); ++index) {
        const Module& module = word[index];
        candidates.clear();
        candidateBindings.clear();

        for (const Production& production : productions_) {
            if (production.predecessor.symbol != module.symbol) {
                continue;
            }
            // Arity is part of the match: A(l) never matches bare A.
            if (production.predecessor.formals.size() != module.params.size()) {
                continue;
            }

            bindings.clear();
            if (!matchLeftContext(word, index, production.leftContext, partner, bindings)) {
                continue;
            }
            bindings.insert(bindings.end(), module.params.begin(), module.params.end());
            if (!matchRightContext(word, index + 1, production.rightContext, 0, partner,
                                   bindings)) {
                continue;
            }

            const EvalContext context{bindings, &rng};
            if (production.condition && production.condition->eval(context) == 0.0f) {
                continue;
            }
            candidates.push_back(&production);
            candidateBindings.push_back(bindings);
        }

        if (candidates.empty()) {
            out.push_back(module);  // identity production
            continue;
        }

        std::size_t chosenIndex = 0;
        if (candidates.size() > 1) {
            float total = 0.0f;
            for (const Production* candidate : candidates) {
                total += candidate->weight;
            }
            float pick = canonicalRandom(rng) * total;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                pick -= candidates[i]->weight;
                if (pick < 0.0f) {
                    chosenIndex = i;
                    break;
                }
            }
        }

        const Production* chosen = candidates[chosenIndex];
        const EvalContext context{candidateBindings[chosenIndex], &rng};
        for (const SuccessorModule& successor : chosen->successor) {
            Module produced(successor.symbol);
            produced.params.reserve(successor.args.size());
            for (const ExprPtr& argument : successor.args) {
                produced.params.push_back(argument->eval(context));
            }
            out.push_back(std::move(produced));
        }

        if (out.size() > maxModules_) {
            throw ExpansionLimitExceeded("grammar " + quoted(name_) + " exceeded its budget of " +
                                         std::to_string(maxModules_) + " modules");
        }
    }
    return out;
}

Word LSystem::expand(int iterations, std::uint32_t seed) const {
    Word word = axiom_;
    std::mt19937 rng(seed);
    for (int i = 0; i < iterations; ++i) {
        word = step(word, rng);
    }
    return word;
}

}  // namespace plant::lsystem
