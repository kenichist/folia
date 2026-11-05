// App preferences kept in the workspace's small key/value settings table
// (Store::setting). Shared by AppController (applies them) and the Settings
// window (edits them). Appearance choices are stored as overrides with the
// same shape as a layout's `theme` table and are layered over it, so the
// user's Lua layout files are never rewritten.
#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>

namespace folia::prefs {

inline const QString kThemeOverrides = QStringLiteral("ui.theme_overrides"); // JSON object, "" = none
inline const QString kRestoreLastPage = QStringLiteral("app.restore_last_page"); // "1" (default) | "0"
inline const QString kMath = QStringLiteral("app.math");                       // "1" (default) | "0"; next launch
inline const QString kLiveReload = QStringLiteral("shell.live_reload");        // "1" (default) | "0"

// Missing or anything but "0" means on.
inline bool flag(const QString &stored)
{
    return stored != QLatin1String("0");
}

inline QString flagValue(bool on)
{
    return on ? QStringLiteral("1") : QStringLiteral("0");
}

inline QVariantMap parseThemeOverrides(const QString &json)
{
    if (json.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
}

inline QString serializeThemeOverrides(const QVariantMap &overrides)
{
    if (overrides.isEmpty())
        return QString();
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(overrides)).toJson(QJsonDocument::Compact));
}

} // namespace folia::prefs
