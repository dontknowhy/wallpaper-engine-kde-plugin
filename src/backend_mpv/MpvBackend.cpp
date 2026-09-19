#include "MpvBackend.hpp"

#include <QtGlobal>
#include <QtCore/QObject>
#include <QtCore/QDir>
#include <QtGui/QGuiApplication>
#include <QtGui/qguiapplication_platform.h>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>
#include <QtOpenGL/QOpenGLFramebufferObject>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QQuickOpenGLUtils>

#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QDateTime>
#include <QtCore/QSet>
#include <QtCore/QVector>
#include <QtCore/QFile>

#include <clocale>
#include <memory>
#include <atomic>

Q_LOGGING_CATEGORY(wekdeMpv, "wekde.mpv")

#define _Q_DEBUG() qCDebug(wekdeMpv)

using namespace mpv;

/// some api tips
/*
 * Assumes the OpenGL context lives on a certain thread
 * All mpv_render_* APIs have to be assumed to implicitly use the OpenGL context, if you pass a
 * mpv_render_context using the OpenGL backend
 *
 */

namespace
{
void on_mpv_events(void* ctx) { Q_UNUSED(ctx) }

void on_mpv_redraw(void* ctx);

void* get_proc_address_mpv(void* ctx, const char* name) {
    Q_UNUSED(ctx)

    QOpenGLContext* glctx = QOpenGLContext::currentContext();
    if (! glctx) return nullptr;

    return reinterpret_cast<void*>(glctx->getProcAddress(QByteArray(name)));
}

int CreateMpvContex(mpv_handle* mpv, mpv_render_context** mpv_gl) {
    mpv_opengl_init_params gl_init_params { get_proc_address_mpv, nullptr };
    mpv_render_param       params[] { { MPV_RENDER_PARAM_API_TYPE,
                                        const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
                                      { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params },
                                      { MPV_RENDER_PARAM_INVALID, nullptr },
                                      { MPV_RENDER_PARAM_INVALID, nullptr } };

    // The native display is sometimes required for hardware decode interop.
    const QString platform = QGuiApplication::platformName();
    if (platform.contains("xcb")) {
        if (auto* x11App = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()) {
            params[2].type = MPV_RENDER_PARAM_X11_DISPLAY;
            params[2].data = x11App->display();
        }
    } else if (platform.contains("wayland")) {
        if (auto* waylandApp = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) {
            params[2].type = MPV_RENDER_PARAM_WL_DISPLAY;
            params[2].data = waylandApp->display();
        }
    }

    int code = mpv_render_context_create(mpv_gl, mpv, params);
    return code;
}

} // namespace

namespace
{
QString videoCacheDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty()) base = QDir::homePath() + "/.cache";
    return base + "/wescene-renderer/video";
}

QString cacheHashFor(const QString& src, int w, int h) {
    const QByteArray key = (src + "|" + QString::number(w) + "x" + QString::number(h)).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
}

QString localPathOf(const QUrl& source) {
    if (source.isLocalFile()) return source.toLocalFile();
    if (source.scheme().isEmpty()) return source.path();
    return QString();
}

struct VideoInfo {
    int     width { 0 };
    int     height { 0 };
    QString codec;
    QString pixFmt;
};

bool probeVideoInfo(const QString& file, VideoInfo* out) {
    const QString ffprobe = QStandardPaths::findExecutable("ffprobe");
    if (ffprobe.isEmpty()) return false;

    QProcess p;
    p.start(ffprobe, { "-v", "error", "-select_streams", "v:0", "-show_entries",
                       "stream=width,height,codec_name,pix_fmt", "-of", "json", file });
    if (! p.waitForFinished(3000)) {
        p.kill();
        p.waitForFinished();
        return false;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return false;

    const QJsonArray streams =
        QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("streams").toArray();
    if (streams.isEmpty()) return false;
    const QJsonObject s = streams.first().toObject();
    out->width  = s.value("width").toInt();
    out->height = s.value("height").toInt();
    out->codec  = s.value("codec_name").toString();
    out->pixFmt = s.value("pix_fmt").toString();
    return out->width > 0 && out->height > 0;
}

const QSet<QString>& availableEncoders() {
    static QSet<QString> cache;
    static bool          loaded = false;
    if (loaded) return cache;
    loaded = true;

    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) return cache;

    QProcess p;
    p.start(ffmpeg, { "-hide_banner", "-encoders" });
    if (! p.waitForFinished(4000)) {
        p.kill();
        p.waitForFinished();
        return cache;
    }
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n');
    for (const QString& line : lines) {
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.size() >= 2 && parts[0].size() == 6 && parts[0].startsWith('V'))
            cache.insert(parts[1]);
    }
    return cache;
}

