#include "LweSceneBackend.hpp"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>
#include <QtOpenGL/QOpenGLFramebufferObject>
#include <QtQuick/QQuickOpenGLUtils>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>

extern "C" {
#include <linux-wallpaperengine/configuration.h>
#include <linux-wallpaperengine/context.h>
#include <linux-wallpaperengine/project.h>
#include <linux-wallpaperengine/render.h>
}

Q_LOGGING_CATEGORY(wekdeLwe, "wekde.lwe")

#define _LWE_DEBUG() qCDebug(wekdeLwe)
#define _LWE_WARN() qCWarning(wekdeLwe)

namespace lwebackend
{

namespace
{
// The destination rectangle, in normalised device coordinates, is built in the
// vertex shader from gl_VertexID, so no vertex buffers are involved.
constexpr const char* kBlitVertexShader = R"(#version 330 core
uniform vec4 uRectNdc;   // xy = bottom-left corner, zw = size
uniform vec2 uFlip;      // negative components mirror the sampled texture
out vec2 vUV;
void main() {
    int id = gl_VertexID;
    vec2 t = vec2(float(id == 1 || id == 4 || id == 5),
                  float(id == 2 || id == 3 || id == 5));
    gl_Position = vec4(uRectNdc.xy + t * uRectNdc.zw, 0.0, 1.0);

    // linux-wallpaperengine writes the top row of the image into row 0 of its
    // framebuffer. Qt composites the framebuffer object with the matching
    // orientation, so no vertical flip is applied here.
    vec2 uv = t;
    if (uFlip.x < 0.0) uv.x = 1.0 - uv.x;
    if (uFlip.y < 0.0) uv.y = 1.0 - uv.y;
    vUV = uv;
}
)";

constexpr const char* kBlitFragmentShader = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTexture;
void main() {
    // alpha is forced to 1: the engine leaves the alpha channel at 0, and Qt
    // would otherwise blend the whole wallpaper away.
    fragColor = vec4(texture(uTexture, vUV).rgb, 1.0);
}
)";

// The QML side hands paths over as `file://` URLs (the previous scene backend
// declared them as QUrl and called toLocalFile() on them). Accept a plain
// filesystem path as well, since that is what someone loading the component
// directly would reach for.
QString toLocalPath(const QString& value) {
    if (value.isEmpty()) {
        return value;
    }
    const QUrl url(value);
    return url.isLocalFile() ? url.toLocalFile() : value;
}

// userProperties arrives as a JSON object of property name -> value.
QJsonObject parseUserProperties(const QString& json) {
    if (json.isEmpty()) {
        return {};
    }

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }

    return document.object();
}

QString userPropertyValueToString(const QJsonValue& value) {
    switch (value.type()) {
        case QJsonValue::Bool:
            return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        case QJsonValue::Double:
            return QString::number(value.toDouble());
        case QJsonValue::String:
            return value.toString();
        default:
            return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
}

} // namespace

class LweSceneRenderer : public QQuickFramebufferObject::Renderer {
public:
    LweSceneRenderer() = default;
    ~LweSceneRenderer() override;

    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override;
    void synchronize(QQuickFramebufferObject* item) override;
    void render() override;

private:
    // callbacks handed to the engine
    static void*  s_procAddress(void* user, const char* name);
    static float  s_getTime(void* user);
    static double s_mouseX(void* user);
    static double s_mouseY(void* user);
    static int    s_mousePressed(void* user, int button);

    void ensureGl();
    void ensureBlitProgram();
    bool ensureSceneFbo(int width, int height);
    void restart();
    void teardown();
    bool applyUserProperties();
    void blitTo(int targetFbo, const QSize& targetSize);

    QPointer<LweSceneViewer> m_item;
    QOpenGLFunctions*        m_gl { nullptr };

    // Not part of the ES2 subset Qt exposes; resolved through the context.
    void (*m_genVertexArrays) (GLsizei, GLuint*) { nullptr };
    void (*m_bindVertexArray) (GLuint) { nullptr };
    void (*m_deleteVertexArrays) (GLsizei, const GLuint*) { nullptr };

