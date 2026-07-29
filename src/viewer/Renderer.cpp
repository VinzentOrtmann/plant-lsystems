#include "viewer/Renderer.h"

#include <glad/gl.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace plant::viewer {
namespace {

void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, description);
}

/// GLFW has no reference counting of its own; the viewer is single-window, so
/// a small counter keeps init/terminate balanced if that ever changes.
int g_glfwUsers = 0;

constexpr const char* kLineVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
uniform mat4 uViewProj;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)";

constexpr const char* kLineFragmentShader = R"(#version 330 core
in vec3 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, 1.0);
}
)";

GLuint compileShader(GLenum stage, const char* source) {
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("shader compilation failed: ") + log);
    }
    return shader;
}

GLuint linkProgram(const char* vertexSource, const char* fragmentSource) {
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    // The shaders are referenced by the program now; flagging them for deletion
    // here means they go away with it.
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE) {
        char log[1024] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        glDeleteProgram(program);
        throw std::runtime_error(std::string("shader link failed: ") + log);
    }
    return program;
}

constexpr const char* kVoxelVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aCorner;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 iCentre;
layout(location = 3) in vec3 iColor;
uniform mat4 uViewProj;
uniform float uVoxelSize;
out vec3 vColor;
out vec3 vNormal;
void main() {
    vColor = iColor;
    vNormal = aNormal;
    gl_Position = uViewProj * vec4(iCentre + aCorner * uVoxelSize, 1.0);
}
)";

constexpr const char* kVoxelFragmentShader = R"(#version 330 core
in vec3 vColor;
in vec3 vNormal;
out vec4 fragColor;
void main() {
    // Two fixed lights, no shadowing: enough to read the shape of a voxel
    // model, and the faces are axis-aligned so flat normals are exact.
    const vec3 keyDirection = normalize(vec3(0.45, 1.0, 0.65));
    const vec3 fillDirection = normalize(vec3(-0.6, 0.2, -0.4));
    vec3 normal = normalize(vNormal);
    float key = max(dot(normal, keyDirection), 0.0);
    float fill = max(dot(normal, fillDirection), 0.0);
    fragColor = vec4(vColor * (0.30 + 0.70 * key + 0.16 * fill), 1.0);
}
)";

struct CubeVertex {
    glm::vec3 corner;
    glm::vec3 normal;
};

/// Unit cube centred on the origin: 24 vertices so each face carries its own
/// normal, plus 36 indices.
void buildCube(std::vector<CubeVertex>& vertices, std::vector<unsigned short>& indices) {
    constexpr glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    // Two in-plane axes per face, ordered so the winding stays counter-clockwise
    // when viewed from outside.
    constexpr glm::vec3 tangents[6] = {{0, 0, -1}, {0, 0, 1}, {0, 0, 1},
                                       {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}};
    constexpr glm::vec3 bitangents[6] = {{0, 1, 0}, {0, 1, 0}, {1, 0, 0},
                                         {1, 0, 0}, {0, 1, 0}, {0, 1, 0}};

    for (int face = 0; face < 6; ++face) {
        const glm::vec3 centre = normals[face] * 0.5f;
        const glm::vec3 u = tangents[face] * 0.5f;
        const glm::vec3 v = bitangents[face] * 0.5f;
        const auto base = static_cast<unsigned short>(vertices.size());

        vertices.push_back({centre - u - v, normals[face]});
        vertices.push_back({centre + u - v, normals[face]});
        vertices.push_back({centre + u + v, normals[face]});
        vertices.push_back({centre - u + v, normals[face]});

        for (int offset : {0, 1, 2, 0, 2, 3}) {
            indices.push_back(static_cast<unsigned short>(base + offset));
        }
    }
}

void describeLineVertexLayout() {
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<void*>(offsetof(LineVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
                          reinterpret_cast<void*>(offsetof(LineVertex, color)));
}

constexpr float kMaxPitch = 1.5f;  // just short of the pole, where lookAt degenerates

}  // namespace

// ---------------------------------------------------------------------------
// OrbitCamera
// ---------------------------------------------------------------------------

glm::vec3 OrbitCamera::eye() const {
    const float cosPitch = std::cos(pitch);
    return target + distance * glm::vec3(cosPitch * std::sin(yaw), std::sin(pitch),
                                         cosPitch * std::cos(yaw));
}

