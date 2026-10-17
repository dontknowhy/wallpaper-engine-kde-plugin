#pragma once
#include <QObject>

namespace wekde
{

class PluginInfo : public QObject {
    Q_OBJECT
    // Q_PROPERTY(READ WRITE NOTIFY)
    Q_PROPERTY(QUrl cache_path READ cache_path NOTIFY cache_pathChanged)
    Q_PROPERTY(QUrl version READ version NOTIFY versionChanged)
    Q_PROPERTY(bool hasLweScene READ hasLweScene CONSTANT)

public:
    PluginInfo(QObject* parent = nullptr);
    virtual ~PluginInfo();

    QUrl cache_path() const;

    QString version() const { return "0.6.0"; };

    // True when this build renders scene wallpapers with
    // linux-wallpaperengine instead of the bundled backend_scene.
    bool hasLweScene() const;

protected:
signals:
    void cache_pathChanged();
    void versionChanged();
};
} // namespace wekde