QString drmRenderNode() {
    const QFileInfoList nodes =
        QDir("/dev/dri").entryInfoList({ "renderD*" }, QDir::System | QDir::Files, QDir::Name);
    if (nodes.isEmpty()) return QString();
    return nodes.first().absoluteFilePath();
}

enum class HwBackend
{
    None,
    Nvidia,
    Vaapi,
    Qsv,
};

HwBackend backendFromHwdec(const QString& hw) {
    if (hw.contains("nvdec") || hw.contains("cuda")) return HwBackend::Nvidia;
    if (hw.contains("vaapi")) return HwBackend::Vaapi;
    if (hw.contains("qsv")) return HwBackend::Qsv;
    return HwBackend::None;
}

HwBackend detectVendorBackend() {
    const QFileInfoList cards = QDir("/sys/class/drm").entryInfoList(
        { "card[0-9]*" }, QDir::Dirs | QDir::System, QDir::Name);
    for (const QFileInfo& card : cards) {
        QFile f(card.absoluteFilePath() + "/device/vendor");
        if (! f.open(QIODevice::ReadOnly)) continue;
        const QString vendor = QString::fromUtf8(f.readAll()).trimmed();
        if (vendor == "0x10de") return HwBackend::Nvidia;
        if (vendor == "0x1002" || vendor == "0x1022") return HwBackend::Vaapi;
        if (vendor == "0x8086") return HwBackend::Qsv;
    }
    return HwBackend::None;
}

struct EncoderChoice {
    QString     label;
    QStringList globals;
    QStringList decoderArgs;
    QStringList videoArgs;
    QString     scaleName { "scale" };
};

QVector<EncoderChoice> buildEncoderChoices(const QString& hwdec, const VideoInfo& info) {
    const bool hevc = info.codec == "hevc" || info.codec == "h265"
                   || info.pixFmt.contains("10") || info.pixFmt.contains("12");
    const QString       vcodec = hevc ? "hevc" : "h264";
    const QSet<QString>& enc   = availableEncoders();

    HwBackend hw = backendFromHwdec(hwdec);
    if (hw == HwBackend::None && ! hwdec.contains("no") && ! hwdec.contains("software"))
        hw = detectVendorBackend();

    QVector<EncoderChoice> list;

    auto appendHw = [&](HwBackend b) {
        EncoderChoice c;
        if (b == HwBackend::Nvidia) {
            const QString name = vcodec + "_nvenc";
            if (! enc.contains(name)) return;
            c.label       = name;
            c.decoderArgs = { "-hwaccel", "cuda", "-hwaccel_output_format", "cuda" };
            c.videoArgs   = { "-c:v", name, "-preset", "p4", "-tune", "hq", "-b:v", "6M" };
            c.scaleName   = "scale_cuda";
        } else if (b == HwBackend::Vaapi) {
            const QString name = vcodec + "_vaapi";
            if (! enc.contains(name)) return;
            const QString node = drmRenderNode();
            c.label       = name;
            c.decoderArgs = { "-hwaccel", "vaapi", "-hwaccel_output_format", "vaapi" };
            if (! node.isEmpty()) c.globals = { "-vaapi_device", node };
            c.videoArgs = { "-c:v", name, "-b:v", "6M" };
            c.scaleName = "scale_vaapi";
        } else if (b == HwBackend::Qsv) {
            const QString name = vcodec + "_qsv";
            if (! enc.contains(name)) return;
            c.label       = name;
            c.globals     = { "-init_hw_device", "qsv=hw" };
            c.decoderArgs = { "-hwaccel", "qsv", "-hwaccel_output_format", "qsv" };
            c.videoArgs   = { "-c:v", name, "-global_quality", "23" };
            c.scaleName   = "scale_qsv";
        } else {
            return;
        }
        list.append(c);
    };

    appendHw(hw);

    auto appendSw = [&](const QString& codec) {
        if (codec == "hevc" && enc.contains("libx265")) {
            list.append({ "libx265", {}, {},
                          { "-c:v", "libx265", "-preset", "veryfast", "-crf", "25" }, "scale" });
        } else if (enc.contains("libx264")) {
            list.append({ "libx264", {}, {},
                          { "-c:v", "libx264", "-preset", "veryfast", "-crf", "23" }, "scale" });
        }
    };
    appendSw(vcodec);
    if (list.isEmpty()) appendSw("h264");

    return list;
}

