#include "WorkspaceManager.h"

#include <QSettings>
#include <QVariantList>
#include <algorithm>

namespace {
bool validSizes(const QList<int>& sizes) noexcept
{
    if (sizes.size() != 2) return false;
    int total = 0;
    for (int size : sizes) {
        if (size <= 0 || size > 100000) return false;
        total += size;
    }
    return total > 0;
}

QVariantList storedSizes(const QList<int>& sizes)
{
    QVariantList result;
    result.reserve(sizes.size());
    for (int size : sizes) result.push_back(size);
    return result;
}

QList<int> restoredSizes(const QVariant& value, const QList<int>& fallback)
{
    const QVariantList stored = value.toList();
    QList<int> result;
    result.reserve(stored.size());
    for (const QVariant& item : stored) {
        bool ok = false;
        const int size = item.toInt(&ok);
        if (!ok) return fallback;
        result.push_back(size);
    }
    return validSizes(result) ? result : fallback;
}
}

WorkspaceManager::WorkspaceManager(QObject* parent)
    : QObject(parent)
{
    addBuiltIn({"arrangement", "Arrangement",
        "Browser, track headers, arrangement, and compact detail zone.",
        true, true, true, {240, 1040}, {540, 170}, {214, 826}, true});
    addBuiltIn({"session-performance", "Session / Performance",
        "Maximum performance surface with Browser and detail zone collapsed.",
        false, true, false, {220, 1060}, {540, 170}, {214, 1066}, true});
    addBuiltIn({"midi-editing", "Piano Roll / MIDI Editing",
        "Arrangement plus a deep lower editor and visible Browser.",
        true, true, true, {220, 1060}, {360, 350}, {190, 890}, true});
    addBuiltIn({"recording", "Recording",
        "Wide arrangement, recording controls, and compact input inspector.",
        false, true, true, {220, 1060}, {570, 140}, {230, 1050}, true});
    addBuiltIn({"mixing", "Mixing",
        "Large mixer/device detail zone with arrangement context.",
        false, true, true, {220, 1060}, {300, 410}, {185, 1095}, true});
    addBuiltIn({"mastering", "Mastering",
        "Focused arrangement and maximum master/device detail area.",
        true, true, true, {190, 1090}, {250, 460}, {170, 920}, true});
    addBuiltIn({"dual-monitor", "Dual-Monitor",
        "All anchors visible and proportioned for a wide or secondary display.",
        true, true, true, {260, 1660}, {700, 300}, {250, 1410}, true});
    loadUserCustom();
    if (!m_presets.contains(QStringLiteral("user-custom"))) {
        WorkspacePreset custom = m_presets.value(QStringLiteral("arrangement"));
        custom.id = QStringLiteral("user-custom");
        custom.name = QStringLiteral("User Custom");
        custom.description = QStringLiteral("User-saved workspace layout.");
        custom.builtIn = false;
        m_presets.insert(custom.id, custom);
        m_order.push_back(custom.id);
    }
}

void WorkspaceManager::addBuiltIn(const WorkspacePreset& preset)
{
    if (!isValid(preset) || m_presets.contains(preset.id)) return;
    m_presets.insert(preset.id, preset);
    m_order.push_back(preset.id);
}

WorkspacePreset WorkspaceManager::preset(const QString& id) const
{
    return m_presets.value(id.trimmed().toLower());
}

bool WorkspaceManager::activate(const QString& id)
{
    const QString key = id.trimmed().toLower();
    const auto found = m_presets.constFind(key);
    if (found == m_presets.cend() || !isValid(*found)) return false;
    m_currentPresetId = key;
    emit activationRequested(*found);
    emit currentPresetChanged(key);
    return true;
}

bool WorkspaceManager::selectCurrentWithoutActivation(const QString& id)
{
    const QString key = id.trimmed().toLower();
    if (!m_presets.contains(key)) return false;
    m_currentPresetId = key;
    emit currentPresetChanged(key);
    return true;
}

bool WorkspaceManager::saveUserCustom(const WorkspacePreset& requested)
{
    WorkspacePreset custom = requested;
    custom.id = QStringLiteral("user-custom");
    custom.name = QStringLiteral("User Custom");
    custom.description = QStringLiteral("User-saved workspace layout.");
    custom.builtIn = false;
    if (!isValid(custom)) return false;
    const bool added = !m_presets.contains(custom.id);
    m_presets.insert(custom.id, custom);
    if (added) m_order.push_back(custom.id);

    QSettings settings;
    settings.beginGroup(QStringLiteral("WorkspacePresets/UserCustom"));
    settings.setValue(QStringLiteral("BrowserVisible"), custom.browserVisible);
    settings.setValue(QStringLiteral("ArrangementVisible"), custom.arrangementVisible);
    settings.setValue(QStringLiteral("DetailVisible"), custom.detailVisible);
    settings.setValue(QStringLiteral("MainSplitterSizes"), storedSizes(custom.mainSplitterSizes));
    settings.setValue(QStringLiteral("CenterSplitterSizes"), storedSizes(custom.centerSplitterSizes));
    settings.setValue(QStringLiteral("PlaylistSplitterSizes"), storedSizes(custom.playlistSplitterSizes));
    settings.endGroup();
    settings.sync();
    if (added) emit presetsChanged();
    return activate(custom.id);
}

bool WorkspaceManager::isValid(const WorkspacePreset& preset) noexcept
{
    const QString id = preset.id.trimmed();
    return !id.isEmpty() && id.size() <= 64 && !preset.name.trimmed().isEmpty() &&
        validSizes(preset.mainSplitterSizes) && validSizes(preset.centerSplitterSizes) &&
        validSizes(preset.playlistSplitterSizes);
}

void WorkspaceManager::loadUserCustom()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("WorkspacePresets/UserCustom"));
    if (!settings.contains(QStringLiteral("MainSplitterSizes"))) {
        settings.endGroup();
        return;
    }
    WorkspacePreset custom;
    custom.id = QStringLiteral("user-custom");
    custom.name = QStringLiteral("User Custom");
    custom.description = QStringLiteral("User-saved workspace layout.");
    custom.browserVisible = settings.value(QStringLiteral("BrowserVisible"), true).toBool();
    custom.arrangementVisible = settings.value(QStringLiteral("ArrangementVisible"), true).toBool();
    custom.detailVisible = settings.value(QStringLiteral("DetailVisible"), true).toBool();
    custom.mainSplitterSizes = restoredSizes(
        settings.value(QStringLiteral("MainSplitterSizes")), {240, 1040});
    custom.centerSplitterSizes = restoredSizes(
        settings.value(QStringLiteral("CenterSplitterSizes")), {540, 170});
    custom.playlistSplitterSizes = restoredSizes(
        settings.value(QStringLiteral("PlaylistSplitterSizes")), {214, 826});
    custom.builtIn = false;
    settings.endGroup();
    if (isValid(custom)) {
        m_presets.insert(custom.id, custom);
        m_order.push_back(custom.id);
    }
}
