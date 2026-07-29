#include "export/VoxWriter.h"
#include "lsystem/GrammarIO.h"
#include "lsystem/LSystem.h"
#include "lsystem/Presets.h"
#include "turtle/Turtle.h"
#include "viewer/Renderer.h"
#include "viewer/UI.h"
#include "voxelize/Metrics.h"
#include "voxelize/Rasterizer.h"

#include <glm/common.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

using namespace plant;

namespace {

struct Options {
    std::string preset = "simple-tree";
    int iterations = 12;
    std::uint32_t seed = 1;
    /// Voxels along the longest axis of everything on screen.
    int resolution = 128;
    /// Side length of the specimen grid; 1 is a single plant.
    int sheetSize = 1;
    /// Non-empty means "export once at startup, then open the viewer".
    std::string outputPath;
    /// Where to look for grammar JSON. Empty means "search for assets/presets".
    std::string presetDirectory;
    /// Non-empty means "write the built-in grammars here as JSON and exit".
    std::string dumpDirectory;
    /// Non-empty means "render frames into this folder on startup, then exit".
    std::string captureDirectory;
    int captureFrames = 1;
    /// Non-empty means "restore everything from this .vox.json sidecar".
    std::string loadPath;
};

void printUsage() {
    std::printf(
        "usage: plant-gen [preset] [iterations] [seed] [resolution]\n"
        "                 [-o out.vox] [--sheet N]\n                 [--presets DIR] [--dump-presets DIR]\n"
        "\n"
        "Grammars are loaded from DIR (default: the nearest assets/presets), and\n"
        "reloaded automatically when a file there changes. Falls back to the\n"
        "built-in set if no directory is found.\n"
        "\n"
        "built-in presets:\n");
    for (const auto& source : lsystem::presets::all()) {
        std::printf("  %-12s %s\n", source.name.c_str(), source.description.c_str());
    }
}

/// Positional and forgiving on purpose: this only sets the viewer's starting
/// state, which the sliders take over from there.
bool parseOptions(int argc, char** argv, Options& options) {
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            printUsage();
            return false;
        }
        const auto takeValue = [&](std::string& destination) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a path\n", argument.c_str());
                return false;
            }
            destination = argv[++i];
            return true;
        };
        if (argument == "-o" || argument == "--out") {
            if (!takeValue(options.outputPath)) {
                return false;
            }
            continue;
        }
        if (argument == "--load") {
            if (!takeValue(options.loadPath)) {
                return false;
            }
            continue;
        }
        if (argument == "--capture") {
            // Renders frames on startup and exits. Everything the buttons do,
            // minus the buttons -- which is also what makes it testable.
            if (!takeValue(options.captureDirectory)) {
                return false;
            }
            continue;
        }
        if (argument == "--capture-frames") {
            std::string value;
            if (!takeValue(value)) {
                return false;
            }
            options.captureFrames = std::max(1, std::atoi(value.c_str()));
            continue;
        }
        if (argument == "--sheet") {
            std::string value;
            if (!takeValue(value)) {
                return false;
            }
            options.sheetSize = std::clamp(std::atoi(value.c_str()), 1, 4);
            continue;
        }
        if (argument == "--presets") {
            if (!takeValue(options.presetDirectory)) {
                return false;
            }
            continue;
        }
        if (argument == "--dump-presets") {
            if (!takeValue(options.dumpDirectory)) {
                return false;
            }
            continue;
        }
        switch (++positional) {
            case 1: options.preset = argument; break;
            case 2: options.iterations = std::atoi(argument.c_str()); break;
            case 3:
                options.seed =
                    static_cast<std::uint32_t>(std::strtoul(argument.c_str(), nullptr, 10));
                break;
            case 4: options.resolution = std::atoi(argument.c_str()); break;
            default: break;
        }
    }
    return true;
}

double millisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

/// Which part of the pipeline a parameter change invalidates. The stages are
/// ordered: rerunning one implies rerunning everything after it.
enum class Stage { None = 0, Voxelize = 1, Skeleton = 2, Expand = 3 };

