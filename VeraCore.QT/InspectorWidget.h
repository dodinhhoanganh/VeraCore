#pragma once
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QDial>
#include <QProgressBar>
#include <QStringList>
#include "Track.h"
#include "ExternalPluginHost.h"

class QCheckBox;

class InspectorWidget : public QWidget {
    Q_OBJECT
public:
    enum class PluginHostStatus { Internal, Pending, Active, Faulted, Failed, Unsupported };
    enum class AutomationWriteMode { Read, Write, Touch, Latch };
    Q_ENUM(AutomationWriteMode)

    explicit InspectorWidget(QWidget* parent = nullptr);
    virtual ~InspectorWidget() = default;

    void setTrack(Track* track);
    void refreshMixerControls();
    void refreshTrackDevices();
    void setReturnBuses(const std::array<ReturnBusState, kReturnBusCount>& buses);
	void setReturnMeters(const std::array<VeraCoreAudioMeterState, kReturnBusCount>& meters);
    void showReturnFxBus(int returnBus);
    int currentReturnFxBus() const noexcept { return m_currentReturnFxBus; }
    void setMasterBus(const MasterBusState& master);
    void setPluginHostStates(std::vector<PluginHostStatus> layerStates,
                             std::vector<PluginHostStatus> audioFxStates);
    void setReturnPluginHostStates(
        std::array<std::vector<PluginHostStatus>, kReturnBusCount> audioFxStates);
    void setPluginParameters(const QString& deviceName, bool audioFx, int slotIndex,
                             std::vector<PluginParameterInfo> parameters,
                             bool automationAvailable = true);
    void clearPluginParameters();
    void setActiveAutomationTarget(bool audioFx, int slotIndex, uint32_t parameterId);
    void clearActiveAutomationTarget();
    void setAraStatus(bool available, bool dirty,
                      const QString& operation, float progress);
    void setAraProcessingAlgorithms(const QString& pluginName,
                                    const QStringList& algorithmNames,
                                    int currentIndex);
    void setTimeSignature(int numerator, int denominator);
    void setMidiClip(const QString& name, double startBeat, double lengthBeats);
    void setAudioClip(const QString& name, double startBeat, double lengthBeats, const QString& sourcePath,
                      double sourceTempoBpm = 0.0, double pitchShiftSemitones = 0.0,
                      int syncMode = 0, double targetBars = 0.0,
                      double tempoConfidence = 0.0, int sourceSignatureNumerator = 0,
                      int sourceSignatureDenominator = 0, bool tempoDetected = false,
                      float gain = 1.0f, float pan = 0.0f, bool normalize = false,
                       bool reverse = false, bool reversePolarity = false,
                       double sourceOffsetBeats = 0.0, double sourceRangeLengthBeats = 0.0,
                       double warpLengthBeats = 0.0,
                       double fadeInBeats = 0.0, double fadeOutBeats = 0.0,
                       bool mediaOffline = false, int transientCount = 0,
                       int warpMarkerCount = 0);

signals:
    void openMidiEditorRequested();
    void addLayerRequested();
    void layerEnabledChanged(int layerIndex, bool enabled);
    void layerParametersChanged(int layerIndex, float gain, float pan, int transpose, int midiChannel);
    void layerOutputRoutingRequested(int layerIndex);
    void layerReloadRequested(int layerIndex);
    void removeLayerRequested(int layerIndex);
    void audioFxEnabledChanged(int slotIndex, bool enabled);
    void audioFxReloadRequested(int slotIndex);
    void removeAudioFxRequested(int slotIndex);
    void addAudioFxRequested();
    void returnAudioFxEnabledChanged(int returnBus, int slotIndex, bool enabled);
    void returnAudioFxReloadRequested(int returnBus, int slotIndex);
    void removeReturnAudioFxRequested(int returnBus, int slotIndex);
    void addReturnAudioFxRequested(int returnBus);
    void pluginParametersRequested(bool audioFx, int slotIndex, const QString& deviceName);
    void pluginParameterGestureBegin(bool audioFx, int slotIndex, uint32_t parameterId);
    void pluginParameterChanged(bool audioFx, int slotIndex, uint32_t parameterId,
                                double normalizedValue);
    void pluginParameterGestureEnd(bool audioFx, int slotIndex, uint32_t parameterId);
    void pluginAutomationTargetRequested(bool audioFx, int slotIndex,
                                         uint32_t parameterId, const QString& parameterName);
    void pluginAutomationModeChanged(AutomationWriteMode mode);
    void audioClipSyncChanged(double sourceTempoBpm, double pitchShiftSemitones);
    void audioClipSyncModeChanged(int syncMode, double targetBars);
    void audioClipProcessingChanged(float gain, float pan, bool normalize,
                                    bool reverse, bool reversePolarity);
    void audioClipFadesChanged(double fadeInBeats, double fadeOutBeats);
    void audioClipSourceRangeChanged(double sourceInBeats, double sourceOutBeats);
    void audioClipRelinkRequested();
    void araProcessingAlgorithmChanged(int algorithmIndex);
    void mixerControlsEdited(Track* track, float volume, float pan, float stereoSeparation,
                             bool muted, bool soloed, bool armed, bool monitoring);
    void spatialPannerEdited(Track* track, VeraCoreSpatialPannerState state);
    void trackSendEdited(Track* track, int returnBus, float level);
    void returnBusEdited(int returnBus, float volume, float pan, float stereoSeparation,
                         bool muted, bool soloed, bool preFader);
    void masterBusEdited(float volume, float pan, float stereoSeparation, bool muted);

private:
    Track* m_currentTrack;
    int m_currentReturnFxBus = -1;
    bool m_showingMidiClip;
    bool m_showingAudioClip;
    QString m_audioClipSourcePath;
    double m_beatsPerBar;
    double m_notatedBeatWidth;
    double m_currentClipStartBeat;
    double m_currentClipLengthBeats;
    int m_timeSignatureNumerator = 4;
    int m_timeSignatureDenominator = 4;

