#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace plant::viewer {

/// Upper bound of the iteration slider, and so of the generation cache.
inline constexpr int kMaxIterations = 32;

/// Playback state for scrubbing through generations. Separate from PlantParams
/// because it does not describe a plant -- it only drives `iterations`.
struct GrowthPlayback {
    bool playing = false;
    float generationsPerSecond = 2.5f;
    bool loop = true;
};

/// Which algorithm grows the plant. Both produce a turtle::Skeleton, so
/// everything downstream is indifferent to the choice.
enum class Generator { LSystem, SpaceColonization };

/// Everything the sliders drive. Which stages of the pipeline have to rerun is
/// decided by comparing two of these, so the UI never has to know about the
/// pipeline at all.
struct PlantParams {
    Generator generator = Generator::LSystem;
    int presetIndex = 0;
    int iterations = 12;
    int seed = 1;
    float angleScale = 1.0f;
    float thicknessScale = 1.0f;
    /// Positive droops under gravity, negative reaches for the light.
    float tropism = 0.0f;
    int resolution = 128;
    /// Side length of the specimen grid: 1 is a single plant, 3 lays out nine
    /// from nine consecutive seeds.
    int sheetSize = 1;

    // Space colonization only. The crown is an ellipsoid of attraction points;
    // its proportions are what shape the tree, in place of grammar rules.
    int attractors = 900;
    float crownWidth = 1.2f;
    float crownHeight = 1.4f;
    float crownRise = 2.2f;
    float influenceRadius = 0.75f;
    float killDistance = 0.16f;
    float stepLength = 0.09f;
    float radiusExponent = 2.4f;
};

enum class ViewMode { Voxels, Lines, Both };

struct ViewOptions {
    ViewMode mode = ViewMode::Voxels;
    bool showGrid = true;
    bool showImGuiDemo = false;
    /// Set for one frame when the user asks to re-centre the camera.
    bool frameCamera = false;
};

/// Read-only description of what is currently on screen.
struct SceneStats {
    std::string description;
    /// Where the grammars came from, shown so it is obvious whether edits to a
    /// file will be picked up. Empty means the built-in set.
    std::string grammarSource;
    std::size_t modules = 0;
    std::size_t segments = 0;
    int maxDepth = 0;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};

    std::size_t voxels = 0;
    glm::ivec3 gridDimensions{0};
    float voxelSize = 0.0f;
    /// How many plants are on screen; 1 unless a seed sheet is laid out.
    int specimens = 1;
    /// Box-counting dimension of the voxel model; 0 when too small to measure.
    double dimension = 0.0;

    /// Space colonization reports nodes and attractors where the L-system
    /// reports modules; `colonized` says which set is meaningful.
    bool colonized = false;
    std::size_t nodes = 0;
    std::size_t attractorsReached = 0;
    std::size_t attractorsTotal = 0;

    double expandMs = 0.0;
    double skeletonMs = 0.0;
    double voxelizeMs = 0.0;
    /// Generations currently held in the cache, and whether the last change
    /// was served from it rather than recomputed.
    int cachedGenerations = 0;
    bool servedFromCache = false;
};

/// Dear ImGui context plus the GLFW/OpenGL3 backends, scoped to the lifetime
/// of this object. Must be constructed after the Renderer (it needs a current
/// GL context) and destroyed before it.
class UI {
public:
    explicit UI(GLFWwindow* window);
    ~UI();

    UI(const UI&) = delete;
    UI& operator=(const UI&) = delete;
    UI(UI&&) = delete;
    UI& operator=(UI&&) = delete;

    /// Starts a new ImGui frame. Call between Renderer::beginFrame/endFrame.
    void beginFrame();

    /// Draws the panels, writing any slider changes straight into `params`.
    void draw(const std::vector<std::string>& presetNames, PlantParams& params,
              const SceneStats& stats, ViewOptions& options, GrowthPlayback& playback);

    /// Renders the accumulated draw lists into the current framebuffer.
    void endFrame();

    /// True while the pointer is over a panel, so the viewer can stop the
    /// camera from reacting to the same drag.
    [[nodiscard]] bool wantsMouse() const;

    /// Returns the export path once, on the frame the button was pressed.
    [[nodiscard]] std::optional<std::string> takeExportRequest();

    /// What the user asked to capture, returned once on the frame they asked.
    struct CaptureRequest {
        std::string directory;
        /// Multiplies the window size; 1 captures at the size you see.
        int scale = 2;
        /// 1 writes a single screenshot, more sweeps a full orbit.
        int frames = 1;
        /// Advance growth from bare to the current iteration across the frames.
        bool grow = false;
    };
    [[nodiscard]] std::optional<CaptureRequest> takeCaptureRequest();

    /// Progress line while a turntable is rendering; empty when idle.
    void setCaptureProgress(std::string progress);

    /// One-line result of the last action, shown under the export button.
    void setStatus(std::string status, bool isError = false);

private:
    GLFWwindow* window_ = nullptr;
    std::string exportPath_ = "plant.vox";
    bool exportRequested_ = false;
    std::string status_;
    bool statusIsError_ = false;

    CaptureRequest capture_{"captures", 2, 1, false};
    bool captureRequested_ = false;
    std::string captureProgress_;
    int turntableFrames_ = 120;
};

}  // namespace plant::viewer