Stage stageFor(const viewer::PlantParams& before, const viewer::PlantParams& after) {
    if (before.presetIndex != after.presetIndex || before.iterations != after.iterations ||
        before.seed != after.seed || before.sheetSize != after.sheetSize) {
        return Stage::Expand;
    }
    if (before.angleScale != after.angleScale || before.thicknessScale != after.thicknessScale ||
        before.tropism != after.tropism) {
        return Stage::Skeleton;
    }
    if (before.resolution != after.resolution) {
        return Stage::Voxelize;
    }
    return Stage::None;
}

glm::vec3 colorForDepth(int depth, int maxDepth) {
    // Bark at the trunk, leaf green at the tips: makes the branching order
    // readable at a glance in the line view.
    const float t = maxDepth > 0 ? static_cast<float>(depth) / static_cast<float>(maxDepth) : 0.0f;
    return glm::mix(glm::vec3(0.48f, 0.34f, 0.21f), glm::vec3(0.36f, 0.74f, 0.30f), t);
}

/// Holds the intermediate results of the pipeline so a slider change only
/// redoes the stages below it. Dragging the resolution slider does not
/// re-expand a 70,000 module word.
class Plant {
public:
    explicit Plant(std::vector<lsystem::GrammarSource> sources) { setSources(std::move(sources)); }

    /// Swaps in a reloaded grammar library. Everything downstream is stale
    /// afterwards, so the caller must rebuild from Stage::Expand.
    void setSources(std::vector<lsystem::GrammarSource> sources) {
        sources_ = std::move(sources);
        names_.clear();
        for (const lsystem::GrammarSource& source : sources_) {
            names_.push_back(source.name);
        }
        // A file may have been added, removed or renamed, so nothing cached
        // about the previous library can be trusted.
        compiledIndex_ = -1;
        cachedIndex_ = -1;
        generations_.clear();
        current_ = 0;
    }

    /// Index of `name` in the current library, or the nearest valid index.
    /// Keeps the selection pointing at the same grammar across a reload that
    /// changed the ordering.
    [[nodiscard]] int indexOf(const std::string& name, int fallback) const {
        const auto found = std::find(names_.begin(), names_.end(), name);
        if (found != names_.end()) {
            return static_cast<int>(std::distance(names_.begin(), found));
        }
        return std::clamp(fallback, 0, static_cast<int>(names_.size()) - 1);
    }

    [[nodiscard]] const std::vector<std::string>& presetNames() const { return names_; }
    [[nodiscard]] const lsystem::GrammarSource& source(int index) const {
        return sources_[static_cast<std::size_t>(index)];
    }
    /// Adds a grammar carried in from a provenance file, so a restored export
    /// works even when the preset it came from has since been edited away.
    int adopt(lsystem::GrammarSource grammar) {
        const std::string name = grammar.name;
        sources_.push_back(std::move(grammar));
        names_.push_back(name);
        compiledIndex_ = -1;
        cachedIndex_ = -1;
        generations_.clear();
        return static_cast<int>(sources_.size()) - 1;
    }
    /// One entry per specimen: a single plant, or every cell of the seed sheet.
    [[nodiscard]] const std::vector<turtle::Skeleton>& skeletons() const { return skeletons_; }
    /// World-space position each specimen is laid out at.
    [[nodiscard]] const std::vector<glm::vec3>& offsets() const { return offsets_; }
    [[nodiscard]] const glm::vec3& boundsMin() const { return boundsMin_; }
    [[nodiscard]] const glm::vec3& boundsMax() const { return boundsMax_; }
    /// Every specimen merged into one volume, which is what gets exported.
    [[nodiscard]] const voxelize::VoxelGrid& grid() const { return grid_; }
    [[nodiscard]] const voxelize::RasterizerConfig& rasterizer() const { return rasterizer_; }
    [[nodiscard]] const viewer::SceneStats& stats() const { return stats_; }
    [[nodiscard]] const std::string& error() const { return error_; }

