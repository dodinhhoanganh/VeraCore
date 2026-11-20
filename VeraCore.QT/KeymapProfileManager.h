#pragma once

#include <QObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include "CommandRegistry.h"

struct KeymapProfile {
    QString id;
    QString name;
    QString description;
    CommandRegistry::ShortcutMap shortcuts;
};

class KeymapProfileManager final : public QObject
{
    Q_OBJECT

public:
    explicit KeymapProfileManager(CommandRegistry* registry, QObject* parent = nullptr);

    QStringList profileIds() const { return m_order; }
    KeymapProfile profile(const QString& id) const;
    QString currentProfileId() const { return m_currentProfileId; }
    QStringList collisions(const QString& id) const;
    bool applyProfile(const QString& id, QString* errorText = nullptr);

signals:
    void profileApplied(const QString& id);

private:
    void addProfile(const QString& id, const QString& name, const QString& description,
                    const QHash<QString, QStringList>& overrides = {});

    CommandRegistry* m_registry = nullptr;
    CommandRegistry::ShortcutMap m_baseShortcuts;
    QHash<QString, KeymapProfile> m_profiles;
    QStringList m_order;
    QString m_currentProfileId = QStringLiteral("veracore");
};
