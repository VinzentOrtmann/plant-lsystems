#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <string>
#include <vector>

struct GLFWwindow;

namespace plant::viewer {

struct WindowConfig {
    int width = 1280;
    int height = 800;
    std::string title = "plant-lsystems";
};

struct LineVertex {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
};

/// One voxel, drawn as an instanced unit cube. Meshing the surface would emit
/// far fewer triangles, but at these counts the GPU does not care and
/// regeneration only has to re-upload a flat array.
struct VoxelInstance {
    glm::vec3 position{0.0f};  // world-space centre
    glm::vec3 color{1.0f};
};

/// Orbit camera in spherical coordinates around a target point.
struct OrbitCamera {
    glm::vec3 target{0.0f};
    float distance = 4.0f;
    float yaw = 0.7f;    // radians, around +Y
    float pitch = 0.3f;  // radians, clamped short of the poles
    float fovDegrees = 50.0f;
    float nearPlane = 0.01f;
    float farPlane = 500.0f;

    [[nodiscard]] glm::vec3 eye() const;
    [[nodiscard]] glm::mat4 view() const;
    [[nodiscard]] glm::mat4 projection(float aspect) const;
    [[nodiscard]] glm::mat4 viewProjection(float aspect) const;

    /// Centres on an axis-aligned box and backs off far enough to fit it,
    /// adjusting the depth range to suit the new distance.
    void frame(const glm::vec3& boundsMin, const glm::vec3& boundsMax);
};

/// Owns the GLFW window, the OpenGL 3.3 core context, the orbit camera and a
/// debug line pass. Construction is the only failure point: it throws
/// std::runtime_error if the window, the GL loader or the shaders fail.
class Renderer {
public:
    explicit Renderer(const WindowConfig& config = {});
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    [[nodiscard]] bool shouldClose() const;

    /// Pumps events, applies camera input, resizes the viewport and clears.
    void beginFrame();

    /// Presents the frame.
    void endFrame();

    [[nodiscard]] GLFWwindow* window() const { return window_; }

    /// Framebuffer size in pixels (not screen coordinates: these differ on
    /// HiDPI displays).
    [[nodiscard]] int framebufferWidth() const { return fbWidth_; }
    [[nodiscard]] int framebufferHeight() const { return fbHeight_; }
    [[nodiscard]] float aspectRatio() const;

    [[nodiscard]] OrbitCamera& camera() { return camera_; }
    [[nodiscard]] const OrbitCamera& camera() const { return camera_; }

    /// Turn off while ImGui has the mouse, so dragging a slider does not also
    /// spin the camera.
    void setInputEnabled(bool enabled) { inputEnabled_ = enabled; }

    /// Replaces the debug line buffer. Vertices are consumed in pairs.
    void setLines(const std::vector<LineVertex>& vertices);
    void drawLines();

    /// Drawn independently of the line and voxel passes, so the ground stays
    /// visible in every view mode.
    void drawGrid();

    /// Replaces the voxel instance buffer. `voxelSize` is the cube edge length
    /// in world units.
    void setVoxels(const std::vector<VoxelInstance>& instances, float voxelSize);
    void drawVoxels();
    [[nodiscard]] std::size_t voxelInstanceCount() const { return voxelCount_; }

    /// Ground reference grid in the y = 0 plane.
    void setGrid(float halfExtent, int divisions);
    void setShowGrid(bool show) { showGrid_ = show; }
    [[nodiscard]] bool showGrid() const { return showGrid_; }

    void setClearColor(const glm::vec3& rgb) { clearColor_ = rgb; }

    /// Redirects drawing into an offscreen target of the given pixel size and
    /// clears it. The draw calls in between are the ordinary ones, so a capture
    /// is by construction the same image as the window, only larger.
    void beginCapture(int width, int height);

    /// Ends the capture and returns tightly packed RGB rows, top down. Empty if
    /// no capture was running.
    [[nodiscard]] std::vector<unsigned char> endCapture();

private:
    void applyCameraInput();
    void uploadGrid();
    void createVoxelResources();
    void drawLineBuffer(unsigned int vao, std::size_t vertexCount);
    static void onScroll(GLFWwindow* window, double xOffset, double yOffset);

    GLFWwindow* window_ = nullptr;
    int fbWidth_ = 0;
    int fbHeight_ = 0;
    glm::vec3 clearColor_{0.09f, 0.10f, 0.12f};

    OrbitCamera camera_;
    bool inputEnabled_ = true;
    bool orbiting_ = false;
    bool panning_ = false;
    double lastCursorX_ = 0.0;
    double lastCursorY_ = 0.0;
    double pendingScroll_ = 0.0;

    // GLuint/GLsizei kept as plain integers so this header does not drag in glad.
    unsigned int lineProgram_ = 0;
    int viewProjUniform_ = -1;
    unsigned int lineVao_ = 0;
    unsigned int lineVbo_ = 0;
    std::size_t lineVertexCount_ = 0;
    std::size_t lineCapacity_ = 0;
    unsigned int gridVao_ = 0;
    unsigned int gridVbo_ = 0;
    std::size_t gridVertexCount_ = 0;

    unsigned int voxelProgram_ = 0;
    int voxelViewProjUniform_ = -1;
    int voxelSizeUniform_ = -1;
    unsigned int voxelVao_ = 0;
    unsigned int cubeVbo_ = 0;
    unsigned int cubeEbo_ = 0;
    unsigned int voxelInstanceVbo_ = 0;
    std::size_t voxelCount_ = 0;
    std::size_t voxelCapacity_ = 0;
    float voxelSize_ = 1.0f;
    bool showGrid_ = true;
    float gridHalfExtent_ = 2.0f;
    int gridDivisions_ = 20;

    unsigned int captureFbo_ = 0;
    unsigned int captureColor_ = 0;
    unsigned int captureDepth_ = 0;
    int captureWidth_ = 0;
    int captureHeight_ = 0;
    bool capturing_ = false;
    // The viewport size the window was using, restored when a capture ends.
    int savedWidth_ = 0;
    int savedHeight_ = 0;
};

/// Writes tightly packed RGB rows, top down, as a PNG. Returns false if the
/// file could not be written.
bool writePng(const std::string& path, int width, int height,
              const std::vector<unsigned char>& rgb);

}  // namespace plant::viewer