void pruneVideoCache() {
    QDir dir(videoCacheDir());
    if (! dir.exists()) return;

    const QFileInfoList entries = dir.entryInfoList(QDir::Files);
    for (const QFileInfo& info : entries) {
        const QString path = info.absoluteFilePath();
        const QString base = info.completeBaseName();
        const QString suffix = info.suffix();

        if (suffix == "part") {
            if (info.lastModified().secsTo(QDateTime::currentDateTime()) > 6 * 3600)
                QFile::remove(path);
            continue;
        }
        if (suffix != "json") continue;

        QFile f(path);
        QByteArray data;
        if (f.open(QIODevice::ReadOnly)) data = f.readAll();
        const QString source = QJsonDocument::fromJson(data).object().value("source").toString();
        const QString video  = info.absolutePath() + "/" + base + ".mp4";
        if (source.isEmpty() || ! QFileInfo::exists(source)) {
            QFile::remove(path);
            QFile::remove(video);
        }
    }

    const QFileInfoList leftovers = dir.entryInfoList({ "*.mp4" }, QDir::Files);
    for (const QFileInfo& info : leftovers) {
        const QString sidecar = info.absolutePath() + "/" + info.completeBaseName() + ".json";
        if (! QFileInfo::exists(sidecar)) QFile::remove(info.absoluteFilePath());
    }
}
} // namespace

// ── MpvObject property/command methods ──────────────────────────────────────

bool MpvObject::command(const QVariant& params) {
    auto* mpv       = m_mpv;
    int   errorCode = mpv::qt::get_error(mpv::qt::command(mpv, params));
    return (errorCode >= 0);
}

bool MpvObject::setProperty(const QString& name, const QVariant& value) {
    auto* mpv       = m_mpv;
    int   errorCode = mpv::qt::get_error(mpv::qt::set_property(mpv, name, value));
    _Q_DEBUG() << "Setting property" << name << "to" << value;
    return (errorCode >= 0);
}

QVariant MpvObject::getProperty(const QString& name, bool* ok) const {
    auto* mpv = m_mpv;
    if (ok) *ok = false;

    if (name.isEmpty()) {
        return QVariant();
    }
    QVariant  result    = mpv::qt::get_property(mpv, name);
    const int errorCode = mpv::qt::get_error(result);
    if (errorCode >= 0) {
        if (ok) {
            *ok = true;
        }
    } else {
        _Q_DEBUG() << "Failed to query property: " << name << "code" << errorCode << " result"
                   << result;
    }
    return result;
}

void MpvObject::initCallback() {
    QUrl temp(m_source.toString());
    m_source.clear();
    inited = true;
    setSource(temp);
    Q_EMIT initFinished();
}

void MpvObject::play() {
    if (status() != Paused) return;
    this->setProperty("pause", false);
}

void MpvObject::pause() {
    if (status() != Playing) return;
    this->setProperty("pause", true);
}

void MpvObject::stop() {
    if (status() == Stopped) return;
    bool result = this->command(QVariantList { "stop" });
    if (result) {
        m_source.clear();
        Q_EMIT sourceChanged();
    }
}

MpvObject::Status MpvObject::status() const {
    const bool stopped = getProperty("idle-active").toBool();
    const bool paused  = getProperty("pause").toBool();
    return stopped ? Stopped : (paused ? Paused : Playing);
}

QUrl MpvObject::source() const { return m_source; }

bool MpvObject::mute() const {
    QString aid = getProperty("aid").toString();
    return aid == "no";
}

QString MpvObject::logfile() const { return getProperty("log-file").toString(); }

int MpvObject::volume() const { return getProperty("volume").toInt(); }

void MpvObject::setMute(const bool& mute) { setProperty("aid", mute ? "no" : "auto"); }

void MpvObject::setVolume(const int& volume) { setProperty("volume", volume); }

QString MpvObject::hwdec() const { return m_hwdec; }

void MpvObject::setHwdec(const QString& hwdec) {
    if (m_hwdec == hwdec) return;
    m_hwdec = hwdec;
    // Use mpv_set_property_string for runtime changes (after mpv_initialize)
    mpv_set_property_string(m_mpv, "hwdec", hwdec.toUtf8().constData());
}