    /// Runs `from` and every stage after it. Returns false and leaves the last
    /// good result in place if the parameters produce something unbuildable.
    bool rebuild(const viewer::PlantParams& params, Stage from) {
        // An empty cache means the last rebuild threw before producing a
        // generation -- a reload that brought in a grammar which does not
        // compile. Later stages read the cache directly, so a skeleton-only
        // rebuild would index an empty vector; retry the expansion instead.
        if (generations_.empty()) {
            from = Stage::Expand;
        }
        try {
            if (from >= Stage::Expand) {
                const auto& source = sources_[static_cast<std::size_t>(params.presetIndex)];
                if (params.presetIndex != compiledIndex_) {
                    system_ = lsystem::LSystem::compile(source);
                    compiledIndex_ = params.presetIndex;
                }
                stats_.description = source.description;

                const auto start = std::chrono::steady_clock::now();
                expandAll(params);
                stats_.expandMs = millisecondsSince(start);
                stats_.cachedGenerations = static_cast<int>(generations_.size());
            }

            if (from >= Stage::Skeleton) {
                turtle::TurtleConfig config;
                config.angleScale = params.angleScale;
                config.radiusScale = params.thicknessScale;
                config.tropismStrength = params.tropism;

                const auto start = std::chrono::steady_clock::now();
                skeletons_.clear();
                for (const lsystem::Word& word : words_) {
                    skeletons_.push_back(turtle::buildSkeleton(word, config));
                }
                layOut(params);
                stats_.skeletonMs = millisecondsSince(start);
            }

            if (from >= Stage::Voxelize) {
                const auto start = std::chrono::steady_clock::now();
                voxelizeAll(params);
                stats_.voxelizeMs = millisecondsSince(start);
            }

            stats_.modules = 0;
            stats_.segments = 0;
            stats_.maxDepth = 0;
            for (std::size_t i = 0; i < skeletons_.size(); ++i) {
                stats_.modules += words_[i].size();
                stats_.segments += skeletons_[i].segments.size();
                stats_.maxDepth = std::max(stats_.maxDepth, skeletons_[i].maxDepth());
            }
            stats_.specimens = static_cast<int>(skeletons_.size());
            stats_.boundsMin = boundsMin_;
            stats_.boundsMax = boundsMax_;
            stats_.voxels = grid_.voxelCount();
            stats_.dimension = voxelize::boxCountingDimension(grid_);
            stats_.gridDimensions = grid_.dimensions();
            stats_.voxelSize = grid_.voxelSize();
            error_.clear();
            return true;
        } catch (const std::exception& e) {
            error_ = e.what();
            return false;
        }
    }

private:
    /// Generation `n` of the current grammar and seed, computed on demand and
    /// kept.
    ///
    /// expand(n) is just step() applied n times from the axiom, so successive
    /// generations extend the cache by one rather than starting over. That is
    /// what makes scrubbing -- and playback -- cost a lookup instead of a full
    /// re-expansion of a 70,000 module word every frame.
    ///
    /// One generator drives the whole chain, exactly as expand() would, so
    /// generations_[n] is identical to expand(n, seed) draw for draw.
    /// One word per specimen. A single plant comes from the generation cache so
    /// scrubbing stays free; a sheet expands each seed directly, since caching
    /// every generation of nine plants would cost far more than it saves.
    void expandAll(const viewer::PlantParams& params) {
        const int specimens = std::max(1, params.sheetSize * params.sheetSize);
        words_.clear();

        if (specimens == 1) {
            selectGeneration(params);
            words_.push_back(generations_[current_]);
            return;
        }

        generations_.clear();  // the cache belongs to a single plant
        stats_.servedFromCache = false;
        for (int index = 0; index < specimens; ++index) {
            const auto seed = static_cast<std::uint32_t>(params.seed + index);
            words_.push_back(system_.expand(params.iterations, seed));
        }
    }