    wp_configuration* m_config { nullptr };
    wp_context*       m_context { nullptr };
    wp_project*       m_project { nullptr };

    wp_gl_proc_address m_glProc {};
    wp_time_counter    m_timeCounter {};
    wp_mouse_input     m_mouseInput {};

    QString m_loadedFolder;
    QString m_loadedAssets;
    QString m_loadedProperties;

    GLuint m_sceneFbo { 0 };
    GLuint m_sceneTex { 0 };
    int    m_sceneW { 0 };
    int    m_sceneH { 0 };

    GLuint m_blitProgram { 0 };
    GLuint m_blitVao { 0 };
    GLint  m_uRectNdc { -1 };
    GLint  m_uFlip { -1 };
    GLint  m_uTexture { -1 };

    std::chrono::steady_clock::time_point m_lastTick {};
    double                                m_clock { 0.0 };
    bool                                  m_firstFrameDone { false };
    std::atomic<bool>                     m_firstFramePending { false };

    // shadow copy of the item's state, refreshed in synchronize()
    QString m_folder;
    QString m_assets;
    QString m_props;
    int     m_fillMode { LweSceneViewer::ASPECTCROP };
    double  m_speed { 1.0 };
    bool    m_paused { false };
    bool    m_mirror { false };
    QSizeF  m_itemSize { 0, 0 };
    QPointF m_mouse { -1, -1 };
    bool    m_mouseCentered { true };
    bool    m_leftPressed { false };
    bool    m_rightPressed { false };
};

// ---------------------------------------------------------------------------
// engine callbacks
// ---------------------------------------------------------------------------

void* LweSceneRenderer::s_procAddress(void* user, const char* name) {
    Q_UNUSED(user)
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(context->getProcAddress(name));
}

float LweSceneRenderer::s_getTime(void* user) {
    return static_cast<float>(static_cast<LweSceneRenderer*>(user)->m_clock);
}

double LweSceneRenderer::s_mouseX(void* user) {
    const auto* self = static_cast<LweSceneRenderer*>(user);
    if (self->m_mouseCentered || self->m_itemSize.width() <= 0) {
        return self->m_sceneW / 2.0;
    }
    const double t = std::clamp(self->m_mouse.x() / self->m_itemSize.width(), 0.0, 1.0);
    return t * self->m_sceneW;
}

double LweSceneRenderer::s_mouseY(void* user) {
    const auto* self = static_cast<LweSceneRenderer*>(user);
    if (self->m_mouseCentered || self->m_itemSize.height() <= 0) {
        return self->m_sceneH / 2.0;
    }
    // The item is top-left based, the engine bottom-left based.
    const double t = std::clamp(self->m_mouse.y() / self->m_itemSize.height(), 0.0, 1.0);
    return (1.0 - t) * self->m_sceneH;
}

