#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AraAudioSourceDescriptor
{
    std::string persistentId;
    std::string nameUtf8;
    int sampleRate = 0;
    int channelCount = 0;
    std::shared_ptr<const std::vector<float>> interleavedStereoSamples;
};

struct AraPlaybackRegionDescriptor
{
    std::string persistentId;
    std::string audioSourcePersistentId;
    std::string regionSequencePersistentId;
    std::string nameUtf8;
    std::string regionSequenceNameUtf8;
    int regionSequenceOrder = 0;
    int routingTrackIndex = -1;
    double sourceStartSeconds = 0.0;
    double sourceDurationSeconds = 0.0;
    double playbackStartSeconds = 0.0;
    double playbackDurationSeconds = 0.0;
    bool requestTimeStretch = false;
};

struct AraTempoEntryDescriptor
{
    double timeSeconds = 0.0;
    double quarterPosition = 0.0;
};

struct AraBarSignatureDescriptor
{
    double quarterPosition = 0.0;
    int numerator = 4;
    int denominator = 4;
};

struct AraDocumentArchive
{
    std::string factoryPersistentId;
    std::string documentArchiveId;
    std::vector<std::uint8_t> bytes;
};

struct AraTransportRequest
{
    bool startRequested = false;
    bool stopRequested = false;
    bool seekRequested = false;
    double positionSeconds = 0.0;
    bool cycleRangeRequested = false;
    double cycleStartSeconds = 0.0;
    double cycleDurationSeconds = 0.0;
    bool cycleEnableRequested = false;
    bool cycleEnabled = false;
};

struct AraHostStatus
{
    bool available = false;
    bool documentDirty = false;
    bool analysisActive = false;
    float analysisProgress = 0.0f;
    bool archiving = false;
    float archiveProgress = 0.0f;
    bool unarchiving = false;
    float unarchiveProgress = 0.0f;
};

struct AraProcessingAlgorithm
{
    std::string persistentId;
    std::string nameUtf8;
};

struct AraProcessingAlgorithmSelection
{
    std::string factoryPersistentId;
    std::string pluginNameUtf8;
    std::vector<AraProcessingAlgorithm> algorithms;
    int currentIndex = -1;
};

class AraDocumentRegistry;

#if defined(VERACORE_ARA_SDK_AVAILABLE)

#include <ARAInterface.h>

#include <mutex>
#include <unordered_map>

struct AraBindingStatus
{
    bool documentController = false;
    bool playbackRenderer = false;
    bool editorRenderer = false;
    bool editorView = false;
    bool contentAccess = false;

    bool complete() const noexcept
    {
        return documentController && playbackRenderer && editorRenderer &&
            editorView && contentAccess;
    }
};

class AraDocumentContext
{
public:
    ~AraDocumentContext();

    ARA::ARADocumentControllerRef documentControllerRef() const noexcept;
    bool ready() const noexcept;
    std::vector<ARA::ARAPlaybackRegionRef> playbackRegionRefsForTrack(
        int trackIndex) const;
    std::vector<ARA::ARAPlaybackRegionRef> playbackRegionRefsForPersistentIds(
        int trackIndex, const std::vector<std::string>& persistentIds) const;
    bool captureArchive(AraDocumentArchive& archive);
    bool restoreArchive(const AraDocumentArchive& archive);
    bool consumeTransportRequest(AraTransportRequest& request) noexcept;
    bool consumeDocumentDirty() noexcept;
    AraHostStatus pollStatus() noexcept;
    bool processingAlgorithmSelection(
        const std::vector<std::string>& playbackRegionPersistentIds,
        AraProcessingAlgorithmSelection& selection);
    bool requestProcessingAlgorithm(
        const std::vector<std::string>& playbackRegionPersistentIds,
        int algorithmIndex);
    bool syncModelGraph(const std::vector<AraAudioSourceDescriptor>& sources,
                        const std::vector<AraPlaybackRegionDescriptor>& regions,
                        const std::vector<AraTempoEntryDescriptor>& tempoEntries,
                        const std::vector<AraBarSignatureDescriptor>& barSignatures);

private:
    friend class AraDocumentRegistry;
    struct Impl;
    explicit AraDocumentContext(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

class AraDocumentRegistry
{
public:
    std::shared_ptr<AraDocumentContext> acquire(const ARA::ARAFactory* factory);
    void syncModelGraph(const std::vector<AraAudioSourceDescriptor>& sources,
                        const std::vector<AraPlaybackRegionDescriptor>& regions,
                        const std::vector<AraTempoEntryDescriptor>& tempoEntries,
                        const std::vector<AraBarSignatureDescriptor>& barSignatures);
    bool captureArchives(std::vector<AraDocumentArchive>& archives);
    bool restoreArchives(const std::vector<AraDocumentArchive>& archives);
    bool consumeTransportRequest(AraTransportRequest& request) noexcept;
    bool pollStatus(AraHostStatus& status) noexcept;
    bool processingAlgorithmSelection(
        const std::vector<std::string>& playbackRegionPersistentIds,
        AraProcessingAlgorithmSelection& selection);
    bool requestProcessingAlgorithm(
        const std::string& factoryPersistentId,
        const std::vector<std::string>& playbackRegionPersistentIds,
        int algorithmIndex);

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, std::weak_ptr<AraDocumentContext>> m_documents;
    std::vector<AraAudioSourceDescriptor> m_audioSources;
    std::vector<AraPlaybackRegionDescriptor> m_playbackRegions;
    std::vector<AraTempoEntryDescriptor> m_tempoEntries;
    std::vector<AraBarSignatureDescriptor> m_barSignatures;
};

AraBindingStatus validateAraBinding(
    const ARA::ARAPlugInExtensionInstance* instance,
    ARA::ARAPlugInInstanceRoleFlags assignedRoles,
    const std::shared_ptr<AraDocumentContext>& document) noexcept;

#endif
