#include "viewer/UI.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace plant::viewer {
namespace {

// Must match the context requested in Renderer.
constexpr const char* kGlslVersion = "#version 330";

}  // namespace

UI::UI(GLFWwindow* window) : window_(window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // No imgui.ini: window layout is defined in code, not persisted per machine.
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, /*install_callbacks=*/true)) {
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui_ImplGlfw_InitForOpenGL failed");
    }
    if (!ImGui_ImplOpenGL3_Init(kGlslVersion)) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui_ImplOpenGL3_Init failed");
    }
}

UI::~UI() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void UI::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

bool UI::wantsMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

std::optional<std::string> UI::takeExportRequest() {
    if (!exportRequested_) {
        return std::nullopt;
    }
    exportRequested_ = false;
    return exportPath_;
}

void UI::setStatus(std::string status, bool isError) {
    status_ = std::move(status);
    statusIsError_ = isError;
}

std::optional<UI::CaptureRequest> UI::takeCaptureRequest() {
    if (!captureRequested_) {
        return std::nullopt;
    }
    captureRequested_ = false;
    return capture_;
}

void UI::setCaptureProgress(std::string progress) {
    captureProgress_ = std::move(progress);
}

void UI::draw(const std::vector<std::string>& presetNames, PlantParams& params,
              const SceneStats& stats, ViewOptions& options, GrowthPlayback& playback) {
    const ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("plant-lsystems")) {
        ImGui::Text("%.1f FPS (%.2f ms/frame)", io.Framerate, 1000.0f / io.Framerate);
        ImGui::Separator();

        // ----------------------------------------------------------- generator
        ImGui::TextDisabled("Generator");
        int generator = static_cast<int>(params.generator);
        ImGui::RadioButton("L-system", &generator, 0);
        ImGui::SameLine();
        ImGui::RadioButton("space colonization", &generator, 1);
        params.generator = static_cast<Generator>(generator);
        const bool grammarDriven = params.generator == Generator::LSystem;

        if (!grammarDriven) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled(
                "Branches grow towards a cloud of attraction points and consume them on "
                "arrival. The crown's shape decides the tree's, with no rules involved.");
            ImGui::PopTextWrapPos();

            ImGui::SliderInt("attractors", &params.attractors, 50, 4000);
            ImGui::SliderFloat("crown width", &params.crownWidth, 0.2f, 3.0f, "%.2f");
            ImGui::SliderFloat("crown height", &params.crownHeight, 0.2f, 3.0f, "%.2f");
            ImGui::SliderFloat("crown rise", &params.crownRise, 0.5f, 5.0f, "%.2f");
            ImGui::SliderFloat("influence", &params.influenceRadius, 0.1f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How far a point reaches for a branch tip.\n"
                                  "Too small stalls growth; too large averages into one trunk.");
            }
            ImGui::SliderFloat("kill distance", &params.killDistance, 0.02f, 0.6f, "%.3f");
            ImGui::SliderFloat("step", &params.stepLength, 0.02f, 0.3f, "%.3f");
            ImGui::SliderFloat("Murray exponent", &params.radiusExponent, 1.5f, 3.5f, "%.2f");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Thickness rule: parent^n = sum of child^n.\n"
                                  "2 conserves cross-sectional area; real trees measure ~2.5.");
            }

            ImGui::Spacing();
            ImGui::SliderInt("seed", &params.seed, 0, 999);
            ImGui::SameLine();
            if (ImGui::SmallButton("roll")) {
                params.seed = (params.seed * 1103515245 + 12345) % 1000;
            }
        }

        // ------------------------------------------------------------- grammar
        // Grammar, growth playback and the turtle knobs belong to the L-system
        // path only: space colonization has no rules to rewrite and never runs
        // the turtle. Resolution, view, export and capture are shared, because
        // both paths hand the same Skeleton to the same downstream stages.
        if (grammarDriven) {
        ImGui::Spacing();
        ImGui::TextDisabled("Grammar");
        const char* currentPreset =
            presetNames.empty() ? "none" : presetNames[static_cast<std::size_t>(params.presetIndex)].c_str();
        if (ImGui::BeginCombo("preset", currentPreset)) {
            for (int index = 0; index < static_cast<int>(presetNames.size()); ++index) {
                const bool selected = index == params.presetIndex;
                if (ImGui::Selectable(presetNames[static_cast<std::size_t>(index)].c_str(), selected)) {
                    params.presetIndex = index;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (!stats.description.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", stats.description.c_str());
            ImGui::PopTextWrapPos();
        }
        if (stats.grammarSource.empty()) {
            ImGui::TextDisabled("built-in grammars (no files found)");
        } else {
            ImGui::TextDisabled("watching %s", stats.grammarSource.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Edit a .json in this folder and the plant reloads.");
            }
        }

        ImGui::SliderInt("iterations", &params.iterations, 0, kMaxIterations);
        ImGui::SliderInt("seed", &params.seed, 0, 999);
        ImGui::SameLine();
        if (ImGui::SmallButton("roll")) {
            params.seed = (params.seed * 1103515245 + 12345) % 1000;
        }

        // ------------------------------------------------------------- growth
        ImGui::Spacing();
        ImGui::TextDisabled("Growth");
        if (ImGui::Button(playback.playing ? "Pause" : "Play")) {
            playback.playing = !playback.playing;
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart")) {
            params.iterations = 0;
            playback.playing = true;
        }
        ImGui::SameLine();
        ImGui::Checkbox("loop", &playback.loop);
        ImGui::SliderFloat("gen/sec", &playback.generationsPerSecond, 0.25f, 12.0f, "%.2f");
        if (stats.cachedGenerations > 0) {
            ImGui::TextDisabled("%d generation%s cached%s", stats.cachedGenerations,
                                stats.cachedGenerations == 1 ? "" : "s",
                                stats.servedFromCache ? ", last from cache" : "");
        }

        // ------------------------------------------------------------ geometry
        ImGui::Spacing();
        ImGui::TextDisabled("Geometry");
        ImGui::SliderFloat("angle", &params.angleScale, 0.0f, 2.5f, "%.2f x");
        ImGui::SliderFloat("thickness", &params.thicknessScale, 0.1f, 4.0f, "%.2f x");
        ImGui::SliderFloat("tropism", &params.tropism, -0.25f, 0.25f, "%.3f rad");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Bends each segment towards gravity.\nNegative reaches upwards instead.");
        }
        }  // grammarDriven

        // ------------------------------------------------------------- shared
        ImGui::Spacing();
        ImGui::TextDisabled("Voxels");
        ImGui::SliderInt("resolution", &params.resolution, 16, 256);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Voxels across the longest axis of everything on screen.");
        }
        ImGui::SliderInt("seed sheet", &params.sheetSize, 1, 4,
                         params.sheetSize == 1 ? "single plant" : "%dx%d");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Lays out N*N plants from consecutive seeds, for comparison.");
        }

        // --------------------------------------------------------------- view
        ImGui::Spacing();
        ImGui::TextDisabled("View");
        int mode = static_cast<int>(options.mode);
        if (ImGui::RadioButton("voxels", &mode, 0) || ImGui::RadioButton("lines", &mode, 1) ||
            ImGui::RadioButton("both", &mode, 2)) {
        }
        options.mode = static_cast<ViewMode>(mode);
        ImGui::Checkbox("ground grid", &options.showGrid);
        options.frameCamera = ImGui::Button("Frame plant");

        // -------------------------------------------------------------- stats
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen) &&
            ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingStretchProp)) {
            const auto row = [](const char* label, const char* format, auto... value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
                ImGui::Text(format, value...);
            };
            if (stats.specimens > 1) {
                row("specimens", "%d", stats.specimens);
            }
            if (stats.colonized) {
                row("nodes", "%zu", stats.nodes);
                row("attractors", "%zu of %zu", stats.attractorsReached,
                    stats.attractorsTotal);
            } else {
                row("modules", "%zu", stats.modules);
            }
            row("segments", "%zu", stats.segments);
            row("branch depth", "%d", stats.maxDepth);
            row("height", "%.2f", stats.boundsMax.y - stats.boundsMin.y);
            row("spread", "%.2f x %.2f", stats.boundsMax.x - stats.boundsMin.x,
                stats.boundsMax.z - stats.boundsMin.z);
            row("voxel grid", "%d x %d x %d", stats.gridDimensions.x, stats.gridDimensions.y,
                stats.gridDimensions.z);
            row("voxels", "%zu", stats.voxels);
            if (stats.dimension > 0.0) {
                row("box dimension", "%.3f", stats.dimension);
            }
            row(stats.colonized ? "colonize" : "rewrite", "%.1f ms", stats.expandMs);
            if (!stats.colonized) {
                row("turtle", "%.1f ms", stats.skeletonMs);
            }
            row("voxelize", "%.1f ms", stats.voxelizeMs);
            ImGui::EndTable();
        }

        // ------------------------------------------------------------- export
        ImGui::Spacing();
        ImGui::TextDisabled("Export");
        char pathBuffer[260];
        std::snprintf(pathBuffer, sizeof(pathBuffer), "%s", exportPath_.c_str());
        if (ImGui::InputText("path", pathBuffer, sizeof(pathBuffer))) {
            exportPath_ = pathBuffer;
        }
        // MagicaVoxel stores coordinates in a single byte, so the grid may be
        // previewable but not exportable.
        const bool tooLarge = stats.gridDimensions.x > 256 || stats.gridDimensions.y > 256 ||
                              stats.gridDimensions.z > 256;
        ImGui::BeginDisabled(tooLarge || stats.voxels == 0);
        if (ImGui::Button("Write .vox")) {
            exportRequested_ = true;
        }
        ImGui::EndDisabled();
        if (tooLarge) {
            ImGui::SameLine();
            ImGui::TextDisabled("(grid exceeds 256)");
        }
        if (!status_.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            if (statusIsError_) {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", status_.c_str());
            } else {
                ImGui::TextDisabled("%s", status_.c_str());
            }
            ImGui::PopTextWrapPos();
        }

        // ------------------------------------------------------------ capture
        ImGui::Spacing();
        ImGui::TextDisabled("Capture");
        char directoryBuffer[260];
        std::snprintf(directoryBuffer, sizeof(directoryBuffer), "%s", capture_.directory.c_str());
        if (ImGui::InputText("folder", directoryBuffer, sizeof(directoryBuffer))) {
            capture_.directory = directoryBuffer;
        }
        ImGui::SliderInt("scale", &capture_.scale, 1, 4, "%dx window");
        const bool busy = !captureProgress_.empty();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Screenshot")) {
            capture_.frames = 1;
            captureRequested_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Turntable")) {
            capture_.frames = turntableFrames_;
            captureRequested_ = true;
        }
        ImGui::EndDisabled();
        ImGui::SliderInt("frames", &turntableFrames_, 12, 240);
        ImGui::Checkbox("sweep growth over the orbit", &capture_.grow);
        if (busy) {
            ImGui::TextDisabled("%s", captureProgress_.c_str());
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Left drag orbit / right drag pan / wheel zoom");
    }
    ImGui::End();

    if (options.showImGuiDemo) {
        ImGui::ShowDemoWindow(&options.showImGuiDemo);
    }
}

void UI::endFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

}  // namespace plant::viewer