int LweSceneRenderer::s_mousePressed(void* user, int button) {
    const auto* self = static_cast<LweSceneRenderer*>(user);
    if ((button & WP_MOUSE_INPUT_BUTTON_LEFT) != 0 && self->m_leftPressed) {
        return 1;
    }
    if ((button & WP_MOUSE_INPUT_BUTTON_RIGHT) != 0 && self->m_rightPressed) {
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// renderer
// ---------------------------------------------------------------------------

LweSceneRenderer::~LweSceneRenderer() {
    // The renderer is destroyed on the render thread. If the context is already
    // gone there is nothing we can (or need to) clean up.
    if (m_gl == nullptr) {
        return;
    }

    teardown();

    if (m_blitProgram != 0) {
        m_gl->glDeleteProgram(m_blitProgram);
    }
    if (m_blitVao != 0 && m_deleteVertexArrays != nullptr) {
        m_deleteVertexArrays(1, &m_blitVao);
    }
}

QOpenGLFramebufferObject* LweSceneRenderer::createFramebufferObject(const QSize& size) {
    return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
}

void LweSceneRenderer::synchronize(QQuickFramebufferObject* item) {
    auto* viewer = static_cast<LweSceneViewer*>(item);
    m_item = viewer;

    m_folder = viewer->m_source.isLocalFile() ? viewer->m_source.toLocalFile() : viewer->m_source.toString();
    while (m_folder.endsWith(QLatin1Char('/'))) {
        m_folder.chop(1);
    }
    // The plugin points `source` at the wallpaper's scene.json - it builds that
    // path from the workshop folder even though the file usually does not exist.
    // linux-wallpaperengine loads a project from a directory (project.json,
    // scene.pkg), so fall back to the containing directory for anything that is
    // not a directory itself.
    if (!m_folder.isEmpty()) {
        const QFileInfo folderInfo(m_folder);
        if (!folderInfo.isDir()) {
            const QString containingDirectory = folderInfo.absolutePath();
            if (!containingDirectory.isEmpty() && containingDirectory != m_folder) {
                m_folder = containingDirectory;
            }
        }
    }
    m_assets = toLocalPath(viewer->m_assets);
    m_props = viewer->m_userProperties;

    m_fillMode = viewer->m_fillMode;
    m_speed = viewer->m_speed;

    if (m_paused && !viewer->m_paused) {
        // Resuming: restart the clock from the next frame, so it does not carry
        // the time spent paused as a delta.
        m_lastTick = {};
    }

    m_paused = viewer->m_paused;
    m_mirror = viewer->m_mirrorScene;
    m_itemSize = QSizeF(viewer->width(), viewer->height());
    m_mouse = viewer->m_mousePos;
    m_mouseCentered = viewer->m_mouseCentered;
    m_leftPressed = viewer->m_leftPressed;
    m_rightPressed = viewer->m_rightPressed;
}

void LweSceneRenderer::ensureGl() {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr || m_gl != nullptr) {
        return;
    }

    m_gl = context->functions();
    m_genVertexArrays = reinterpret_cast<decltype(m_genVertexArrays)>(context->getProcAddress("glGenVertexArrays"));
    m_bindVertexArray = reinterpret_cast<decltype(m_bindVertexArray)>(context->getProcAddress("glBindVertexArray"));
    m_deleteVertexArrays =
        reinterpret_cast<decltype(m_deleteVertexArrays)>(context->getProcAddress("glDeleteVertexArrays"));
}

void LweSceneRenderer::ensureBlitProgram() {
    if (m_blitProgram != 0) {
        return;
    }

    const auto compile = [this](GLenum type, const char* source) -> GLuint {
        const GLuint shader = m_gl->glCreateShader(type);
        m_gl->glShaderSource(shader, 1, &source, nullptr);
        m_gl->glCompileShader(shader);

        GLint ok = GL_FALSE;
        m_gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok == GL_FALSE) {
            char log[2048] = {};
            m_gl->glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            _LWE_WARN() << "blit shader failed to compile:" << log;
            m_gl->glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    const GLuint vs = compile(GL_VERTEX_SHADER, kBlitVertexShader);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kBlitFragmentShader);
    if (vs == 0 || fs == 0) {
        return;
    }

    m_blitProgram = m_gl->glCreateProgram();
    m_gl->glAttachShader(m_blitProgram, vs);
    m_gl->glAttachShader(m_blitProgram, fs);
    m_gl->glLinkProgram(m_blitProgram);
    m_gl->glDeleteShader(vs);
    m_gl->glDeleteShader(fs);

    GLint ok = GL_FALSE;
    m_gl->glGetProgramiv(m_blitProgram, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE) {
        char log[2048] = {};
        m_gl->glGetProgramInfoLog(m_blitProgram, sizeof(log) - 1, nullptr, log);
        _LWE_WARN() << "blit program failed to link:" << log;
        m_gl->glDeleteProgram(m_blitProgram);
        m_blitProgram = 0;
        return;
    }

    m_uRectNdc = m_gl->glGetUniformLocation(m_blitProgram, "uRectNdc");
    m_uFlip = m_gl->glGetUniformLocation(m_blitProgram, "uFlip");
    m_uTexture = m_gl->glGetUniformLocation(m_blitProgram, "uTexture");

    // A non-zero VAO is required before drawing in a core profile context even
    // when no attributes are enabled.
    if (m_genVertexArrays != nullptr && m_bindVertexArray != nullptr) {
        m_genVertexArrays(1, &m_blitVao);
    }
}

bool LweSceneRenderer::ensureSceneFbo(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);

    if (m_sceneFbo != 0 && m_sceneW == width && m_sceneH == height) {
        return true;
    }

    if (m_sceneFbo != 0) {
        m_gl->glDeleteFramebuffers(1, &m_sceneFbo);
        m_sceneFbo = 0;
    }
    if (m_sceneTex != 0) {
        m_gl->glDeleteTextures(1, &m_sceneTex);
        m_sceneTex = 0;
    }

    m_gl->glGenTextures(1, &m_sceneTex);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_sceneTex);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    m_gl->glGenFramebuffers(1, &m_sceneFbo);
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_sceneFbo);
    m_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sceneTex, 0);
    const GLenum status = m_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        _LWE_WARN() << "scene framebuffer is incomplete:" << status;
        return false;
    }

    m_sceneW = width;
    m_sceneH = height;
    return true;
}

