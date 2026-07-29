#pragma once

#include "lsystem/LSystem.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// Reading and writing grammars as JSON.
///
/// Deliberately a separate library from `plant_lsystem`: rewriting itself has
/// no dependencies at all, and that is worth keeping -- the core and its tests
/// build without nlohmann/json, GLM or anything else.
///
/// A grammar file mirrors GrammarSource one field at a time:
///
///     {
///       "name": "simple-tree",
///       "description": "Monopodial tree ...",
///       "axiom": "A(1, 0.08)",
///       "constants": { "trunkAngle": 40.0, "extend": 0.9 },
///       "rules": [ "A(l,w) : l > minLength -> F(l,w) ...", ... ]
///     }
namespace plant::lsystem {

struct GrammarIOError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Human-editable JSON, field order chosen for reading rather than alphabetical.
[[nodiscard]] std::string toJson(const GrammarSource& source);

/// Throws GrammarIOError for malformed JSON or a missing/mistyped field. Does
/// not compile the grammar: a file can be well-formed JSON and still be a
/// broken L-system, which LSystem::compile reports separately.
[[nodiscard]] GrammarSource fromJson(std::string_view text);

[[nodiscard]] GrammarSource loadGrammar(const std::filesystem::path& path);
void saveGrammar(const std::filesystem::path& path, const GrammarSource& source);

/// Everything loadable from one directory, plus what went wrong with the rest.
struct GrammarDirectory {
    std::filesystem::path directory;
    /// Successfully parsed grammars, ordered by filename so the UI is stable.
    std::vector<GrammarSource> grammars;
    /// One entry per file that could not be read, already prefixed with its
    /// name. A single bad file must not empty the library.
    std::vector<std::string> errors;
    /// Changes whenever a file is added, removed, renamed or written. Comparing
    /// two scans is how the viewer notices an edit without diffing content.
    std::uint64_t revision = 0;
};

/// Reads every `*.json` in `directory`. A missing directory is not an error:
/// the result is simply empty, and the caller falls back to the built-ins.
[[nodiscard]] GrammarDirectory scanGrammarDirectory(const std::filesystem::path& directory);

/// Everything needed to reproduce one exported model, written beside it.
///
/// The grammar is embedded rather than referenced by name: a preset file can be
/// edited after the fact, and a record that pointed at "leafy" would then
/// describe a plant that no longer exists.
///
/// Deliberately its own schema rather than a mirror of the viewer's parameter
/// struct. The file should outlive a UI refactor.
struct Provenance {
    GrammarSource grammar;

    int iterations = 0;
    std::uint32_t seed = 0;
    float angleScale = 1.0f;
    float thicknessScale = 1.0f;
    float tropism = 0.0f;
    int resolution = 0;
    int sheetSize = 1;

    /// What came out, for reference. Not needed to regenerate.
    int specimens = 1;
    std::size_t segments = 0;
    std::size_t voxels = 0;
    double dimension = 0.0;
};

[[nodiscard]] std::string toJson(const Provenance& record);
[[nodiscard]] Provenance provenanceFromJson(std::string_view text);
[[nodiscard]] Provenance loadProvenance(const std::filesystem::path& path);
void saveProvenance(const std::filesystem::path& path, const Provenance& record);

/// Walks up from `startingPoint` looking for `assets/presets`, so the viewer
/// finds its grammars whether it was launched from the project root or from
/// deep inside a build tree. Returns an empty path if there is none.
[[nodiscard]] std::filesystem::path findAssetDirectory(const std::filesystem::path& startingPoint,
                                                       int maxLevels = 6);

}  // namespace plant::lsystem
