#include "KeymapProfileManager.h"

namespace {
QList<QKeySequence> sequences(const QStringList& portableSequences)
{
    QList<QKeySequence> result;
    result.reserve(portableSequences.size());
    for (const QString& text : portableSequences) {
        const QKeySequence sequence(text, QKeySequence::PortableText);
        if (!sequence.isEmpty()) result.push_back(sequence);
    }
    return result;
}
}

KeymapProfileManager::KeymapProfileManager(CommandRegistry* registry, QObject* parent)
    : QObject(parent)
    , m_registry(registry)
    , m_baseShortcuts(registry ? registry->shortcutMap() : CommandRegistry::ShortcutMap{})
{
    addProfile("veracore", "VeraCore",
        "Native hybrid workflow with FL function keys and Ableton-style detail switching.");
    addProfile("ableton-live", "Ableton Live",
        "Arrangement/device workflow with Ableton-oriented duplicate and editor shortcuts.", {
            {"clip.duplicate-linked", {"Ctrl+D"}}, {"clip.make-unique", {"Ctrl+Shift+U"}},
            {"view.arrangement", {"Ctrl+Alt+A"}}, {"view.piano-roll", {"Ctrl+Shift+M"}},
            {"view.mixer", {"Ctrl+Alt+M"}}, {"view.inspector", {"Shift+Tab"}},
            {"view.browser", {"Ctrl+Alt+B"}}
        });
    addProfile("fl-studio", "FL Studio",
        "F5 Playlist, F7 Piano Roll, F9 Mixer, and FL-style duplication.", {
            {"clip.duplicate-linked", {"Ctrl+B"}}, {"clip.make-unique", {"Ctrl+U"}},
            {"view.arrangement", {"F5"}}, {"view.piano-roll", {"F7"}},
            {"view.mixer", {"F9"}}, {"view.inspector", {"Shift+Tab"}},
            {"view.browser", {"Ctrl+Alt+B"}}
        });
    addProfile("cubase-nuendo", "Cubase / Nuendo",
        "Key Editor, MixConsole, Browser, and Inspector-oriented bindings.", {
            {"clip.duplicate-linked", {"Ctrl+D"}}, {"clip.make-unique", {"Ctrl+Shift+U"}},
            {"view.arrangement", {"Ctrl+Alt+A"}}, {"view.piano-roll", {"Ctrl+E"}},
            {"view.mixer", {"F3"}}, {"view.inspector", {"Ctrl+Alt+I"}},
            {"view.browser", {"F5"}}
        });
    addProfile("reaper", "REAPER",
        "REAPER-oriented arrange, MIDI editor, mixer, and routing-view bindings.", {
            {"clip.duplicate-linked", {"Ctrl+D"}}, {"clip.make-unique", {"Ctrl+Shift+U"}},
            {"view.arrangement", {"Ctrl+Alt+A"}}, {"view.piano-roll", {"Ctrl+Alt+E"}},
            {"view.mixer", {"Ctrl+M"}}, {"view.inspector", {"Ctrl+Alt+I"}},
            {"view.browser", {"Ctrl+Alt+B"}}
        });
    addProfile("logic-pro", "Logic Pro",
        "Logic-oriented repeat, editor, mixer, inspector, and browser bindings.", {
            {"clip.duplicate-linked", {"Ctrl+R"}}, {"clip.make-unique", {"Ctrl+Shift+U"}},
            {"view.arrangement", {"Ctrl+Alt+A"}}, {"view.piano-roll", {"Ctrl+Alt+P"}},
            {"view.mixer", {"Ctrl+Alt+M"}}, {"view.inspector", {"Ctrl+Alt+I"}},
            {"view.browser", {"Ctrl+Alt+B"}}
        });
    addProfile("pro-tools", "Pro Tools",
        "Edit/Mix-oriented shortcuts with explicit Browser and MIDI editor access.", {
            {"clip.duplicate-linked", {"Ctrl+D"}}, {"clip.make-unique", {"Ctrl+Shift+U"}},
            {"view.arrangement", {"Ctrl+Alt+A"}}, {"view.piano-roll", {"Ctrl+Alt+E"}},
            {"view.mixer", {"Ctrl+Alt+M"}}, {"view.inspector", {"Ctrl+Alt+I"}},
            {"view.browser", {"Ctrl+Alt+B"}}
        });
    addProfile("studio-one", "Studio One",
        "F2 Editor, F3 Console, F4 Inspector, and F5 Browser.", {
            {"clip.duplicate-linked", {"Ctrl+D"}}, {"clip.make-unique", {"Ctrl+Shift+U"}},
            {"view.arrangement", {"Ctrl+Alt+A"}}, {"view.piano-roll", {"F2"}},
            {"view.mixer", {"F3"}}, {"view.inspector", {"F4"}},
            {"view.browser", {"F5"}}
        });
    addProfile("bitwig", "Bitwig Studio",
        "Hybrid arrange/device workflow with explicit editor and mixer switching.", {
            {"clip.duplicate-linked", {"Ctrl+D"}}, {"clip.make-unique", {"Ctrl+Shift+U"}},
            {"view.arrangement", {"Ctrl+Alt+A"}}, {"view.piano-roll", {"Ctrl+Alt+E"}},
            {"view.mixer", {"Ctrl+Alt+M"}}, {"view.inspector", {"Shift+Tab"}},
            {"view.browser", {"Ctrl+Alt+B"}}
        });
}

void KeymapProfileManager::addProfile(const QString& rawId, const QString& name,
                                      const QString& description,
                                      const QHash<QString, QStringList>& overrides)
{
    const QString id = rawId.trimmed().toLower();
    if (id.isEmpty() || !m_registry || m_profiles.contains(id)) return;
    KeymapProfile profile{id, name.trimmed(), description.trimmed(), m_baseShortcuts};
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        profile.shortcuts.insert(it.key().trimmed().toLower(), sequences(it.value()));
    if (!m_registry->shortcutCollisions(profile.shortcuts).isEmpty()) return;
    m_profiles.insert(id, profile);
    m_order.push_back(id);
}

KeymapProfile KeymapProfileManager::profile(const QString& id) const
{
    return m_profiles.value(id.trimmed().toLower());
}

QStringList KeymapProfileManager::collisions(const QString& id) const
{
    if (!m_registry) return {QStringLiteral("command registry unavailable")};
    const auto found = m_profiles.constFind(id.trimmed().toLower());
    return found == m_profiles.cend()
        ? QStringList{QStringLiteral("unknown keymap profile")}
        : m_registry->shortcutCollisions(found->shortcuts);
}

bool KeymapProfileManager::applyProfile(const QString& rawId, QString* errorText)
{
    const QString id = rawId.trimmed().toLower();
    const auto found = m_profiles.constFind(id);
    if (!m_registry || found == m_profiles.cend()) {
        if (errorText) *errorText = QStringLiteral("Unknown keymap profile: %1").arg(rawId);
        return false;
    }
    if (!m_registry->applyShortcutMap(found->shortcuts, errorText)) return false;
    m_currentProfileId = id;
    emit profileApplied(id);
    return true;
}
