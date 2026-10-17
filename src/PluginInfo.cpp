#include "PluginInfo.hpp"
#include <iostream>
#include <QLoggingCategory>
#include <QCoreApplication>

#include "SceneBackend.hpp"

using namespace wekde;

PluginInfo::PluginInfo(QObject* parent): QObject(parent) {}

PluginInfo::~PluginInfo() {}

QUrl PluginInfo::cache_path() const {
    return QUrl::fromLocalFile(
        QString::fromStdString(scenebackend::SceneObject::GetDefaultCachePath()));
}

bool PluginInfo::hasLweScene() const {
#ifdef WEKDE_HAVE_LWE_SCENE
    return true;
#else
    return false;
#endif
}
