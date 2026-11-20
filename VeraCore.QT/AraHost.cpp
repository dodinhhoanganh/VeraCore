#include "AraHost.h"

#if defined(VERACORE_ARA_SDK_AVAILABLE)

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace
{
ARA::ARAAssertFunction gAraAssertFunction = nullptr;

class ScopedAtomicActivity
{
public:
    explicit ScopedAtomicActivity(std::atomic<bool>& activity) noexcept
        : m_activity(activity),
          m_previous(activity.exchange(true, std::memory_order_acq_rel))
    {
    }

    ~ScopedAtomicActivity()
    {
        m_activity.store(m_previous, std::memory_order_release);
    }

    ScopedAtomicActivity(const ScopedAtomicActivity&) = delete;
    ScopedAtomicActivity& operator=(const ScopedAtomicActivity&) = delete;

private:
    std::atomic<bool>& m_activity;
    bool m_previous = false;
};

bool initializeFactory(const ARA::ARAFactory* factory,
                       const ARA::ARAInterfaceConfiguration* configuration) noexcept
{
    __try {
        if (!factory || !factory->initializeARAWithConfiguration) return false;
        factory->initializeARAWithConfiguration(configuration);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void uninitializeFactory(const ARA::ARAFactory* factory) noexcept
{
    __try { if (factory && factory->uninitializeARA) factory->uninitializeARA(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

const ARA::ARADocumentControllerInstance* createDocumentController(
    const ARA::ARAFactory* factory,
    const ARA::ARADocumentControllerHostInstance* host,
    const ARA::ARADocumentProperties* properties) noexcept
{
    __try {
        return factory && factory->createDocumentControllerWithDocument
            ? factory->createDocumentControllerWithDocument(host, properties)
            : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void destroyDocumentController(const ARA::ARADocumentControllerInstance* instance) noexcept
{
    __try {
        if (instance && instance->documentControllerInterface &&
            instance->documentControllerInterface->destroyDocumentController) {
            instance->documentControllerInterface->destroyDocumentController(
                instance->documentControllerRef);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

const ARA::ARADocumentControllerInterface* controllerInterfaceSeh(
    const ARA::ARADocumentControllerInstance* instance) noexcept
{
    __try {
        return instance ? instance->documentControllerInterface : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void beginEditingSeh(const ARA::ARADocumentControllerInterface* api,
                     ARA::ARADocumentControllerRef controller) noexcept
{
    __try { if (api && api->beginEditing) api->beginEditing(controller); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void endEditingSeh(const ARA::ARADocumentControllerInterface* api,
                   ARA::ARADocumentControllerRef controller) noexcept
{
    __try { if (api && api->endEditing) api->endEditing(controller); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void notifyModelUpdatesSeh(const ARA::ARADocumentControllerInterface* api,
                           ARA::ARADocumentControllerRef controller) noexcept
{
    __try { if (api && api->notifyModelUpdates) api->notifyModelUpdates(controller); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool storeObjectsToArchiveSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAArchiveWriterHostRef writer) noexcept
{
    __try {
        return api && api->structSize >= ARA_IMPLEMENTED_STRUCT_SIZE(
                   ARADocumentControllerInterface, storeObjectsToArchive) &&
            api->storeObjectsToArchive &&
            api->storeObjectsToArchive(controller, writer, nullptr) != ARA::kARAFalse;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool restoreObjectsFromArchiveSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAArchiveReaderHostRef reader) noexcept
{
    __try {
        return api && api->structSize >= ARA_IMPLEMENTED_STRUCT_SIZE(
                   ARADocumentControllerInterface, restoreObjectsFromArchive) &&
            api->restoreObjectsFromArchive &&
            api->restoreObjectsFromArchive(controller, reader, nullptr) != ARA::kARAFalse;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

ARA::ARAAudioSourceRef createAudioSourceSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAAudioSourceHostRef hostRef,
    const ARA::ARAAudioSourceProperties* properties) noexcept
{
    __try {
        return api && api->createAudioSource
            ? api->createAudioSource(controller, hostRef, properties) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void updateAudioSourcePropertiesSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAAudioSourceRef source,
    const ARA::ARAAudioSourceProperties* properties) noexcept
{
    __try {
        if (api && api->updateAudioSourceProperties)
            api->updateAudioSourceProperties(controller, source, properties);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void updateAudioSourceContentSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAAudioSourceRef source) noexcept
{
    __try {
        if (api && api->updateAudioSourceContent)
            api->updateAudioSourceContent(
                controller, source, nullptr, ARA::kARAContentUpdateEverythingChanged);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void enableAudioSourceSeh(const ARA::ARADocumentControllerInterface* api,
                          ARA::ARADocumentControllerRef controller,
                          ARA::ARAAudioSourceRef source,
                          bool enabled) noexcept
{
    __try {
        if (api && api->enableAudioSourceSamplesAccess)
            api->enableAudioSourceSamplesAccess(
                controller, source, enabled ? ARA::kARATrue : ARA::kARAFalse);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void destroyAudioSourceSeh(const ARA::ARADocumentControllerInterface* api,
                           ARA::ARADocumentControllerRef controller,
                           ARA::ARAAudioSourceRef source) noexcept
{
    __try {
        if (api && api->destroyAudioSource)
            api->destroyAudioSource(controller, source);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

ARA::ARAMusicalContextRef createMusicalContextSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAMusicalContextHostRef hostRef,
    const ARA::ARAMusicalContextProperties* properties) noexcept
{
    __try {
        return api && api->createMusicalContext
            ? api->createMusicalContext(controller, hostRef, properties) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void destroyMusicalContextSeh(const ARA::ARADocumentControllerInterface* api,
                              ARA::ARADocumentControllerRef controller,
                              ARA::ARAMusicalContextRef context) noexcept
{
    __try { if (api && api->destroyMusicalContext) api->destroyMusicalContext(controller, context); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void updateMusicalContextContentSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAMusicalContextRef context) noexcept
{
    __try {
        if (api && api->updateMusicalContextContent)
            api->updateMusicalContextContent(controller, context, nullptr,
                ARA::kARAContentUpdateEverythingChanged);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

ARA::ARARegionSequenceRef createRegionSequenceSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARARegionSequenceHostRef hostRef,
    const ARA::ARARegionSequenceProperties* properties) noexcept
{
    __try {
        return api && api->createRegionSequence
            ? api->createRegionSequence(controller, hostRef, properties) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void updateRegionSequenceSeh(const ARA::ARADocumentControllerInterface* api,
                             ARA::ARADocumentControllerRef controller,
                             ARA::ARARegionSequenceRef sequence,
                             const ARA::ARARegionSequenceProperties* properties) noexcept
{
    __try { if (api && api->updateRegionSequenceProperties) api->updateRegionSequenceProperties(controller, sequence, properties); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void destroyRegionSequenceSeh(const ARA::ARADocumentControllerInterface* api,
                              ARA::ARADocumentControllerRef controller,
                              ARA::ARARegionSequenceRef sequence) noexcept
{
    __try { if (api && api->destroyRegionSequence) api->destroyRegionSequence(controller, sequence); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

ARA::ARAAudioModificationRef createAudioModificationSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAAudioSourceRef source,
    ARA::ARAAudioModificationHostRef hostRef,
    const ARA::ARAAudioModificationProperties* properties) noexcept
{
    __try {
        return api && api->createAudioModification
            ? api->createAudioModification(controller, source, hostRef, properties) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void updateAudioModificationSeh(const ARA::ARADocumentControllerInterface* api,
                                ARA::ARADocumentControllerRef controller,
                                ARA::ARAAudioModificationRef modification,
                                const ARA::ARAAudioModificationProperties* properties) noexcept
{
    __try { if (api && api->updateAudioModificationProperties) api->updateAudioModificationProperties(controller, modification, properties); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void destroyAudioModificationSeh(const ARA::ARADocumentControllerInterface* api,
                                 ARA::ARADocumentControllerRef controller,
                                 ARA::ARAAudioModificationRef modification) noexcept
{
    __try { if (api && api->destroyAudioModification) api->destroyAudioModification(controller, modification); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

ARA::ARAPlaybackRegionRef createPlaybackRegionSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAAudioModificationRef modification,
    ARA::ARAPlaybackRegionHostRef hostRef,
    const ARA::ARAPlaybackRegionProperties* properties) noexcept
{
    __try {
        return api && api->createPlaybackRegion
            ? api->createPlaybackRegion(controller, modification, hostRef, properties) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void updatePlaybackRegionSeh(const ARA::ARADocumentControllerInterface* api,
                             ARA::ARADocumentControllerRef controller,
                             ARA::ARAPlaybackRegionRef region,
                             const ARA::ARAPlaybackRegionProperties* properties) noexcept
{
    __try { if (api && api->updatePlaybackRegionProperties) api->updatePlaybackRegionProperties(controller, region, properties); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void destroyPlaybackRegionSeh(const ARA::ARADocumentControllerInterface* api,
                              ARA::ARADocumentControllerRef controller,
                              ARA::ARAPlaybackRegionRef region) noexcept
{
    __try { if (api && api->destroyPlaybackRegion) api->destroyPlaybackRegion(controller, region); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

ARA::ARAPlaybackTransformationFlags supportedPlaybackTransformationFlagsSeh(
    const ARA::ARAFactory* factory) noexcept
{
    __try {
        if (!factory || factory->structSize < ARA::kARAFactoryMinSize)
            return ARA::kARAPlaybackTransformationNoChanges;
        return factory->supportedPlaybackTransformationFlags;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ARA::kARAPlaybackTransformationNoChanges;
    }
}

constexpr int kMaximumAraProcessingAlgorithms = 64;
constexpr size_t kMaximumAraAlgorithmTextBytes = 1024;

size_t copyPersistentIdSeh(const ARA::ARAPersistentID id,
                           char* output, size_t capacity) noexcept;

int copyAnalyzeableContentTypesSeh(
    const ARA::ARAFactory* factory,
    std::array<ARA::ARAContentType, kMaximumAraProcessingAlgorithms>& types) noexcept
{
    types.fill(ARA::kARAContentTypeNotes);
    __try {
        if (!factory || factory->structSize < ARA::kARAFactoryMinSize ||
            factory->analyzeableContentTypesCount == 0 ||
            factory->analyzeableContentTypesCount > types.size() ||
            !factory->analyzeableContentTypes) return 0;
        for (ARA::ARASize index = 0;
             index < factory->analyzeableContentTypesCount; ++index) {
            types[static_cast<size_t>(index)] = factory->analyzeableContentTypes[index];
            for (ARA::ARASize previous = 0; previous < index; ++previous)
                if (types[static_cast<size_t>(previous)] ==
                    types[static_cast<size_t>(index)]) return 0;
        }
        return static_cast<int>(factory->analyzeableContentTypesCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        types.fill(ARA::kARAContentTypeNotes);
        return 0;
    }
}

bool processingAlgorithmApiAvailableSeh(
    const ARA::ARADocumentControllerInterface* api) noexcept
{
    __try {
        return api && api->structSize >= ARA_IMPLEMENTED_STRUCT_SIZE(
            ARADocumentControllerInterface, requestProcessingAlgorithmForAudioSource) &&
            api->getProcessingAlgorithmsCount && api->getProcessingAlgorithmProperties &&
            api->getProcessingAlgorithmForAudioSource &&
            api->requestProcessingAlgorithmForAudioSource;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int processingAlgorithmCountSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller) noexcept
{
    __try {
        if (!processingAlgorithmApiAvailableSeh(api) || !controller) return 0;
        const ARA::ARAInt32 count = api->getProcessingAlgorithmsCount(controller);
        return count > 0 && count <= kMaximumAraProcessingAlgorithms
            ? static_cast<int>(count) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool copyProcessingAlgorithmSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller, int index,
    char* persistentId, size_t persistentIdCapacity,
    char* name, size_t nameCapacity) noexcept
{
    __try {
        if (!processingAlgorithmApiAvailableSeh(api) || !controller || index < 0)
            return false;
        const auto* properties = api->getProcessingAlgorithmProperties(
            controller, static_cast<ARA::ARAInt32>(index));
        if (!properties || properties->structSize < ARA::kARAProcessingAlgorithmPropertiesMinSize)
            return false;
        return copyPersistentIdSeh(
                   properties->persistentID, persistentId, persistentIdCapacity) > 0 &&
               copyPersistentIdSeh(properties->name, name, nameCapacity) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int currentProcessingAlgorithmSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAAudioSourceRef source, int count) noexcept
{
    __try {
        if (!processingAlgorithmApiAvailableSeh(api) || !controller || !source || count <= 0)
            return -1;
        const ARA::ARAInt32 index = api->getProcessingAlgorithmForAudioSource(
            controller, source);
        return index >= 0 && index < count ? static_cast<int>(index) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool requestProcessingAlgorithmSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAAudioSourceRef source, int index) noexcept
{
    __try {
        if (!processingAlgorithmApiAvailableSeh(api) || !controller || !source || index < 0)
            return false;
        api->requestProcessingAlgorithmForAudioSource(
            controller, source, static_cast<ARA::ARAInt32>(index));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool requestAudioSourceAnalysisSeh(
    const ARA::ARADocumentControllerInterface* api,
    ARA::ARADocumentControllerRef controller,
    ARA::ARAAudioSourceRef source, int contentTypeCount,
    const ARA::ARAContentType* contentTypes) noexcept
{
    __try {
        if (!api || !controller || !source || contentTypeCount <= 0 || !contentTypes ||
            api->structSize < ARA_IMPLEMENTED_STRUCT_SIZE(
                ARADocumentControllerInterface, requestAudioSourceContentAnalysis) ||
            !api->requestAudioSourceContentAnalysis) return false;
        api->requestAudioSourceContentAnalysis(
            controller, source, static_cast<ARA::ARASize>(contentTypeCount), contentTypes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

size_t copyFactoryIdSeh(const ARA::ARAFactory* factory, char* output, size_t capacity) noexcept
{
    __try {
        if (!factory || !factory->factoryID || !output || capacity < 2) return 0;
        size_t length = 0;
        while (length + 1 < capacity && factory->factoryID[length] != '\0') {
            output[length] = factory->factoryID[length];
            ++length;
        }
        if (length == 0 || factory->factoryID[length] != '\0') return 0;
        output[length] = '\0';
        return length;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

size_t copyFactoryPluginNameSeh(
    const ARA::ARAFactory* factory, char* output, size_t capacity) noexcept
{
    __try {
        return factory
            ? copyPersistentIdSeh(factory->plugInName, output, capacity) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

size_t copyPersistentIdSeh(const ARA::ARAPersistentID id,
                           char* output, size_t capacity) noexcept
{
    __try {
        if (!id || !output || capacity < 2) return 0;
        size_t length = 0;
        while (length + 1 < capacity && id[length] != '\0') {
            output[length] = id[length];
            ++length;
        }
        if (length == 0 || id[length] != '\0') return 0;
        output[length] = '\0';
        return length;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool archiveIdCompatibleSeh(const ARA::ARAFactory* factory,
                            const std::string& archiveId) noexcept
{
    __try {
        if (!factory || archiveId.empty()) return false;
        const auto matches = [&archiveId](ARA::ARAPersistentID candidate) noexcept {
            if (!candidate) return false;
            size_t index = 0;
            while (index < archiveId.size() && candidate[index] != '\0' &&
                   candidate[index] == archiveId[index]) ++index;
            return index == archiveId.size() && candidate[index] == '\0';
        };
        if (matches(factory->documentArchiveID)) return true;
        if (factory->compatibleDocumentArchiveIDsCount > 0 &&
            !factory->compatibleDocumentArchiveIDs) return false;
        for (ARA::ARASize index = 0;
             index < factory->compatibleDocumentArchiveIDsCount; ++index)
            if (matches(factory->compatibleDocumentArchiveIDs[index])) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool factoryApiRangeSeh(const ARA::ARAFactory* factory,
                        ARA::ARAAPIGeneration& lowest,
                        ARA::ARAAPIGeneration& highest) noexcept
{
    __try {
        if (!factory) return false;
        lowest = factory->lowestSupportedApiGeneration;
        highest = factory->highestSupportedApiGeneration;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool validControllerInstanceSeh(const ARA::ARADocumentControllerInstance* instance) noexcept
{
    __try {
        return instance && instance->structSize >= ARA::kARADocumentControllerInstanceMinSize &&
            instance->documentControllerRef && instance->documentControllerInterface &&
            instance->documentControllerInterface->structSize >=
                ARA::kARADocumentControllerInterfaceMinSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

ARA::ARADocumentControllerRef documentControllerRefSeh(
    const ARA::ARADocumentControllerInstance* instance) noexcept
{
    __try {
        return instance ? instance->documentControllerRef : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

AraBindingStatus validateBindingSeh(
    const ARA::ARAPlugInExtensionInstance* instance,
    ARA::ARAPlugInInstanceRoleFlags assignedRoles,
    bool documentReady) noexcept
{
    AraBindingStatus result;
    result.documentController = documentReady;
    result.contentAccess = documentReady;
    __try {
        if (!instance || instance->structSize < ARA_IMPLEMENTED_STRUCT_SIZE(
                ARAPlugInExtensionInstance, editorViewInterface)) return result;
        if ((assignedRoles & ARA::kARAPlaybackRendererRole) != 0) {
            result.playbackRenderer = instance->playbackRendererRef &&
                instance->playbackRendererInterface &&
                instance->playbackRendererInterface->structSize >=
                    ARA::kARAPlaybackRendererInterfaceMinSize;
        }
        if ((assignedRoles & ARA::kARAEditorRendererRole) != 0) {
            result.editorRenderer = instance->editorRendererRef &&
                instance->editorRendererInterface &&
                instance->editorRendererInterface->structSize >=
                    ARA::kARAEditorRendererInterfaceMinSize;
        }
        if ((assignedRoles & ARA::kARAEditorViewRole) != 0) {
            result.editorView = instance->editorViewRef && instance->editorViewInterface &&
                instance->editorViewInterface->structSize >= ARA::kARAEditorViewInterfaceMinSize;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.playbackRenderer = false;
        result.editorRenderer = false;
        result.editorView = false;
    }
    return result;
}

std::string factoryKey(const ARA::ARAFactory* factory)
{
    std::array<char, 1024> id{};
    const size_t length = copyFactoryIdSeh(factory, id.data(), id.size());
    if (length == 0) return {};
    std::string key(id.data(), length);
    key.push_back('@');
    key += std::to_string(reinterpret_cast<uintptr_t>(factory));
    return key;
}
} // namespace

struct AraDocumentContext::Impl
{
    static constexpr size_t kMaxAraArchiveBytes = 512u * 1024u * 1024u;
    static constexpr std::uint32_t kSeekRequestFlag = 1u << 0;
    static constexpr std::uint32_t kCycleRangeRequestFlag = 1u << 1;
    static constexpr std::uint32_t kCycleEnableRequestFlag = 1u << 2;

    struct MusicalContextState
    {
        ARA::ARAMusicalContextRef araRef = nullptr;
    };

    struct RegionSequenceState
    {
        std::string persistentId;
        std::string nameUtf8;
        int order = 0;
        ARA::ARARegionSequenceRef araRef = nullptr;
    };

    struct AudioSourceState
    {
        std::string persistentId;
        std::string nameUtf8;
        int sampleRate = 0;
        int channelCount = 0;
        std::shared_ptr<const std::vector<float>> samples;
        ARA::ARAAudioSourceRef araRef = nullptr;
        std::atomic<bool> accessEnabled{false};
        std::atomic<bool> analysisActive{false};
        std::atomic<float> analysisProgress{0.0f};
    };

    struct AudioModificationState
    {
        std::string persistentId;
        std::string sourceId;
        std::string nameUtf8;
        ARA::ARAAudioModificationRef araRef = nullptr;
    };

    struct PlaybackRegionState
    {
        std::string persistentId;
        std::string sourceId;
        std::string sequenceId;
        std::string nameUtf8;
        int routingTrackIndex = -1;
        double sourceStartSeconds = 0.0;
        double sourceDurationSeconds = 0.0;
        double playbackStartSeconds = 0.0;
        double playbackDurationSeconds = 0.0;
        bool timeStretch = false;
        ARA::ARAPlaybackRegionRef araRef = nullptr;
    };

    struct AudioReader
    {
        std::shared_ptr<const std::vector<float>> samples;
        ARA::ARAChannelCount channelCount = 0;
        bool use64BitSamples = false;
    };

    struct ContentReader
    {
        ARA::ARAContentType type = ARA::kARAContentTypeTempoEntries;
        std::vector<ARA::ARAContentTempoEntry> tempoEntries;
        std::vector<ARA::ARAContentBarSignature> barSignatures;
    };

    struct ArchiveReader
    {
        const std::vector<std::uint8_t>* bytes = nullptr;
        std::string documentArchiveId;
    };

    struct ArchiveWriter
    {
        std::vector<std::uint8_t>* bytes = nullptr;
        bool failed = false;
    };

    const ARA::ARAFactory* factory = nullptr;
    ARA::ARAPlaybackTransformationFlags supportedPlaybackTransformationFlags =
        ARA::kARAPlaybackTransformationNoChanges;
    std::array<ARA::ARAContentType, kMaximumAraProcessingAlgorithms>
        analyzeableContentTypes{};
    int analyzeableContentTypeCount = 0;
    const ARA::ARADocumentControllerInstance* controller = nullptr;
    std::mutex controllerMutex;
    bool initialized = false;
    std::atomic<bool> documentDirty{false};
    std::atomic<float> archiveProgress{0.0f};
    std::atomic<float> unarchiveProgress{0.0f};
    std::atomic<bool> archiveActive{false};
    std::atomic<bool> unarchiving{false};
    std::atomic<int> playbackRequest{0};
    std::atomic<std::uint32_t> transportRequestFlags{0};
    std::atomic<double> requestedPosition{0.0};
    std::atomic<double> requestedCycleStart{0.0};
    std::atomic<double> requestedCycleDuration{0.0};
    std::atomic<bool> requestedCycleEnabled{false};

    ARA::ARAAudioAccessControllerInterface audio{};
    ARA::ARAArchivingControllerInterface archiving{};
    ARA::ARAContentAccessControllerInterface content{};
    ARA::ARAModelUpdateControllerInterface modelUpdate{};
    ARA::ARAPlaybackControllerInterface playback{};
    ARA::ARADocumentControllerHostInstance host{};
    MusicalContextState musicalContext;
    std::unordered_map<std::string, std::unique_ptr<RegionSequenceState>> regionSequences;
    std::unordered_map<std::string, std::unique_ptr<AudioSourceState>> audioSources;
    std::unordered_map<std::string, std::unique_ptr<AudioModificationState>> audioModifications;
    std::unordered_map<std::string, std::unique_ptr<PlaybackRegionState>> playbackRegions;
    std::vector<ARA::ARAContentTempoEntry> tempoEntries;
    std::vector<ARA::ARAContentBarSignature> barSignatures;

    ~Impl()
    {
        clearModelGraph();
        destroyDocumentController(controller);
        controller = nullptr;
        if (initialized) uninitializeFactory(factory);
        initialized = false;
    }

    static Impl* from(const void* ref) noexcept
    {
        return const_cast<Impl*>(static_cast<const Impl*>(ref));
    }

    static ARA::ARAAudioReaderHostRef ARA_CALL createAudioReader(
        ARA::ARAAudioAccessControllerHostRef, ARA::ARAAudioSourceHostRef sourceRef,
        ARA::ARABool use64BitSamples) noexcept
    {
        auto* source = reinterpret_cast<AudioSourceState*>(sourceRef);
        if (!source || !source->accessEnabled.load(std::memory_order_acquire) ||
            !source->samples || source->sampleRate <= 0 ||
            (source->channelCount != 1 && source->channelCount != 2)) return nullptr;
        auto* reader = new (std::nothrow) AudioReader;
        if (!reader) return nullptr;
        reader->samples = source->samples;
        reader->channelCount = static_cast<ARA::ARAChannelCount>(source->channelCount);
        reader->use64BitSamples = use64BitSamples != ARA::kARAFalse;
        return reinterpret_cast<ARA::ARAAudioReaderHostRef>(reader);
    }
    static ARA::ARABool ARA_CALL readAudio(
        ARA::ARAAudioAccessControllerHostRef, ARA::ARAAudioReaderHostRef,
        ARA::ARASamplePosition, ARA::ARASampleCount, void* const[]) noexcept;
    static void ARA_CALL destroyAudioReader(
        ARA::ARAAudioAccessControllerHostRef, ARA::ARAAudioReaderHostRef readerRef) noexcept
    {
        delete reinterpret_cast<AudioReader*>(readerRef);
    }

    void clearModelGraph() noexcept;
    bool syncAudioSources(const std::vector<AraAudioSourceDescriptor>& sources);
    bool syncModelGraph(const std::vector<AraAudioSourceDescriptor>& sources,
                        const std::vector<AraPlaybackRegionDescriptor>& regions,
                        const std::vector<AraTempoEntryDescriptor>& incomingTempoEntries,
                        const std::vector<AraBarSignatureDescriptor>& incomingBarSignatures);
    static std::string normalizedPersistentId(const std::string& sourceId);
    static std::string normalizedObjectId(const char* prefix, const std::string& objectId);
    static void fillSourceProperties(const AudioSourceState& source,
                                     ARA::ARAAudioSourceProperties& properties) noexcept;

    static ARA::ARASize ARA_CALL archiveSize(
        ARA::ARAArchivingControllerHostRef, ARA::ARAArchiveReaderHostRef readerRef) noexcept
    {
        const auto* reader = reinterpret_cast<const ArchiveReader*>(readerRef);
        return reader && reader->bytes
            ? static_cast<ARA::ARASize>(reader->bytes->size()) : 0;
    }
    static ARA::ARABool ARA_CALL readArchive(
        ARA::ARAArchivingControllerHostRef, ARA::ARAArchiveReaderHostRef readerRef,
        ARA::ARASize position, ARA::ARASize length, ARA::ARAByte buffer[]) noexcept
    {
        const auto* reader = reinterpret_cast<const ArchiveReader*>(readerRef);
        if (!reader || !reader->bytes || (length > 0 && !buffer)) return ARA::kARAFalse;
        const size_t size = reader->bytes->size();
        if (position > size || length > size - static_cast<size_t>(position))
            return ARA::kARAFalse;
        if (length > 0)
            std::memcpy(buffer, reader->bytes->data() + static_cast<size_t>(position),
                        static_cast<size_t>(length));
        return ARA::kARATrue;
    }
    static ARA::ARABool ARA_CALL writeArchive(
        ARA::ARAArchivingControllerHostRef, ARA::ARAArchiveWriterHostRef writerRef,
        ARA::ARASize position, ARA::ARASize length, const ARA::ARAByte buffer[]) noexcept
    {
        auto* writer = reinterpret_cast<ArchiveWriter*>(writerRef);
        if (!writer || !writer->bytes || writer->failed || (length > 0 && !buffer))
            return ARA::kARAFalse;
        if (position > kMaxAraArchiveBytes ||
            length > kMaxAraArchiveBytes - static_cast<size_t>(position)) {
            writer->failed = true;
            return ARA::kARAFalse;
        }
        try {
            const size_t end = static_cast<size_t>(position + length);
            if (writer->bytes->size() < end) writer->bytes->resize(end, 0);
            if (length > 0)
                std::memcpy(writer->bytes->data() + static_cast<size_t>(position), buffer,
                            static_cast<size_t>(length));
            return ARA::kARATrue;
        } catch (...) {
            writer->failed = true;
            return ARA::kARAFalse;
        }
    }
    static void ARA_CALL archiveProgressChanged(ARA::ARAArchivingControllerHostRef ref, float value) noexcept
    {
        if (auto* self = from(ref)) self->archiveProgress.store(std::clamp(value, 0.0f, 1.0f));
    }
    static void ARA_CALL unarchiveProgressChanged(ARA::ARAArchivingControllerHostRef ref, float value) noexcept
    {
        if (auto* self = from(ref)) self->unarchiveProgress.store(std::clamp(value, 0.0f, 1.0f));
    }
    static ARA::ARAPersistentID ARA_CALL archiveId(
        ARA::ARAArchivingControllerHostRef, ARA::ARAArchiveReaderHostRef readerRef) noexcept
    {
        const auto* reader = reinterpret_cast<const ArchiveReader*>(readerRef);
        return reader && !reader->documentArchiveId.empty()
            ? reader->documentArchiveId.c_str() : nullptr;
    }

    static ARA::ARABool ARA_CALL musicalContentAvailable(
        ARA::ARAContentAccessControllerHostRef ref, ARA::ARAMusicalContextHostRef contextRef,
        ARA::ARAContentType type) noexcept
    {
        auto* self = from(ref);
        if (!self || contextRef != reinterpret_cast<ARA::ARAMusicalContextHostRef>(
                &self->musicalContext)) return ARA::kARAFalse;
        if (type == ARA::kARAContentTypeTempoEntries)
            return self->tempoEntries.size() >= 2u ? ARA::kARATrue : ARA::kARAFalse;
        if (type == ARA::kARAContentTypeBarSignatures)
            return self->tempoEntries.size() >= 2u && !self->barSignatures.empty()
                ? ARA::kARATrue : ARA::kARAFalse;
        return ARA::kARAFalse;
    }
    static ARA::ARAContentGrade ARA_CALL musicalContentGrade(
        ARA::ARAContentAccessControllerHostRef ref, ARA::ARAMusicalContextHostRef contextRef,
        ARA::ARAContentType type) noexcept
    {
        return musicalContentAvailable(ref, contextRef, type) != ARA::kARAFalse
            ? ARA::kARAContentGradeApproved : ARA::kARAContentGradeInitial;
    }
    static ARA::ARAContentReaderHostRef ARA_CALL createMusicalContentReader(
        ARA::ARAContentAccessControllerHostRef ref, ARA::ARAMusicalContextHostRef contextRef,
        ARA::ARAContentType type, const ARA::ARAContentTimeRange*) noexcept
    {
        auto* self = from(ref);
        if (musicalContentAvailable(ref, contextRef, type) == ARA::kARAFalse) return nullptr;
        auto* reader = new (std::nothrow) ContentReader;
        if (!reader) return nullptr;
        try {
            reader->type = type;
            if (type == ARA::kARAContentTypeTempoEntries)
                reader->tempoEntries = self->tempoEntries;
            else
                reader->barSignatures = self->barSignatures;
        } catch (...) {
            delete reader;
            return nullptr;
        }
        return reinterpret_cast<ARA::ARAContentReaderHostRef>(reader);
    }
    static ARA::ARABool ARA_CALL noAudioContent(
        ARA::ARAContentAccessControllerHostRef, ARA::ARAAudioSourceHostRef,
        ARA::ARAContentType) noexcept { return ARA::kARAFalse; }
    static ARA::ARAContentGrade ARA_CALL initialAudioGrade(
        ARA::ARAContentAccessControllerHostRef, ARA::ARAAudioSourceHostRef,
        ARA::ARAContentType) noexcept { return ARA::kARAContentGradeInitial; }
    static ARA::ARAContentReaderHostRef ARA_CALL noAudioReader(
        ARA::ARAContentAccessControllerHostRef, ARA::ARAAudioSourceHostRef,
        ARA::ARAContentType, const ARA::ARAContentTimeRange*) noexcept { return nullptr; }
    static ARA::ARAInt32 ARA_CALL contentEventCount(
        ARA::ARAContentAccessControllerHostRef, ARA::ARAContentReaderHostRef readerRef) noexcept
    {
        const auto* reader = reinterpret_cast<const ContentReader*>(readerRef);
        if (!reader) return 0;
        const size_t count = reader->type == ARA::kARAContentTypeTempoEntries
            ? reader->tempoEntries.size() : reader->barSignatures.size();
        return static_cast<ARA::ARAInt32>(std::min<size_t>(
            count, static_cast<size_t>(std::numeric_limits<ARA::ARAInt32>::max())));
    }
    static const void* ARA_CALL contentData(
        ARA::ARAContentAccessControllerHostRef, ARA::ARAContentReaderHostRef readerRef,
        ARA::ARAInt32 index) noexcept
    {
        const auto* reader = reinterpret_cast<const ContentReader*>(readerRef);
        if (!reader || index < 0) return nullptr;
        const size_t safeIndex = static_cast<size_t>(index);
        if (reader->type == ARA::kARAContentTypeTempoEntries)
            return safeIndex < reader->tempoEntries.size() ? &reader->tempoEntries[safeIndex] : nullptr;
        return safeIndex < reader->barSignatures.size() ? &reader->barSignatures[safeIndex] : nullptr;
    }
    static void ARA_CALL destroyContentReader(
        ARA::ARAContentAccessControllerHostRef, ARA::ARAContentReaderHostRef readerRef) noexcept
    {
        delete reinterpret_cast<ContentReader*>(readerRef);
    }

    static void ARA_CALL analysisChanged(
        ARA::ARAModelUpdateControllerHostRef, ARA::ARAAudioSourceHostRef sourceRef,
        ARA::ARAAnalysisProgressState state, float value) noexcept
    {
        auto* source = reinterpret_cast<AudioSourceState*>(sourceRef);
        if (!source) return;
        if (state == ARA::kARAAnalysisProgressStarted)
            source->analysisActive.store(true, std::memory_order_release);
        source->analysisProgress.store(
            std::clamp(value, 0.0f, 1.0f), std::memory_order_release);
        if (state == ARA::kARAAnalysisProgressCompleted)
            source->analysisActive.store(false, std::memory_order_release);
    }
    static void ARA_CALL sourceContentChanged(
        ARA::ARAModelUpdateControllerHostRef ref, ARA::ARAAudioSourceHostRef,
        const ARA::ARAContentTimeRange*, ARA::ARAContentUpdateFlags) noexcept
    {
        if (auto* self = from(ref)) self->documentDirty.store(true, std::memory_order_release);
    }
    static void ARA_CALL modificationContentChanged(
        ARA::ARAModelUpdateControllerHostRef ref, ARA::ARAAudioModificationHostRef,
        const ARA::ARAContentTimeRange*, ARA::ARAContentUpdateFlags) noexcept
    {
        if (auto* self = from(ref)) self->documentDirty.store(true, std::memory_order_release);
    }
    static void ARA_CALL regionContentChanged(
        ARA::ARAModelUpdateControllerHostRef ref, ARA::ARAPlaybackRegionHostRef,
        const ARA::ARAContentTimeRange*, ARA::ARAContentUpdateFlags) noexcept
    {
        if (auto* self = from(ref)) self->documentDirty.store(true, std::memory_order_release);
    }
    static void ARA_CALL documentDataChanged(ARA::ARAModelUpdateControllerHostRef ref) noexcept
    {
        if (auto* self = from(ref)) self->documentDirty.store(true, std::memory_order_release);
    }

    static void ARA_CALL startPlayback(ARA::ARAPlaybackControllerHostRef ref) noexcept
    {
        if (auto* self = from(ref)) self->playbackRequest.store(1, std::memory_order_release);
    }
    static void ARA_CALL stopPlayback(ARA::ARAPlaybackControllerHostRef ref) noexcept
    {
        if (auto* self = from(ref)) self->playbackRequest.store(-1, std::memory_order_release);
    }
    static void ARA_CALL setPlaybackPosition(
        ARA::ARAPlaybackControllerHostRef ref, ARA::ARATimePosition position) noexcept
    {
        if (auto* self = from(ref)) {
            self->requestedPosition.store(position, std::memory_order_relaxed);
            self->transportRequestFlags.fetch_or(kSeekRequestFlag, std::memory_order_release);
        }
    }
    static void ARA_CALL setCycleRange(
        ARA::ARAPlaybackControllerHostRef ref, ARA::ARATimePosition start,
        ARA::ARATimeDuration duration) noexcept
    {
        if (auto* self = from(ref)) {
            self->requestedCycleStart.store(start, std::memory_order_release);
            self->requestedCycleDuration.store(std::max(0.0, duration), std::memory_order_relaxed);
            self->transportRequestFlags.fetch_or(
                kCycleRangeRequestFlag, std::memory_order_release);
        }
    }
    static void ARA_CALL enableCycle(ARA::ARAPlaybackControllerHostRef ref, ARA::ARABool enabled) noexcept
    {
        if (auto* self = from(ref)) {
            self->requestedCycleEnabled.store(enabled != ARA::kARAFalse, std::memory_order_release);
            self->transportRequestFlags.fetch_or(
                kCycleEnableRequestFlag, std::memory_order_release);
        }
    }

    void configureInterfaces() noexcept
    {
        audio.structSize = ARA::kARAAudioAccessControllerInterfaceMinSize;
        audio.createAudioReaderForSource = &createAudioReader;
        audio.readAudioSamples = &readAudio;
        audio.destroyAudioReader = &destroyAudioReader;

        archiving.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(
            ARAArchivingControllerInterface, getDocumentArchiveID);
        archiving.getArchiveSize = &archiveSize;
        archiving.readBytesFromArchive = &readArchive;
        archiving.writeBytesToArchive = &writeArchive;
        archiving.notifyDocumentArchivingProgress = &archiveProgressChanged;
        archiving.notifyDocumentUnarchivingProgress = &unarchiveProgressChanged;
        archiving.getDocumentArchiveID = &archiveId;

        content.structSize = ARA::kARAContentAccessControllerInterfaceMinSize;
        content.isMusicalContextContentAvailable = &musicalContentAvailable;
        content.getMusicalContextContentGrade = &musicalContentGrade;
        content.createMusicalContextContentReader = &createMusicalContentReader;
        content.isAudioSourceContentAvailable = &noAudioContent;
        content.getAudioSourceContentGrade = &initialAudioGrade;
        content.createAudioSourceContentReader = &noAudioReader;
        content.getContentReaderEventCount = &contentEventCount;
        content.getContentReaderDataForEvent = &contentData;
        content.destroyContentReader = &destroyContentReader;

        modelUpdate.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(
            ARAModelUpdateControllerInterface, notifyDocumentDataChanged);
        modelUpdate.notifyAudioSourceAnalysisProgress = &analysisChanged;
        modelUpdate.notifyAudioSourceContentChanged = &sourceContentChanged;
        modelUpdate.notifyAudioModificationContentChanged = &modificationContentChanged;
        modelUpdate.notifyPlaybackRegionContentChanged = &regionContentChanged;
        modelUpdate.notifyDocumentDataChanged = &documentDataChanged;

        playback.structSize = ARA::kARAPlaybackControllerInterfaceMinSize;
        playback.requestStartPlayback = &startPlayback;
        playback.requestStopPlayback = &stopPlayback;
        playback.requestSetPlaybackPosition = &setPlaybackPosition;
        playback.requestSetCycleRange = &setCycleRange;
        playback.requestEnableCycle = &enableCycle;

        host.structSize = ARA::kARADocumentControllerHostInstanceMinSize;
        host.audioAccessControllerHostRef = reinterpret_cast<ARA::ARAAudioAccessControllerHostRef>(this);
        host.audioAccessControllerInterface = &audio;
        host.archivingControllerHostRef = reinterpret_cast<ARA::ARAArchivingControllerHostRef>(this);
        host.archivingControllerInterface = &archiving;
        host.contentAccessControllerHostRef = reinterpret_cast<ARA::ARAContentAccessControllerHostRef>(this);
        host.contentAccessControllerInterface = &content;
        host.modelUpdateControllerHostRef = reinterpret_cast<ARA::ARAModelUpdateControllerHostRef>(this);
        host.modelUpdateControllerInterface = &modelUpdate;
        host.playbackControllerHostRef = reinterpret_cast<ARA::ARAPlaybackControllerHostRef>(this);
        host.playbackControllerInterface = &playback;
    }
};

ARA::ARABool ARA_CALL AraDocumentContext::Impl::readAudio(
    ARA::ARAAudioAccessControllerHostRef,
    ARA::ARAAudioReaderHostRef readerRef,
    ARA::ARASamplePosition samplePosition,
    ARA::ARASampleCount samplesPerChannel,
    void* const buffers[]) noexcept
{
    const auto* reader = reinterpret_cast<const AudioReader*>(readerRef);
    if (!reader || !reader->samples || !buffers || samplesPerChannel < 0 ||
        (reader->channelCount != 1 && reader->channelCount != 2)) return ARA::kARAFalse;
    if (samplesPerChannel == 0) return ARA::kARATrue;
    for (ARA::ARAChannelCount channel = 0; channel < reader->channelCount; ++channel)
        if (!buffers[channel]) return ARA::kARAFalse;

    const uint64_t requested = static_cast<uint64_t>(samplesPerChannel);
    const uint64_t available = static_cast<uint64_t>(reader->samples->size() / 2u);
    const uint64_t negativeOffset = samplePosition < 0
        ? static_cast<uint64_t>(-(samplePosition + 1)) + 1u : 0u;
    for (uint64_t offset = 0; offset < requested; ++offset) {
        bool inRange = false;
        uint64_t frame = 0;
        if (samplePosition >= 0) {
            const uint64_t start = static_cast<uint64_t>(samplePosition);
            inRange = start < available && offset < available - start;
            if (inRange) frame = start + offset;
        } else if (offset >= negativeOffset) {
            frame = offset - negativeOffset;
            inRange = frame < available;
        }
        for (ARA::ARAChannelCount channel = 0; channel < reader->channelCount; ++channel) {
            const float value = inRange
                ? (*reader->samples)[static_cast<size_t>(frame * 2u +
                    static_cast<uint64_t>(channel))] : 0.0f;
            if (reader->use64BitSamples)
                static_cast<double*>(buffers[channel])[offset] = static_cast<double>(value);
            else
                static_cast<float*>(buffers[channel])[offset] = value;
        }
    }
    return ARA::kARATrue;
}

std::string AraDocumentContext::Impl::normalizedPersistentId(const std::string& sourceId)
{
    return normalizedObjectId("audio", sourceId);
}

std::string AraDocumentContext::Impl::normalizedObjectId(
    const char* prefix, const std::string& objectId)
{
    constexpr size_t kMaximumSourceIdBytes = 512;
    if (!prefix || !*prefix || objectId.empty() || objectId.size() > kMaximumSourceIdBytes) return {};
    std::string result("com.veracore.");
    result += prefix;
    result.push_back('.');
    result.reserve(result.size() + objectId.size());
    for (const unsigned char byte : objectId) {
        if (byte == 0 || byte > 0x7f) return {};
        const bool accepted = (byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            byte == '.' || byte == '-' || byte == '_';
        result.push_back(accepted ? static_cast<char>(byte) : '_');
    }
    return result;
}

void AraDocumentContext::Impl::fillSourceProperties(
    const AudioSourceState& source, ARA::ARAAudioSourceProperties& properties) noexcept
{
    properties = {};
    properties.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(
        ARAAudioSourceProperties, channelArrangement);
    properties.name = source.nameUtf8.empty() ? nullptr : source.nameUtf8.c_str();
    properties.persistentID = source.persistentId.c_str();
    properties.sampleCount = source.samples
        ? static_cast<ARA::ARASampleCount>(source.samples->size() / 2u) : 0;
    properties.sampleRate = static_cast<ARA::ARASampleRate>(source.sampleRate);
    properties.channelCount = static_cast<ARA::ARAChannelCount>(source.channelCount);
    properties.merits64BitSamples = ARA::kARAFalse;
    properties.channelArrangementDataType = ARA::kARAChannelArrangementUndefined;
    properties.channelArrangement = nullptr;
}

void AraDocumentContext::Impl::clearModelGraph() noexcept
{
    if (!controller) {
        playbackRegions.clear();
        audioModifications.clear();
        audioSources.clear();
        regionSequences.clear();
        musicalContext.araRef = nullptr;
        return;
    }
    const auto* api = controllerInterfaceSeh(controller);
    const auto controllerRef = documentControllerRefSeh(controller);
    for (auto& entry : audioSources) {
        AudioSourceState& source = *entry.second;
        if (source.araRef && source.accessEnabled.exchange(false, std::memory_order_acq_rel))
            enableAudioSourceSeh(api, controllerRef, source.araRef, false);
    }
    beginEditingSeh(api, controllerRef);
    for (auto& entry : playbackRegions)
        if (entry.second->araRef) destroyPlaybackRegionSeh(api, controllerRef, entry.second->araRef);
    playbackRegions.clear();
    for (auto& entry : audioModifications)
        if (entry.second->araRef) destroyAudioModificationSeh(api, controllerRef, entry.second->araRef);
    audioModifications.clear();
    for (auto& entry : audioSources)
        if (entry.second->araRef) destroyAudioSourceSeh(api, controllerRef, entry.second->araRef);
    audioSources.clear();
    for (auto& entry : regionSequences)
        if (entry.second->araRef) destroyRegionSequenceSeh(api, controllerRef, entry.second->araRef);
    regionSequences.clear();
    if (musicalContext.araRef)
        destroyMusicalContextSeh(api, controllerRef, musicalContext.araRef);
    musicalContext.araRef = nullptr;
    endEditingSeh(api, controllerRef);
}

bool AraDocumentContext::Impl::syncAudioSources(
    const std::vector<AraAudioSourceDescriptor>& sources)
{
    if (!controller) return false;
    const auto* api = controllerInterfaceSeh(controller);
    const auto controllerRef = documentControllerRefSeh(controller);
    if (!api || !controllerRef) return false;

    std::unordered_map<std::string, AraAudioSourceDescriptor> desired;
    desired.reserve(sources.size());
    for (const auto& descriptor : sources) {
        const std::string id = normalizedPersistentId(descriptor.persistentId);
        if (id.empty() || descriptor.sampleRate < 8000 || descriptor.sampleRate > 384000 ||
            (descriptor.channelCount != 1 && descriptor.channelCount != 2) ||
            !descriptor.interleavedStereoSamples ||
            descriptor.interleavedStereoSamples->size() < 2u ||
            (descriptor.interleavedStereoSamples->size() % 2u) != 0u ||
            descriptor.interleavedStereoSamples->size() / 2u >
                static_cast<size_t>(std::numeric_limits<ARA::ARASampleCount>::max())) continue;
        desired.emplace(id, descriptor);
    }

    std::vector<std::string> removed;
    std::vector<AudioSourceState*> metadataChanged;
    std::vector<AudioSourceState*> contentChanged;
    std::vector<AudioSourceState*> created;
    removed.reserve(audioSources.size());
    for (const auto& entry : audioSources)
        if (desired.find(entry.first) == desired.end()) removed.push_back(entry.first);

    for (const std::string& id : removed) {
        AudioSourceState& source = *audioSources.at(id);
        if (source.araRef && source.accessEnabled.exchange(false, std::memory_order_acq_rel))
            enableAudioSourceSeh(api, controllerRef, source.araRef, false);
    }

    for (const auto& entry : desired) {
        const auto found = audioSources.find(entry.first);
        const auto& descriptor = entry.second;
        const std::string safeName = descriptor.nameUtf8.size() <= 1024u
            ? descriptor.nameUtf8 : std::string{};
        if (found == audioSources.end()) {
            auto source = std::make_unique<AudioSourceState>();
            source->persistentId = entry.first;
            source->nameUtf8 = safeName;
            source->sampleRate = descriptor.sampleRate;
            source->channelCount = descriptor.channelCount;
            source->samples = descriptor.interleavedStereoSamples;
            AudioSourceState* sourcePtr = source.get();
            audioSources.emplace(entry.first, std::move(source));
            created.push_back(sourcePtr);
            continue;
        }

        AudioSourceState& source = *found->second;
        const bool dataChanged = source.sampleRate != descriptor.sampleRate ||
            source.channelCount != descriptor.channelCount ||
            source.samples.get() != descriptor.interleavedStereoSamples.get();
        const bool nameChanged = source.nameUtf8 != safeName;
        if (dataChanged && source.araRef &&
            source.accessEnabled.exchange(false, std::memory_order_acq_rel))
            enableAudioSourceSeh(api, controllerRef, source.araRef, false);
        source.nameUtf8 = safeName;
        source.sampleRate = descriptor.sampleRate;
        source.channelCount = descriptor.channelCount;
        source.samples = descriptor.interleavedStereoSamples;
        if (dataChanged) contentChanged.push_back(&source);
        else if (nameChanged) metadataChanged.push_back(&source);
    }

    beginEditingSeh(api, controllerRef);
    for (const std::string& id : removed) {
        auto found = audioSources.find(id);
        if (found != audioSources.end() && found->second->araRef)
            destroyAudioSourceSeh(api, controllerRef, found->second->araRef);
        audioSources.erase(id);
    }
    for (AudioSourceState* source : metadataChanged) {
        ARA::ARAAudioSourceProperties properties{};
        fillSourceProperties(*source, properties);
        updateAudioSourcePropertiesSeh(api, controllerRef, source->araRef, &properties);
    }
    for (AudioSourceState* source : contentChanged) {
        ARA::ARAAudioSourceProperties properties{};
        fillSourceProperties(*source, properties);
        updateAudioSourcePropertiesSeh(api, controllerRef, source->araRef, &properties);
        updateAudioSourceContentSeh(api, controllerRef, source->araRef);
    }
    for (AudioSourceState* source : created) {
        ARA::ARAAudioSourceProperties properties{};
        fillSourceProperties(*source, properties);
        source->araRef = createAudioSourceSeh(api, controllerRef,
            reinterpret_cast<ARA::ARAAudioSourceHostRef>(source), &properties);
    }
    endEditingSeh(api, controllerRef);
    for (AudioSourceState* source : contentChanged) {
        if (source->araRef) {
            source->accessEnabled.store(true, std::memory_order_release);
            enableAudioSourceSeh(api, controllerRef, source->araRef, true);
        }
    }
    for (AudioSourceState* source : created) {
        if (source->araRef) {
            source->accessEnabled.store(true, std::memory_order_release);
            enableAudioSourceSeh(api, controllerRef, source->araRef, true);
        }
    }
    notifyModelUpdatesSeh(api, controllerRef);
    for (auto it = audioSources.begin(); it != audioSources.end();) {
        if (!it->second->araRef) it = audioSources.erase(it);
        else ++it;
    }
    return audioSources.size() == desired.size();
}

bool AraDocumentContext::Impl::syncModelGraph(
    const std::vector<AraAudioSourceDescriptor>& sources,
    const std::vector<AraPlaybackRegionDescriptor>& regions,
    const std::vector<AraTempoEntryDescriptor>& incomingTempoEntries,
    const std::vector<AraBarSignatureDescriptor>& incomingBarSignatures)
{
    if (!controller) return false;
    const auto* api = controllerInterfaceSeh(controller);
    const auto controllerRef = documentControllerRefSeh(controller);
    if (!api || !controllerRef) return false;

    std::vector<ARA::ARAContentTempoEntry> nextTempoEntries;
    nextTempoEntries.reserve(incomingTempoEntries.size());
    for (const auto& entry : incomingTempoEntries) {
        if (!std::isfinite(entry.timeSeconds) || entry.timeSeconds < 0.0 ||
            !std::isfinite(entry.quarterPosition) || entry.quarterPosition < 0.0) continue;
        nextTempoEntries.push_back({entry.timeSeconds, entry.quarterPosition});
    }
    std::sort(nextTempoEntries.begin(), nextTempoEntries.end(),
        [](const auto& left, const auto& right) {
            return left.quarterPosition < right.quarterPosition;
        });
    nextTempoEntries.erase(std::unique(nextTempoEntries.begin(), nextTempoEntries.end(),
        [](const auto& left, const auto& right) {
            return left.quarterPosition == right.quarterPosition;
        }), nextTempoEntries.end());
    for (size_t index = 1; index < nextTempoEntries.size();) {
        if (nextTempoEntries[index].timePosition <= nextTempoEntries[index - 1].timePosition)
            nextTempoEntries.erase(nextTempoEntries.begin() + static_cast<std::ptrdiff_t>(index));
        else ++index;
    }
    if (nextTempoEntries.size() < 2u || nextTempoEntries.front().quarterPosition != 0.0 ||
        nextTempoEntries.front().timePosition != 0.0) nextTempoEntries.clear();

    std::vector<ARA::ARAContentBarSignature> nextBarSignatures;
    nextBarSignatures.reserve(incomingBarSignatures.size());
    for (const auto& signature : incomingBarSignatures) {
        const bool powerOfTwo = signature.denominator > 0 &&
            (signature.denominator & (signature.denominator - 1)) == 0;
        if (!std::isfinite(signature.quarterPosition) || signature.quarterPosition < 0.0 ||
            signature.numerator < 1 || signature.numerator > 64 || !powerOfTwo ||
            signature.denominator > 64) continue;
        nextBarSignatures.push_back({static_cast<ARA::ARAInt32>(signature.numerator),
            static_cast<ARA::ARAInt32>(signature.denominator), signature.quarterPosition});
    }
    std::sort(nextBarSignatures.begin(), nextBarSignatures.end(),
        [](const auto& left, const auto& right) { return left.position < right.position; });
    nextBarSignatures.erase(std::unique(nextBarSignatures.begin(), nextBarSignatures.end(),
        [](const auto& left, const auto& right) { return left.position == right.position; }),
        nextBarSignatures.end());
    for (size_t index = 1; index < nextBarSignatures.size();) {
        const auto& previous = nextBarSignatures[index - 1];
        const double barLength = static_cast<double>(previous.numerator) * 4.0 /
            static_cast<double>(previous.denominator);
        const double bars = (nextBarSignatures[index].position - previous.position) / barLength;
        if (!std::isfinite(bars) || std::abs(bars - std::round(bars)) > 0.000001)
            nextBarSignatures.erase(nextBarSignatures.begin() + static_cast<std::ptrdiff_t>(index));
        else ++index;
    }
    const auto tempoEqual = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index)
            if (left[index].timePosition != right[index].timePosition ||
                left[index].quarterPosition != right[index].quarterPosition) return false;
        return true;
    };
    const auto signaturesEqual = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index)
            if (left[index].position != right[index].position ||
                left[index].numerator != right[index].numerator ||
                left[index].denominator != right[index].denominator) return false;
        return true;
    };
    const bool musicalContentChanged = !tempoEqual(tempoEntries, nextTempoEntries) ||
        !signaturesEqual(barSignatures, nextBarSignatures);

    struct DesiredRegion
    {
        AraPlaybackRegionDescriptor descriptor;
        std::string sourceId;
        std::string sequenceId;
    };
    struct DesiredSequence
    {
        std::string nameUtf8;
        int order = 0;
    };

    std::unordered_map<std::string, const AraAudioSourceDescriptor*> desiredSources;
    desiredSources.reserve(sources.size());
    for (const auto& source : sources) {
        const std::string id = normalizedPersistentId(source.persistentId);
        if (id.empty() || source.sampleRate < 8000 || source.sampleRate > 384000 ||
            (source.channelCount != 1 && source.channelCount != 2) ||
            !source.interleavedStereoSamples || source.interleavedStereoSamples->size() < 2u ||
            (source.interleavedStereoSamples->size() % 2u) != 0u ||
            source.interleavedStereoSamples->size() / 2u >
                static_cast<size_t>(std::numeric_limits<ARA::ARASampleCount>::max())) continue;
        desiredSources.emplace(id, &source);
    }
    std::unordered_map<std::string, DesiredRegion> desiredRegions;
    std::unordered_map<std::string, DesiredSequence> desiredSequences;
    desiredRegions.reserve(regions.size());
    desiredSequences.reserve(regions.size());
    for (const auto& region : regions) {
        const std::string id = normalizedObjectId("region", region.persistentId);
        const std::string sourceId = normalizedPersistentId(region.audioSourcePersistentId);
        const std::string sequenceId = normalizedObjectId(
            "sequence", region.regionSequencePersistentId);
        if (id.empty() || sourceId.empty() || sequenceId.empty() ||
            region.routingTrackIndex < 0 ||
            desiredSources.find(sourceId) == desiredSources.end() ||
            !std::isfinite(region.sourceStartSeconds) || region.sourceStartSeconds < 0.0 ||
            !std::isfinite(region.sourceDurationSeconds) || region.sourceDurationSeconds <= 0.0 ||
            !std::isfinite(region.playbackStartSeconds) || region.playbackStartSeconds < 0.0 ||
            !std::isfinite(region.playbackDurationSeconds) || region.playbackDurationSeconds <= 0.0)
            continue;
        DesiredRegion desired{region, sourceId, sequenceId};
        if (!desiredRegions.emplace(id, std::move(desired)).second) continue;
        const std::string sequenceName = region.regionSequenceNameUtf8.size() <= 1024u
            ? region.regionSequenceNameUtf8 : std::string{};
        desiredSequences.emplace(sequenceId,
            DesiredSequence{sequenceName, region.regionSequenceOrder});
    }

    beginEditingSeh(api, controllerRef);
    for (auto it = playbackRegions.begin(); it != playbackRegions.end();) {
        const auto desired = desiredRegions.find(it->first);
        const bool dependencyChanged = desired != desiredRegions.end() &&
            (it->second->sourceId != desired->second.sourceId ||
             it->second->sequenceId != desired->second.sequenceId ||
             it->second->routingTrackIndex != desired->second.descriptor.routingTrackIndex);
        if (desired == desiredRegions.end() || dependencyChanged) {
            if (it->second->araRef)
                destroyPlaybackRegionSeh(api, controllerRef, it->second->araRef);
            it = playbackRegions.erase(it);
        } else ++it;
    }
    for (auto it = audioModifications.begin(); it != audioModifications.end();) {
        if (desiredSources.find(it->first) == desiredSources.end()) {
            if (it->second->araRef)
                destroyAudioModificationSeh(api, controllerRef, it->second->araRef);
            it = audioModifications.erase(it);
        } else ++it;
    }
    endEditingSeh(api, controllerRef);

    const bool sourcesComplete = syncAudioSources(sources);
    tempoEntries = std::move(nextTempoEntries);
    barSignatures = std::move(nextBarSignatures);
    beginEditingSeh(api, controllerRef);
    bool createdMusicalContext = false;
    for (auto it = regionSequences.begin(); it != regionSequences.end();) {
        if (desiredSequences.find(it->first) == desiredSequences.end()) {
            if (it->second->araRef)
                destroyRegionSequenceSeh(api, controllerRef, it->second->araRef);
            it = regionSequences.erase(it);
        } else ++it;
    }
    if (desiredRegions.empty() && musicalContext.araRef) {
        destroyMusicalContextSeh(api, controllerRef, musicalContext.araRef);
        musicalContext.araRef = nullptr;
    }
    if (!desiredRegions.empty() && !musicalContext.araRef) {
        ARA::ARAMusicalContextProperties properties{};
        properties.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(ARAMusicalContextProperties, color);
        properties.name = nullptr;
        properties.orderIndex = 0;
        properties.color = nullptr;
        musicalContext.araRef = createMusicalContextSeh(api, controllerRef,
            reinterpret_cast<ARA::ARAMusicalContextHostRef>(&musicalContext), &properties);
        createdMusicalContext = musicalContext.araRef != nullptr;
    }
    if (musicalContentChanged && musicalContext.araRef && !createdMusicalContext)
        updateMusicalContextContentSeh(api, controllerRef, musicalContext.araRef);

    if (musicalContext.araRef) {
        for (const auto& entry : desiredSequences) {
            auto found = regionSequences.find(entry.first);
            if (found == regionSequences.end()) {
                auto sequence = std::make_unique<RegionSequenceState>();
                sequence->persistentId = entry.first;
                sequence->nameUtf8 = entry.second.nameUtf8;
                sequence->order = entry.second.order;
                ARA::ARARegionSequenceProperties properties{};
                properties.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(ARARegionSequenceProperties, color);
                properties.name = sequence->nameUtf8.empty() ? nullptr : sequence->nameUtf8.c_str();
                properties.orderIndex = static_cast<ARA::ARAInt32>(sequence->order);
                properties.musicalContextRef = musicalContext.araRef;
                properties.color = nullptr;
                sequence->araRef = createRegionSequenceSeh(api, controllerRef,
                    reinterpret_cast<ARA::ARARegionSequenceHostRef>(sequence.get()), &properties);
                if (sequence->araRef) regionSequences.emplace(entry.first, std::move(sequence));
            } else {
                RegionSequenceState& sequence = *found->second;
                sequence.nameUtf8 = entry.second.nameUtf8;
                sequence.order = entry.second.order;
                ARA::ARARegionSequenceProperties properties{};
                properties.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(ARARegionSequenceProperties, color);
                properties.name = sequence.nameUtf8.empty() ? nullptr : sequence.nameUtf8.c_str();
                properties.orderIndex = static_cast<ARA::ARAInt32>(sequence.order);
                properties.musicalContextRef = musicalContext.araRef;
                properties.color = nullptr;
                updateRegionSequenceSeh(api, controllerRef, sequence.araRef, &properties);
            }
        }
    }

    for (const auto& entry : desiredSources) {
        const auto existing = audioModifications.find(entry.first);
        if (existing != audioModifications.end()) {
            AudioModificationState& modification = *existing->second;
            modification.nameUtf8 = entry.second->nameUtf8.size() <= 1024u
                ? entry.second->nameUtf8 : std::string{};
            ARA::ARAAudioModificationProperties properties{};
            properties.structSize = ARA::kARAAudioModificationPropertiesMinSize;
            properties.name = modification.nameUtf8.empty() ? nullptr : modification.nameUtf8.c_str();
            properties.persistentID = modification.persistentId.c_str();
            updateAudioModificationSeh(api, controllerRef, modification.araRef, &properties);
            continue;
        }
        const auto source = audioSources.find(entry.first);
        if (source == audioSources.end() || !source->second->araRef) continue;
        auto modification = std::make_unique<AudioModificationState>();
        modification->sourceId = entry.first;
        modification->persistentId = normalizedObjectId(
            "modification", entry.second->persistentId);
        if (modification->persistentId.empty()) continue;
        modification->nameUtf8 = entry.second->nameUtf8.size() <= 1024u
            ? entry.second->nameUtf8 : std::string{};
        ARA::ARAAudioModificationProperties properties{};
        properties.structSize = ARA::kARAAudioModificationPropertiesMinSize;
        properties.name = modification->nameUtf8.empty() ? nullptr : modification->nameUtf8.c_str();
        properties.persistentID = modification->persistentId.c_str();
        modification->araRef = createAudioModificationSeh(api, controllerRef,
            source->second->araRef,
            reinterpret_cast<ARA::ARAAudioModificationHostRef>(modification.get()), &properties);
        if (modification->araRef)
            audioModifications.emplace(entry.first, std::move(modification));
    }

    const bool supportsTimeStretch =
        (supportedPlaybackTransformationFlags & ARA::kARAPlaybackTransformationTimestretch) != 0;
    for (const auto& entry : desiredRegions) {
        const auto modification = audioModifications.find(entry.second.sourceId);
        const auto sequence = regionSequences.find(entry.second.sequenceId);
        const auto source = audioSources.find(entry.second.sourceId);
        if (modification == audioModifications.end() || sequence == regionSequences.end() ||
            source == audioSources.end()) continue;
        const double sourceLength = source->second->samples
            ? static_cast<double>(source->second->samples->size() / 2u) /
                static_cast<double>(source->second->sampleRate) : 0.0;
        const double sourceStart = std::clamp(
            entry.second.descriptor.sourceStartSeconds, 0.0, sourceLength);
        const double available = std::max(0.0, sourceLength - sourceStart);
        double sourceDuration = std::min(entry.second.descriptor.sourceDurationSeconds, available);
        double playbackDuration = entry.second.descriptor.playbackDurationSeconds;
        const bool timeStretch = entry.second.descriptor.requestTimeStretch && supportsTimeStretch;
        if (!timeStretch) playbackDuration = sourceDuration;
        if (sourceDuration <= 0.0 || playbackDuration <= 0.0) continue;

        auto found = playbackRegions.find(entry.first);
        if (found == playbackRegions.end()) {
            auto region = std::make_unique<PlaybackRegionState>();
            region->persistentId = entry.first;
            region->sourceId = entry.second.sourceId;
            region->sequenceId = entry.second.sequenceId;
            region->nameUtf8 = entry.second.descriptor.nameUtf8.size() <= 1024u
                ? entry.second.descriptor.nameUtf8 : std::string{};
            region->routingTrackIndex = entry.second.descriptor.routingTrackIndex;
            region->sourceStartSeconds = sourceStart;
            region->sourceDurationSeconds = sourceDuration;
            region->playbackStartSeconds = entry.second.descriptor.playbackStartSeconds;
            region->playbackDurationSeconds = playbackDuration;
            region->timeStretch = timeStretch;
            ARA::ARAPlaybackRegionProperties properties{};
            properties.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(ARAPlaybackRegionProperties, color);
            properties.transformationFlags = timeStretch
                ? ARA::kARAPlaybackTransformationTimestretch
                : ARA::kARAPlaybackTransformationNoChanges;
            properties.startInModificationTime = region->sourceStartSeconds;
            properties.durationInModificationTime = region->sourceDurationSeconds;
            properties.startInPlaybackTime = region->playbackStartSeconds;
            properties.durationInPlaybackTime = region->playbackDurationSeconds;
            properties.musicalContextRef = musicalContext.araRef;
            properties.regionSequenceRef = sequence->second->araRef;
            properties.name = region->nameUtf8.empty() ? nullptr : region->nameUtf8.c_str();
            properties.color = nullptr;
            region->araRef = createPlaybackRegionSeh(api, controllerRef,
                modification->second->araRef,
                reinterpret_cast<ARA::ARAPlaybackRegionHostRef>(region.get()), &properties);
            if (region->araRef) playbackRegions.emplace(entry.first, std::move(region));
        } else {
            PlaybackRegionState& region = *found->second;
            region.nameUtf8 = entry.second.descriptor.nameUtf8.size() <= 1024u
                ? entry.second.descriptor.nameUtf8 : std::string{};
            region.routingTrackIndex = entry.second.descriptor.routingTrackIndex;
            region.sourceStartSeconds = sourceStart;
            region.sourceDurationSeconds = sourceDuration;
            region.playbackStartSeconds = entry.second.descriptor.playbackStartSeconds;
            region.playbackDurationSeconds = playbackDuration;
            region.timeStretch = timeStretch;
            ARA::ARAPlaybackRegionProperties properties{};
            properties.structSize = ARA_IMPLEMENTED_STRUCT_SIZE(ARAPlaybackRegionProperties, color);
            properties.transformationFlags = timeStretch
                ? ARA::kARAPlaybackTransformationTimestretch
                : ARA::kARAPlaybackTransformationNoChanges;
            properties.startInModificationTime = region.sourceStartSeconds;
            properties.durationInModificationTime = region.sourceDurationSeconds;
            properties.startInPlaybackTime = region.playbackStartSeconds;
            properties.durationInPlaybackTime = region.playbackDurationSeconds;
            properties.musicalContextRef = musicalContext.araRef;
            properties.regionSequenceRef = sequence->second->araRef;
            properties.name = region.nameUtf8.empty() ? nullptr : region.nameUtf8.c_str();
            properties.color = nullptr;
            updatePlaybackRegionSeh(api, controllerRef, region.araRef, &properties);
        }
    }
    endEditingSeh(api, controllerRef);
    notifyModelUpdatesSeh(api, controllerRef);
    return sourcesComplete && playbackRegions.size() == desiredRegions.size();
}

AraDocumentContext::AraDocumentContext(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl))
{
}

AraDocumentContext::~AraDocumentContext()
 = default;

ARA::ARADocumentControllerRef AraDocumentContext::documentControllerRef() const noexcept
{
    return m_impl ? documentControllerRefSeh(m_impl->controller) : nullptr;
}

bool AraDocumentContext::ready() const noexcept
{
    return documentControllerRef() != nullptr;
}

std::vector<ARA::ARAPlaybackRegionRef> AraDocumentContext::playbackRegionRefsForTrack(
    int trackIndex) const
{
    std::vector<std::pair<std::string, ARA::ARAPlaybackRegionRef>> ordered;
    if (!m_impl || trackIndex < 0) return {};
    ordered.reserve(m_impl->playbackRegions.size());
    for (const auto& entry : m_impl->playbackRegions) {
        const auto& region = entry.second;
        if (region && region->araRef && region->routingTrackIndex == trackIndex)
            ordered.emplace_back(entry.first, region->araRef);
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    std::vector<ARA::ARAPlaybackRegionRef> result;
    result.reserve(ordered.size());
    for (const auto& entry : ordered) result.push_back(entry.second);
    return result;
}

std::vector<ARA::ARAPlaybackRegionRef>
AraDocumentContext::playbackRegionRefsForPersistentIds(
    int trackIndex, const std::vector<std::string>& persistentIds) const
{
    std::vector<ARA::ARAPlaybackRegionRef> result;
    if (!m_impl || trackIndex < 0) return result;
    result.reserve(persistentIds.size());
    std::set<ARA::ARAPlaybackRegionRef> seen;
    for (const std::string& persistentId : persistentIds) {
        const std::string id = Impl::normalizedObjectId("region", persistentId);
        const auto found = m_impl->playbackRegions.find(id);
        if (found == m_impl->playbackRegions.end() || !found->second ||
            !found->second->araRef || found->second->routingTrackIndex != trackIndex ||
            !seen.insert(found->second->araRef).second) continue;
        result.push_back(found->second->araRef);
    }
    return result;
}

bool AraDocumentContext::captureArchive(AraDocumentArchive& archive)
{
    archive = {};
    if (!m_impl || !ready() || !m_impl->factory) return false;
    std::lock_guard<std::mutex> controllerLock(m_impl->controllerMutex);
    std::array<char, 1024> factoryId{};
    std::array<char, 1024> documentArchiveId{};
    const size_t factoryLength = copyFactoryIdSeh(
        m_impl->factory, factoryId.data(), factoryId.size());
    const size_t archiveLength = copyPersistentIdSeh(
        m_impl->factory->documentArchiveID,
        documentArchiveId.data(), documentArchiveId.size());
    if (factoryLength == 0 || archiveLength == 0) return false;
    try {
        archive.factoryPersistentId.assign(factoryId.data(), factoryLength);
        archive.documentArchiveId.assign(documentArchiveId.data(), archiveLength);
        Impl::ArchiveWriter writer{&archive.bytes, false};
        m_impl->archiveProgress.store(0.0f, std::memory_order_release);
        ScopedAtomicActivity activity(m_impl->archiveActive);
        const auto* api = controllerInterfaceSeh(m_impl->controller);
        const bool stored = storeObjectsToArchiveSeh(api, documentControllerRef(),
            reinterpret_cast<ARA::ARAArchiveWriterHostRef>(&writer));
        if (!stored || writer.failed || archive.bytes.size() > Impl::kMaxAraArchiveBytes) {
            archive = {};
            return false;
        }
        m_impl->archiveProgress.store(1.0f, std::memory_order_release);
        m_impl->documentDirty.store(false, std::memory_order_release);
        return true;
    } catch (...) {
        archive = {};
        return false;
    }
}

bool AraDocumentContext::restoreArchive(const AraDocumentArchive& archive)
{
    if (!m_impl || !ready() || !m_impl->factory || archive.bytes.empty() ||
        archive.bytes.size() > Impl::kMaxAraArchiveBytes ||
        archive.factoryPersistentId.empty() || archive.documentArchiveId.empty()) return false;
    std::lock_guard<std::mutex> controllerLock(m_impl->controllerMutex);
    std::array<char, 1024> factoryId{};
    const size_t factoryLength = copyFactoryIdSeh(
        m_impl->factory, factoryId.data(), factoryId.size());
    if (factoryLength == 0 || archive.factoryPersistentId.size() != factoryLength ||
        !std::equal(archive.factoryPersistentId.begin(), archive.factoryPersistentId.end(),
                    factoryId.begin()) ||
        !archiveIdCompatibleSeh(m_impl->factory, archive.documentArchiveId)) return false;
    try {
        Impl::ArchiveReader reader{&archive.bytes, archive.documentArchiveId};
        const auto* api = controllerInterfaceSeh(m_impl->controller);
        const auto controllerRef = documentControllerRef();
        m_impl->unarchiveProgress.store(0.0f, std::memory_order_release);
        ScopedAtomicActivity activity(m_impl->unarchiving);
        beginEditingSeh(api, controllerRef);
        const bool restored = restoreObjectsFromArchiveSeh(api, controllerRef,
            reinterpret_cast<ARA::ARAArchiveReaderHostRef>(&reader));
        endEditingSeh(api, controllerRef);
        notifyModelUpdatesSeh(api, controllerRef);
        if (!restored) return false;
        m_impl->unarchiveProgress.store(1.0f, std::memory_order_release);
        m_impl->documentDirty.store(false, std::memory_order_release);
        return true;
    } catch (...) { return false; }
}

bool AraDocumentContext::consumeTransportRequest(
    AraTransportRequest& request) noexcept
{
    request = {};
    if (!m_impl) return false;
    const int playback = m_impl->playbackRequest.exchange(0, std::memory_order_acq_rel);
    const std::uint32_t flags = m_impl->transportRequestFlags.exchange(
        0, std::memory_order_acq_rel);
    request.startRequested = playback > 0;
    request.stopRequested = playback < 0;
    if ((flags & Impl::kSeekRequestFlag) != 0u) {
        const double position = m_impl->requestedPosition.load(std::memory_order_acquire);
        request.seekRequested = std::isfinite(position);
        request.positionSeconds = request.seekRequested ? std::max(0.0, position) : 0.0;
    }
    if ((flags & Impl::kCycleRangeRequestFlag) != 0u) {
        const double start = m_impl->requestedCycleStart.load(std::memory_order_acquire);
        const double duration = m_impl->requestedCycleDuration.load(std::memory_order_acquire);
        request.cycleRangeRequested = std::isfinite(start) && std::isfinite(duration) &&
            duration > 0.0;
        request.cycleStartSeconds = request.cycleRangeRequested ? std::max(0.0, start) : 0.0;
        request.cycleDurationSeconds = request.cycleRangeRequested ? duration : 0.0;
    }
    if ((flags & Impl::kCycleEnableRequestFlag) != 0u) {
        request.cycleEnableRequested = true;
        request.cycleEnabled = m_impl->requestedCycleEnabled.load(std::memory_order_acquire);
    }
    return request.startRequested || request.stopRequested || request.seekRequested ||
        request.cycleRangeRequested || request.cycleEnableRequested;
}

bool AraDocumentContext::consumeDocumentDirty() noexcept
{
    return m_impl && m_impl->documentDirty.exchange(false, std::memory_order_acq_rel);
}

AraHostStatus AraDocumentContext::pollStatus() noexcept
{
    AraHostStatus status;
    if (!m_impl) return status;
    status.available = ready();
    std::unique_lock<std::mutex> controllerLock(
        m_impl->controllerMutex, std::try_to_lock);
    if (status.available && controllerLock.owns_lock()) {
        notifyModelUpdatesSeh(
            controllerInterfaceSeh(m_impl->controller), documentControllerRef());
        status.documentDirty = consumeDocumentDirty();
        for (const auto& entry : m_impl->audioSources) {
            const auto* source = entry.second.get();
            if (!source || !source->analysisActive.load(std::memory_order_acquire)) continue;
            const float progress = std::clamp(
                source->analysisProgress.load(std::memory_order_acquire), 0.0f, 1.0f);
            status.analysisProgress = status.analysisActive
                ? std::min(status.analysisProgress, progress) : progress;
            status.analysisActive = true;
        }
    }
    status.archiving = m_impl->archiveActive.load(std::memory_order_acquire);
    status.archiveProgress = std::clamp(
        m_impl->archiveProgress.load(std::memory_order_acquire), 0.0f, 1.0f);
    status.unarchiving = m_impl->unarchiving.load(std::memory_order_acquire);
    status.unarchiveProgress = std::clamp(
        m_impl->unarchiveProgress.load(std::memory_order_acquire), 0.0f, 1.0f);
    return status;
}

bool AraDocumentContext::processingAlgorithmSelection(
    const std::vector<std::string>& playbackRegionPersistentIds,
    AraProcessingAlgorithmSelection& selection)
{
    selection = {};
    if (!m_impl || playbackRegionPersistentIds.empty()) return false;
    std::lock_guard<std::mutex> controllerLock(m_impl->controllerMutex);
    const auto* api = controllerInterfaceSeh(m_impl->controller);
    const auto controllerRef = documentControllerRef();
    const int count = processingAlgorithmCountSeh(api, controllerRef);
    if (count <= 0) return false;

    Impl::AudioSourceState* selectedSource = nullptr;
    for (const std::string& persistentId : playbackRegionPersistentIds) {
        const std::string regionId = Impl::normalizedObjectId("region", persistentId);
        const auto region = m_impl->playbackRegions.find(regionId);
        if (region == m_impl->playbackRegions.end() || !region->second) return false;
        const auto source = m_impl->audioSources.find(region->second->sourceId);
        if (source == m_impl->audioSources.end() || !source->second || !source->second->araRef)
            return false;
        if (selectedSource && selectedSource != source->second.get()) return false;
        selectedSource = source->second.get();
    }
    if (!selectedSource) return false;

    std::array<char, kMaximumAraAlgorithmTextBytes> factoryId{};
    std::array<char, kMaximumAraAlgorithmTextBytes> pluginName{};
    const size_t factoryIdLength = copyFactoryIdSeh(
        m_impl->factory, factoryId.data(), factoryId.size());
    if (factoryIdLength == 0) return false;
    const size_t pluginNameLength = copyFactoryPluginNameSeh(
        m_impl->factory, pluginName.data(), pluginName.size());

    AraProcessingAlgorithmSelection next;
    next.factoryPersistentId.assign(factoryId.data(), factoryIdLength);
    next.pluginNameUtf8.assign(
        pluginNameLength > 0 ? pluginName.data() : factoryId.data(),
        pluginNameLength > 0 ? pluginNameLength : factoryIdLength);
    next.algorithms.reserve(static_cast<size_t>(count));
    std::set<std::string> algorithmIds;
    for (int index = 0; index < count; ++index) {
        std::array<char, kMaximumAraAlgorithmTextBytes> algorithmId{};
        std::array<char, kMaximumAraAlgorithmTextBytes> algorithmName{};
        if (!copyProcessingAlgorithmSeh(
                api, controllerRef, index,
                algorithmId.data(), algorithmId.size(),
                algorithmName.data(), algorithmName.size())) return false;
        AraProcessingAlgorithm algorithm;
        algorithm.persistentId.assign(algorithmId.data());
        algorithm.nameUtf8.assign(algorithmName.data());
        if (!algorithmIds.insert(algorithm.persistentId).second) return false;
        next.algorithms.push_back(std::move(algorithm));
    }
    if (!selectedSource->analysisActive.load(std::memory_order_acquire))
        next.currentIndex = currentProcessingAlgorithmSeh(
            api, controllerRef, selectedSource->araRef, count);
    selection = std::move(next);
    return true;
}

bool AraDocumentContext::requestProcessingAlgorithm(
    const std::vector<std::string>& playbackRegionPersistentIds,
    int algorithmIndex)
{
    if (!m_impl || playbackRegionPersistentIds.empty() || algorithmIndex < 0)
        return false;
    std::lock_guard<std::mutex> controllerLock(m_impl->controllerMutex);
    const auto* api = controllerInterfaceSeh(m_impl->controller);
    const auto controllerRef = documentControllerRef();
    const int count = processingAlgorithmCountSeh(api, controllerRef);
    if (algorithmIndex >= count) return false;

    Impl::AudioSourceState* selectedSource = nullptr;
    for (const std::string& persistentId : playbackRegionPersistentIds) {
        const std::string regionId = Impl::normalizedObjectId("region", persistentId);
        const auto region = m_impl->playbackRegions.find(regionId);
        if (region == m_impl->playbackRegions.end() || !region->second) return false;
        const auto source = m_impl->audioSources.find(region->second->sourceId);
        if (source == m_impl->audioSources.end() || !source->second || !source->second->araRef)
            return false;
        if (selectedSource && selectedSource != source->second.get()) return false;
        selectedSource = source->second.get();
    }
    if (!selectedSource) return false;
    if (!selectedSource->analysisActive.load(std::memory_order_acquire) &&
        currentProcessingAlgorithmSeh(
            api, controllerRef, selectedSource->araRef, count) == algorithmIndex)
        return true;

    beginEditingSeh(api, controllerRef);
    const bool requested = requestProcessingAlgorithmSeh(
        api, controllerRef, selectedSource->araRef, algorithmIndex);
    endEditingSeh(api, controllerRef);
    if (requested && m_impl->analyzeableContentTypeCount > 0)
        requestAudioSourceAnalysisSeh(
            api, controllerRef, selectedSource->araRef,
            m_impl->analyzeableContentTypeCount,
            m_impl->analyzeableContentTypes.data());
    notifyModelUpdatesSeh(api, controllerRef);
    if (requested) m_impl->documentDirty.store(true, std::memory_order_release);
    return requested;
}

bool AraDocumentContext::syncModelGraph(
    const std::vector<AraAudioSourceDescriptor>& sources,
    const std::vector<AraPlaybackRegionDescriptor>& regions,
    const std::vector<AraTempoEntryDescriptor>& tempoEntries,
    const std::vector<AraBarSignatureDescriptor>& barSignatures)
{
    if (!m_impl) return false;
    std::lock_guard<std::mutex> controllerLock(m_impl->controllerMutex);
    return m_impl->syncModelGraph(
        sources, regions, tempoEntries, barSignatures);
}

std::shared_ptr<AraDocumentContext> AraDocumentRegistry::acquire(const ARA::ARAFactory* factory)
{
    const std::string key = factoryKey(factory);
    if (key.empty()) return {};
    std::shared_ptr<AraDocumentContext> document;
    std::vector<AraAudioSourceDescriptor> audioSources;
    std::vector<AraPlaybackRegionDescriptor> playbackRegions;
    std::vector<AraTempoEntryDescriptor> tempoEntries;
    std::vector<AraBarSignatureDescriptor> barSignatures;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto existing = m_documents.find(key);
        if (existing != m_documents.end()) {
            if (auto existingDocument = existing->second.lock()) return existingDocument;
        }

        std::unique_ptr<AraDocumentContext::Impl> impl(new (std::nothrow) AraDocumentContext::Impl);
        if (!impl) return {};
        impl->factory = factory;
        impl->supportedPlaybackTransformationFlags =
            supportedPlaybackTransformationFlagsSeh(factory);
        impl->analyzeableContentTypeCount = copyAnalyzeableContentTypesSeh(
            factory, impl->analyzeableContentTypes);
        impl->configureInterfaces();

        ARA::ARAInterfaceConfiguration configuration{};
        configuration.structSize = ARA::kARAInterfaceConfigurationMinSize;
        ARA::ARAAPIGeneration lowest = ARA::kARAAPIGeneration_2_0_Final;
        ARA::ARAAPIGeneration highest = ARA::kARAAPIGeneration_2_0_Final;
        if (!factoryApiRangeSeh(factory, lowest, highest) ||
            lowest > ARA::kARAAPIGeneration_2_3_Final ||
            highest < ARA::kARAAPIGeneration_2_0_Final) return {};
        configuration.desiredApiGeneration = std::min<ARA::ARAAPIGeneration>(
            highest, static_cast<ARA::ARAAPIGeneration>(
                ARA::kARAAPIGeneration_2_3_Final));
        configuration.assertFunctionAddress = &gAraAssertFunction;
        if (!initializeFactory(factory, &configuration)) return {};
        impl->initialized = true;

        ARA::ARADocumentProperties properties{};
        properties.structSize = ARA::kARADocumentPropertiesMinSize;
        properties.name = nullptr;
        impl->controller = createDocumentController(factory, &impl->host, &properties);
        if (!validControllerInstanceSeh(impl->controller)) return {};

        document = std::shared_ptr<AraDocumentContext>(
            new (std::nothrow) AraDocumentContext(std::move(impl)));
        if (!document) return {};
        m_documents[key] = document;
        audioSources = m_audioSources;
        playbackRegions = m_playbackRegions;
        tempoEntries = m_tempoEntries;
        barSignatures = m_barSignatures;
    }
    document->syncModelGraph(audioSources, playbackRegions, tempoEntries, barSignatures);
    return document;
}

void AraDocumentRegistry::syncModelGraph(
    const std::vector<AraAudioSourceDescriptor>& sources,
    const std::vector<AraPlaybackRegionDescriptor>& regions,
    const std::vector<AraTempoEntryDescriptor>& tempoEntries,
    const std::vector<AraBarSignatureDescriptor>& barSignatures)
{
    std::vector<std::shared_ptr<AraDocumentContext>> documents;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_audioSources = sources;
        m_playbackRegions = regions;
        m_tempoEntries = tempoEntries;
        m_barSignatures = barSignatures;
        documents.reserve(m_documents.size());
        for (auto it = m_documents.begin(); it != m_documents.end();) {
            if (auto document = it->second.lock()) {
                documents.push_back(std::move(document));
                ++it;
            } else {
                it = m_documents.erase(it);
            }
        }
    }
    for (const auto& document : documents)
        document->syncModelGraph(sources, regions, tempoEntries, barSignatures);
}

bool AraDocumentRegistry::captureArchives(std::vector<AraDocumentArchive>& archives)
{
    archives.clear();
    try {
        std::vector<std::shared_ptr<AraDocumentContext>> documents;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            documents.reserve(m_documents.size());
            for (auto it = m_documents.begin(); it != m_documents.end();) {
                if (auto document = it->second.lock()) {
                    documents.push_back(std::move(document));
                    ++it;
                } else {
                    it = m_documents.erase(it);
                }
            }
        }
        archives.reserve(documents.size());
        for (const auto& document : documents) {
            AraDocumentArchive archive;
            if (!document->captureArchive(archive)) {
                archives.clear();
                return false;
            }
            archives.push_back(std::move(archive));
        }
        return true;
    } catch (...) {
        archives.clear();
        return false;
    }
}

bool AraDocumentRegistry::restoreArchives(
    const std::vector<AraDocumentArchive>& archives)
{
    if (archives.empty()) return true;
    try {
        std::vector<std::shared_ptr<AraDocumentContext>> documents;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            documents.reserve(m_documents.size());
            for (auto it = m_documents.begin(); it != m_documents.end();) {
                if (auto document = it->second.lock()) {
                    documents.push_back(std::move(document));
                    ++it;
                } else {
                    it = m_documents.erase(it);
                }
            }
        }
        for (const AraDocumentArchive& archive : archives) {
            bool restored = false;
            for (const auto& document : documents) {
                if (document->restoreArchive(archive)) {
                    restored = true;
                    break;
                }
            }
            if (!restored) return false;
        }
        return true;
    } catch (...) { return false; }
}

bool AraDocumentRegistry::consumeTransportRequest(
    AraTransportRequest& request) noexcept
{
    request = {};
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool consumed = false;
        for (auto it = m_documents.begin(); it != m_documents.end();) {
            const auto document = it->second.lock();
            if (!document) {
                it = m_documents.erase(it);
                continue;
            }
            ++it;
            AraTransportRequest next;
            if (!document->consumeTransportRequest(next)) continue;
            consumed = true;
            request.startRequested = request.startRequested || next.startRequested;
            request.stopRequested = request.stopRequested || next.stopRequested;
            if (!request.seekRequested && next.seekRequested) {
                request.seekRequested = true;
                request.positionSeconds = next.positionSeconds;
            }
            if (!request.cycleRangeRequested && next.cycleRangeRequested) {
                request.cycleRangeRequested = true;
                request.cycleStartSeconds = next.cycleStartSeconds;
                request.cycleDurationSeconds = next.cycleDurationSeconds;
            }
            if (!request.cycleEnableRequested && next.cycleEnableRequested) {
                request.cycleEnableRequested = true;
                request.cycleEnabled = next.cycleEnabled;
            }
        }
        if (request.stopRequested) request.startRequested = false;
        return consumed;
    } catch (...) {
        request = {};
        return false;
    }
}

bool AraDocumentRegistry::pollStatus(AraHostStatus& status) noexcept
{
    status = {};
    try {
        std::vector<std::shared_ptr<AraDocumentContext>> documents;
        {
            std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
            if (!lock.owns_lock()) return false;
            documents.reserve(m_documents.size());
            for (auto it = m_documents.begin(); it != m_documents.end();) {
                if (auto document = it->second.lock()) {
                    documents.push_back(std::move(document));
                    ++it;
                } else {
                    it = m_documents.erase(it);
                }
            }
        }
        for (const auto& document : documents) {
            const AraHostStatus next = document->pollStatus();
            status.available = status.available || next.available;
            status.documentDirty = status.documentDirty || next.documentDirty;
            if (next.analysisActive) {
                status.analysisProgress = status.analysisActive
                    ? std::min(status.analysisProgress, next.analysisProgress)
                    : next.analysisProgress;
                status.analysisActive = true;
            }
            if (next.archiving) {
                status.archiveProgress = status.archiving
                    ? std::min(status.archiveProgress, next.archiveProgress)
                    : next.archiveProgress;
                status.archiving = true;
            }
            if (next.unarchiving) {
                status.unarchiveProgress = status.unarchiving
                    ? std::min(status.unarchiveProgress, next.unarchiveProgress)
                    : next.unarchiveProgress;
                status.unarchiving = true;
            }
        }
        return true;
    } catch (...) {
        status = {};
        return false;
    }
}

bool AraDocumentRegistry::processingAlgorithmSelection(
    const std::vector<std::string>& playbackRegionPersistentIds,
    AraProcessingAlgorithmSelection& selection)
{
    selection = {};
    if (playbackRegionPersistentIds.empty()) return false;
    try {
        std::vector<std::shared_ptr<AraDocumentContext>> documents;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            documents.reserve(m_documents.size());
            for (auto it = m_documents.begin(); it != m_documents.end();) {
                if (auto document = it->second.lock()) {
                    documents.push_back(std::move(document));
                    ++it;
                } else {
                    it = m_documents.erase(it);
                }
            }
        }
        for (const auto& document : documents)
            if (document->processingAlgorithmSelection(
                    playbackRegionPersistentIds, selection)) return true;
    } catch (...) {}
    selection = {};
    return false;
}

bool AraDocumentRegistry::requestProcessingAlgorithm(
    const std::string& factoryPersistentId,
    const std::vector<std::string>& playbackRegionPersistentIds,
    int algorithmIndex)
{
    if (factoryPersistentId.empty() || playbackRegionPersistentIds.empty() ||
        algorithmIndex < 0) return false;
    try {
        std::vector<std::shared_ptr<AraDocumentContext>> documents;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            documents.reserve(m_documents.size());
            for (auto it = m_documents.begin(); it != m_documents.end();) {
                if (auto document = it->second.lock()) {
                    documents.push_back(std::move(document));
                    ++it;
                } else {
                    it = m_documents.erase(it);
                }
            }
        }
        for (const auto& document : documents) {
            AraProcessingAlgorithmSelection selection;
            if (!document->processingAlgorithmSelection(
                    playbackRegionPersistentIds, selection) ||
                selection.factoryPersistentId != factoryPersistentId) continue;
            return document->requestProcessingAlgorithm(
                playbackRegionPersistentIds, algorithmIndex);
        }
    } catch (...) {}
    return false;
}

AraBindingStatus validateAraBinding(
    const ARA::ARAPlugInExtensionInstance* instance,
    ARA::ARAPlugInInstanceRoleFlags assignedRoles,
    const std::shared_ptr<AraDocumentContext>& document) noexcept
{
    return validateBindingSeh(instance, assignedRoles, document && document->ready());
}

#endif
