#ifndef MPVRENDERER_H_
#define MPVRENDERER_H_

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickFramebufferObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QProcess>
#include <memory>

#include "qthelper.hpp"

Q_DECLARE_LOGGING_CATEGORY(wekdeMpv)

namespace mpv
{

struct MpvHandle {
    MpvHandle(mpv_handle* mpv): handle(mpv) {}
    ~MpvHandle() { mpv_terminate_destroy(handle); }
    mpv_handle* handle;
};

class MpvRender;

class MpvObject : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool mute READ mute WRITE setMute)
    Q_PROPERTY(QString logfile READ logfile WRITE setLogfile)
    Q_PROPERTY(int volume READ volume WRITE setVolume)
    Q_PROPERTY(QString hwdec READ hwdec WRITE setHwdec)
    Q_PROPERTY(QString gpuDevice READ gpuDevice WRITE setGpuDevice)

    friend class MpvRender;

public:
    explicit MpvObject(QQuickItem* parent = nullptr);
    virtual ~MpvObject();
    Renderer* createRenderer() const override;

    enum Status
    {
        Stopped,
        Playing,
        Paused,
    };
    Q_ENUM(Status)
    Status  status() const;
    QUrl    source() const;
    bool    mute() const;
    QString logfile() const;
    int     volume() const;

    void    setSource(const QUrl& source);
    void    setMute(const bool& mute);
    void    setLogfile(const QString& logfile);
    void    setVolume(const int& volume);
    QString hwdec() const;
    void    setHwdec(const QString& hwdec);
    QString gpuDevice() const;
    void    setGpuDevice(const QString& device);

public slots:
    void play();
    void pause();
    void stop();

    bool     command(const QVariant& params);
    bool     setProperty(const QString& name, const QVariant& value);
    QVariant getProperty(const QString& name, bool* ok = nullptr) const;
    void     initCallback();
    void     checkAndEmitFirstFrame();

signals:
    void initFinished();
    void statusChanged();
    void sourceChanged();
    void firstFrame();

private slots:
    void onDownscaleFinished(int exitCode, QProcess::ExitStatus status);

private:
    void    loadFile(const QString& path);
    QString resolveVideoSource(const QUrl& source);
    void    startDownscale(const QString& srcFile, const QUrl& sourceUrl, int tw, int th);
    void    launchDownscale();

private:
    bool    inited = false;
    QUrl    m_source;
    Status  m_status = Stopped;
    QString m_hwdec { "auto" };
    QString m_gpuDevice { "auto" };

private:
    mpv_handle*                m_mpv { nullptr };
    std::shared_ptr<MpvHandle> m_shared_mpv { nullptr };
    bool                       m_first_frame { true };

    QProcess* m_downscale { nullptr };
    QUrl      m_downscaleUrl;
    QString   m_downscaleSrc;
    QString   m_downscaleDir;
    QString   m_downscaleHash;
    int       m_downscaleWidth { 0 };
    int       m_downscaleHeight { 0 };
    int       m_downscaleChoiceIndex { 0 };
    QString   m_downscaleHwdec;
    QString   m_srcCodec;
    QString   m_srcPixFmt;
    int       m_srcWidth { 0 };
    int       m_srcHeight { 0 };
};
} // namespace mpv

#endif