void LweSceneRenderer::restart() {
    teardown();

    m_loadedFolder = m_folder;
    m_loadedAssets = m_assets;
    m_loadedProperties = m_props;

    m_config = wp_config_create();
    if (m_config == nullptr) {
        _LWE_WARN() << "could not create an engine configuration";
        return;
    }

    bool assetsDirectorySet = false;
    if (!m_assets.isEmpty()) {
        assetsDirectorySet = wp_config_set_assets_dir(m_config, m_assets.toUtf8().constData());
        if (!assetsDirectorySet) {
            _LWE_WARN() << "assets directory rejected:" << m_assets;
        }
    }
    if (!assetsDirectorySet) {
        // Nothing usable was handed over; fall back to the standard Steam
        // locations rather than loading a project without any assets.
        wp_config_detect_steam_dir(m_config);
    }

    // userProperties arrives as a JSON object of property name -> value
    const QJsonObject properties = parseUserProperties(m_props);
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        const QByteArray name = it.key().toUtf8();
        const QByteArray value = userPropertyValueToString(it.value()).toUtf8();
        wp_config_set_property(m_config, name.constData(), value.constData());
    }

    // Scene audio is not routed by this component.
    wp_config_enable_audio(m_config, false);

    m_context = wp_context_create(m_config);
    if (m_context == nullptr) {
        _LWE_WARN() << "could not create an engine context";
        teardown();
        return;
    }

    m_timeCounter = { this, &LweSceneRenderer::s_getTime };
    m_glProc = { this, &LweSceneRenderer::s_procAddress };
    m_mouseInput = { this, &LweSceneRenderer::s_mouseX, &LweSceneRenderer::s_mouseY, &LweSceneRenderer::s_mousePressed };

    wp_context_set_gl_proc_address(m_context, &m_glProc);
    wp_context_set_time_counter(m_context, &m_timeCounter);

    m_project = wp_project_load_folder(m_context, &m_mouseInput, m_folder.toUtf8().constData());
    if (m_project == nullptr) {
        _LWE_WARN() << "could not load project:" << m_folder;
        teardown();
        return;
    }

    // The render context, the wallpaper and glad's function table are all
    // created on the first render, and only then are the real dimensions known.
    // Render into a 1x1 target until the project reports a usable size; nothing
    // is drawn into the window in the meantime.
    ensureSceneFbo(1, 1);
    wp_project_set_output_framebuffer(m_project, m_sceneFbo);

    m_clock = 0.0;
    m_lastTick = {};
    m_firstFrameDone = false;
    m_firstFramePending = false;

    _LWE_DEBUG() << "loaded" << m_loadedFolder;
}