QString MpvObject::gpuDevice() const { return m_gpuDevice; }

void MpvObject::setGpuDevice(const QString& device) {
    if (m_gpuDevice == device) return;
    m_gpuDevice = device;
    // "auto" or an integer index; applies to CUDA/NVDEC decoding.
    mpv_set_property_string(m_mpv, "cuda-decode-device", device.toUtf8().constData());
}

void MpvObject::setLogfile(const QString& logfile) { setProperty("log-file", logfile); }

void MpvObject::setSource(const QUrl& source) {
    if (source.isEmpty()) {
        stop();
        return;
    }
    if (! source.isValid() || (source == m_source)) {
        return;
    }
    if (! inited) {
        m_source = source;
        return;
    }

    const QString toLoad = resolveVideoSource(source);
    const bool    result = this->command(QVariantList { "loadfile", toLoad });
    if (result) {
        m_source = source;
        Q_EMIT sourceChanged();

        m_first_frame = false;
    }
}

void MpvObject::loadFile(const QString& path) {
    this->command(QVariantList { "loadfile", path });
}

QString MpvObject::resolveVideoSource(const QUrl& source) {
    const QString src      = localPathOf(source);
    const QString fallback = src.isEmpty() ? source.url() : QDir::toNativeSeparators(src);
    if (src.isEmpty() || ! QFileInfo::exists(src)) return fallback;

    pruneVideoCache();

    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    int tw = qRound(width() * dpr) & ~1;
    int th = qRound(height() * dpr) & ~1;
    if (tw <= 0 || th <= 0) return fallback;

    VideoInfo info;
    if (! probeVideoInfo(src, &info)) return fallback;
    if (info.width <= tw && info.height <= th) return fallback;
    m_srcCodec   = info.codec;
    m_srcPixFmt  = info.pixFmt;
    m_srcWidth   = info.width;
    m_srcHeight  = info.height;

    const QString hash    = cacheHashFor(src, tw, th);
    const QString dir     = videoCacheDir();
    const QString cached  = dir + "/" + hash + ".mp4";
    const QString sidecar = dir + "/" + hash + ".json";
    QDir().mkpath(dir);

    if (QFileInfo::exists(cached) && QFileInfo::exists(sidecar)) {
        QFile f(sidecar);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            const QFileInfo   info(src);
            if (obj.value("source").toString() == src
                && static_cast<qint64>(obj.value("mtime").toDouble())
                       == info.lastModified().toSecsSinceEpoch()
                && static_cast<qint64>(obj.value("size").toDouble()) == info.size()
                && obj.value("width").toInt() == tw && obj.value("height").toInt() == th) {
                return cached;
            }
        }
    }

    startDownscale(src, source, tw, th);
    return fallback;
}

void MpvObject::startDownscale(const QString& srcFile, const QUrl& sourceUrl, int tw, int th) {
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        _Q_DEBUG() << "ffmpeg not found, skipping video downscale cache";
        return;
    }

    if (m_downscale) {
        if (m_downscale->state() != QProcess::NotRunning) {
            m_downscale->disconnect(this);
            m_downscale->kill();
            m_downscale->waitForFinished(2000);
        }
        m_downscale->deleteLater();
        m_downscale = nullptr;
    }

    const QString dir = videoCacheDir();
    QDir().mkpath(dir);

    m_downscale       = new QProcess(this);
    m_downscaleUrl    = sourceUrl;
    m_downscaleSrc    = srcFile;
    m_downscaleDir    = dir;
    m_downscaleHash   = cacheHashFor(srcFile, tw, th);
    m_downscaleWidth  = tw;
    m_downscaleHeight = th;

    QString hwdec = getProperty("hwdec-current").toString();
    if (hwdec.isEmpty() || hwdec == "auto" || hwdec == "auto-safe") hwdec = m_hwdec;
    m_downscaleHwdec       = hwdec;
    m_downscaleChoiceIndex = 0;

    connect(m_downscale, &QProcess::finished, this, &MpvObject::onDownscaleFinished);
    connect(m_downscale, &QProcess::readyReadStandardError, this, [this]() {
        _Q_DEBUG() << "ffmpeg stderr:" << m_downscale->readAllStandardError().trimmed();
    });
    launchDownscale();
}

