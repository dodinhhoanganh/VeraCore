#pragma once

#include <QObject>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>

class QAction;

class CommandRegistry final : public QObject
{
    Q_OBJECT

public:
    using ShortcutMap = QHash<QString, QList<QKeySequence>>;

    struct CommandInfo {
        QString id;
        QString category;
        QString description;
        QPointer<QAction> action;
    };

    explicit CommandRegistry(QObject* parent = nullptr);

    bool registerAction(const QString& id, QAction* action, const QString& category,
                        const QString& description = {});
    QAction* action(const QString& id) const;
    QStringList commandIds() const;
    QString shortcutOwner(const QKeySequence& shortcut) const;
    ShortcutMap shortcutMap() const;
    QStringList shortcutCollisions(const ShortcutMap& shortcuts) const;
    bool applyShortcutMap(const ShortcutMap& shortcuts, QString* errorText = nullptr);
    bool trigger(const QString& id);
    bool setEnabled(const QString& id, bool enabled);

signals:
    void commandRegistered(const QString& id);
    void commandTriggered(const QString& id);
    void shortcutsChanged();

private:
    static QString normalizedId(const QString& id);
    void indexShortcuts(const QString& id, const QList<QKeySequence>& shortcuts);
    void rebuildShortcutIndex();

    QHash<QString, CommandInfo> m_commands;
    QHash<QString, QString> m_shortcutOwners;
};