void LweSceneRenderer::teardown() {
    if (m_project != nullptr) {
        wp_project_destroy(m_project);
        m_project = nullptr;
    }
    if (m_context != nullptr) {
        wp_context_destroy(m_context);
        m_context = nullptr;
    }
    if (m_config != nullptr) {
        wp_config_destroy(m_config);
        m_config = nullptr;
    }

    if (m_gl != nullptr) {
        if (m_sceneFbo != 0) {
            m_gl->glDeleteFramebuffers(1, &m_sceneFbo);
            m_sceneFbo = 0;
        }
        if (m_sceneTex != 0) {
            m_gl->glDeleteTextures(1, &m_sceneTex);
            m_sceneTex = 0;
        }
    }

    m_sceneW = 0;
    m_sceneH = 0;
    m_loadedFolder.clear();
    m_loadedAssets.clear();
    m_loadedProperties.clear();
}

bool LweSceneRenderer::applyUserProperties() {
    const QJsonObject properties = parseUserProperties(m_props);

    if (properties.isEmpty()) {
        // Properties cannot be unset on a running project, and a reload is what
        // resets them to their default values.
        return parseUserProperties(m_loadedProperties).isEmpty();
    }

    for (auto it = properties.begin(); it != properties.end(); ++it) {
        const QByteArray name = it.key().toUtf8();
        const QByteArray value = userPropertyValueToString(it.value()).toUtf8();

        if (!wp_project_property_set_value_by_name(m_project, name.constData(), value.constData())) {
            // The engine does not know this property, and reloading the project
            // would not change that, so carry on with the other values.
            _LWE_DEBUG() << "scene property is unknown to the engine:" << it.key();
            continue;
        }

        // Keep the configuration in sync so a later reload keeps the value too.
        wp_config_set_property(m_config, name.constData(), value.constData());
    }

    return true;
}

void LweSceneRenderer::blitTo(int targetFbo, const QSize& targetSize) {
    if (m_blitProgram == 0 || m_sceneW <= 0 || m_sceneH <= 0 || targetSize.isEmpty()) {
        return;
    }

    const double targetW = targetSize.width();
    const double targetH = targetSize.height();

    double destX = 0.0;
    double destY = 0.0;
    double destW = targetW;
    double destH = targetH;

    if (m_fillMode == LweSceneViewer::ASPECTFIT || m_fillMode == LweSceneViewer::ASPECTCROP) {
        const double fit = std::min(targetW / m_sceneW, targetH / m_sceneH);
        const double crop = std::max(targetW / m_sceneW, targetH / m_sceneH);
        const double scale = m_fillMode == LweSceneViewer::ASPECTFIT ? fit : crop;
        destW = m_sceneW * scale;
        destH = m_sceneH * scale;
        destX = (targetW - destW) / 2.0;
        destY = (targetH - destH) / 2.0;
    }

    const float rectX = static_cast<float>(2.0 * destX / targetW - 1.0);
    const float rectY = static_cast<float>(2.0 * destY / targetH - 1.0);
    const float rectW = static_cast<float>(2.0 * destW / targetW);
    const float rectH = static_cast<float>(2.0 * destH / targetH);

    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(targetFbo));
    m_gl->glViewport(0, 0, targetSize.width(), targetSize.height());
    m_gl->glDisable(GL_BLEND);
    m_gl->glDisable(GL_DEPTH_TEST);
    m_gl->glDisable(GL_CULL_FACE);
    m_gl->glDisable(GL_SCISSOR_TEST);

    // Qt's scene graph renders with the alpha write mask disabled, and it also
    // makes glClear() and every draw call ignore the alpha channel. The engine
    // leaves alpha at 0, and Qt composites this item with premultiplied "over"
    // blending, so without a written alpha the desktop behind the wallpaper
    // would be added on top of it (washed out, tinted by the background).
    // Force the alpha channel through and restore Qt's mask afterwards.
    GLboolean previousMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    m_gl->glGetBooleanv(GL_COLOR_WRITEMASK, previousMask);
    m_gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    m_gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    m_gl->glClear(GL_COLOR_BUFFER_BIT);

    m_gl->glUseProgram(m_blitProgram);
    m_gl->glActiveTexture(GL_TEXTURE0);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_sceneTex);
    m_gl->glUniform1i(m_uTexture, 0);
    m_gl->glUniform4f(m_uRectNdc, rectX, rectY, rectW, rectH);
    m_gl->glUniform2f(m_uFlip, m_mirror ? -1.0f : 1.0f, 1.0f);

    if (m_bindVertexArray != nullptr) {
        m_bindVertexArray(m_blitVao);
    }
    m_gl->glDrawArrays(GL_TRIANGLES, 0, 6);
    if (m_bindVertexArray != nullptr) {
        m_bindVertexArray(0);
    }

    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_gl->glColorMask(previousMask[0], previousMask[1], previousMask[2], previousMask[3]);
}

