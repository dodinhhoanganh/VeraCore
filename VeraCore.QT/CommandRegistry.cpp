#include "CommandRegistry.h"

#include <QAction>
#include <algorithm>

CommandRegistry::CommandRegistry(QObject* parent)
    : QObject(parent)
{
}

bool CommandRegistry::registerAction(const QString& id, QAction* action,
                                     const QString& category, const QString& description)
{
    const QString key = normalizedId(id);
    if (key.isEmpty() || !action || m_commands.contains(key)) return false;
    CommandInfo info{key, category.trimmed(), description.trimmed(), action};
    action->setProperty("veracoreCommandId", key);
    action->setProperty("veracoreCommandCategory", info.category);
    if (!info.description.isEmpty()) action->setStatusTip(info.description);
    m_commands.insert(key, info);
    indexShortcuts(key, action->shortcuts());
    connect(action, &QAction::triggered, this, [this, key]() {
        emit commandTriggered(key);
    });
    emit commandRegistered(key);
    return true;
}

QAction* CommandRegistry::action(const QString& id) const
{
    const auto found = m_commands.constFind(normalizedId(id));
    return found == m_commands.cend() ? nullptr : found->action.data();
}

QStringList CommandRegistry::commandIds() const
{
    QStringList ids = m_commands.keys();
    ids.sort(Qt::CaseInsensitive);
    return ids;
}

QString CommandRegistry::shortcutOwner(const QKeySequence& shortcut) const
{
    return m_shortcutOwners.value(shortcut.toString(QKeySequence::PortableText));
}

CommandRegistry::ShortcutMap CommandRegistry::shortcutMap() const
{
    ShortcutMap result;
    for (auto it = m_commands.cbegin(); it != m_commands.cend(); ++it)
        result.insert(it.key(), it->action ? it->action->shortcuts() : QList<QKeySequence>{});
    return result;
}

QStringList CommandRegistry::shortcutCollisions(const ShortcutMap& shortcuts) const
{
    QHash<QString, QString> owners;
    QStringList collisions;
    QStringList ids = shortcuts.keys();
    ids.sort(Qt::CaseInsensitive);
    for (const QString& rawId : ids) {
        const QString id = normalizedId(rawId);
        if (!m_commands.contains(id)) {
            collisions.push_back(QStringLiteral("unknown command: %1").arg(rawId));
            continue;
        }
        for (const QKeySequence& shortcut : shortcuts.value(rawId)) {
            const QString key = shortcut.toString(QKeySequence::PortableText);
            if (key.isEmpty()) continue;
            const QString owner = owners.value(key);
            if (!owner.isEmpty() && owner != id)
                collisions.push_back(QStringLiteral("%1: %2 / %3").arg(key, owner, id));
            else
                owners.insert(key, id);
        }
    }
    collisions.removeDuplicates();
    collisions.sort(Qt::CaseInsensitive);
    return collisions;
}

bool CommandRegistry::applyShortcutMap(const ShortcutMap& requested, QString* errorText)
{
    ShortcutMap candidate = shortcutMap();
    for (auto it = requested.cbegin(); it != requested.cend(); ++it) {
        const QString id = normalizedId(it.key());
        if (!m_commands.contains(id)) {
            if (errorText) *errorText = QStringLiteral("Unknown command: %1").arg(it.key());
            return false;
        }
        candidate.insert(id, it.value());
    }
    const QStringList collisions = shortcutCollisions(candidate);
    if (!collisions.isEmpty()) {
        if (errorText) *errorText = QStringLiteral("Shortcut collision: %1").arg(
            collisions.join(QStringLiteral("; ")));
        return false;
    }
    for (auto it = m_commands.begin(); it != m_commands.end(); ++it) {
        if (it->action) it->action->setShortcuts(candidate.value(it.key()));
    }
    rebuildShortcutIndex();
    emit shortcutsChanged();
    return true;
}

bool CommandRegistry::trigger(const QString& id)
{
    QAction* commandAction = action(id);
    if (!commandAction || !commandAction->isEnabled()) return false;
    commandAction->trigger();
    return true;
}

bool CommandRegistry::setEnabled(const QString& id, bool enabled)
{
    QAction* commandAction = action(id);
    if (!commandAction) return false;
    commandAction->setEnabled(enabled);
    return true;
}

QString CommandRegistry::normalizedId(const QString& id)
{
    return id.trimmed().toLower();
}

void CommandRegistry::indexShortcuts(const QString& id, const QList<QKeySequence>& shortcuts)
{
    for (const QKeySequence& shortcut : shortcuts) {
        const QString key = shortcut.toString(QKeySequence::PortableText);
        if (!key.isEmpty() && !m_shortcutOwners.contains(key)) m_shortcutOwners.insert(key, id);
    }
}

void CommandRegistry::rebuildShortcutIndex()
{
    m_shortcutOwners.clear();
    QStringList ids = m_commands.keys();
    ids.sort(Qt::CaseInsensitive);
    for (const QString& id : ids) {
        const auto found = m_commands.constFind(id);
        if (found != m_commands.cend() && found->action)
            indexShortcuts(id, found->action->shortcuts());
    }
}
