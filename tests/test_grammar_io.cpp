#include "lsystem/GrammarIO.h"
#include "lsystem/LSystem.h"
#include "lsystem/Presets.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace plant;
using Catch::Matchers::ContainsSubstring;

namespace {

/// Fresh directory per test, removed on the way out.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("plant-grammar-io-" + name)) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

    void write(const std::string& name, const std::string& contents) const {
        std::ofstream out(path_ / name, std::ios::binary | std::ios::trunc);
        out << contents;
    }

private:
    std::filesystem::path path_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

TEST_CASE("every built-in grammar survives a JSON round trip", "[grammar-io]") {
    for (const lsystem::GrammarSource& original : lsystem::presets::all()) {
        CAPTURE(original.name);
        const lsystem::GrammarSource restored = lsystem::fromJson(lsystem::toJson(original));

        CHECK(restored.name == original.name);
        CHECK(restored.description == original.description);
        CHECK(restored.axiom == original.axiom);
        CHECK(restored.rules == original.rules);
        REQUIRE(restored.constants.size() == original.constants.size());
        for (const auto& [name, value] : original.constants) {
            // Exactly equal, not approximately: a constant that drifted would
            // grow a subtly different plant from the same file.
            CHECK(restored.constants.at(name) == value);
        }
    }
}

TEST_CASE("a round-tripped grammar grows the identical plant", "[grammar-io]") {
    // The round trip that actually matters is not field equality but that the
    // compiled grammar behaves the same.
    for (const lsystem::GrammarSource& original : lsystem::presets::all()) {
        CAPTURE(original.name);
        const lsystem::GrammarSource restored = lsystem::fromJson(lsystem::toJson(original));

        const std::string before = toString(lsystem::LSystem::compile(original).expand(8, 99));
        const std::string after = toString(lsystem::LSystem::compile(restored).expand(8, 99));
        CHECK(before == after);
    }
}

TEST_CASE("constants are written as the shortest decimal that reads back exactly",
          "[grammar-io]") {
    // Straight to JSON a float widens to double and 0.16f becomes
    // 0.1599999964237213, which is correct and unreadable in a file meant to be
    // hand-edited.
    lsystem::GrammarSource source;
    source.name = "numbers";
    source.axiom = "A";
    source.rules = {"A -> F(1)"};
    source.constants = {{"sixteenth", 0.16f}, {"golden", 137.5f}, {"third", 1.0f / 3.0f}};

    const std::string text = lsystem::toJson(source);
    CHECK_THAT(text, ContainsSubstring("0.16"));
    CHECK_THAT(text, ContainsSubstring("137.5"));
    CHECK_THAT(text, !ContainsSubstring("0.1599999"));

    const lsystem::GrammarSource restored = lsystem::fromJson(text);
    CHECK(restored.constants.at("sixteenth") == 0.16f);
    CHECK(restored.constants.at("golden") == 137.5f);
    CHECK(restored.constants.at("third") == 1.0f / 3.0f);
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST_CASE("malformed grammar JSON is rejected with a reason", "[grammar-io][errors]") {
    CHECK_THROWS_AS(lsystem::fromJson("{ not json"), lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::fromJson("[1,2,3]"), lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::fromJson(R"({"axiom":"A","rules":[]})"), lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::fromJson(R"({"name":"x","rules":[]})"), lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::fromJson(R"({"name":"","axiom":"A","rules":[]})"),
                    lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::fromJson(R"({"name":"x","axiom":"A"})"), lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::fromJson(R"({"name":"x","axiom":"A","rules":"nope"})"),
                    lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::fromJson(R"({"name":"x","axiom":"A","rules":[7]})"),
                    lsystem::GrammarIOError);
    CHECK_THROWS_AS(
        lsystem::fromJson(R"({"name":"x","axiom":"A","rules":[],"constants":{"a":"big"}})"),
        lsystem::GrammarIOError);
}

TEST_CASE("valid JSON describing a broken L-system parses, then fails to compile",
          "[grammar-io][errors]") {
    // The two failure modes are separate on purpose: a file can be perfectly
    // well-formed JSON and still be nonsense as a grammar, and the reader
    // should not be the thing that says so.
    // Custom delimiter: the rule contains `)"`, which would close a plain R"( ).
    const lsystem::GrammarSource source =
        lsystem::fromJson(R"json({"name":"broken","axiom":"A","rules":["A -> F(nope)"]})json");

    CHECK(source.name == "broken");
    CHECK_THROWS_AS(lsystem::LSystem::compile(source), lsystem::ParseError);
}

// ---------------------------------------------------------------------------
// Files and directories
// ---------------------------------------------------------------------------

TEST_CASE("a grammar survives a trip through a file", "[grammar-io][io]") {
    const ScratchDirectory scratch("file");
    const lsystem::GrammarSource original = lsystem::presets::signalStem();

    lsystem::saveGrammar(scratch.file("signal.json"), original);
    REQUIRE(std::filesystem::exists(scratch.file("signal.json")));

    const lsystem::GrammarSource restored = lsystem::loadGrammar(scratch.file("signal.json"));
    CHECK(restored.rules == original.rules);
}

TEST_CASE("loading a missing file reports the path", "[grammar-io][io][errors]") {
    CHECK_THROWS_AS(lsystem::loadGrammar("no-such-directory/no-such-file.json"),
                    lsystem::GrammarIOError);
}

TEST_CASE("scanning a directory loads every grammar, sorted", "[grammar-io][io]") {
    const ScratchDirectory scratch("scan");
    lsystem::saveGrammar(scratch.file("zebra.json"), lsystem::presets::bush());
    lsystem::saveGrammar(scratch.file("alpha.json"), lsystem::presets::fern());
    scratch.write("notes.txt", "ignored: not a .json file");

    const lsystem::GrammarDirectory library = lsystem::scanGrammarDirectory(scratch.path());

    REQUIRE(library.grammars.size() == 2);
    // Ordered by filename, so the viewer's dropdown does not shuffle itself.
    CHECK(library.grammars[0].name == "fern");
    CHECK(library.grammars[1].name == "bush");
    CHECK(library.errors.empty());
}

TEST_CASE("one broken file does not empty the library", "[grammar-io][io][errors]") {
    // Mid-edit a file is routinely invalid. Losing every other grammar because
    // of it would make live editing unusable.
    const ScratchDirectory scratch("partial");
    lsystem::saveGrammar(scratch.file("good.json"), lsystem::presets::fern());
    scratch.write("broken.json", "{ oops");

    const lsystem::GrammarDirectory library = lsystem::scanGrammarDirectory(scratch.path());

    REQUIRE(library.grammars.size() == 1);
    CHECK(library.grammars[0].name == "fern");
    REQUIRE(library.errors.size() == 1);
    CHECK_THAT(library.errors[0], ContainsSubstring("broken.json"));
}

TEST_CASE("a missing directory is empty, not an error", "[grammar-io][io]") {
    const lsystem::GrammarDirectory library =
        lsystem::scanGrammarDirectory("definitely/not/a/real/directory");

    CHECK(library.grammars.empty());
    CHECK(library.errors.empty());
    CHECK(library.revision == 0);
}

// ---------------------------------------------------------------------------
// Change detection
// ---------------------------------------------------------------------------

TEST_CASE("the revision changes when a file is added, removed or rewritten",
          "[grammar-io][io]") {
    const ScratchDirectory scratch("revision");
    lsystem::saveGrammar(scratch.file("one.json"), lsystem::presets::fern());

    const std::uint64_t initial = lsystem::scanGrammarDirectory(scratch.path()).revision;
    CHECK(initial != 0);
    // Unchanged directory, unchanged revision: this is what stops the viewer
    // rebuilding the plant twice a second forever.
    CHECK(lsystem::scanGrammarDirectory(scratch.path()).revision == initial);

    lsystem::saveGrammar(scratch.file("two.json"), lsystem::presets::bush());
    const std::uint64_t afterAdd = lsystem::scanGrammarDirectory(scratch.path()).revision;
    CHECK(afterAdd != initial);

    std::filesystem::remove(scratch.file("two.json"));
    CHECK(lsystem::scanGrammarDirectory(scratch.path()).revision == initial);

    // File-system timestamps are coarse, so wait before rewriting.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    lsystem::saveGrammar(scratch.file("one.json"), lsystem::presets::leafyShoot());
    CHECK(lsystem::scanGrammarDirectory(scratch.path()).revision != initial);
}

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

TEST_CASE("a provenance record round-trips every parameter", "[grammar-io][provenance]") {
    lsystem::Provenance original;
    original.grammar = lsystem::presets::leafyShoot();
    original.iterations = 11;
    original.seed = 4242;
    original.angleScale = 1.25f;
    original.thicknessScale = 0.8f;
    original.tropism = -0.125f;
    original.resolution = 192;
    original.sheetSize = 3;
    original.specimens = 9;
    original.segments = 1234;
    original.voxels = 56789;
    original.dimension = 1.875;

    const lsystem::Provenance restored =
        lsystem::provenanceFromJson(lsystem::toJson(original));

    CHECK(restored.iterations == 11);
    CHECK(restored.seed == 4242);
    CHECK(restored.angleScale == 1.25f);
    CHECK(restored.thicknessScale == 0.8f);
    CHECK(restored.tropism == -0.125f);
    CHECK(restored.resolution == 192);
    CHECK(restored.sheetSize == 3);
    // The grammar travels with the record, not a reference to it by name.
    CHECK(restored.grammar.name == original.grammar.name);
    CHECK(restored.grammar.rules == original.grammar.rules);
    CHECK(restored.grammar.constants == original.grammar.constants);
}

TEST_CASE("a restored record regrows the identical plant", "[grammar-io][provenance]") {
    // The whole point: an exported model on disk can be reproduced exactly.
    lsystem::Provenance original;
    original.grammar = lsystem::presets::bush();
    original.iterations = 9;
    original.seed = 77;

    const lsystem::Provenance restored =
        lsystem::provenanceFromJson(lsystem::toJson(original));

    const std::string before = toString(lsystem::LSystem::compile(original.grammar)
                                            .expand(original.iterations, original.seed));
    const std::string after = toString(lsystem::LSystem::compile(restored.grammar)
                                           .expand(restored.iterations, restored.seed));
    CHECK(before == after);
}

TEST_CASE("a record with no parameters block still restores its grammar",
          "[grammar-io][provenance]") {
    // Hand-trimmed or older files should degrade to defaults rather than fail.
    const lsystem::Provenance record = lsystem::provenanceFromJson(
        R"json({"grammar":{"name":"x","axiom":"A","rules":["A -> F(1)"]}})json");

    CHECK(record.grammar.name == "x");
    CHECK(record.iterations == 0);
    CHECK(record.angleScale == 1.0f);
}

TEST_CASE("a malformed provenance record is rejected", "[grammar-io][provenance][errors]") {
    CHECK_THROWS_AS(lsystem::provenanceFromJson("{}"), lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::provenanceFromJson("[]"), lsystem::GrammarIOError);
    CHECK_THROWS_AS(lsystem::provenanceFromJson(R"({"grammar":{"name":"x"}})"),
                    lsystem::GrammarIOError);
    CHECK_THROWS_AS(
        lsystem::provenanceFromJson(
            R"({"grammar":{"name":"x","axiom":"A","rules":[]},"parameters":{"seed":"soon"}})"),
        lsystem::GrammarIOError);
}

TEST_CASE("a provenance record survives a trip through a file", "[grammar-io][provenance][io]") {
    const ScratchDirectory scratch("provenance");
    lsystem::Provenance record;
    record.grammar = lsystem::presets::fern();
    record.iterations = 14;
    record.seed = 5;

    lsystem::saveProvenance(scratch.file("plant.vox.json"), record);
    const lsystem::Provenance restored = lsystem::loadProvenance(scratch.file("plant.vox.json"));

    CHECK(restored.grammar.name == "fern");
    CHECK(restored.iterations == 14);
    CHECK(restored.seed == 5);
}

TEST_CASE("the asset directory is found from anywhere below it", "[grammar-io][io]") {
    const ScratchDirectory scratch("assets");
    std::filesystem::create_directories(scratch.path() / "assets" / "presets");
    const std::filesystem::path deep = scratch.path() / "build" / "msvc" / "bin" / "Debug";
    std::filesystem::create_directories(deep);

    CHECK(lsystem::findAssetDirectory(deep) == scratch.path() / "assets" / "presets");
    CHECK(lsystem::findAssetDirectory(scratch.path()) == scratch.path() / "assets" / "presets");
    // Not found within the allowed number of levels.
    CHECK(lsystem::findAssetDirectory(deep, /*maxLevels=*/1).empty());
}