void LweSceneRenderer::render() {
    ensureGl();
    if (m_gl == nullptr || m_folder.isEmpty()) {
        return;
    }

    ensureBlitProgram();

    // Optional frame timing diagnostic: with WEKDE_LWE_TIMING=1 in the
    // environment every 30th frame logs how long the render takes (engine render
    // including the mpv video textures, the blit into Qt's target, and the
    // glFinish that waits for the GPU). Used to check how much of the
    // compositor's frame budget a wallpaper actually takes.
    static const bool timingEnabled = qEnvironmentVariableIsSet("WEKDE_LWE_TIMING");
    QElapsedTimer frameClock;
    static double engineSumMs = 0.0;
    static double blitSumMs = 0.0;

    if (timingEnabled) {
        frameClock.start();
    }

    if (m_project == nullptr || m_folder != m_loadedFolder || m_assets != m_loadedAssets
        || m_props != m_loadedProperties) {
        // The engine reads scene properties from the project on every frame, so
        // changed values can be applied in place. Everything else (another
        // wallpaper, another assets directory) needs a full reload.
        const bool propertiesOnly = m_project != nullptr && m_folder == m_loadedFolder
            && m_assets == m_loadedAssets && m_props != m_loadedProperties;

        if (propertiesOnly && applyUserProperties()) {
            m_loadedProperties = m_props;
        } else {
            restart();
        }
    }
    if (m_project == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_lastTick.time_since_epoch().count() == 0) {
        m_lastTick = now;
    }
    double delta = std::chrono::duration<double>(now - m_lastTick).count();
    m_lastTick = now;
    if (m_paused) {
        delta = 0.0;
    }
    // Never let a stall (window hidden, suspended, ...) jump the animation.
    delta = std::clamp(delta, 0.0, 0.25);
    m_clock += delta * m_speed;

    // Scene videos are decoded by mpv in real time, so the playback speed has to
    // be forwarded to it for them to follow the speed setting. The engine's own
    // clock (above) only drives shader animations and particles.
    wp_config_set_playback_rate(m_config, m_speed);
    // Pausing the wallpaper has to reach mpv as well, or the video keeps
    // decoding while paused and jumps forward by the paused time on resume.
    wp_config_set_playback_paused(m_config, m_paused);

    wp_render_update_time(m_context);
    wp_render_frame(m_project);
    if (timingEnabled) {
        engineSumMs += frameClock.nsecsElapsed() / 1e6;
    }

    // The first render creates the wallpaper, and its viewport size is only
    // known from that point on. Until then the engine draws into a 1x1 target.
    bool justResized = false;
    if (m_sceneW <= 1 || m_sceneH <= 1) {
        const int width = wp_project_get_width(m_project);
        const int height = wp_project_get_height(m_project);
        if (width > 1 && height > 1 && ensureSceneFbo(width, height)) {
            wp_project_set_output_framebuffer(m_project, m_sceneFbo);
            m_firstFramePending = true;
            justResized = true;
            _LWE_DEBUG() << "project viewport is" << width << "x" << height;
        }
    }

    QOpenGLFramebufferObject* fbo = framebufferObject();
    if (fbo != nullptr) {
        if (m_sceneW > 1 && m_sceneH > 1 && !justResized) {
            blitTo(fbo->handle(), fbo->size());
        } else {
            // Nothing usable to show yet; keep Qt's target clean.
            m_gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo->handle()));
            m_gl->glViewport(0, 0, fbo->width(), fbo->height());
            m_gl->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            m_gl->glClear(GL_COLOR_BUFFER_BIT);
            m_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }
    if (timingEnabled) {
        blitSumMs += frameClock.nsecsElapsed() / 1e6;
    }

    if (m_firstFramePending.exchange(false)) {
        m_firstFrameDone = true;
        QPointer<LweSceneViewer> item = m_item;
        if (item != nullptr) {
            QMetaObject::invokeMethod(
                item.data(),
                [item]() {
                    if (item != nullptr) {
                        Q_EMIT item->firstFrame();
                    }
                },
                Qt::QueuedConnection
            );
        }
    }

    if (timingEnabled) {
        m_gl->glFinish();
        static QElapsedTimer sampleWindow;
        static double total = 0.0;
        static double worst = 0.0;
        static int frames = 0;

        if (!sampleWindow.isValid()) {
            sampleWindow.start();
        }

        const double ms = frameClock.nsecsElapsed() / 1e6;
        total += ms;
        worst = std::max(worst, ms);
        ++frames;
        if (frames >= 30) {
            const double avg = total / frames;
            const double blit = blitSumMs / frames;
            _LWE_DEBUG() << "frame cost: avg" << avg << "ms, worst" << worst << "ms,"
                         << (1000.0 * frames / sampleWindow.elapsed()) << "render/s"
                         << "| engine" << (engineSumMs / frames) << "ms, blit" << (blit - engineSumMs / frames)
                         << "ms, glFinish" << (avg - blit) << "ms";
            total = 0.0;
            worst = 0.0;
            frames = 0;
            engineSumMs = 0.0;
            blitSumMs = 0.0;
            sampleWindow.restart();
        }
    }

    QQuickOpenGLUtils::resetOpenGLState();
}