glm::mat4 OrbitCamera::view() const {
    return glm::lookAt(eye(), target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 OrbitCamera::projection(float aspect) const {
    return glm::perspective(glm::radians(fovDegrees), aspect, nearPlane, farPlane);
}

glm::mat4 OrbitCamera::viewProjection(float aspect) const {
    return projection(aspect) * view();
}

void OrbitCamera::frame(const glm::vec3& boundsMin, const glm::vec3& boundsMax) {
    target = 0.5f * (boundsMin + boundsMax);
    const float radius = std::max(0.001f, 0.5f * glm::length(boundsMax - boundsMin));
    // Distance at which a sphere of that radius fills the vertical field of view,
    // with a little margin so the plant does not touch the window edge.
    distance = radius / std::sin(glm::radians(fovDegrees * 0.5f)) * 1.15f;
    nearPlane = std::max(0.001f, distance * 0.001f);
    farPlane = distance * 10.0f + radius * 10.0f;
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

Renderer::Renderer(const WindowConfig& config) {
    glfwSetErrorCallback(glfwErrorCallback);
    if (g_glfwUsers == 0 && glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("glfwInit failed");
    }
    ++g_glfwUsers;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_SAMPLES, 4);

    window_ = glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        if (--g_glfwUsers == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("failed to create GLFW window (OpenGL 3.3 core required)");
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);  // vsync

    // GLFW returns GLFWglproc (void(*)(void)); glad wants void*(*)(const char*).
    const int version = gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress));
    if (version == 0) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        if (--g_glfwUsers == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("gladLoadGL failed: no usable OpenGL 3.3 core context");
    }

    std::printf("[gl] %s | %s | GLSL %s\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));

    // Installed before ImGui initialises its backend, which chains to whatever
    // callback it finds rather than replacing it.
    glfwSetWindowUserPointer(window_, this);
    glfwSetScrollCallback(window_, &Renderer::onScroll);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_LINE_SMOOTH);
    glfwGetFramebufferSize(window_, &fbWidth_, &fbHeight_);

    lineProgram_ = linkProgram(kLineVertexShader, kLineFragmentShader);
    viewProjUniform_ = glGetUniformLocation(lineProgram_, "uViewProj");

    glGenVertexArrays(1, &lineVao_);
    glGenBuffers(1, &lineVbo_);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    describeLineVertexLayout();

    glGenVertexArrays(1, &gridVao_);
    glGenBuffers(1, &gridVbo_);
    glBindVertexArray(gridVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    describeLineVertexLayout();
    glBindVertexArray(0);

    uploadGrid();
    createVoxelResources();
}

void Renderer::createVoxelResources() {
    voxelProgram_ = linkProgram(kVoxelVertexShader, kVoxelFragmentShader);
    voxelViewProjUniform_ = glGetUniformLocation(voxelProgram_, "uViewProj");
    voxelSizeUniform_ = glGetUniformLocation(voxelProgram_, "uVoxelSize");

    std::vector<CubeVertex> vertices;
    std::vector<unsigned short> indices;
    buildCube(vertices, indices);

    glGenVertexArrays(1, &voxelVao_);
    glGenBuffers(1, &cubeVbo_);
    glGenBuffers(1, &cubeEbo_);
    glGenBuffers(1, &voxelInstanceVbo_);

    glBindVertexArray(voxelVao_);

    glBindBuffer(GL_ARRAY_BUFFER, cubeVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(CubeVertex)),
                 vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex),
                          reinterpret_cast<void*>(offsetof(CubeVertex, corner)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex),
                          reinterpret_cast<void*>(offsetof(CubeVertex, normal)));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeEbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned short)), indices.data(),
                 GL_STATIC_DRAW);

    // Divisor 1: these advance once per cube rather than once per vertex.
    glBindBuffer(GL_ARRAY_BUFFER, voxelInstanceVbo_);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelInstance),
                          reinterpret_cast<void*>(offsetof(VoxelInstance, position)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelInstance),
                          reinterpret_cast<void*>(offsetof(VoxelInstance, color)));
    glVertexAttribDivisor(3, 1);

    glBindVertexArray(0);
}

Renderer::~Renderer() {
    if (window_ != nullptr) {
        // Deleting GL objects needs the context that owns them.
        glfwMakeContextCurrent(window_);
        glDeleteBuffers(1, &lineVbo_);
        glDeleteVertexArrays(1, &lineVao_);
        glDeleteBuffers(1, &gridVbo_);
        glDeleteVertexArrays(1, &gridVao_);
        glDeleteProgram(lineProgram_);
        glDeleteBuffers(1, &cubeVbo_);
        glDeleteBuffers(1, &cubeEbo_);
        glDeleteBuffers(1, &voxelInstanceVbo_);
        glDeleteVertexArrays(1, &voxelVao_);
        glDeleteProgram(voxelProgram_);
        if (captureFbo_ != 0) {
            glDeleteFramebuffers(1, &captureFbo_);
            glDeleteTextures(1, &captureColor_);
            glDeleteRenderbuffers(1, &captureDepth_);
        }
        glfwDestroyWindow(window_);
    }
    if (g_glfwUsers > 0 && --g_glfwUsers == 0) {
        glfwTerminate();
    }
}