    /// Arranges the specimens on a square grid, spaced by the widest footprint
    /// so neighbours never overlap however differently they grew.
    void layOut(const viewer::PlantParams& params) {
        offsets_.assign(skeletons_.size(), glm::vec3(0.0f));
        const int side = std::max(1, params.sheetSize);

        float cell = 0.0f;
        for (const turtle::Skeleton& skeleton : skeletons_) {
            cell = std::max({cell, skeleton.size().x, skeleton.size().z});
        }
        cell *= 1.15f;  // a little air between specimens

        boundsMin_ = glm::vec3(std::numeric_limits<float>::max());
        boundsMax_ = glm::vec3(std::numeric_limits<float>::lowest());
        for (std::size_t index = 0; index < skeletons_.size(); ++index) {
            const auto column = static_cast<int>(index) % side;
            const auto row = static_cast<int>(index) / side;
            // Centred on the origin, so the camera behaves the same in both modes.
            const glm::vec3 offset(
                (static_cast<float>(column) - 0.5f * static_cast<float>(side - 1)) * cell, 0.0f,
                (static_cast<float>(row) - 0.5f * static_cast<float>(side - 1)) * cell);
            offsets_[index] = offset;
            boundsMin_ = glm::min(boundsMin_, skeletons_[index].boundsMin + offset);
            boundsMax_ = glm::max(boundsMax_, skeletons_[index].boundsMax + offset);
        }
        if (skeletons_.empty()) {
            boundsMin_ = glm::vec3(0.0f);
            boundsMax_ = glm::vec3(0.0f);
        }
    }

    /// Voxelizes every specimen at one shared voxel size, then merges them into
    /// a single volume. Sharing the size is what makes the merge meaningful --
    /// and resolution counts voxels across the whole sheet, so a 3x3 export
    /// stays inside MagicaVoxel's 256 limit instead of blowing past it.
    void voxelizeAll(const viewer::PlantParams& params) {
        const glm::vec3 extent = boundsMax_ - boundsMin_;
        const float longest = std::max({extent.x, extent.y, extent.z, 1e-6f});
        rasterizer_.voxelSize = longest / static_cast<float>(std::max(1, params.resolution));

        if (skeletons_.size() == 1) {
            grid_ = voxelize::voxelize(skeletons_.front(), rasterizer_);
            return;
        }

        std::vector<voxelize::VoxelGrid> parts;
        parts.reserve(skeletons_.size());
        for (const turtle::Skeleton& skeleton : skeletons_) {
            parts.push_back(voxelize::voxelize(skeleton, rasterizer_));
        }

        const glm::vec3 origin = boundsMin_ - glm::vec3(rasterizer_.voxelSize);
        glm::ivec3 dimensions(1);
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const glm::vec3 corner = parts[index].origin() + offsets_[index];
            const glm::ivec3 cell =
                glm::ivec3(glm::round((corner - origin) / rasterizer_.voxelSize));
            dimensions = glm::max(dimensions, cell + parts[index].dimensions());
        }

        grid_ = voxelize::VoxelGrid(dimensions, origin, rasterizer_.voxelSize);
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const glm::vec3 corner = parts[index].origin() + offsets_[index];
            const glm::ivec3 cell =
                glm::ivec3(glm::round((corner - origin) / rasterizer_.voxelSize));
            voxelize::blit(grid_, parts[index], cell);
        }
    }

    void selectGeneration(const viewer::PlantParams& params) {
        const auto seed = static_cast<std::uint32_t>(params.seed);
        if (generations_.empty() || params.presetIndex != cachedIndex_ || seed != cachedSeed_) {
            generations_.assign(1, system_.axiom());
            rng_.seed(seed);
            cachedIndex_ = params.presetIndex;
            cachedSeed_ = seed;
        }

        const auto wanted = static_cast<std::size_t>(
            std::clamp(params.iterations, 0, viewer::kMaxIterations));
        stats_.servedFromCache = wanted < generations_.size();
        while (generations_.size() <= wanted) {
            generations_.push_back(system_.step(generations_.back(), rng_));
        }
        // An index, not a pointer or a copy: push_back above can reallocate, and
        // the word is re-read on later skeleton-only rebuilds.
        current_ = wanted;
    }

    std::vector<lsystem::GrammarSource> sources_;
    std::vector<std::string> names_;
    lsystem::LSystem system_;
    int compiledIndex_ = -1;

    std::vector<lsystem::Word> generations_;
    std::size_t current_ = 0;
    std::mt19937 rng_;
    int cachedIndex_ = -1;
    std::uint32_t cachedSeed_ = 0;

    std::vector<lsystem::Word> words_;
    std::vector<turtle::Skeleton> skeletons_;
    std::vector<glm::vec3> offsets_;
    glm::vec3 boundsMin_{0.0f};
    glm::vec3 boundsMax_{0.0f};
    voxelize::VoxelGrid grid_;
    voxelize::RasterizerConfig rasterizer_;
    viewer::SceneStats stats_;
    std::string error_;
};