// ---------------------------------------------------------------------------
// QML item
// ---------------------------------------------------------------------------

LweSceneViewer::LweSceneViewer(QQuickItem* parent) : QQuickFramebufferObject(parent) {
    setFlag(QQuickItem::ItemHasContents, true);

    // QGuiApplication installs the environment locale; the engine's parsers and
    // its mpv-based video texture expect the C locale for numbers.
    std::setlocale(LC_NUMERIC, "C");

    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(1000 / std::max(1, m_fps));
    connect(&m_timer, &QTimer::timeout, this, &LweSceneViewer::tick);
}

LweSceneViewer::~LweSceneViewer() = default;

QQuickFramebufferObject::Renderer* LweSceneViewer::createRenderer() const {
    return new LweSceneRenderer();
}

void LweSceneViewer::tick() {
    update();
}

QUrl LweSceneViewer::source() const { return m_source; }
QString LweSceneViewer::assets() const { return m_assets; }
int LweSceneViewer::fps() const { return m_fps; }
int LweSceneViewer::fillMode() const { return m_fillMode; }
double LweSceneViewer::speed() const { return m_speed; }
float LweSceneViewer::volume() const { return m_volume; }
bool LweSceneViewer::muted() const { return m_muted; }
bool LweSceneViewer::cachePasses() const { return m_cachePasses; }
bool LweSceneViewer::shareGpu() const { return m_shareGpu; }
bool LweSceneViewer::mirrorScene() const { return m_mirrorScene; }
QString LweSceneViewer::userProperties() const { return m_userProperties; }

