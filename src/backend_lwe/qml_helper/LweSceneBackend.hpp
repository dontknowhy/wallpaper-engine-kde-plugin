#pragma once

#include <QtQuick/QQuickFramebufferObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QHoverEvent>
#include <QtGui/QMouseEvent>

Q_DECLARE_LOGGING_CATEGORY(wekdeLwe)

namespace lwebackend
{

class LweSceneRenderer;

/**
 * Drop-in replacement for the old `SceneViewer` QML type, backed by
 * Almamu/linux-wallpaperengine's core renderer (C API).
 *
 * The lwe project renders into an offscreen FBO owned by this component; the
 * renderer then blits that texture into the framebuffer Qt gives us, applying
 * the requested fill mode on the way. Everything still runs on Qt's render
 * thread for now (T1); moving the scene render to its own shared context is a
 * later step.
 */
class LweSceneViewer : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString assets READ assets WRITE setAssets NOTIFY assetsChanged)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged)
    Q_PROPERTY(int fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(float volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(bool cachePasses READ cachePasses WRITE setCachePasses)
    Q_PROPERTY(bool shareGpu READ shareGpu WRITE setShareGpu)
    Q_PROPERTY(bool mirrorScene READ mirrorScene WRITE setMirrorScene NOTIFY mirrorSceneChanged)
    Q_PROPERTY(QString userProperties READ userProperties WRITE setUserProperties NOTIFY userPropertiesChanged)

public:
    enum FillMode { STRETCH = 0, ASPECTFIT = 1, ASPECTCROP = 2 };
    Q_ENUM(FillMode)

    explicit LweSceneViewer(QQuickItem* parent = nullptr);
    ~LweSceneViewer() override;

    Renderer* createRenderer() const override;

    QUrl    source() const;
    QString assets() const;
    int     fps() const;
    int     fillMode() const;
    double  speed() const;
    float   volume() const;
    bool    muted() const;
    bool    cachePasses() const;
    bool    shareGpu() const;
    bool    mirrorScene() const;
    QString userProperties() const;

    void setSource(const QUrl& source);
    void setAssets(const QString& assets);
    void setFps(int fps);
    void setFillMode(int fillMode);
    void setSpeed(double speed);
    void setVolume(float volume);
    void setMuted(bool muted);
    void setCachePasses(bool value);
    void setShareGpu(bool value);
    void setMirrorScene(bool value);
    void setUserProperties(const QString& userProperties);

    Q_INVOKABLE void setAcceptMouse(bool accept);
    Q_INVOKABLE void setAcceptHover(bool accept);
    Q_INVOKABLE void centerPointer();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();

signals:
    void sourceChanged();
    void assetsChanged();
    void fpsChanged();
    void fillModeChanged();
    void speedChanged();
    void volumeChanged();
    void mutedChanged();
    void mirrorSceneChanged();
    void userPropertiesChanged();

    void firstFrame();

private slots:
    void tick();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

private:
    friend class LweSceneRenderer;

    QUrl    m_source;
    QString m_assets;
    int     m_fps { 30 };
    int     m_fillMode { ASPECTCROP };
    double  m_speed { 1.0 };
    float   m_volume { 1.0f };
    bool    m_muted { true };
    bool    m_cachePasses { false };
    bool    m_shareGpu { false };
    bool    m_mirrorScene { false };
    QString m_userProperties;

    QTimer m_timer;
    bool   m_paused { false };

    // Mouse state, in item-local pixels with a top-left origin. The renderer
    // converts it to the wallpaper's own pixel space (bottom-left origin).
    QPointF m_mousePos { -1, -1 };
    bool    m_mouseCentered { true };
    bool    m_leftPressed { false };
    bool    m_rightPressed { false };
};

} // namespace lwebackend