void Renderer::onScroll(GLFWwindow* window, double /*xOffset*/, double yOffset) {
    if (auto* self = static_cast<Renderer*>(glfwGetWindowUserPointer(window))) {
        self->pendingScroll_ += yOffset;
    }
}

bool Renderer::shouldClose() const {
    return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

float Renderer::aspectRatio() const {
    return fbHeight_ > 0 ? static_cast<float>(fbWidth_) / static_cast<float>(fbHeight_) : 1.0f;
}

void Renderer::applyCameraInput() {
    double cursorX = 0.0;
    double cursorY = 0.0;
    glfwGetCursorPos(window_, &cursorX, &cursorY);
    const double deltaX = cursorX - lastCursorX_;
    const double deltaY = cursorY - lastCursorY_;

    const bool leftDown = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool rightDown = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    if (inputEnabled_ && leftDown) {
        if (orbiting_) {
            camera_.yaw -= static_cast<float>(deltaX) * 0.01f;
            camera_.pitch = std::clamp(camera_.pitch + static_cast<float>(deltaY) * 0.01f,
                                       -kMaxPitch, kMaxPitch);
        }
        orbiting_ = true;
    } else {
        orbiting_ = false;
    }

    if (inputEnabled_ && rightDown) {
        if (panning_) {
            // Pan in the camera plane, scaled by distance so the drag tracks the
            // cursor at any zoom level.
            const glm::vec3 forward = glm::normalize(camera_.target - camera_.eye());
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
            const glm::vec3 up = glm::cross(right, forward);
            const float scale = camera_.distance * 0.002f;
            camera_.target += right * (-static_cast<float>(deltaX) * scale);
            camera_.target += up * (static_cast<float>(deltaY) * scale);
        }
        panning_ = true;
    } else {
        panning_ = false;
    }

    lastCursorX_ = cursorX;
    lastCursorY_ = cursorY;

    // Always consumed, even when input is disabled, so scrolling over a panel
    // does not queue up a zoom that fires later.
    if (inputEnabled_ && pendingScroll_ != 0.0) {
        camera_.distance = std::clamp(
            camera_.distance * std::pow(0.9f, static_cast<float>(pendingScroll_)), 0.01f, 1000.0f);
    }
    pendingScroll_ = 0.0;
}

void Renderer::beginFrame() {
    glfwPollEvents();
    applyCameraInput();
    glfwGetFramebufferSize(window_, &fbWidth_, &fbHeight_);
    glViewport(0, 0, fbWidth_, fbHeight_);
    glClearColor(clearColor_.r, clearColor_.g, clearColor_.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::endFrame() {
    glfwSwapBuffers(window_);
}

void Renderer::setLines(const std::vector<LineVertex>& vertices) {
    lineVertexCount_ = vertices.size();
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    const auto bytes = static_cast<GLsizeiptr>(vertices.size() * sizeof(LineVertex));
    if (vertices.size() > lineCapacity_) {
        glBufferData(GL_ARRAY_BUFFER, bytes, vertices.data(), GL_DYNAMIC_DRAW);
        lineCapacity_ = vertices.size();
    } else if (!vertices.empty()) {
        // Regeneration is per-frame once the sliders land; reuse the allocation.
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, vertices.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::setVoxels(const std::vector<VoxelInstance>& instances, float voxelSize) {
    voxelCount_ = instances.size();
    voxelSize_ = voxelSize;

    glBindBuffer(GL_ARRAY_BUFFER, voxelInstanceVbo_);
    const auto bytes = static_cast<GLsizeiptr>(instances.size() * sizeof(VoxelInstance));
    if (instances.size() > voxelCapacity_) {
        glBufferData(GL_ARRAY_BUFFER, bytes, instances.data(), GL_DYNAMIC_DRAW);
        voxelCapacity_ = instances.size();
    } else if (!instances.empty()) {
        // Regenerating on every slider tick, so reuse the allocation.
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, instances.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::drawVoxels() {
    if (fbHeight_ <= 0 || voxelCount_ == 0) {
        return;
    }
    const glm::mat4 viewProj = camera_.viewProjection(aspectRatio());

    glUseProgram(voxelProgram_);
    glUniformMatrix4fv(voxelViewProjUniform_, 1, GL_FALSE, glm::value_ptr(viewProj));
    glUniform1f(voxelSizeUniform_, voxelSize_);

    glBindVertexArray(voxelVao_);
    glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr,
                            static_cast<GLsizei>(voxelCount_));
    glBindVertexArray(0);
    glUseProgram(0);
}

void Renderer::setGrid(float halfExtent, int divisions) {
    gridHalfExtent_ = std::max(0.0f, halfExtent);
    gridDivisions_ = std::max(1, divisions);
    uploadGrid();
}

void Renderer::uploadGrid() {
    std::vector<LineVertex> vertices;
    vertices.reserve(static_cast<std::size_t>(gridDivisions_ + 1) * 4);

    const glm::vec3 minor(0.22f, 0.24f, 0.28f);
    const glm::vec3 axis(0.38f, 0.40f, 0.46f);
    const float step = 2.0f * gridHalfExtent_ / static_cast<float>(gridDivisions_);

    for (int i = 0; i <= gridDivisions_; ++i) {
        const float offset = -gridHalfExtent_ + step * static_cast<float>(i);
        const bool onAxis = std::fabs(offset) < step * 0.5f;
        const glm::vec3 color = onAxis ? axis : minor;
        vertices.push_back({glm::vec3(offset, 0.0f, -gridHalfExtent_), color});
        vertices.push_back({glm::vec3(offset, 0.0f, gridHalfExtent_), color});
        vertices.push_back({glm::vec3(-gridHalfExtent_, 0.0f, offset), color});
        vertices.push_back({glm::vec3(gridHalfExtent_, 0.0f, offset), color});
    }

    gridVertexCount_ = vertices.size();
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(LineVertex)),
                 vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::drawLineBuffer(unsigned int vao, std::size_t vertexCount) {
    if (fbHeight_ <= 0 || vertexCount == 0) {
        return;  // minimised, or nothing to draw
    }
    const glm::mat4 viewProj = camera_.viewProjection(aspectRatio());

    glUseProgram(lineProgram_);
    glUniformMatrix4fv(viewProjUniform_, 1, GL_FALSE, glm::value_ptr(viewProj));
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertexCount));
    glBindVertexArray(0);
    glUseProgram(0);
}

void Renderer::beginCapture(int width, int height) {
    if (capturing_ || width <= 0 || height <= 0) {
        return;
    }

    if (captureFbo_ == 0) {
        glGenFramebuffers(1, &captureFbo_);
        glGenTextures(1, &captureColor_);
        glGenRenderbuffers(1, &captureDepth_);
    }
    if (width != captureWidth_ || height != captureHeight_) {
        glBindTexture(GL_TEXTURE_2D, captureColor_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindRenderbuffer(GL_RENDERBUFFER, captureDepth_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, captureFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, captureColor_,
                               0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                  captureDepth_);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            std::fprintf(stderr, "[capture] %dx%d target is not supported\n", width, height);
            return;
        }
        captureWidth_ = width;
        captureHeight_ = height;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, captureFbo_);
    // The draw calls read fbWidth_/fbHeight_ for the aspect ratio, so pointing
    // them at the capture size is what keeps the framing identical.
    savedWidth_ = fbWidth_;
    savedHeight_ = fbHeight_;
    fbWidth_ = width;
    fbHeight_ = height;
    capturing_ = true;

    glViewport(0, 0, width, height);
    glClearColor(clearColor_.r, clearColor_.g, clearColor_.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

std::vector<unsigned char> Renderer::endCapture() {
    if (!capturing_) {
        return {};
    }

    std::vector<unsigned char> pixels(static_cast<std::size_t>(captureWidth_) *
                                      static_cast<std::size_t>(captureHeight_) * 3u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, captureWidth_, captureHeight_, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fbWidth_ = savedWidth_;
    fbHeight_ = savedHeight_;
    capturing_ = false;
    glViewport(0, 0, fbWidth_, fbHeight_);

    // OpenGL hands back rows bottom up; image files want them the other way.
    const auto stride = static_cast<std::size_t>(captureWidth_) * 3u;
    for (int row = 0; row < captureHeight_ / 2; ++row) {
        const auto top = static_cast<std::size_t>(row) * stride;
        const auto bottom = static_cast<std::size_t>(captureHeight_ - 1 - row) * stride;
        std::swap_ranges(pixels.begin() + static_cast<std::ptrdiff_t>(top),
                         pixels.begin() + static_cast<std::ptrdiff_t>(top + stride),
                         pixels.begin() + static_cast<std::ptrdiff_t>(bottom));
    }
    return pixels;
}

bool writePng(const std::string& path, int width, int height,
              const std::vector<unsigned char>& rgb) {
    if (width <= 0 || height <= 0 ||
        rgb.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return false;
    }
    return stbi_write_png(path.c_str(), width, height, 3, rgb.data(), width * 3) != 0;
}

void Renderer::drawGrid() {
    if (showGrid_) {
        drawLineBuffer(gridVao_, gridVertexCount_);
    }
}

void Renderer::drawLines() {
    drawLineBuffer(lineVao_, lineVertexCount_);
}

}  // namespace plant::viewer