void MpvObject::launchDownscale() {
    if (! m_downscale) return;

    VideoInfo info;
    info.codec  = m_srcCodec;
    info.pixFmt = m_srcPixFmt;
    const QVector<EncoderChoice> choices = buildEncoderChoices(m_downscaleHwdec, info);
    if (m_downscaleChoiceIndex >= choices.size()) {
        _Q_DEBUG() << "no usable encoder for video downscale";
        m_downscale->deleteLater();
        m_downscale = nullptr;
        return;
    }
    const EncoderChoice& choice = choices.at(m_downscaleChoiceIndex);

    const QString temp = m_downscaleDir + "/" + m_downscaleHash + ".mp4.part";

    int ow = m_downscaleWidth;
    int oh = m_downscaleHeight;
    if (m_srcWidth > 0 && m_srcHeight > 0) {
        const double ar = double(m_srcWidth) / double(m_srcHeight);
        if (double(m_downscaleWidth) / double(m_downscaleHeight) > ar)
            ow = int(m_downscaleHeight * ar + 0.5);
        else
            oh = int(m_downscaleWidth / ar + 0.5);
        ow &= ~1;
        oh &= ~1;
        if (ow < 2) ow = 2;
        if (oh < 2) oh = 2;
    }
    const QString vf = QString("%1=w=%2:h=%3").arg(choice.scaleName).arg(ow).arg(oh);

    QStringList args { "-y", "-nostdin", "-loglevel", "error" };
    args << choice.globals;
    args << choice.decoderArgs;
    args << "-i" << m_downscaleSrc;
    args << "-vf" << vf;
    if (choice.scaleName == "scale") args << "-pix_fmt" << "yuv420p";
    args << choice.videoArgs;
    args << "-c:a" << "aac" << "-b:a" << "128k";
    args << "-movflags" << "+faststart" << "-f" << "mp4" << temp;

    _Q_DEBUG() << "video downscale using" << choice.label;
    m_downscale->setProgram(QStandardPaths::findExecutable("ffmpeg"));
    m_downscale->setArguments(args);
    m_downscale->start();
}

void MpvObject::onDownscaleFinished(int exitCode, QProcess::ExitStatus status) {
    if (! m_downscale) return;

    const QString temp = m_downscaleDir + "/" + m_downscaleHash + ".mp4.part";
    const bool    ok = status == QProcess::NormalExit && exitCode == 0
                    && QFileInfo::exists(temp) && QFileInfo(temp).size() > 0;

    if (! ok) {
        QFile::remove(temp);
        ++m_downscaleChoiceIndex;
        _Q_DEBUG() << "downscale encoder failed, trying next candidate" << m_downscaleChoiceIndex;
        launchDownscale();
        return;
    }

    if (ok) {
        const QString cached  = m_downscaleDir + "/" + m_downscaleHash + ".mp4";
        const QString sidecar = m_downscaleDir + "/" + m_downscaleHash + ".json";

        QFile::remove(cached);
        if (QFile::rename(temp, cached)) {
            const QFileInfo info(m_downscaleSrc);
            QJsonObject   obj;
            obj["source"] = m_downscaleSrc;
            obj["mtime"]  = static_cast<double>(info.lastModified().toSecsSinceEpoch());
            obj["size"]   = static_cast<double>(info.size());
            obj["width"]  = m_downscaleWidth;
            obj["height"] = m_downscaleHeight;

            QFile f(sidecar);
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));

            _Q_DEBUG() << "video downscale cached:" << cached;
            if (m_source == m_downscaleUrl) {
                loadFile(cached);
                m_first_frame = false;
            }
        }
    } else {
        _Q_DEBUG() << "video downscale failed (exit" << exitCode << ")";
        QFile::remove(temp);
    }

    m_downscale->deleteLater();
    m_downscale = nullptr;
}

// ── MpvRender (render thread) ───────────────────────────────────────────────

namespace mpv
{

class MpvRender : public QObject, public QQuickFramebufferObject::Renderer {
    Q_OBJECT
public:
    MpvRender(std::shared_ptr<MpvHandle> mpv, QQuickWindow* win)
        : m_shared_mpv(mpv), m_mpv(mpv.get()->handle), m_window(win) {}

    virtual ~MpvRender() {
        _Q_DEBUG() << "destroyed";
        mpv::qt::command(m_mpv, QVariantList { "stop" });

        if (m_mpv_context) mpv_render_context_free(m_mpv_context);
        m_mpv_context = nullptr;
    }