void LweSceneViewer::setSource(const QUrl& source) {
    if (m_source == source) {
        return;
    }
    m_source = source;
    m_mouseCentered = true;
    m_mousePos = QPointF(-1, -1);
    if (!m_source.isEmpty() && !m_paused) {
        m_timer.start(1000 / std::max(1, m_fps));
    } else if (m_source.isEmpty()) {
        m_timer.stop();
    }
    update();
    Q_EMIT sourceChanged();
}

void LweSceneViewer::setAssets(const QString& assets) {
    if (m_assets == assets) {
        return;
    }
    m_assets = assets;
    update();
    Q_EMIT assetsChanged();
}

void LweSceneViewer::setFps(int fps) {
    if (m_fps == fps) {
        return;
    }
    m_fps = fps;
    if (m_timer.isActive()) {
        m_timer.start(1000 / std::max(1, m_fps));
    }
    Q_EMIT fpsChanged();
}

void LweSceneViewer::setFillMode(int fillMode) {
    if (m_fillMode == fillMode) {
        return;
    }
    m_fillMode = fillMode;
    update();
    Q_EMIT fillModeChanged();
}

void LweSceneViewer::setSpeed(double speed) {
    if (m_speed == speed) {
        return;
    }
    m_speed = speed;
    Q_EMIT speedChanged();
}

void LweSceneViewer::setVolume(float volume) {
    if (m_volume == volume) {
        return;
    }
    m_volume = volume;
    Q_EMIT volumeChanged();
}

void LweSceneViewer::setMuted(bool muted) {
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    Q_EMIT mutedChanged();
}

void LweSceneViewer::setCachePasses(bool value) {
    m_cachePasses = value;
}

void LweSceneViewer::setShareGpu(bool value) {
    m_shareGpu = value;
}

void LweSceneViewer::setMirrorScene(bool value) {
    if (m_mirrorScene == value) {
        return;
    }
    m_mirrorScene = value;
    update();
    Q_EMIT mirrorSceneChanged();
}

void LweSceneViewer::setUserProperties(const QString& userProperties) {
    if (m_userProperties == userProperties) {
        return;
    }
    m_userProperties = userProperties;
    update();
    Q_EMIT userPropertiesChanged();
}

void LweSceneViewer::setAcceptMouse(bool accept) {
    setAcceptedMouseButtons(accept ? Qt::LeftButton | Qt::RightButton : Qt::NoButton);
}

void LweSceneViewer::setAcceptHover(bool accept) {
    setAcceptHoverEvents(accept);
}

void LweSceneViewer::centerPointer() {
    m_mouseCentered = true;
    m_mousePos = QPointF(-1, -1);
    update();
}

void LweSceneViewer::play() {
    m_paused = false;
    if (!m_source.isEmpty()) {
        m_timer.start(1000 / std::max(1, m_fps));
    }
    update();
}

void LweSceneViewer::pause() {
    m_paused = true;
    m_timer.stop();
    update();
}

void LweSceneViewer::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_leftPressed = true;
    } else if (event->button() == Qt::RightButton) {
        m_rightPressed = true;
    }
    m_mouseCentered = false;
    m_mousePos = event->position();
    event->accept();
}

void LweSceneViewer::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_leftPressed = false;
    } else if (event->button() == Qt::RightButton) {
        m_rightPressed = false;
    }
    m_mousePos = event->position();
    event->accept();
}

void LweSceneViewer::mouseMoveEvent(QMouseEvent* event) {
    m_mouseCentered = false;
    m_mousePos = event->position();
}

void LweSceneViewer::hoverMoveEvent(QHoverEvent* event) {
    m_mouseCentered = false;
    m_mousePos = event->position();
}

void LweSceneViewer::hoverLeaveEvent(QHoverEvent* event) {
    Q_UNUSED(event)
}

} // namespace lwebackend
