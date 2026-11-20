#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

struct WorkspacePreset {
    QString id;
    QString name;
    QString description;
    bool browserVisible = true;
    bool arrangementVisible = true;
    bool detailVisible = true;
    QList<int> mainSplitterSizes{240, 1040};
    QList<int> centerSplitterSizes{540, 170};
    QList<int> playlistSplitterSizes{214, 826};
    bool builtIn = true;
};

class WorkspaceManager final : public QObject
{
    Q_OBJECT

public:
    explicit WorkspaceManager(QObject* parent = nullptr);

    QStringList presetIds() const { return m_order; }
    WorkspacePreset preset(const QString& id) const;
    QString currentPresetId() const { return m_currentPresetId; }
    bool activate(const QString& id);
    bool selectCurrentWithoutActivation(const QString& id);
    bool saveUserCustom(const WorkspacePreset& state);
    static bool isValid(const WorkspacePreset& preset) noexcept;

signals:
    void activationRequested(const WorkspacePreset& preset);
    void currentPresetChanged(const QString& id);
    void presetsChanged();

private:
    void addBuiltIn(const WorkspacePreset& preset);
    void loadUserCustom();

    QHash<QString, WorkspacePreset> m_presets;
    QStringList m_order;
    QString m_currentPresetId = QStringLiteral("arrangement");
};