    QLabel* m_trackNameLabel;
    QLabel* m_contextLabel;
    QPushButton* m_muteBtn;
    QPushButton* m_soloBtn;
    QPushButton* m_armBtn;
    QPushButton* m_monitorBtn;
    QSlider* m_volumeSlider;
    QSlider* m_panSlider;
    QDial* m_stereoSeparationSlider = nullptr;
    QGroupBox* m_spatialGroup = nullptr;
    QComboBox* m_spatialModeCombo = nullptr;
    QDoubleSpinBox* m_azimuthSpin = nullptr;
    QDoubleSpinBox* m_elevationSpin = nullptr;
    QDoubleSpinBox* m_distanceSpin = nullptr;
    QDoubleSpinBox* m_spreadSpin = nullptr;
    QDoubleSpinBox* m_lfeSendSpin = nullptr;
    QCheckBox* m_linkStereoPanCheck = nullptr;
    QLabel* m_araStatusLabel = nullptr;
    QProgressBar* m_araProgressBar = nullptr;
    
    QGroupBox* m_instrumentGroup;
    QVBoxLayout* m_instrumentLayout;
    QPushButton* m_addLayerBtn = nullptr;

    QGroupBox* m_fxGroup;
    QVBoxLayout* m_fxLayout;
    QPushButton* m_addFxBtn;

    QGroupBox* m_outputGroup;
    QLabel* m_outputLabel;
    std::array<QSlider*, kReturnBusCount> m_sendSliders{};
    std::array<QSlider*, kReturnBusCount> m_returnVolumeSliders{};
    std::array<QSlider*, kReturnBusCount> m_returnPanSliders{};
    std::array<QDial*, kReturnBusCount> m_returnSeparationSliders{};
    std::array<QPushButton*, kReturnBusCount> m_returnMuteButtons{};
    std::array<QPushButton*, kReturnBusCount> m_returnSoloButtons{};
    std::array<QPushButton*, kReturnBusCount> m_returnPreFaderButtons{};
	std::array<QProgressBar*, kReturnBusCount> m_returnMeterBars{};
    std::array<QPushButton*, kReturnBusCount> m_returnFxButtons{};
    std::array<ReturnBusState, kReturnBusCount> m_returnBusView{};
    QSlider* m_masterVolumeSlider = nullptr;
    QSlider* m_masterPanSlider = nullptr;
    QDial* m_masterSeparationSlider = nullptr;
    QPushButton* m_masterMuteButton = nullptr;
    MasterBusState m_masterBusView{};
    std::vector<PluginHostStatus> m_layerHostStates;
    std::vector<PluginHostStatus> m_audioFxHostStates;
    std::array<std::vector<PluginHostStatus>, kReturnBusCount> m_returnAudioFxHostStates{};
    QGroupBox* m_parameterGroup = nullptr;
    QVBoxLayout* m_parameterLayout = nullptr;
    QString m_parameterDeviceName;
    bool m_parameterAudioFx = false;
    int m_parameterSlotIndex = -1;
    std::vector<PluginParameterInfo> m_pluginParameters;
    bool m_parameterAutomationAvailable = true;
    AutomationWriteMode m_automationWriteMode = AutomationWriteMode::Read;
    bool m_hasActiveAutomationTarget = false;
    bool m_activeAutomationAudioFx = false;
    int m_activeAutomationSlotIndex = -1;
    uint32_t m_activeAutomationParameterId = 0;

    QGroupBox* m_clipGroup;
    QLabel* m_clipStartLabel;
    QLabel* m_clipLengthLabel;
    QLabel* m_clipSyncLabel;
    QComboBox* m_syncModeCombo;
    QDoubleSpinBox* m_sourceTempoSpin;
    QDoubleSpinBox* m_pitchShiftSpin;
    QDoubleSpinBox* m_targetBarsSpin;
    QDoubleSpinBox* m_clipGainSpin;
    QDoubleSpinBox* m_clipPanSpin;
    QDoubleSpinBox* m_sourceInSpin;
    QDoubleSpinBox* m_sourceOutSpin;
    QDoubleSpinBox* m_fadeInSpin;
    QDoubleSpinBox* m_fadeOutSpin;
    QPushButton* m_normalizeBtn;
    QPushButton* m_reverseBtn;
    QPushButton* m_reversePolarityBtn;
    QPushButton* m_openMidiEditorBtn;
    QLabel* m_stretchStatusLabel;
    QLabel* m_mediaStatusLabel = nullptr;
    QPushButton* m_relinkMediaBtn = nullptr;
    QComboBox* m_araAlgorithmCombo = nullptr;

    void setupUi();
    void updateUi();
    void rebuildInstrumentRack();
    void rebuildAudioFxRack();
    void rebuildPluginParameterRack();
    void updateClipPositionLabels();
    void emitMixerControlsEdited();
    void emitSpatialPannerEdited();
};