    bool Dirty() const { return m_dirty.load(); }
    bool setDirty(bool v) { return m_dirty.exchange(v); }

signals:
    void mpvRedraw();
    void inited();

public slots:
    // render thread
    void renderFrame(QOpenGLFramebufferObject* fbo) {
        mpv_opengl_fbo mpfbo { .fbo             = static_cast<int>(fbo->handle()),
                               .w               = fbo->width(),
                               .h               = fbo->height(),
                               .internal_format = 0 };
        int            flip_y { 0 };

        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo },
            // Flip rendering (needed due to flipped GL coordinate system).
            { MPV_RENDER_PARAM_FLIP_Y, &flip_y },
            { MPV_RENDER_PARAM_INVALID, nullptr }
        };
        mpv_render_context_render(m_mpv_context, params);
    }

    /*
     * This function is called when a new FBO is needed.
     * This happens on the initial frame.
     */
    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override {
        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    /*
     * called as a result of QQuickFramebufferObject::update()
     * called once before the FBO is created
     * only place when it is safe for the renderer and the item to read and write each others
     * members
     */
    void synchronize(QQuickFramebufferObject* item) override {
        MpvObject* mpv_obj = static_cast<MpvObject*>(item);

        if (m_mpv_context == nullptr) {
            if (CreateMpvContex(m_mpv, &m_mpv_context) >= 0) {
                mpv_render_context_set_update_callback(m_mpv_context, on_mpv_redraw, this);
                Q_EMIT this->inited();
            }
        }

        if (Dirty()) {
            mpv_obj->checkAndEmitFirstFrame();
        }
        QQuickOpenGLUtils::resetOpenGLState();
    }

    void render() override {
        if (setDirty(false)) {
            QOpenGLFramebufferObject* fbo = framebufferObject();
            renderFrame(fbo);
            QQuickOpenGLUtils::resetOpenGLState();
        }
    }

private:
    mpv_render_context* m_mpv_context { nullptr };
    mpv_handle*         m_mpv { nullptr };
    QQuickWindow*       m_window { nullptr };

    std::shared_ptr<MpvHandle> m_shared_mpv { nullptr };

    std::atomic<bool> m_dirty { false };
};

} // namespace mpv

namespace
{
void on_mpv_redraw(void* ctx) {
    auto* mpv = static_cast<mpv::MpvRender*>(ctx);
    mpv->setDirty(true);
    Q_EMIT mpv->mpvRedraw();
}
} // namespace

// ── MpvObject construction and renderer creation ────────────────────────────

MpvObject::MpvObject(QQuickItem* parent)
    : QQuickFramebufferObject(parent), m_shared_mpv(std::make_shared<MpvHandle>(mpv_create())) {
    m_mpv = m_shared_mpv->handle;

    if (! m_mpv) {
        _Q_DEBUG() << "could not create mpv context";
        return;
    }

    // All options MUST be set before mpv_initialize
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=info");
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", m_hwdec.toUtf8().constData());
    mpv_set_option_string(m_mpv, "cuda-decode-device", m_gpuDevice.toUtf8().constData());
    mpv_set_option_string(m_mpv, "loop", "inf");
    mpv_set_option_string(m_mpv, "hwdec-extra-frames", "1");
    mpv_set_option_string(m_mpv, "fbo-format", "rgb10");

    if (mpv_initialize(m_mpv) < 0) {
        _Q_DEBUG() << "could not initialize mpv context";
        m_mpv = nullptr;
        return;
    }
}

MpvObject::~MpvObject() {
    if (m_downscale && m_downscale->state() != QProcess::NotRunning) {
        m_downscale->kill();
        m_downscale->waitForFinished(2000);
    }
}

void MpvObject::checkAndEmitFirstFrame() {
    if (! m_first_frame) {
        m_first_frame = true;
        Q_EMIT firstFrame();
    }
}

QQuickFramebufferObject::Renderer* MpvObject::createRenderer() const {
    window()->setPersistentSceneGraph(true);

    auto* render = new MpvRender(m_shared_mpv, window());

    // Use Queued signal to update at gui thread
    connect(render, &MpvRender::mpvRedraw, this, &MpvObject::update, Qt::QueuedConnection);
    connect(render, &MpvRender::inited, this, &MpvObject::initCallback, Qt::QueuedConnection);
    return render;
}

#include "MpvBackend.moc"