void appendSkeletonLines(std::vector<viewer::LineVertex>& vertices,
                         const turtle::Skeleton& skeleton, const glm::vec3& offset) {
    const int maxDepth = skeleton.maxDepth();
    for (const turtle::Segment& segment : skeleton.segments) {
        const glm::vec3 color = colorForDepth(segment.depth, maxDepth);
        vertices.push_back({segment.start + offset, color});
        vertices.push_back({segment.end + offset, color});
    }

    // Polygons show as closed outlines: the line view is a debug view, and an
    // unfilled leaf is more informative there than a filled one.
    const glm::vec3 leafColor(0.45f, 0.77f, 0.33f);
    for (const turtle::Polygon& polygon : skeleton.polygons) {
        for (std::size_t i = 0; i < polygon.vertices.size(); ++i) {
            const glm::vec3 from = polygon.vertices[i] + offset;
            const glm::vec3 to = polygon.vertices[(i + 1) % polygon.vertices.size()] + offset;
            vertices.push_back({from, leafColor});
            vertices.push_back({to, leafColor});
        }
    }
}

/// A screenshot or turntable in progress. Frames are rendered one per
/// application frame rather than in a blocking loop, so the window keeps
/// responding and the progress line actually updates.
struct CaptureJob {
    bool active = false;
    viewer::UI::CaptureRequest request;
    int frame = 0;
    /// Camera and growth are restored when the job finishes.
    float savedYaw = 0.0f;
    int savedIterations = 0;

    [[nodiscard]] bool isTurntable() const { return request.frames > 1; }
};

/// Writes `<model>.vox.json` beside the model. Without it an exported plant is
/// orphaned: nothing on disk records which grammar and seed produced it.
void writeSidecar(const std::filesystem::path& voxPath, const lsystem::GrammarSource& grammar,
                  const viewer::PlantParams& params, const viewer::SceneStats& stats) {
    lsystem::Provenance record;
    record.grammar = grammar;
    record.iterations = params.iterations;
    record.seed = static_cast<std::uint32_t>(params.seed);
    record.angleScale = params.angleScale;
    record.thicknessScale = params.thicknessScale;
    record.tropism = params.tropism;
    record.resolution = params.resolution;
    record.sheetSize = params.sheetSize;
    record.specimens = stats.specimens;
    record.segments = stats.segments;
    record.voxels = stats.voxels;
    record.dimension = stats.dimension;

    lsystem::saveProvenance(voxPath.string() + ".json", record);
}

/// Uses the same palette the exporter writes, so what you see is what
/// MagicaVoxel will show.
std::vector<viewer::VoxelInstance> gridToInstances(const voxelize::VoxelGrid& grid,
                                                   const vox::Palette& palette) {
    std::vector<viewer::VoxelInstance> instances;
    instances.reserve(grid.voxelCount());
    for (const voxelize::Voxel& voxel : grid.toVector()) {
        const vox::Rgba& rgba = palette[voxel.color];
        instances.push_back({grid.voxelCenter(voxel.position),
                             glm::vec3(rgba.r, rgba.g, rgba.b) / 255.0f});
    }
    return instances;
}

void uploadTo(viewer::Renderer& renderer, const Plant& plant, const vox::Palette& palette) {
    std::vector<viewer::LineVertex> lines;
    for (std::size_t index = 0; index < plant.skeletons().size(); ++index) {
        appendSkeletonLines(lines, plant.skeletons()[index], plant.offsets()[index]);
    }
    renderer.setLines(lines);

    // The merged grid already carries every specimen at its laid-out position.
    renderer.setVoxels(gridToInstances(plant.grid(), palette), plant.grid().voxelSize());

    const glm::vec3 extent = plant.boundsMax() - plant.boundsMin();
    const float halfExtent = std::max(0.5f, 0.5f * std::max(extent.x, extent.z) * 1.5f);
    renderer.setGrid(halfExtent, 20);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 0;
    }

    try {
        if (!options.dumpDirectory.empty()) {
            // How assets/presets is generated: the built-ins are the source of
            // truth, the files are a copy you are then free to edit.
            for (const lsystem::GrammarSource& source : lsystem::presets::all()) {
                const std::filesystem::path path =
                    std::filesystem::path(options.dumpDirectory) / (source.name + ".json");
                lsystem::saveGrammar(path, source);
                std::printf("[presets] wrote %s\n", path.string().c_str());
            }
            return 0;
        }

        std::filesystem::path presetDirectory = options.presetDirectory;
        if (presetDirectory.empty()) {
            presetDirectory = lsystem::findAssetDirectory(std::filesystem::current_path());
        }
        if (presetDirectory.empty() && argc > 0) {
            presetDirectory = lsystem::findAssetDirectory(
                std::filesystem::absolute(argv[0]).parent_path());
        }

        lsystem::GrammarDirectory library = lsystem::scanGrammarDirectory(presetDirectory);
        for (const std::string& error : library.errors) {
            std::fprintf(stderr, "[presets] %s\n", error.c_str());
        }
        const bool usingFiles = !library.grammars.empty();
        if (usingFiles) {
            std::printf("[presets] %zu grammar(s) from %s\n", library.grammars.size(),
                        presetDirectory.string().c_str());
        } else {
            std::printf("[presets] no grammar files found, using the built-in set\n");
            library.grammars = lsystem::presets::all();
        }

        Plant plant(library.grammars);
        const std::vector<std::string>& names = plant.presetNames();
        const auto found = std::find(names.begin(), names.end(), options.preset);
        if (found == names.end()) {
            std::fprintf(stderr, "unknown preset '%s'\n\n", options.preset.c_str());
            printUsage();
            return 1;
        }

        viewer::PlantParams params;
        params.presetIndex = static_cast<int>(std::distance(names.begin(), found));
        params.iterations = options.iterations;
        params.seed = static_cast<int>(options.seed);
        params.resolution = options.resolution;
        params.sheetSize = options.sheetSize;

        if (!options.loadPath.empty()) {
            // The grammar travels inside the record, so this reproduces the
            // original even if the preset it came from has changed since.
            const lsystem::Provenance record = lsystem::loadProvenance(options.loadPath);
            params.presetIndex = plant.adopt(record.grammar);
            params.iterations = record.iterations;
            params.seed = static_cast<int>(record.seed);
            params.angleScale = record.angleScale;
            params.thicknessScale = record.thicknessScale;
            params.tropism = record.tropism;
            params.resolution = record.resolution;
            params.sheetSize = record.sheetSize;
            std::printf("[load] restored '%s' from %s\n", record.grammar.name.c_str(),
                        options.loadPath.c_str());
        }

        if (!plant.rebuild(params, Stage::Expand)) {
            std::fprintf(stderr, "fatal: %s\n", plant.error().c_str());
            return 1;
        }
        std::printf("[plant] %s: %d specimen(s), %zu modules -> %zu segments -> %zu voxels\n",
                    names[static_cast<std::size_t>(params.presetIndex)].c_str(),
                    plant.stats().specimens, plant.stats().modules, plant.stats().segments,
                    plant.stats().voxels);

        const vox::Palette palette = vox::barkToLeafPalette(plant.rasterizer().firstColor,
                                                            plant.rasterizer().colorCount,
                                                            plant.rasterizer().polygonColor);

        if (!options.outputPath.empty()) {
            vox::writeVox(options.outputPath, plant.grid(), palette);
            writeSidecar(options.outputPath, plant.source(params.presetIndex), params,
                         plant.stats());
            std::printf("[export] wrote %s (+ .json)\n", options.outputPath.c_str());
        }

        viewer::WindowConfig windowConfig;
        windowConfig.title = "plant-lsystems";

        viewer::Renderer renderer(windowConfig);
        viewer::UI ui(renderer.window());

        uploadTo(renderer, plant, palette);
        renderer.camera().frame(plant.boundsMin(), plant.boundsMax());

        const std::string grammarSource = usingFiles ? presetDirectory.string() : std::string{};

        viewer::ViewOptions viewOptions;
        viewer::GrowthPlayback playback;
        viewer::PlantParams previous = params;

        // Fractional, so playback speed is independent of frame rate; the
        // integer iteration count is only sampled from it.
        double growthPhase = params.iterations;
        auto lastFrame = std::chrono::steady_clock::now();
        double sinceDirectoryPoll = 0.0;
        CaptureJob capture;
        bool exitAfterCapture = false;
        if (!options.captureDirectory.empty()) {
            capture.active = true;
            capture.request.directory = options.captureDirectory;
            capture.request.scale = 2;
            capture.request.frames = options.captureFrames;
            capture.request.grow = options.captureFrames > 1;
            capture.frame = 0;
            capture.savedYaw = renderer.camera().yaw;
            capture.savedIterations = params.iterations;
            exitAfterCapture = true;
        }

        while (!renderer.shouldClose()) {
            const auto now = std::chrono::steady_clock::now();
            const double deltaSeconds = std::chrono::duration<double>(now - lastFrame).count();
            lastFrame = now;

            // Hot reload. Polling the directory means stat-ing a handful of
            // files, so twice a second is plenty and costs nothing noticeable.
            sinceDirectoryPoll += deltaSeconds;
            if (usingFiles && sinceDirectoryPoll > 0.5) {
                sinceDirectoryPoll = 0.0;
                lsystem::GrammarDirectory rescanned = lsystem::scanGrammarDirectory(presetDirectory);
                if (rescanned.revision != library.revision && !rescanned.grammars.empty()) {
                    const std::string selected = names[static_cast<std::size_t>(params.presetIndex)];
                    library = std::move(rescanned);
                    plant.setSources(library.grammars);
                    // Follow the grammar by name: a reload may have reordered
                    // the directory out from under the index.
                    params.presetIndex = plant.indexOf(selected, params.presetIndex);
                    previous = params;

                    if (plant.rebuild(params, Stage::Expand)) {
                        uploadTo(renderer, plant, palette);
                        ui.setStatus("reloaded " + std::to_string(library.grammars.size()) +
                                     " grammar(s)");
                        std::printf("[presets] reloaded %zu grammar(s): %s now %zu segments\n",
                                    library.grammars.size(), selected.c_str(),
                                    plant.stats().segments);
                        std::fflush(stdout);
                    } else {
                        // The last good plant stays on screen; the message says
                        // why the new one did not replace it.
                        ui.setStatus(plant.error(), /*isError=*/true);
                        std::fprintf(stderr, "[presets] reload failed: %s\n",
                                     plant.error().c_str());
                    }
                    for (const std::string& error : library.errors) {
                        ui.setStatus(error, /*isError=*/true);
                        std::fprintf(stderr, "[presets] %s\n", error.c_str());
                    }
                }
            }

            if (playback.playing) {
                growthPhase += deltaSeconds * playback.generationsPerSecond;
                if (growthPhase >= viewer::kMaxIterations) {
                    if (playback.loop) {
                        growthPhase = 0.0;
                    } else {
                        growthPhase = viewer::kMaxIterations;
                        playback.playing = false;
                    }
                }
                params.iterations = static_cast<int>(growthPhase);
            } else {
                // Scrubbing by hand takes over the phase, so pressing play
                // resumes from wherever the slider was left.
                growthPhase = params.iterations;
            }

            renderer.setInputEnabled(!ui.wantsMouse());
            renderer.setShowGrid(viewOptions.showGrid);

            renderer.beginFrame();

            const auto drawScene = [&] {
                renderer.drawGrid();
                if (viewOptions.mode != viewer::ViewMode::Lines) {
                    renderer.drawVoxels();
                }
                if (viewOptions.mode != viewer::ViewMode::Voxels) {
                    renderer.drawLines();
                }
            };
            drawScene();

            if (capture.active) {
                // Position this frame of the sweep, then render it offscreen at
                // the requested scale using the very same draw calls.
                if (capture.isTurntable()) {
                    const float t = static_cast<float>(capture.frame) /
                                    static_cast<float>(capture.request.frames);
                    renderer.camera().yaw = capture.savedYaw + t * 6.28318531f;
                    if (capture.request.grow) {
                        params.iterations = static_cast<int>(
                            t * static_cast<float>(capture.savedIterations + 1));
                        if (const Stage stage = stageFor(previous, params); stage != Stage::None) {
                            if (plant.rebuild(params, stage)) {
                                uploadTo(renderer, plant, palette);
                            }
                            previous = params;
                        }
                    }
                }

                const int width = renderer.framebufferWidth() * capture.request.scale;
                const int height = renderer.framebufferHeight() * capture.request.scale;
                renderer.beginCapture(width, height);
                drawScene();
                const std::vector<unsigned char> pixels = renderer.endCapture();

                char name[64];
                if (capture.isTurntable()) {
                    std::snprintf(name, sizeof(name), "frame_%04d.png", capture.frame);
                } else {
                    std::snprintf(name, sizeof(name), "%s.png",
                                  names[static_cast<std::size_t>(params.presetIndex)].c_str());
                }
                const std::filesystem::path file =
                    std::filesystem::path(capture.request.directory) / name;

                std::error_code ignored;
                std::filesystem::create_directories(file.parent_path(), ignored);
                if (!viewer::writePng(file.string(), width, height, pixels)) {
                    ui.setStatus("could not write " + file.string(), /*isError=*/true);
                    capture.active = false;
                } else if (++capture.frame >= capture.request.frames) {
                    capture.active = false;
                    ui.setStatus("wrote " + std::to_string(capture.request.frames) +
                                 " frame(s) to " + capture.request.directory);
                    std::printf("[capture] %d frame(s) at %dx%d -> %s\n", capture.request.frames,
                                width, height, capture.request.directory.c_str());
                    std::fflush(stdout);
                }

                if (!capture.active && exitAfterCapture) {
                    break;  // --capture is a batch run, not a session
                }
                if (!capture.active) {
                    // Put the view back the way the user left it.
                    renderer.camera().yaw = capture.savedYaw;
                    if (capture.request.grow) {
                        params.iterations = capture.savedIterations;
                    }
                    ui.setCaptureProgress({});
                } else {
                    ui.setCaptureProgress("rendering frame " + std::to_string(capture.frame) +
                                          " of " + std::to_string(capture.request.frames));
                }
            }

            viewer::SceneStats stats = plant.stats();
            stats.grammarSource = grammarSource;

            ui.beginFrame();
            ui.draw(names, params, stats, viewOptions, playback);
            ui.endFrame();
            renderer.endFrame();

            if (const Stage stage = stageFor(previous, params); stage != Stage::None) {
                if (plant.rebuild(params, stage)) {
                    uploadTo(renderer, plant, palette);
                    ui.setStatus({});
                } else {
                    ui.setStatus(plant.error(), /*isError=*/true);
                }
                previous = params;
            }

            if (viewOptions.frameCamera) {
                renderer.camera().frame(plant.boundsMin(), plant.boundsMax());
            }

            if (const auto request = ui.takeCaptureRequest(); request && !capture.active) {
                capture.active = true;
                capture.request = *request;
                capture.frame = 0;
                capture.savedYaw = renderer.camera().yaw;
                capture.savedIterations = params.iterations;
            }

            if (const auto path = ui.takeExportRequest()) {
                try {
                    vox::writeVox(*path, plant.grid(), palette);
                    writeSidecar(*path, plant.source(params.presetIndex), params, plant.stats());
                    ui.setStatus("wrote " + *path + " + .json (" +
                                 std::to_string(plant.stats().voxels) + " voxels)");
                } catch (const std::exception& e) {
                    ui.setStatus(e.what(), /*isError=*/true);
                }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
