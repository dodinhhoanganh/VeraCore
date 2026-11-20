#include "ExternalPluginHost.h"
#include "AraHost.h"
#include "PluginLoader.h"

#include <vst.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstphysicalui.h>
#include <pluginterfaces/vst/ivstprefetchablesupport.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>
#include <clap/entry.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/audio-ports-config.h>
#include <clap/ext/latency.h>
#include <clap/ext/render.h>
#include <clap/ext/tail.h>
#include <clap/ext/state.h>
#include <clap/ext/surround.h>
#include <clap/ext/note-name.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/params.h>
#include <clap/ext/gui.h>
#include <clap/ext/voice-info.h>
#include <clap/ext/remote-controls.h>
#include <clap/ext/track-info.h>
#include <clap/ext/context-menu.h>
#include <clap/ext/preset-load.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/thread-pool.h>
#include <clap/factory/plugin-factory.h>
#if defined(VERACORE_ARA_SDK_AVAILABLE)
#include <ARAInterface.h>
#include <ARAVST3.h>
#include <ARACLAP.h>
#endif
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{
constexpr int kLayerIdStride = 1024;
constexpr int kDefaultBlockSize = 4096;
constexpr int kMinPluginCycleFrames = 16;
constexpr double kMinPluginTempoBpm = 20.0;
constexpr double kMaxPluginTempoBpm = 999.0;
constexpr int kMaxPluginChannels = 32;
constexpr size_t kMaxMidiEventsPerBlock = 2048;
constexpr size_t kMaxVst3ParameterQueues = kMaxPluginAutomationLanes + 4;
constexpr size_t kMaxVst3PointsPerQueue = 256;
constexpr size_t kMaxAutomationEventsPerBlock = 1024;
constexpr size_t kNativeParameterEditQueueCapacity = 2048;
constexpr size_t kMaxNativeParameterEditsPerPoll = 16384;
constexpr int kMaxPluginStateBytes = 16 * 1024 * 1024;
constexpr int kVst3MidiControllerSlots = Steinberg::Vst::kCtrlProgramChange + 1;
constexpr double kMaxPdcSeconds = 10.0;
constexpr double kMaxPluginTimelineSeconds =
    static_cast<double>(std::numeric_limits<int64_t>::max()) / 384000.0 - 1.0;

struct NativePluginParameterEdit
{
    PluginParameterEditPhase phase = PluginParameterEditPhase::Value;
    uint32_t parameterId = 0;
    double normalizedValue = 0.0;
    uint64_t gestureSerial = 0;
    uint64_t captureCounter = 0;
    double capturedProjectBeat = -1.0;
};

// First-writer-wins overflow identity.  Keeping the complete 64-bit gesture
// serial avoids aliasing two long-running editor gestures after 2^32 edits.
struct NativeParameterEditOverflow
{
    uint32_t parameterId = std::numeric_limits<uint32_t>::max();
    uint64_t gestureSerial = 0;

    explicit operator bool() const noexcept
    {
        return parameterId != std::numeric_limits<uint32_t>::max();
    }
};

uint64_t nativeEditCounterNow() noexcept
{
    LARGE_INTEGER counter{};
    return QueryPerformanceCounter(&counter)
        ? static_cast<uint64_t>(counter.QuadPart) : 0;
}

template <size_t Capacity>
class BoundedParameterEditQueue
{
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "parameter queue capacity must be a power of two");
    struct Slot {
        std::atomic<size_t> sequence{0};
        NativePluginParameterEdit event{};
    };
    static constexpr uint64_t kQuarantined = uint64_t{1} << 63u;
    static constexpr uint64_t kClearing = uint64_t{1} << 62u;
    static constexpr uint64_t kClearContended = uint64_t{1} << 61u;
    static constexpr uint64_t kStateFlags =
        kQuarantined | kClearing | kClearContended;
    static constexpr uint64_t kActivityMask = ~kStateFlags;
    struct ActivityGuard {
        std::atomic<uint64_t>* state = nullptr;
        ActivityGuard(std::atomic<uint64_t>& source, bool enabled) noexcept
            : state(enabled ? &source : nullptr)
        {
            if (state) {
                const uint64_t previous = state->fetch_add(1u, std::memory_order_acq_rel);
                if ((previous & kClearing) != 0)
                    state->fetch_or(kClearContended, std::memory_order_acq_rel);
            }
        }
        ~ActivityGuard()
        {
            if (state) state->fetch_sub(1u, std::memory_order_release);
        }
        ActivityGuard(const ActivityGuard&) = delete;
        ActivityGuard& operator=(const ActivityGuard&) = delete;
    };
    static constexpr int kCasAttempts = 16;

public:
    BoundedParameterEditQueue() noexcept { reset(); }

    ActivityGuard capture(bool enabled) noexcept
    {
        return ActivityGuard{m_publicationState, enabled};
    }

    bool push(const NativePluginParameterEdit& event) noexcept
    {
        ActivityGuard producer{m_publicationState, true};
        if ((m_publicationState.load(std::memory_order_acquire) & kStateFlags) != 0)
            return false;
        NativePluginParameterEdit captured = event;
        if (captured.captureCounter == 0) captured.captureCounter = nativeEditCounterNow();
        size_t position = m_enqueue.load(std::memory_order_relaxed);
        for (int attempt = 0; attempt < kCasAttempts; ++attempt) {
            Slot& slot = m_slots[position & (Capacity - 1)];
            const size_t sequence = slot.sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) -
                static_cast<intptr_t>(position);
            if (difference == 0) {
                if (m_enqueue.compare_exchange_weak(position, position + 1,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    slot.event = captured;
                    slot.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                markOverflow(captured.parameterId, captured.gestureSerial);
                return false;
            } else {
                position = m_enqueue.load(std::memory_order_relaxed);
            }
        }
        markOverflow(captured.parameterId, captured.gestureSerial);
        return false;
    }

    bool pop(NativePluginParameterEdit& event) noexcept
    {
        size_t position = m_dequeue.load(std::memory_order_relaxed);
        for (int attempt = 0; attempt < kCasAttempts; ++attempt) {
            Slot& slot = m_slots[position & (Capacity - 1)];
            const size_t sequence = slot.sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) -
                static_cast<intptr_t>(position + 1);
            if (difference == 0) {
                if (m_dequeue.compare_exchange_weak(position, position + 1,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    event = slot.event;
                    slot.sequence.store(position + Capacity, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = m_dequeue.load(std::memory_order_relaxed);
            }
        }
        return false;
    }

    NativeParameterEditOverflow consumeOverflowDescriptor() noexcept
    {
        NativeParameterEditOverflow descriptor;
        uint32_t expected = kOverflowReady;
        if (!m_overflowState.compare_exchange_strong(
                expected, kOverflowReading, std::memory_order_acq_rel,
                std::memory_order_acquire)) return descriptor;
        descriptor.parameterId = m_overflowParameterId.load(std::memory_order_relaxed);
        descriptor.gestureSerial = m_overflowGestureSerial.load(std::memory_order_relaxed);
        m_overflowState.store(kOverflowConsumed, std::memory_order_release);
        return descriptor;
    }

    bool quiescent() const noexcept
    {
        return (m_publicationState.load(std::memory_order_acquire) &
                kActivityMask) == 0 &&
            m_enqueue.load(std::memory_order_acquire) ==
                m_dequeue.load(std::memory_order_acquire);
    }

    bool quarantined() const noexcept
    {
        return (m_publicationState.load(std::memory_order_acquire) &
                kStateFlags) != 0;
    }

    template <typename TrackerIdle>
    bool tryClearQuarantine(TrackerIdle&& trackerIdle) noexcept
    {
        // A callback that entered during kClearing may publish the sticky bit
        // after the failed clear path restored quarantine.  Normalize those
        // stale reservation flags only after every callback has departed.
        uint64_t stale = m_publicationState.load(std::memory_order_acquire);
        if ((stale & kQuarantined) != 0 && (stale & kActivityMask) == 0 &&
            (stale & (kClearing | kClearContended)) != 0) {
            const uint64_t normalized = kQuarantined;
            m_publicationState.compare_exchange_strong(
                stale, normalized, std::memory_order_acq_rel,
                std::memory_order_acquire);
        }
        if (m_enqueue.load(std::memory_order_acquire) !=
            m_dequeue.load(std::memory_order_acquire)) return false;
        uint64_t expected = kQuarantined;
        if (!m_publicationState.compare_exchange_strong(
                expected, kClearing, std::memory_order_acq_rel,
                std::memory_order_acquire)) return false;

        // kClearing is a reservation: callbacks count as active but cannot
        // publish.  Therefore tracker depth and queue emptiness belong to the
        // same clear linearization point instead of two racy observations.
        const bool idle = trackerIdle() &&
            m_enqueue.load(std::memory_order_acquire) ==
                m_dequeue.load(std::memory_order_acquire);
        if (idle) resetOverflowDescriptor(std::memory_order_release);
        expected = kClearing;
        if (idle && m_publicationState.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel,
                std::memory_order_acquire)) return true;
        m_publicationState.fetch_or(kQuarantined, std::memory_order_acq_rel);
        m_publicationState.fetch_and(
            ~(kClearing | kClearContended), std::memory_order_release);
        return false;
    }

    void markOverflow(uint32_t parameterId, uint64_t gestureSerial) noexcept
    {
        m_publicationState.fetch_or(kQuarantined, std::memory_order_acq_rel);
        uint32_t expected = kOverflowEmpty;
        if (m_overflowState.compare_exchange_strong(
                expected, kOverflowWriting, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            m_overflowParameterId.store(parameterId, std::memory_order_relaxed);
            m_overflowGestureSerial.store(
                gestureSerial != 0 ? gestureSerial : 1u,
                std::memory_order_relaxed);
            m_overflowState.store(kOverflowReady, std::memory_order_release);
        }
    }

    void reset() noexcept
    {
        m_enqueue.store(0, std::memory_order_relaxed);
        m_dequeue.store(0, std::memory_order_relaxed);
        m_publicationState.store(0, std::memory_order_relaxed);
        resetOverflowDescriptor(std::memory_order_relaxed);
        for (size_t i = 0; i < Capacity; ++i)
            m_slots[i].sequence.store(i, std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kOverflowEmpty = 0;
    static constexpr uint32_t kOverflowWriting = 1;
    static constexpr uint32_t kOverflowReady = 2;
    static constexpr uint32_t kOverflowReading = 3;
    static constexpr uint32_t kOverflowConsumed = 4;
    void resetOverflowDescriptor(std::memory_order order) noexcept
    {
        m_overflowParameterId.store(
            std::numeric_limits<uint32_t>::max(), std::memory_order_relaxed);
        m_overflowGestureSerial.store(0, std::memory_order_relaxed);
        m_overflowState.store(kOverflowEmpty, order);
    }
    std::array<Slot, Capacity> m_slots{};
    std::atomic<size_t> m_enqueue{0};
    std::atomic<size_t> m_dequeue{0};
    std::atomic<uint64_t> m_publicationState{0};
    std::atomic<uint32_t> m_overflowState{kOverflowEmpty};
    std::atomic<uint32_t> m_overflowParameterId{
        std::numeric_limits<uint32_t>::max()};
    std::atomic<uint64_t> m_overflowGestureSerial{0};
};

// Allocation-free, generation-tagged gesture table. Nested Begin/End for one
// parameter share one serial/depth; distinct parameters keep independent serials.
class NativeParameterGestureSerials
{
    static constexpr uint64_t kEmptyTag = 0;
    static constexpr uint64_t kBusyTagBit = uint64_t{1} << 63u;
    static constexpr uint32_t kGenerationMask = 0x7fffffffu;
    static constexpr int kContentionPasses = 4;
    static constexpr uint64_t kSaturated = uint64_t{1} << 63u;
    static constexpr uint64_t kResetting = uint64_t{1} << 62u;
    static constexpr uint64_t kStateFlags = kSaturated | kResetting;
    static constexpr uint64_t kDepthMask = ~kStateFlags;
    static constexpr size_t kCapacity = 512;
    struct Slot {
        std::atomic<uint64_t> tag{kEmptyTag};
        std::atomic<uint64_t> sequence{0};
        std::atomic<uint64_t> serial{0};
        std::atomic<uint32_t> depth{0};
    };

public:
    uint64_t begin(uint32_t parameterId, uint64_t freshSerial,
                   bool* outermost = nullptr,
                   bool* overflowed = nullptr) noexcept
    {
        if (outermost) *outermost = false;
        if (overflowed) *overflowed = false;
        const uint64_t stateBefore = incrementActive();
        if ((stateBefore & kStateFlags) != 0) return 0;

        bool matchingBusy = false;
        for (int pass = 0; pass < kContentionPasses; ++pass) {
            matchingBusy = false;
            for (Slot& slot : m_slots) {
                uint64_t tag = slot.tag.load(std::memory_order_acquire);
                if (tag == kEmptyTag || static_cast<uint32_t>(tag) != parameterId)
                    continue;
                if ((tag & kBusyTagBit) != 0) {
                    matchingBusy = true;
                    continue;
                }
                const uint64_t sequence =
                    slot.sequence.load(std::memory_order_acquire);
                const uint64_t busyTag = tag | kBusyTagBit;
                if (!slot.tag.compare_exchange_strong(
                        tag, busyTag, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    matchingBusy = true;
                    continue;
                }
                if (slot.sequence.load(std::memory_order_acquire) != sequence) {
                    slot.tag.store(tag, std::memory_order_release);
                    matchingBusy = true;
                    continue;
                }
                const uint64_t serial = slot.serial.load(std::memory_order_relaxed);
                const uint32_t depth = slot.depth.load(std::memory_order_relaxed);
                slot.depth.store(depth + 1u, std::memory_order_relaxed);
                slot.tag.store(tag, std::memory_order_release);
                if ((m_state.load(std::memory_order_acquire) & kStateFlags) != 0)
                    return 0;
                return serial != 0 ? serial : freshSerial;
            }
            if (!matchingBusy) break;
        }
        if (matchingBusy) return saturate(overflowed);
        for (Slot& slot : m_slots) {
            uint64_t empty = kEmptyTag;
            const uint64_t busyTag = kBusyTagBit | parameterId;
            if (!slot.tag.compare_exchange_strong(
                    empty, busyTag, std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                continue;
            uint64_t generation =
                m_generation.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (generation == 0) generation =
                m_generation.fetch_add(1, std::memory_order_relaxed) + 1u;
            uint32_t tagGeneration =
                static_cast<uint32_t>(generation) & kGenerationMask;
            if (tagGeneration == 0) tagGeneration = 1;
            uint64_t tag =
                (static_cast<uint64_t>(tagGeneration) << 32u) | parameterId;
            slot.sequence.store(generation, std::memory_order_relaxed);
            slot.serial.store(freshSerial, std::memory_order_relaxed);
            slot.depth.store(1u, std::memory_order_relaxed);
            slot.tag.store(tag, std::memory_order_release);
            if ((m_state.load(std::memory_order_acquire) & kStateFlags) != 0)
                return 0;
            if (outermost) *outermost = true;
            return freshSerial;
        }
        return saturate(overflowed);
    }

    uint64_t current(uint32_t parameterId, uint64_t fallback,
                     bool* suppressed = nullptr,
                     bool* overflowed = nullptr) noexcept
    {
        if (suppressed) *suppressed = false;
        if (overflowed) *overflowed = false;
        if ((m_state.load(std::memory_order_acquire) & kStateFlags) != 0) {
            if (suppressed) *suppressed = true;
            return 0;
        }
        bool matchingBusy = false;
        for (int pass = 0; pass < kContentionPasses; ++pass) {
            matchingBusy = false;
            for (const Slot& slot : m_slots) {
                const uint64_t before = slot.tag.load(std::memory_order_acquire);
                if (before == kEmptyTag || static_cast<uint32_t>(before) != parameterId)
                    continue;
                if ((before & kBusyTagBit) != 0) {
                    matchingBusy = true;
                    continue;
                }
                const uint64_t sequenceBefore =
                    slot.sequence.load(std::memory_order_acquire);
                const uint64_t serial = slot.serial.load(std::memory_order_relaxed);
                const uint32_t depth = slot.depth.load(std::memory_order_relaxed);
                const uint64_t after = slot.tag.load(std::memory_order_acquire);
                const uint64_t sequenceAfter =
                    slot.sequence.load(std::memory_order_acquire);
                if (before == after && sequenceBefore == sequenceAfter &&
                    depth > 0 && serial != 0) {
                    if ((m_state.load(std::memory_order_acquire) & kStateFlags) == 0)
                        return serial;
                    if (suppressed) *suppressed = true;
                    return 0;
                }
                matchingBusy = true;
            }
            if (!matchingBusy) break;
        }
        if (matchingBusy) {
            saturate(overflowed);
            if (suppressed) *suppressed = true;
            return 0;
        }
        if ((m_state.load(std::memory_order_acquire) & kStateFlags) == 0)
            return fallback;
        if (suppressed) *suppressed = true;
        return 0;
    }

    uint64_t end(uint32_t parameterId, uint64_t fallback,
                 bool* terminal = nullptr,
                 bool* overflowed = nullptr,
                 bool* consumed = nullptr) noexcept
    {
        if (terminal) *terminal = false;
        if (overflowed) *overflowed = false;
        if (consumed) *consumed = false;
        if ((m_state.load(std::memory_order_acquire) & kStateFlags) != 0) {
            const bool didConsume = finishActive(false, terminal, overflowed);
            if (consumed) *consumed = didConsume;
            return 0;
        }
        uint64_t result = fallback;
        bool terminalCandidate = false;
        bool matchingBusy = false;
        for (int pass = 0; pass < kContentionPasses; ++pass) {
            matchingBusy = false;
            for (Slot& slot : m_slots) {
                uint64_t tag = slot.tag.load(std::memory_order_acquire);
                if (tag == kEmptyTag || static_cast<uint32_t>(tag) != parameterId)
                    continue;
                if ((tag & kBusyTagBit) != 0) {
                    matchingBusy = true;
                    continue;
                }
                const uint64_t sequence =
                    slot.sequence.load(std::memory_order_acquire);
                const uint64_t busyTag = tag | kBusyTagBit;
                if (!slot.tag.compare_exchange_strong(
                        tag, busyTag, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    matchingBusy = true;
                    continue;
                }
                if (slot.sequence.load(std::memory_order_acquire) != sequence) {
                    slot.tag.store(tag, std::memory_order_release);
                    matchingBusy = true;
                    continue;
                }
                const uint64_t serial = slot.serial.load(std::memory_order_relaxed);
                const uint32_t depth = slot.depth.load(std::memory_order_relaxed);
                result = serial != 0 ? serial : fallback;
                if (depth > 1u) {
                    slot.depth.store(depth - 1u, std::memory_order_relaxed);
                    slot.tag.store(tag, std::memory_order_release);
                } else {
                    slot.depth.store(0, std::memory_order_relaxed);
                    slot.serial.store(0, std::memory_order_relaxed);
                    slot.tag.store(kEmptyTag, std::memory_order_release);
                    terminalCandidate = true;
                }
                const bool didConsume = finishActive(
                    terminalCandidate, terminal, overflowed);
                if (consumed) *consumed = didConsume;
                return didConsume && terminal && *terminal ? result : 0;
            }
            if (!matchingBusy) break;
        }
        if (matchingBusy) {
            saturate(overflowed);
            const bool didConsume = finishActive(false, terminal, overflowed);
            if (consumed) *consumed = didConsume;
        }
        return 0;
    }

    void reset() noexcept
    {
        resetSlots();
        m_generation.store(0, std::memory_order_relaxed);
        m_state.store(0, std::memory_order_relaxed);
    }

    int activeDepth() noexcept
    {
        uint64_t state = m_state.load(std::memory_order_acquire);
        if ((state & kSaturated) != 0 && (state & kDepthMask) == 0) {
            recoverIfIdle();
            state = m_state.load(std::memory_order_acquire);
        }
        return static_cast<int>(std::min<uint64_t>(
            state & kDepthMask,
            static_cast<uint64_t>(std::numeric_limits<int>::max())));
    }

private:
    uint64_t incrementActive() noexcept
    {
        return m_state.fetch_add(1u, std::memory_order_acq_rel);
    }

    uint64_t saturate(bool* overflowed) noexcept
    {
        m_state.fetch_or(kSaturated, std::memory_order_acq_rel);
        if (overflowed) *overflowed = true;
        return 0;
    }

    bool finishActive(bool terminalCandidate, bool* terminal,
                      bool* overflowed) noexcept
    {
        const uint64_t previous = m_state.fetch_sub(1u, std::memory_order_acq_rel);
        if ((previous & kDepthMask) == 0) {
            m_state.fetch_add(1u, std::memory_order_release);
            return false;
        }
        const uint64_t desired = previous - 1u;
        if ((previous & kStateFlags) == 0) {
            if (terminal) *terminal = terminalCandidate;
        } else if ((desired & kDepthMask) == 0) {
            recoverIfIdle();
        }
        if ((desired & kStateFlags) != 0 && overflowed) *overflowed = true;
        return true;
    }

    void recoverIfIdle() noexcept
    {
        uint64_t expected = kSaturated;
        if (!m_state.compare_exchange_strong(
                expected, kSaturated | kResetting,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return;
        resetSlots();
        uint64_t state = m_state.load(std::memory_order_acquire);
        for (int attempt = 0; attempt < kContentionPasses; ++attempt) {
            const uint64_t depth = state & kDepthMask;
            const uint64_t desired = depth == 0 ? 0 : (kSaturated | depth);
            if (m_state.compare_exchange_weak(
                    state, desired, std::memory_order_release,
                    std::memory_order_acquire))
                return;
        }
        const uint64_t previous =
            m_state.fetch_and(~kResetting, std::memory_order_acq_rel);
        if ((previous & kDepthMask) == 0) {
            uint64_t idle = kSaturated;
            m_state.compare_exchange_strong(
                idle, 0, std::memory_order_release,
                std::memory_order_relaxed);
        }
    }

    void resetSlots() noexcept
    {
        for (Slot& slot : m_slots) {
            slot.serial.store(0, std::memory_order_relaxed);
            slot.depth.store(0, std::memory_order_relaxed);
            slot.sequence.store(0, std::memory_order_relaxed);
            slot.tag.store(kEmptyTag, std::memory_order_relaxed);
        }
    }

    std::array<Slot, kCapacity> m_slots{};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<uint64_t> m_state{0};
};

#if defined(VERACORE_ARA_SDK_AVAILABLE)
constexpr bool kAraSdkCompiled = true;
constexpr ARA::ARAPlugInInstanceRoleFlags kAraRenderAndEditorRoles =
    static_cast<ARA::ARAPlugInInstanceRoleFlags>(
        ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole | ARA::kARAEditorViewRole);

bool validAraFactory(const ARA::ARAFactory* factory) noexcept
{
    constexpr ARA::ARASize requiredSize =
        ARA_IMPLEMENTED_STRUCT_SIZE(ARAFactory, createDocumentControllerWithDocument);
    __try {
        return factory && factory->structSize >= requiredSize &&
            factory->lowestSupportedApiGeneration <= ARA::kARAAPIGeneration_2_0_Final &&
            factory->highestSupportedApiGeneration >= ARA::kARAAPIGeneration_2_0_Final &&
            factory->initializeARAWithConfiguration && factory->uninitializeARA &&
            factory->createDocumentControllerWithDocument;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#else
constexpr bool kAraSdkCompiled = false;
#endif

float finiteAudioSample(float value) noexcept
{
    return std::isfinite(value) ? value : 0.0f;
}

bool decrementPositive(std::atomic<int>& counter) noexcept
{
    const int previous = counter.fetch_sub(1, std::memory_order_acq_rel);
    if (previous > 0) return true;
    counter.fetch_add(1, std::memory_order_release);
    return false;
}

double pluginAutomationValueAt(const PluginAutomationLane& lane, double beat) noexcept
{
    if (lane.points.empty()) return 0.0;
    if (!std::isfinite(beat)) beat = 0.0;
    const auto next = std::upper_bound(
        lane.points.begin(), lane.points.end(), beat,
        [](double target, const PluginAutomationPoint& point) { return target < point.beat; });
    if (next == lane.points.begin()) return std::clamp(next->normalizedValue, 0.0, 1.0);
    const PluginAutomationPoint& left = *(next - 1);
    if (next == lane.points.end() || lane.interpolation == PluginAutomationInterpolation::Hold)
        return std::clamp(left.normalizedValue, 0.0, 1.0);
    const double width = next->beat - left.beat;
    if (!(width > 0.0)) return std::clamp(next->normalizedValue, 0.0, 1.0);
    double position = std::clamp((beat - left.beat) / width, 0.0, 1.0);
    if (lane.interpolation == PluginAutomationInterpolation::Smooth)
        position = pluginAutomationCurvePosition(position, left);
    return std::clamp(left.normalizedValue +
        (next->normalizedValue - left.normalizedValue) * position, 0.0, 1.0);
}

struct PluginTransportState
{
    bool playing = false;
    bool recording = false;
    double bpm = 120.0;
    double projectBeat = 0.0;
    double projectSeconds = 0.0;
    double barStartBeat = 0.0;
    int barNumber = 0;
    bool cycleActive = false;
    double cycleStartBeat = 0.0;
    double cycleEndBeat = 0.0;
    double cycleStartSeconds = 0.0;
    double cycleEndSeconds = 0.0;
    double cycleStartBpm = 120.0;
    double cycleStartBarStartBeat = 0.0;
    int cycleStartBarNumber = 0;
    int cycleStartNumerator = 4;
    int cycleStartDenominator = 4;
    bool cycleWrappedAtStart = false;
    double sampleRate = 44100.0;
    double panLawDb = -3.0;
    int numerator = 4;
    int denominator = 4;
};

template <typename Emit>
void emitPluginAutomationPoints(const PluginAutomationLane& lane,
                                const PluginTransportState& transport,
                                int frames, int eventBudget, Emit&& emitter) noexcept
{
    if (!lane.enabled || lane.points.empty() || frames <= 0 || eventBudget <= 0) return;
    const double rate = std::isfinite(transport.sampleRate) && transport.sampleRate > 0.0
        ? transport.sampleRate : 44100.0;
    const double bpm = std::isfinite(transport.bpm) && transport.bpm > 0.0
        ? transport.bpm : 120.0;
    const double startBeat = std::isfinite(transport.projectBeat)
        ? std::max(0.0, transport.projectBeat) : 0.0;
    const double beatsPerSample = bpm / (60.0 * rate);
    const bool advancing = transport.playing;
    const double endBeat = startBeat + (advancing
        ? static_cast<double>(std::max(0, frames - 1)) * beatsPerSample : 0.0);
    int pendingOffset = -1;
    double pendingValue = 0.0;
    int emittedCount = 0;
    auto flushPending = [&]() {
        if (pendingOffset >= 0 && emittedCount < eventBudget) {
            emitter(pendingOffset, pendingValue);
            ++emittedCount;
        }
        pendingOffset = -1;
    };
    auto queue = [&](int offset, double value) {
        if (emittedCount >= eventBudget) return;
        offset = std::clamp(offset, 0, std::max(0, frames - 1));
        value = std::clamp(value, 0.0, 1.0);
        if (offset == pendingOffset) {
            pendingValue = value;
            return;
        }
        flushPending();
        if (emittedCount >= eventBudget) return;
        pendingOffset = offset;
        pendingValue = value;
    };
    queue(0, pluginAutomationValueAt(lane, startBeat));
    auto point = std::upper_bound(
        lane.points.begin(), lane.points.end(), startBeat,
        [](double target, const PluginAutomationPoint& candidate) { return target < candidate.beat; });
    if (advancing && lane.interpolation == PluginAutomationInterpolation::Smooth && frames > 1) {
        const auto nodeEnd = std::upper_bound(
            point, lane.points.end(), endBeat,
            [](double target, const PluginAutomationPoint& candidate) {
                return target < candidate.beat;
            });
        const int nodeCount = static_cast<int>(std::distance(point, nodeEnd));
        const int endReserve = eventBudget > 1 ? 1 : 0;
        const int interiorBudget = std::max(0, eventBudget - 1 - endReserve);
        const int emittedNodes = std::min(nodeCount, interiorBudget);
        const int smoothSegments = std::clamp(
            interiorBudget - emittedNodes, 0, kPluginAutomationSmoothSegments);
        const int last = frames - 1;
        if (emittedNodes < nodeCount) {
            // Dense block: retain representative authored nodes, then always
            // chase the exact curve value at block end.
            for (int selected = 0; selected < emittedNodes; ++selected) {
                const int sourceIndex = emittedNodes == 1 ? nodeCount / 2
                    : (selected * (nodeCount - 1)) / (emittedNodes - 1);
                const auto node = point + sourceIndex;
                const double offset = (node->beat - startBeat) / beatsPerSample;
                if (std::isfinite(offset))
                    queue(static_cast<int>(std::llround(offset)), node->normalizedValue);
            }
        } else {
            int segment = 1;
            while (point != nodeEnd || segment <= smoothSegments) {
                const int sampleOffset = segment <= smoothSegments
                    ? (segment * last) / smoothSegments : frames;
                int nodeOffset = frames;
                if (point != nodeEnd) {
                    const double offset = (point->beat - startBeat) / beatsPerSample;
                    if (std::isfinite(offset))
                        nodeOffset = std::clamp(static_cast<int>(std::llround(offset)), 0, last);
                }
                if (point != nodeEnd && nodeOffset <= sampleOffset) {
                    queue(nodeOffset, point->normalizedValue);
                    ++point;
                    if (nodeOffset == sampleOffset) ++segment;
                } else {
                    const double beat = startBeat +
                        static_cast<double>(sampleOffset) * beatsPerSample;
                    queue(sampleOffset, pluginAutomationValueAt(lane, beat));
                    ++segment;
                }
            }
        }
        if (endReserve > 0)
            queue(last, pluginAutomationValueAt(lane, endBeat));
    } else {
        for (; point != lane.points.end() && point->beat <= endBeat; ++point) {
            const double offset = (point->beat - startBeat) / beatsPerSample;
            if (std::isfinite(offset))
                queue(static_cast<int>(std::llround(offset)), point->normalizedValue);
        }
        if (advancing && lane.interpolation == PluginAutomationInterpolation::Linear &&
            frames > 1) {
            queue(frames - 1, pluginAutomationValueAt(lane, endBeat));
        }
    }
    flushPending();
}

struct PanGains
{
    float left = 1.0f;
    float right = 1.0f;
};

struct StereoDownmixCoefficients
{
    std::vector<float> left;
    std::vector<float> right;

    bool validFor(int channelCount) const noexcept
    {
        return channelCount > 0 && left.size() == static_cast<size_t>(channelCount) &&
            right.size() == static_cast<size_t>(channelCount);
    }

    bool requiresProjection(int channelCount) const noexcept
    {
        if (!validFor(channelCount)) return false;
        if (channelCount != 2) return true;
        constexpr float epsilon = 1.0e-7f;
        return std::abs(left[0] - 1.0f) > epsilon || std::abs(right[0]) > epsilon ||
            std::abs(left[1]) > epsilon || std::abs(right[1] - 1.0f) > epsilon;
    }
};

bool assignClapStereoCoefficient(uint8_t channel, PanGains& coefficient) noexcept
{
    constexpr float kMinus3Db = 0.7071067811865476f;
    constexpr float kMinus6Db = 0.5f;
    switch (channel) {
    case CLAP_SURROUND_FL: coefficient = {1.0f, 0.0f}; return true;
    case CLAP_SURROUND_FR: coefficient = {0.0f, 1.0f}; return true;
    case CLAP_SURROUND_FC: coefficient = {kMinus3Db, kMinus3Db}; return true;
    case CLAP_SURROUND_LFE: coefficient = {kMinus6Db, kMinus6Db}; return true;
    case CLAP_SURROUND_BL:
    case CLAP_SURROUND_FLC:
    case CLAP_SURROUND_SL: coefficient = {kMinus3Db, 0.0f}; return true;
    case CLAP_SURROUND_BR:
    case CLAP_SURROUND_FRC:
    case CLAP_SURROUND_SR: coefficient = {0.0f, kMinus3Db}; return true;
    case CLAP_SURROUND_BC: coefficient = {kMinus6Db, kMinus6Db}; return true;
    case CLAP_SURROUND_TFL:
    case CLAP_SURROUND_TBL:
    case CLAP_SURROUND_TSL: coefficient = {kMinus6Db, 0.0f}; return true;
    case CLAP_SURROUND_TFR:
    case CLAP_SURROUND_TBR:
    case CLAP_SURROUND_TSR: coefficient = {0.0f, kMinus6Db}; return true;
    case CLAP_SURROUND_TC:
    case CLAP_SURROUND_TFC:
    case CLAP_SURROUND_TBC: coefficient = {kMinus6Db, kMinus6Db}; return true;
    default: return false;
    }
}

bool assignVst3StereoCoefficient(Steinberg::Vst::Speaker speaker,
                                 PanGains& coefficient) noexcept
{
    using namespace Steinberg::Vst;
    constexpr float kMinus3Db = 0.7071067811865476f;
    constexpr float kMinus6Db = 0.5f;
    if (speaker == kSpeakerL) coefficient = {1.0f, 0.0f};
    else if (speaker == kSpeakerR) coefficient = {0.0f, 1.0f};
    else if (speaker == kSpeakerC || speaker == kSpeakerM)
        coefficient = {kMinus3Db, kMinus3Db};
    else if (speaker == kSpeakerLfe || speaker == kSpeakerLfe2 ||
             speaker == kSpeakerS || speaker == kSpeakerTrc ||
             speaker == kSpeakerBfc || speaker == kSpeakerBrc)
        coefficient = {kMinus6Db, kMinus6Db};
    else if (speaker == kSpeakerLs || speaker == kSpeakerLc || speaker == kSpeakerSl)
        coefficient = {kMinus3Db, 0.0f};
    else if (speaker == kSpeakerRs || speaker == kSpeakerRc || speaker == kSpeakerSr)
        coefficient = {0.0f, kMinus3Db};
    else if (speaker == kSpeakerTfl || speaker == kSpeakerTrl || speaker == kSpeakerTsl ||
             speaker == kSpeakerLcs || speaker == kSpeakerBfl || speaker == kSpeakerPl ||
             speaker == kSpeakerBsl || speaker == kSpeakerBrl || speaker == kSpeakerLw)
        coefficient = {kMinus6Db, 0.0f};
    else if (speaker == kSpeakerTfr || speaker == kSpeakerTrr || speaker == kSpeakerTsr ||
             speaker == kSpeakerRcs || speaker == kSpeakerBfr || speaker == kSpeakerPr ||
             speaker == kSpeakerBsr || speaker == kSpeakerBrr || speaker == kSpeakerRw)
        coefficient = {0.0f, kMinus6Db};
    else if (speaker == kSpeakerTc || speaker == kSpeakerTfc)
        coefficient = {kMinus6Db, kMinus6Db};
    else
        return false;
    return true;
}

bool buildClapStereoDownmix(const uint8_t* channelMap, int channelCount,
                            StereoDownmixCoefficients& result)
{
    result = {};
    if (!channelMap || channelCount <= 0 || channelCount > kMaxPluginChannels) return false;
    std::array<bool, CLAP_SURROUND_TSR + 1> seen{};
    result.left.resize(static_cast<size_t>(channelCount));
    result.right.resize(static_cast<size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        const uint8_t id = channelMap[channel];
        PanGains coefficient{};
        if (id > CLAP_SURROUND_TSR || seen[id] ||
            !assignClapStereoCoefficient(id, coefficient)) {
            result = {};
            return false;
        }
        seen[id] = true;
        result.left[static_cast<size_t>(channel)] = coefficient.left;
        result.right[static_cast<size_t>(channel)] = coefficient.right;
    }
    return true;
}

bool buildVst3StereoDownmix(Steinberg::Vst::SpeakerArrangement arrangement, int channelCount,
                            StereoDownmixCoefficients& result)
{
    result = {};
    if (arrangement == 0 || channelCount <= 0 || channelCount > kMaxPluginChannels) return false;
    result.left.reserve(static_cast<size_t>(channelCount));
    result.right.reserve(static_cast<size_t>(channelCount));
    for (int bit = 0; bit < 64; ++bit) {
        const auto speaker = static_cast<Steinberg::Vst::Speaker>(uint64_t{1} << bit);
        if ((arrangement & speaker) == 0) continue;
        PanGains coefficient{};
        if (!assignVst3StereoCoefficient(speaker, coefficient)) {
            result = {};
            return false;
        }
        result.left.push_back(coefficient.left);
        result.right.push_back(coefficient.right);
    }
    if (!result.validFor(channelCount)) {
        result = {};
        return false;
    }
    return true;
}

Steinberg::Vst::SpeakerArrangement vst3ArrangementForLayout(
    VeraCoreSpeakerLayout layout) noexcept
{
    using namespace Steinberg::Vst;
    switch (layout) {
    case VeraCoreSpeakerLayout::Surround51: return SpeakerArr::k51;
    case VeraCoreSpeakerLayout::Surround71: return SpeakerArr::k71Music;
    case VeraCoreSpeakerLayout::Atmos712: return SpeakerArr::k71_2_TF;
    case VeraCoreSpeakerLayout::Atmos714: return SpeakerArr::k71_4;
    case VeraCoreSpeakerLayout::Stereo:
    default: return SpeakerArr::kStereo;
    }
}

int canonicalIndexForClapSurround(uint8_t speaker) noexcept
{
    switch (speaker) {
    case CLAP_SURROUND_FL: return 0;
    case CLAP_SURROUND_FR: return 1;
    case CLAP_SURROUND_FC: return 2;
    case CLAP_SURROUND_LFE: return 3;
    case CLAP_SURROUND_BL: return 4;
    case CLAP_SURROUND_BR: return 5;
    case CLAP_SURROUND_SL: return 6;
    case CLAP_SURROUND_SR: return 7;
    case CLAP_SURROUND_TFL:
    case CLAP_SURROUND_TSL: return 8;
    case CLAP_SURROUND_TFR:
    case CLAP_SURROUND_TSR: return 9;
    case CLAP_SURROUND_TBL: return 10;
    case CLAP_SURROUND_TBR: return 11;
    default: return -1;
    }
}

int canonicalIndexForVst3Speaker(Steinberg::Vst::Speaker speaker) noexcept
{
    using namespace Steinberg::Vst;
    if (speaker == kSpeakerL) return 0;
    if (speaker == kSpeakerR) return 1;
    if (speaker == kSpeakerC) return 2;
    if (speaker == kSpeakerLfe) return 3;
    if (speaker == kSpeakerLs) return 4;
    if (speaker == kSpeakerRs) return 5;
    if (speaker == kSpeakerSl) return 6;
    if (speaker == kSpeakerSr) return 7;
    if (speaker == kSpeakerTfl || speaker == kSpeakerTsl) return 8;
    if (speaker == kSpeakerTfr || speaker == kSpeakerTsr) return 9;
    if (speaker == kSpeakerTrl) return 10;
    if (speaker == kSpeakerTrr) return 11;
    return -1;
}

bool buildVst3CanonicalMap(Steinberg::Vst::SpeakerArrangement arrangement,
                           int channelCount, std::vector<int>& result)
{
    result.clear();
    if (arrangement == 0 || channelCount <= 0 || channelCount > kMaxPluginChannels)
        return false;
    result.reserve(static_cast<size_t>(channelCount));
    for (int bit = 0; bit < 64; ++bit) {
        const auto speaker = static_cast<Steinberg::Vst::Speaker>(uint64_t{1} << bit);
        if ((arrangement & speaker) != 0)
            result.push_back(canonicalIndexForVst3Speaker(speaker));
    }
    if (result.size() != static_cast<size_t>(channelCount)) {
        result.clear();
        return false;
    }
    return true;
}

PanGains panGains(float gain, float pan, bool monoSource, double centerAttenuationDb) noexcept
{
    const float safeGain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 2.0f) : 1.0f;
    const float safePan = std::isfinite(pan) ? std::clamp(pan, -1.0f, 1.0f) : 0.0f;
    if (monoSource) {
        constexpr double halfPi = 1.57079632679489661923;
        constexpr double equalPowerCenterDb = -3.010299956639812;
        const double lawDb = std::isfinite(centerAttenuationDb)
            ? std::clamp(centerAttenuationDb, -12.0, 0.0) : -3.0;
        if (lawDb >= -1.0e-9) {
            const double left = safePan <= 0.0f ? 1.0 : std::cos(safePan * halfPi);
            const double right = safePan >= 0.0f ? 1.0 : std::cos(-safePan * halfPi);
            return {static_cast<float>(safeGain * left), static_cast<float>(safeGain * right)};
        }
        const double exponent = std::clamp(lawDb / equalPowerCenterDb, 0.0, 4.0);
        const double angle = (static_cast<double>(safePan) + 1.0) * (halfPi * 0.5);
        const double leftBase = std::max(0.0, std::cos(angle));
        const double rightBase = std::max(0.0, std::sin(angle));
        return {
            static_cast<float>(safeGain * (leftBase <= 1.0e-15 ? 0.0 : std::pow(leftBase, exponent))),
            static_cast<float>(safeGain * (rightBase <= 1.0e-15 ? 0.0 : std::pow(rightBase, exponent)))};
    }
    return {safeGain * (safePan <= 0.0f ? 1.0f : 1.0f - safePan),
            safeGain * (safePan >= 0.0f ? 1.0f : 1.0f + safePan)};
}

const PluginOutputRoute* outputRouteForBus(
    const std::vector<PluginOutputRoute>* routes, int busIndex) noexcept
{
    if (!routes) return nullptr;
    const auto found = std::find_if(routes->begin(), routes->end(), [busIndex](const auto& route) {
        return route.outputBusIndex == busIndex;
    });
    return found != routes->end() ? &*found : nullptr;
}

// Stereo mixer fallback for plug-in buses wider than two channels. VST3 and
// CLAP expose planar buses, while VeraCore's mixer is currently stereo. Keep
// L/R at unity, fold common cinema-order channels with standard coefficients,
// and retain unknown extra channels instead of silently dropping them.
void downmixPlanarPluginBus(float* const* channels, int channelCount, int frames,
                            float* stereoLeft, float* stereoRight,
                            const StereoDownmixCoefficients* coefficients = nullptr) noexcept
{
    if (!channels || channelCount <= 0 || frames <= 0 ||
        !stereoLeft || !stereoRight || !channels[0]) return;
    constexpr float kMinus3Db = 0.7071067811865476f;
    constexpr float kMinus6Db = 0.5f;
    const auto sample = [channels, channelCount](int channel, int frame) noexcept {
        return channel >= 0 && channel < channelCount && channels[channel]
            ? finiteAudioSample(channels[channel][frame]) : 0.0f;
    };
    if (coefficients && coefficients->validFor(channelCount)) {
        for (int frame = 0; frame < frames; ++frame) {
            float left = 0.0f;
            float right = 0.0f;
            for (int channel = 0; channel < channelCount; ++channel) {
                const float value = sample(channel, frame);
                left += value * coefficients->left[static_cast<size_t>(channel)];
                right += value * coefficients->right[static_cast<size_t>(channel)];
            }
            stereoLeft[frame] = finiteAudioSample(left);
            stereoRight[frame] = finiteAudioSample(right);
        }
        return;
    }
    if (channelCount <= 2 || !channels[1]) return;
    for (int frame = 0; frame < frames; ++frame) {
        float left = sample(0, frame);
        float right = sample(1, frame);
        if (channelCount == 3) {
            const float center = sample(2, frame) * kMinus3Db;
            left += center;
            right += center;
        } else if (channelCount == 4) {
            left += sample(2, frame) * kMinus3Db;
            right += sample(3, frame) * kMinus3Db;
        } else {
            const float center = sample(2, frame) * kMinus3Db;
            left += center;
            right += center;
            if (channelCount == 5) {
                left += sample(3, frame) * kMinus3Db;
                right += sample(4, frame) * kMinus3Db;
            } else {
                const float lfe = sample(3, frame) * kMinus6Db;
                left += lfe;
                right += lfe;
                left += sample(4, frame) * kMinus3Db;
                right += sample(5, frame) * kMinus3Db;
                if (channelCount == 7) {
                    const float centerBack = sample(6, frame) * kMinus6Db;
                    left += centerBack;
                    right += centerBack;
                } else if (channelCount >= 8) {
                    left += sample(6, frame) * kMinus6Db;
                    right += sample(7, frame) * kMinus6Db;
                    for (int channel = 8; channel < channelCount; ++channel) {
                        if ((channel & 1) == 0)
                            left += sample(channel, frame) * kMinus6Db;
                        else
                            right += sample(channel, frame) * kMinus6Db;
                    }
                }
            }
        }
        stereoLeft[frame] = finiteAudioSample(left);
        stereoRight[frame] = finiteAudioSample(right);
    }
}

void deliverPluginOutput(float* ownTrackMix, int frames, const float* left, const float* right,
                         bool mono, int layerId, int busIndex, bool mainBus,
                         float gain, float pan, double panLawDb,
                         const std::vector<PluginOutputRoute>* routes,
                         VeraCorePluginOutputSink sink, void* sinkContext, bool replaceOwnTrack)
{
    if (!ownTrackMix || !left || frames <= 0) return;
    const PluginOutputRoute* route = outputRouteForBus(routes, busIndex);
    if (route && !route->enabled) return;
    const int sourceTrack = layerId >= 0 ? layerId / kLayerIdStride : -1;
    const int destination = route ? route->destinationTrackIndex : sourceTrack;
    const bool toOwnTrack = destination < 0 || destination == sourceTrack;
    if (!mainBus && !route) return;
    if (!toOwnTrack) {
        if (sink) sink(sinkContext, destination, left, right ? right : left,
                       static_cast<unsigned long>(frames), gain, pan, mono);
        return;
    }
    const PanGains gains = panGains(gain, pan, mono, panLawDb);
    const float* safeRight = right ? right : left;
    for (int frame = 0; frame < frames; ++frame) {
        const float outputLeft = finiteAudioSample(left[frame] * gains.left);
        const float outputRight = finiteAudioSample(safeRight[frame] * gains.right);
        if (replaceOwnTrack) {
            ownTrackMix[frame * 2] = outputLeft;
            ownTrackMix[frame * 2 + 1] = outputRight;
        } else {
            ownTrackMix[frame * 2] = finiteAudioSample(
                finiteAudioSample(ownTrackMix[frame * 2]) + outputLeft);
            ownTrackMix[frame * 2 + 1] = finiteAudioSample(
                finiteAudioSample(ownTrackMix[frame * 2 + 1]) + outputRight);
        }
    }
}

union ClapInputEvent {
    clap_event_header_t header;
    clap_event_midi_t midi;
    clap_event_note_t note;
    clap_event_param_value_t parameter;
    clap_event_param_gesture_t gesture;
};

struct ClapStateWriter { QByteArray bytes; };
struct ClapStateReader { const QByteArray* bytes = nullptr; int offset = 0; };

int64_t CLAP_ABI clapStateWrite(const clap_ostream_t* stream, const void* buffer, uint64_t size)
{
    auto* writer = stream ? static_cast<ClapStateWriter*>(stream->ctx) : nullptr;
    if (!writer || (!buffer && size > 0) || size > static_cast<uint64_t>(kMaxPluginStateBytes) ||
        writer->bytes.size() > kMaxPluginStateBytes - static_cast<int>(size)) return -1;
    writer->bytes.append(static_cast<const char*>(buffer), static_cast<int>(size));
    return static_cast<int64_t>(size);
}

int64_t CLAP_ABI clapStateRead(const clap_istream_t* stream, void* buffer, uint64_t size)
{
    auto* reader = stream ? static_cast<ClapStateReader*>(stream->ctx) : nullptr;
    if (!reader || !reader->bytes || (!buffer && size > 0)) return -1;
    const int available = reader->bytes->size() - reader->offset;
    const int count = static_cast<int>(std::min<uint64_t>(size, static_cast<uint64_t>(std::max(0, available))));
    if (count > 0) std::memcpy(buffer, reader->bytes->constData() + reader->offset, count);
    reader->offset += count;
    return count;
}

double transportBeatsPerBar(const PluginTransportState& transport)
{
    return static_cast<double>(transport.numerator) * 4.0 /
        static_cast<double>(transport.denominator);
}

double boundedPluginTimelineSeconds(double value, double fallback) noexcept
{
    const double candidate = std::isfinite(value) && value >= 0.0
        ? value : fallback;
    if (!std::isfinite(candidate)) return kMaxPluginTimelineSeconds;
    return std::clamp(candidate, 0.0, kMaxPluginTimelineSeconds);
}

int64_t saturatedPluginTimelineSamples(double seconds, double sampleRate) noexcept
{
    const long double scaled = static_cast<long double>(
        boundedPluginTimelineSeconds(seconds, 0.0)) *
        static_cast<long double>(std::isfinite(sampleRate) && sampleRate > 0.0
            ? sampleRate : 44100.0);
    constexpr int64_t maximum = std::numeric_limits<int64_t>::max();
    if (!(scaled > 0.0L)) return 0;
    if (scaled >= static_cast<long double>(maximum)) return maximum;
    return static_cast<int64_t>(std::llround(scaled));
}

bool validPluginCycle(const PluginTransportState& transport) noexcept
{
    return transport.cycleActive &&
        std::isfinite(transport.cycleStartBeat) &&
        std::isfinite(transport.cycleEndBeat) &&
        std::isfinite(transport.cycleStartSeconds) &&
        std::isfinite(transport.cycleEndSeconds) &&
        transport.cycleStartBeat >= 0.0 &&
        transport.cycleEndBeat > transport.cycleStartBeat &&
        transport.cycleStartSeconds >= 0.0 &&
        transport.cycleEndSeconds > transport.cycleStartSeconds;
}

void resetPluginTransportToCycleStart(PluginTransportState& transport) noexcept
{
    transport.projectBeat = transport.cycleStartBeat;
    transport.projectSeconds = transport.cycleStartSeconds;
    transport.bpm = transport.cycleStartBpm;
    transport.barStartBeat = transport.cycleStartBarStartBeat;
    transport.barNumber = transport.cycleStartBarNumber;
    transport.numerator = transport.cycleStartNumerator;
    transport.denominator = transport.cycleStartDenominator;
    transport.cycleWrappedAtStart = true;
}

int pluginFramesUntilCycleEnd(const PluginTransportState& transport,
                              double sampleRate) noexcept
{
    if (!transport.playing || !validPluginCycle(transport) ||
        !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return INT32_MAX;
    const long double cycleFrames =
        (static_cast<long double>(transport.cycleEndSeconds) -
         static_cast<long double>(transport.cycleStartSeconds)) *
        static_cast<long double>(sampleRate);
    // Preserve hard realtime bounds for pathological sub-millisecond loops.
    // Plug-ins still receive loop metadata, but the host does not issue one
    // process call per sample for cycles shorter than a useful SIMD quantum.
    if (cycleFrames < kMinPluginCycleFrames) return INT32_MAX;
    const long double remaining =
        (static_cast<long double>(transport.cycleEndSeconds) -
         static_cast<long double>(transport.projectSeconds)) *
        static_cast<long double>(sampleRate);
    if (!(remaining > 0.0L)) return 0;
    if (remaining >= static_cast<long double>(INT32_MAX)) return INT32_MAX;
    const long double nearest = std::round(remaining);
    const long double stable = std::fabs(remaining - nearest) <= 1.0e-9L
        ? nearest : remaining;
    return std::max(1, static_cast<int>(std::ceil(stable)));
}

void alignPluginTransportBarToBeat(PluginTransportState& transport) noexcept
{
    const double beatsPerBar = transportBeatsPerBar(transport);
    if (!(beatsPerBar > 0.0) || !std::isfinite(transport.barStartBeat) ||
        transport.barStartBeat < 0.0 || transport.projectBeat < transport.barStartBeat)
        return;
    const double advanced = std::floor(
        (transport.projectBeat - transport.barStartBeat + 1.0e-9) / beatsPerBar);
    const int64_t barsAdvanced = static_cast<int64_t>(
        std::min<double>(INT32_MAX, std::max(0.0, advanced)));
    if (barsAdvanced <= 0) return;
    transport.barStartBeat += static_cast<double>(barsAdvanced) * beatsPerBar;
    transport.barNumber = static_cast<int>(std::min<int64_t>(
        INT32_MAX, static_cast<int64_t>(transport.barNumber) + barsAdvanced));
}

void advancePluginTransportBySamples(PluginTransportState& transport,
                                     int sampleOffset, double sampleRate) noexcept
{
    if (!transport.playing || sampleOffset <= 0 || !std::isfinite(sampleRate) ||
        sampleRate <= 0.0) return;
    transport.cycleWrappedAtStart = false;
    const double seconds = static_cast<double>(sampleOffset) / sampleRate;
    const bool cycle = validPluginCycle(transport);
    const double targetSeconds = transport.projectSeconds + seconds;
    if (cycle && targetSeconds >= transport.cycleEndSeconds - 1.0e-12) {
        const double duration = transport.cycleEndSeconds - transport.cycleStartSeconds;
        double wrapped = std::fmod(targetSeconds - transport.cycleStartSeconds, duration);
        if (!std::isfinite(wrapped) || wrapped < 0.0) wrapped = 0.0;
        resetPluginTransportToCycleStart(transport);
        transport.projectSeconds = boundedPluginTimelineSeconds(
            transport.cycleStartSeconds + wrapped, transport.cycleStartSeconds);
        transport.projectBeat = std::max(0.0, transport.cycleStartBeat +
            wrapped * transport.cycleStartBpm / 60.0);
    } else {
        transport.projectSeconds = boundedPluginTimelineSeconds(
            targetSeconds, kMaxPluginTimelineSeconds);
        transport.projectBeat = std::max(0.0, transport.projectBeat +
            seconds * transport.bpm / 60.0);
    }
    alignPluginTransportBarToBeat(transport);
}

QString resolveClapBinary(const QString& path)
{
    const QFileInfo info(path);
    if (info.isFile()) {
        return info.absoluteFilePath();
    }
    const QDir binaryDir(info.absoluteFilePath() + "/Contents/x86_64-win");
    const QFileInfoList binaries = binaryDir.entryInfoList({"*.clap"}, QDir::Files);
    return binaries.isEmpty() ? QString() : binaries.first().absoluteFilePath();
}

using Vst2EntryProc = vst_effect_t* (VST_FUNCTION_INTERFACE*)(vst_host_callback_t);

class Vst2Instance;
thread_local Vst2Instance* gCreatingVst2 = nullptr;

vst_effect_t* createVst2Seh(Vst2EntryProc entry, vst_host_callback_t callback) noexcept
{
    __try {
        return entry ? entry(callback) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

intptr_t controlVst2Seh(vst_effect_t* effect, int32_t opcode, int32_t index,
                        intptr_t value, void* data, float option) noexcept
{
    __try {
        return effect && effect->control
            ? effect->control(effect, opcode, index, value, data, option)
            : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool clapEntryInitSeh(const clap_plugin_entry_t* entry, const char* path) noexcept
{
    __try { return entry && entry->init && entry->init(path); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const void* clapGetFactorySeh(const clap_plugin_entry_t* entry, const char* id) noexcept
{
    __try { return entry && entry->get_factory ? entry->get_factory(id) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

const clap_plugin_t* clapCreatePluginSeh(const clap_plugin_factory_t* factory,
                                         const clap_host_t* host, const char* id) noexcept
{
    __try { return factory && factory->create_plugin ? factory->create_plugin(factory, host, id) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool clapPluginInitSeh(const clap_plugin_t* plugin) noexcept
{
    __try { return plugin && plugin->init && plugin->init(plugin); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool clapPluginActivateSeh(const clap_plugin_t* plugin, double sampleRate,
                           uint32_t minFrames, uint32_t maxFrames) noexcept
{
    __try { return plugin && plugin->activate && plugin->activate(plugin, sampleRate, minFrames, maxFrames); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool clapPluginStartSeh(const clap_plugin_t* plugin) noexcept
{
    __try { return plugin && plugin->start_processing && plugin->start_processing(plugin); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

float getVst2ParameterSeh(vst_effect_t* effect, uint32_t index) noexcept
{
    __try { return effect && effect->get_parameter ? effect->get_parameter(effect, index) : 0.0f; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

bool setVst2ParameterSeh(vst_effect_t* effect, uint32_t index, float value) noexcept
{
    __try {
        if (!effect || !effect->set_parameter) return false;
        effect->set_parameter(effect, index, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void clapPluginStopSeh(const clap_plugin_t* plugin) noexcept
{
    __try { if (plugin && plugin->stop_processing) plugin->stop_processing(plugin); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void clapPluginDeactivateSeh(const clap_plugin_t* plugin) noexcept
{
    __try { if (plugin && plugin->deactivate) plugin->deactivate(plugin); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void clapPluginDestroySeh(const clap_plugin_t* plugin) noexcept
{
    __try { if (plugin && plugin->destroy) plugin->destroy(plugin); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void clapPluginOnMainThreadSeh(const clap_plugin_t* plugin) noexcept
{
    __try { if (plugin && plugin->on_main_thread) plugin->on_main_thread(plugin); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void clapEntryDeinitSeh(const clap_plugin_entry_t* entry) noexcept
{
    __try { if (entry && entry->deinit) entry->deinit(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

const void* clapGetExtensionSeh(const clap_plugin_t* plugin, const char* id) noexcept
{
    __try { return plugin && plugin->get_extension ? plugin->get_extension(plugin, id) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

uint32_t clapParamCountSeh(const clap_plugin_params_t* params,
                           const clap_plugin_t* plugin) noexcept
{
    __try { return params && params->count ? params->count(plugin) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool clapParamInfoSeh(const clap_plugin_params_t* params, const clap_plugin_t* plugin,
                      uint32_t index, clap_param_info_t& info) noexcept
{
    __try { return params && params->get_info && params->get_info(plugin, index, &info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool clapParamValueSeh(const clap_plugin_params_t* params, const clap_plugin_t* plugin,
                       clap_id id, double& value) noexcept
{
    __try { return params && params->get_value && params->get_value(plugin, id, &value); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool clapParamTextSeh(const clap_plugin_params_t* params, const clap_plugin_t* plugin,
                      clap_id id, double value, char* text, uint32_t capacity) noexcept
{
    __try {
        return params && params->value_to_text && text && capacity > 0 &&
            params->value_to_text(plugin, id, value, text, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void clapParamFlushSeh(const clap_plugin_params_t* params, const clap_plugin_t* plugin,
                       const clap_input_events_t* input,
                       const clap_output_events_t* output) noexcept
{
    __try {
        const auto flush = params ? params->flush : nullptr;
        if (flush) flush(plugin, input, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

#if defined(VERACORE_ARA_SDK_AVAILABLE)
const ARA::ARAFactory* clapAraFactorySeh(const clap_ara_plugin_extension_t* extension,
                                         const clap_plugin_t* plugin) noexcept
{
    __try { return extension && extension->get_factory ? extension->get_factory(plugin) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

const ARA::ARAPlugInExtensionInstance* clapAraBindSeh(
    const clap_ara_plugin_extension_t* extension,
    const clap_plugin_t* plugin,
    ARA::ARADocumentControllerRef documentController,
    ARA::ARAPlugInInstanceRoleFlags knownRoles,
    ARA::ARAPlugInInstanceRoleFlags assignedRoles) noexcept
{
    __try {
        return extension && extension->bind_to_document_controller
            ? extension->bind_to_document_controller(
                plugin, documentController, knownRoles, assignedRoles)
            : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool addAraRendererRegionSeh(const ARA::ARAPlugInExtensionInstance* instance,
                             ARA::ARAPlaybackRegionRef region,
                             bool addEditorRenderer) noexcept
{
    bool playbackAdded = false;
    __try {
        if (!instance || !region || !instance->playbackRendererRef ||
            !instance->playbackRendererInterface ||
            !instance->playbackRendererInterface->addPlaybackRegion ||
            !instance->playbackRendererInterface->removePlaybackRegion) return false;
        instance->playbackRendererInterface->addPlaybackRegion(
            instance->playbackRendererRef, region);
        playbackAdded = true;
        if (addEditorRenderer) {
            if (!instance->editorRendererRef || !instance->editorRendererInterface ||
                !instance->editorRendererInterface->addPlaybackRegion) {
                instance->playbackRendererInterface->removePlaybackRegion(
                    instance->playbackRendererRef, region);
                return false;
            }
            instance->editorRendererInterface->addPlaybackRegion(
                instance->editorRendererRef, region);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (playbackAdded) {
            __try {
                if (instance && instance->playbackRendererInterface &&
                    instance->playbackRendererInterface->removePlaybackRegion)
                    instance->playbackRendererInterface->removePlaybackRegion(
                        instance->playbackRendererRef, region);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        return false;
    }
}

void removeAraRendererRegionSeh(const ARA::ARAPlugInExtensionInstance* instance,
                                ARA::ARAPlaybackRegionRef region,
                                bool removeEditorRenderer) noexcept
{
    __try {
        if (removeEditorRenderer && instance && instance->editorRendererRef &&
            instance->editorRendererInterface &&
            instance->editorRendererInterface->removePlaybackRegion)
            instance->editorRendererInterface->removePlaybackRegion(
                instance->editorRendererRef, region);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        if (instance && instance->playbackRendererRef &&
            instance->playbackRendererInterface &&
            instance->playbackRendererInterface->removePlaybackRegion)
            instance->playbackRendererInterface->removePlaybackRegion(
                instance->playbackRendererRef, region);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool addAraEditorRegionSeh(const ARA::ARAPlugInExtensionInstance* instance,
                           ARA::ARAPlaybackRegionRef region) noexcept
{
    __try {
        if (!instance || !region || !instance->editorRendererRef ||
            !instance->editorRendererInterface ||
            !instance->editorRendererInterface->addPlaybackRegion) return false;
        instance->editorRendererInterface->addPlaybackRegion(
            instance->editorRendererRef, region);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void removeAraEditorRegionSeh(const ARA::ARAPlugInExtensionInstance* instance,
                              ARA::ARAPlaybackRegionRef region) noexcept
{
    __try {
        if (instance && region && instance->editorRendererRef &&
            instance->editorRendererInterface &&
            instance->editorRendererInterface->removePlaybackRegion)
            instance->editorRendererInterface->removePlaybackRegion(
                instance->editorRendererRef, region);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool notifyAraSelectionSeh(const ARA::ARAPlugInExtensionInstance* instance,
                           const std::vector<ARA::ARAPlaybackRegionRef>& regions) noexcept
{
    __try {
        if (!instance || !instance->editorViewRef || !instance->editorViewInterface ||
            !instance->editorViewInterface->notifySelection) return false;
        ARA::ARAViewSelection selection{};
        selection.structSize = ARA::kARAViewSelectionMinSize;
        selection.playbackRegionRefsCount = static_cast<ARA::ARASize>(regions.size());
        selection.playbackRegionRefs = regions.empty() ? nullptr : regions.data();
        selection.regionSequenceRefsCount = 0;
        selection.regionSequenceRefs = nullptr;
        selection.timeRange = nullptr;
        instance->editorViewInterface->notifySelection(instance->editorViewRef, &selection);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#endif

bool clapSetRenderModeSeh(const clap_plugin_render_t* render, const clap_plugin_t* plugin,
                          clap_plugin_render_mode mode) noexcept
{
    __try { return render && render->set && render->set(plugin, mode); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint32_t clapNoteNameCountSeh(const clap_plugin_note_name_t* names,
                              const clap_plugin_t* plugin) noexcept
{
    __try { return names && names->count ? names->count(plugin) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool clapNoteNameGetSeh(const clap_plugin_note_name_t* names, const clap_plugin_t* plugin,
                        uint32_t index, clap_note_name_t* noteName) noexcept
{
    __try { return names && names->get && noteName && names->get(plugin, index, noteName); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint32_t clapAudioPortCountSeh(const clap_plugin_audio_ports_t* ports,
                               const clap_plugin_t* plugin, bool input) noexcept
{
    __try { return ports && ports->count ? ports->count(plugin, input) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool clapAudioPortGetSeh(const clap_plugin_audio_ports_t* ports, const clap_plugin_t* plugin,
                         uint32_t index, bool input, clap_audio_port_info_t* info) noexcept
{
    __try { return ports && ports->get && ports->get(plugin, index, input, info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint32_t clapAudioPortsConfigCountSeh(const clap_plugin_audio_ports_config_t* configs,
                                      const clap_plugin_t* plugin) noexcept
{
    __try { return configs && configs->count ? configs->count(plugin) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool clapAudioPortsConfigGetSeh(const clap_plugin_audio_ports_config_t* configs,
                                const clap_plugin_t* plugin, uint32_t index,
                                clap_audio_ports_config_t* info) noexcept
{
    __try { return configs && configs->get && info && configs->get(plugin, index, info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool clapAudioPortsConfigSelectSeh(const clap_plugin_audio_ports_config_t* configs,
                                   const clap_plugin_t* plugin, clap_id id) noexcept
{
    __try { return configs && configs->select && configs->select(plugin, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint32_t clapSurroundChannelMapSeh(const clap_plugin_surround_t* surround,
                                   const clap_plugin_t* plugin, bool input,
                                   uint32_t portIndex, uint8_t* channelMap,
                                   uint32_t capacity) noexcept
{
    __try {
        return surround && surround->get_channel_map && channelMap && capacity > 0
            ? surround->get_channel_map(plugin, input, portIndex, channelMap, capacity) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint32_t clapNotePortCountSeh(const clap_plugin_note_ports_t* ports,
                              const clap_plugin_t* plugin, bool input) noexcept
{
    __try { return ports && ports->count ? ports->count(plugin, input) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool clapNotePortGetSeh(const clap_plugin_note_ports_t* ports, const clap_plugin_t* plugin,
                        uint32_t index, bool input, clap_note_port_info_t* info) noexcept
{
    __try { return ports && ports->get && info && ports->get(plugin, index, input, info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint32_t clapTailSamplesSeh(const clap_plugin_tail_t* tail, const clap_plugin_t* plugin) noexcept
{
    __try { return tail && tail->get ? tail->get(plugin) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint32_t clapLatencySamplesSeh(const clap_plugin_latency_t* latency,
                               const clap_plugin_t* plugin) noexcept
{
    __try { return latency && latency->get ? latency->get(plugin) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool clapStateSaveSeh(const clap_plugin_state_t* state, const clap_plugin_t* plugin,
                      const clap_ostream_t* stream) noexcept
{
    __try { return state && state->save && state->save(plugin, stream); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool clapStateLoadSeh(const clap_plugin_state_t* state, const clap_plugin_t* plugin,
                      const clap_istream_t* stream) noexcept
{
    __try { return state && state->load && state->load(plugin, stream); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Vst2TimeInfo
{
    double samplePos = 0.0;
    double sampleRate = 44100.0;
    double nanoSeconds = 0.0;
    double ppqPos = 0.0;
    double tempo = 120.0;
    double barStartPos = 0.0;
    double cycleStartPos = 0.0;
    double cycleEndPos = 0.0;
    int32_t timeSigNumerator = 4;
    int32_t timeSigDenominator = 4;
    int32_t smpteOffset = 0;
    int32_t smpteFrameRate = 0;
    int32_t samplesToNextClock = 0;
    int32_t flags = 0;
};
static_assert(sizeof(Vst2TimeInfo) == 88, "Unexpected VST2 time-info ABI layout.");

struct Vst2MidiKeyName
{
    int32_t programIndex = -1;
    int32_t keyNumber = 0;
    char name[64]{};
    int32_t reserved = 0;
    int32_t flags = 0;
};
static_assert(sizeof(Vst2MidiKeyName) == 80, "Unexpected VST2 MIDI-key-name ABI layout.");
constexpr int32_t kVst2GetMidiKeyNameOpcode = 66;

enum Vst2TimeFlags : int32_t
{
    kVst2TransportChanged = 1 << 0,
    kVst2TransportPlaying = 1 << 1,
    kVst2TransportCycleActive = 1 << 2,
    kVst2TransportRecording = 1 << 3,
    kVst2PpqPosValid = 1 << 9,
    kVst2TempoValid = 1 << 10,
    kVst2BarsValid = 1 << 11,
    kVst2CyclePosValid = 1 << 12,
    kVst2TimeSigValid = 1 << 13
};

class Vst2Instance
{
public:
    ~Vst2Instance() { close(); }

    bool open(const QString& path, double sampleRate, const QByteArray& state = {}, bool offline = false)
    {
        close();
        m_crashed = false;
        m_offline = offline;
        m_path = path;
        m_sampleRate = validSampleRate(sampleRate);
        const std::wstring widePath = path.toStdWString();
        m_module = LoadLibraryExW(widePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_module) {
            m_module = LoadLibraryExW(widePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (!m_module) {
            return false;
        }

        auto entry = reinterpret_cast<Vst2EntryProc>(GetProcAddress(m_module, "VSTPluginMain"));
        if (!entry) {
            entry = reinterpret_cast<Vst2EntryProc>(GetProcAddress(m_module, "main"));
        }
        if (!entry) {
            close();
            return false;
        }

        gCreatingVst2 = this;
        m_effect = createVst2Seh(entry, &hostCallback);
        gCreatingVst2 = nullptr;
        if (!m_effect || m_effect->magic_number != VST_MAGICNUMBER || !m_effect->control) {
            close();
            return false;
        }
        m_effect->host_internal = this;
        controlVst2Seh(m_effect, VST_EFFECT_OPCODE_CREATE, 0, 0, nullptr, 0.0f);
        controlVst2Seh(m_effect, VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr,
                       static_cast<float>(m_sampleRate));
        controlVst2Seh(m_effect, VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, kDefaultBlockSize, nullptr, 0.0f);
        // VST2 exposes no default-value query.  Snapshot the plug-in's freshly
        // initialized values before restoring the project chunk so Reset is a
        // real factory-default operation rather than a no-op at the current value.
        const int parameterCount = std::clamp(m_effect->num_params, 0, 65536);
        m_parameterDefaults.resize(static_cast<size_t>(parameterCount));
        for (int index = 0; index < parameterCount; ++index) {
            m_parameterDefaults[static_cast<size_t>(index)] = std::clamp(
                getVst2ParameterSeh(m_effect, static_cast<uint32_t>(index)), 0.0f, 1.0f);
        }
        if (!state.isEmpty()) restoreState(state);
        m_stateDirty.store(false, std::memory_order_relaxed);
        m_editDepth.store(0, std::memory_order_relaxed);
        m_gestureSerial.store(0, std::memory_order_relaxed);
        m_hostParameterCallDepth.store(0, std::memory_order_relaxed);
        m_hostParameterThreadId.store(0, std::memory_order_relaxed);
        m_parameterEdits.reset();
        m_nativeGestureSerials.reset();
        m_timeInfo = {};
        m_hasTimeInfo = false;
        m_previousBlockFrames = 0;
        controlVst2Seh(m_effect, VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 1, nullptr, 0.0f);
        controlVst2Seh(m_effect, VST_EFFECT_OPCODE_PROCESS_BEGIN, 0, 0, nullptr, 0.0f);
        m_midiEvents.reserve(kMaxMidiEventsPerBlock);
        prepareBuffers(kDefaultBlockSize);
        return m_effect->process_float != nullptr || m_effect->process != nullptr;
    }

    void close()
    {
        if (m_effect && m_effect->control) {
            controlVst2Seh(m_effect, VST_EFFECT_OPCODE_PROCESS_END, 0, 0, nullptr, 0.0f);
            controlVst2Seh(m_effect, VST_EFFECT_OPCODE_SUSPEND_RESUME, 0, 0, nullptr, 0.0f);
            controlVst2Seh(m_effect, VST_EFFECT_OPCODE_DESTROY, 0, 0, nullptr, 0.0f);
        }
        m_effect = nullptr;
        if (m_module) {
            FreeLibrary(m_module);
            m_module = nullptr;
        }
        m_inputs.clear();
        m_outputs.clear();
        m_inputPointers.clear();
        m_outputPointers.clear();
        m_midiEvents.clear();
        m_parameterDefaults.clear();
        m_automationCacheCount = 0;
        m_automationInvalidation.fetch_add(1, std::memory_order_relaxed);
        m_crashed = false;
        m_panicPending = false;
        m_offline = false;
        m_stateDirty.store(false, std::memory_order_relaxed);
        m_editDepth.store(0, std::memory_order_relaxed);
        m_gestureSerial.store(0, std::memory_order_relaxed);
        m_hostParameterCallDepth.store(0, std::memory_order_relaxed);
        m_hostParameterThreadId.store(0, std::memory_order_relaxed);
        m_parameterEdits.reset();
        m_nativeGestureSerials.reset();
    }

    bool matches(const QString& path, double sampleRate, bool offline) const
    {
        return m_effect && !m_crashed && m_path == path &&
            std::fabs(m_sampleRate - validSampleRate(sampleRate)) < 0.5 && m_offline == offline;
    }

    bool healthy() const { return m_effect && !m_crashed; }

    PluginTechnologyCapabilities capabilities() const
    {
        PluginTechnologyCapabilities result;
        result.format = "VST2";
        result.audioPorts = m_effect != nullptr;
        result.notePorts = m_effect != nullptr;
        result.parameters = m_effect && m_effect->num_params > 0;
        result.persistentState = m_effect != nullptr;
        result.nativeGui = m_effect && (m_effect->flags & VST_EFFECT_FLAG_EDITOR) != 0;
        result.latency = m_effect != nullptr;
        result.tail = m_effect != nullptr;
        result.offlineRender = m_effect != nullptr;
        result.noteNames = m_effect != nullptr;
        // VST2 MIDI key names are labels only; the ABI exposes no standardized
        // articulation/keyswitch semantics comparable to VST3 IKeyswitchController.
        result.keyswitches = false;
        result.araSdkCompiled = kAraSdkCompiled;
        return result;
    }

    std::vector<PluginAudioOutputBus> outputBuses() const
    {
        std::vector<PluginAudioOutputBus> buses;
        const int channelCount = m_effect ? std::clamp(m_effect->num_outputs, 0, kMaxPluginChannels) : 0;
        buses.reserve(static_cast<size_t>((channelCount + 1) / 2));
        for (int channel = 0, bus = 0; channel < channelCount; channel += 2, ++bus) {
            const int channels = std::min(2, channelCount - channel);
            buses.push_back({QString("vst2-pair-%1").arg(bus),
                             channels == 1
                                 ? QString("Output %1").arg(channel + 1)
                                 : QString("Outputs %1/%2").arg(channel + 1).arg(channel + 2),
                             bus, channels, bus == 0});
        }
        return buses;
    }

    std::vector<PluginParameterInfo> parameters() const
    {
        std::vector<PluginParameterInfo> result;
        if (!m_effect || m_crashed || m_effect->num_params <= 0) return result;
        const int count = std::clamp(m_effect->num_params, 0, 65536);
        result.reserve(static_cast<size_t>(count));
        for (int index = 0; index < count; ++index) {
            char name[VST_BUFFER_SIZE_PARAM_NAME]{};
            char unit[VST_BUFFER_SIZE_PARAM_LABEL]{};
            char display[VST_BUFFER_SIZE_PARAM_VALUE]{};
            controlVst2Seh(m_effect, VST_EFFECT_OPCODE_PARAM_GET_NAME,
                           index, 0, name, 0.0f);
            controlVst2Seh(m_effect, VST_EFFECT_OPCODE_PARAM_GET_LABEL,
                           index, 0, unit, 0.0f);
            controlVst2Seh(m_effect, VST_EFFECT_OPCODE_PARAM_GET_VALUE,
                           index, 0, display, 0.0f);
            const double value = std::clamp(static_cast<double>(
                getVst2ParameterSeh(m_effect, static_cast<uint32_t>(index))), 0.0, 1.0);
            PluginParameterInfo info;
            info.stableId = static_cast<uint32_t>(index);
            info.name = QString::fromLocal8Bit(name, strnlen_s(name, sizeof(name))).trimmed();
            if (info.name.isEmpty()) info.name = QString("Parameter %1").arg(index + 1);
            info.unit = QString::fromLocal8Bit(unit, strnlen_s(unit, sizeof(unit))).trimmed();
            info.displayValue = QString::fromLocal8Bit(
                display, strnlen_s(display, sizeof(display))).trimmed();
            info.normalizedValue = value;
            info.defaultNormalizedValue = static_cast<size_t>(index) < m_parameterDefaults.size()
                ? m_parameterDefaults[static_cast<size_t>(index)] : value;
            info.automatable = controlVst2Seh(
                m_effect, VST_EFFECT_OPCODE_PARAM_IS_AUTOMATABLE,
                index, 0, nullptr, 0.0f) != 0;
            result.push_back(std::move(info));
        }
        return result;
    }

    bool beginParameterGesture(uint32_t parameterId) noexcept
    {
        if (!m_effect || m_crashed || parameterId >= static_cast<uint32_t>(
                std::max(0, m_effect->num_params))) return false;
        if (m_editDepth.fetch_add(1, std::memory_order_acq_rel) == 0)
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }
    bool setParameterNormalized(uint32_t parameterId, double value) noexcept
    {
        if (!m_effect || m_crashed || parameterId >= static_cast<uint32_t>(
                std::max(0, m_effect->num_params)) || !std::isfinite(value)) return false;
        if (m_editDepth.load(std::memory_order_acquire) == 0)
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
        beginHostParameterCall();
        const bool changed = setVst2ParameterSeh(
            m_effect, parameterId, static_cast<float>(std::clamp(value, 0.0, 1.0)));
        endHostParameterCall();
        if (!changed) return false;
        m_automationInvalidation.fetch_add(1, std::memory_order_release);
        m_stateDirty.store(true, std::memory_order_release);
        return true;
    }
    bool endParameterGesture(uint32_t parameterId) noexcept
    {
        if (!m_effect || parameterId >= static_cast<uint32_t>(
                std::max(0, m_effect->num_params))) return false;
        decrementPositive(m_editDepth);
        m_stateDirty.store(true, std::memory_order_release);
        return true;
    }

    void requestPanic() { m_panicPending = true; }
    bool hasPendingPanic() const { return m_panicPending; }
    uint32_t tailSamples() const
    {
        const intptr_t samples = controlVst2Seh(
            m_effect, VST_EFFECT_OPCODE_TAIL_SAMPLES, 0, 0, nullptr, 0.0f);
        return samples > 1
            ? static_cast<uint32_t>(std::min<intptr_t>(samples, std::numeric_limits<uint32_t>::max()))
            : 0;
    }
    uint32_t latencySamples() const
    {
        return m_effect && !m_crashed && m_effect->delay > 0
            ? static_cast<uint32_t>(m_effect->delay)
            : 0;
    }

    bool consumeStateDirty(uint64_t& gestureSerial) noexcept
    {
        if (m_editDepth.load(std::memory_order_acquire) > 0) return false;
        if (!m_stateDirty.exchange(false, std::memory_order_acq_rel)) return false;
        gestureSerial = m_gestureSerial.load(std::memory_order_acquire);
        return true;
    }
    bool popParameterEdit(NativePluginParameterEdit& event) noexcept
    {
        return m_parameterEdits.pop(event);
    }
    NativeParameterEditOverflow consumeParameterEditOverflow() noexcept
    {
        return m_parameterEdits.consumeOverflowDescriptor();
    }
    bool parameterEditsQuiescent() const noexcept
    {
        return m_parameterEdits.quiescent();
    }
    bool parameterEditsQuarantined() const noexcept
    {
        return m_parameterEdits.quarantined();
    }
    bool tryClearParameterEditQuarantine() noexcept
    {
        return m_parameterEdits.tryClearQuarantine([this]() noexcept {
            return m_nativeGestureSerials.activeDepth() == 0;
        });
    }
    int nativeEditDepth() noexcept
    {
        return m_nativeGestureSerials.activeDepth();
    }
    std::vector<MidiExpressionMapEntry> expressionMap(int midiChannel) const
    {
        std::vector<MidiExpressionMapEntry> entries;
        if (!m_effect || m_crashed) return entries;
        const int channel = std::clamp(midiChannel, 0, 15);
        for (int pitch = 0; pitch < 128; ++pitch) {
            Vst2MidiKeyName keyName;
            keyName.keyNumber = pitch;
            if (controlVst2Seh(m_effect, kVst2GetMidiKeyNameOpcode, 0, 0, &keyName, 0.0f) == 0) continue;
            int length = 0;
            while (length < static_cast<int>(sizeof(keyName.name)) && keyName.name[length] != '\0') ++length;
            const QString name = QString::fromLocal8Bit(keyName.name, length).trimmed();
            if (name.isEmpty()) continue;
            entries.push_back({pitch, channel, -1, name, name});
        }
        return entries;
    }

    QByteArray captureState() const
    {
        if (!m_effect || m_crashed) return {};
        if ((m_effect->flags & VST_EFFECT_FLAG_CHUNKS) != 0) {
            void* chunk = nullptr;
            const intptr_t size = controlVst2Seh(
                m_effect, VST_EFFECT_OPCODE_GET_CHUNK_DATA, 0, 0, &chunk, 0.0f);
            if (chunk && size > 0 && size <= kMaxPluginStateBytes - 6) {
                QByteArray state("VCV2C1", 6);
                state.append(static_cast<const char*>(chunk), static_cast<int>(size));
                return state;
            }
        }
        const int count = std::clamp(m_effect->num_params, 0,
            (kMaxPluginStateBytes - 10) / static_cast<int>(sizeof(float)));
        if (count <= 0 || !m_effect->get_parameter) return {};
        QByteArray state("VCV2P1", 6);
        const uint32_t storedCount = static_cast<uint32_t>(count);
        state.append(reinterpret_cast<const char*>(&storedCount), sizeof(storedCount));
        for (int index = 0; index < count; ++index) {
            const float value = getVst2ParameterSeh(m_effect, static_cast<uint32_t>(index));
            state.append(reinterpret_cast<const char*>(&value), sizeof(value));
        }
        return state.size() <= kMaxPluginStateBytes ? state : QByteArray{};
    }

    void process(float* mix, int frames, const std::vector<VeraCoreMidiEvent>& events,
                  int layerId, float gain, float pan, const PluginTransportState& transport,
                  const std::vector<PluginAutomationLane>* automation = nullptr,
                  bool effectMode = false,
                 const std::vector<PluginOutputRoute>* routes = nullptr,
                 VeraCorePluginOutputSink sink = nullptr, void* sinkContext = nullptr)
    {
        if (!mix || frames <= 0 || !m_effect || m_crashed) {
            return;
        }
        // Allocation and pointer publication belong to open(). Drop a broken
        // host/plugin block contract instead of resizing on the audio thread.
        if (!clearBuffers(frames)) return;
        applyAutomation(automation, transport);
        updateTimeInfo(transport, frames);
        sendMidi(events, layerId, frames);
        if (m_outputPointers.empty() || (effectMode && m_inputPointers.empty())) {
            return;
        }
        if (effectMode) {
            for (int frame = 0; frame < frames; ++frame) {
                const float inputLeft = finiteAudioSample(mix[frame * 2]);
                const float inputRight = finiteAudioSample(mix[frame * 2 + 1]);
                m_inputs[0][frame] = m_inputPointers.size() > 1
                    ? inputLeft
                    : finiteAudioSample(0.5f * inputLeft + 0.5f * inputRight);
                if (m_inputPointers.size() > 1) m_inputs[1][frame] = inputRight;
            }
        }
        if (!processPlugin(m_effect, m_inputPointers.empty() ? nullptr : m_inputPointers.data(),
                           m_outputPointers.data(), frames)) {
            m_crashed = true;
            return;
        }

        const int busCount = static_cast<int>((m_outputPointers.size() + 1u) / 2u);
        for (int bus = 0; bus < busCount; ++bus) {
            const size_t leftIndex = static_cast<size_t>(bus * 2);
            const size_t rightIndex = leftIndex + 1u;
            const float* left = m_outputPointers[leftIndex];
            const float* right = rightIndex < m_outputPointers.size() ? m_outputPointers[rightIndex] : left;
            deliverPluginOutput(mix, frames, left, right, rightIndex >= m_outputPointers.size(),
                                layerId, bus, bus == 0, gain, pan, transport.panLawDb,
                                effectMode ? nullptr : routes,
                                effectMode ? nullptr : sink, sinkContext, effectMode);
            if (effectMode) break;
        }
    }

private:
    void applyAutomation(const std::vector<PluginAutomationLane>* automation,
                         const PluginTransportState& transport) noexcept
    {
        if (!automation || automation->empty() ||
            m_editDepth.load(std::memory_order_acquire) > 0) {
            if (!automation || automation->empty()) m_automationCacheCount = 0;
            return;
        }
        const uint64_t invalidation = m_automationInvalidation.load(std::memory_order_acquire);
        const bool force = invalidation != m_observedAutomationInvalidation;
        size_t cacheIndex = 0;
        const size_t count = std::min(automation->size(), kMaxPluginAutomationLanes);
        for (size_t laneIndex = 0; laneIndex < count; ++laneIndex) {
            const PluginAutomationLane& lane = (*automation)[laneIndex];
            if (!lane.enabled || lane.points.empty() ||
                lane.parameterId >= static_cast<uint32_t>(std::max(0, m_effect->num_params))) continue;
            const float value = static_cast<float>(pluginAutomationValueAt(lane, transport.projectBeat));
            const bool unchanged = !force && cacheIndex < m_automationCacheCount &&
                m_automationCacheIds[cacheIndex] == lane.parameterId &&
                std::fabs(m_automationCacheValues[cacheIndex] - value) <= 1.0e-7f;
            if (!unchanged && !setVst2ParameterSeh(m_effect, lane.parameterId, value)) continue;
            m_automationCacheIds[cacheIndex] = lane.parameterId;
            m_automationCacheValues[cacheIndex] = value;
            ++cacheIndex;
        }
        m_automationCacheCount = cacheIndex;
        m_observedAutomationInvalidation = invalidation;
    }

    bool restoreState(const QByteArray& state)
    {
        if (!m_effect || state.size() <= 6 || state.size() > kMaxPluginStateBytes) return false;
        if (state.startsWith("VCV2C1")) {
            return controlVst2Seh(m_effect, VST_EFFECT_OPCODE_SET_CHUNK_DATA, 0,
                                  state.size() - 6, const_cast<char*>(state.constData() + 6), 0.0f) != 0;
        }
        if (!state.startsWith("VCV2P1") || state.size() < 10 || !m_effect->set_parameter) return false;
        uint32_t count = 0;
        std::memcpy(&count, state.constData() + 6, sizeof(count));
        const int available = (state.size() - 10) / static_cast<int>(sizeof(float));
        if (count > static_cast<uint32_t>(available) || count > static_cast<uint32_t>(std::max(0, m_effect->num_params)))
            return false;
        const int safeCount = static_cast<int>(count);
        for (int index = 0; index < safeCount; ++index) {
            float value = 0.0f;
            std::memcpy(&value, state.constData() + 10 + index * sizeof(float), sizeof(value));
            if (!std::isfinite(value) || !setVst2ParameterSeh(
                    m_effect, static_cast<uint32_t>(index), std::clamp(value, 0.0f, 1.0f))) return false;
        }
        return safeCount == static_cast<int>(count);
    }

    static double validSampleRate(double value)
    {
        return std::isfinite(value) && value >= 8000.0 && value <= 384000.0 ? value : 44100.0;
    }

    void beginHostParameterCall() noexcept
    {
        if (m_hostParameterCallDepth.fetch_add(1, std::memory_order_acq_rel) == 0)
            m_hostParameterThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    }

    void endHostParameterCall() noexcept
    {
        const int previous = m_hostParameterCallDepth.fetch_sub(1, std::memory_order_acq_rel);
        if (previous <= 1) {
            m_hostParameterCallDepth.store(0, std::memory_order_release);
            m_hostParameterThreadId.store(0, std::memory_order_release);
        }
    }

    bool hostParameterCallOnCurrentThread() const noexcept
    {
        return m_hostParameterCallDepth.load(std::memory_order_acquire) > 0 &&
            m_hostParameterThreadId.load(std::memory_order_acquire) == GetCurrentThreadId();
    }

    static intptr_t VST_FUNCTION_INTERFACE hostCallback(vst_effect_t* effect, int32_t opcode,
                                                         int32_t index, int64_t, const char* data,
                                                         float option)
    {
        Vst2Instance* self = effect && effect->host_internal
            ? static_cast<Vst2Instance*>(effect->host_internal)
            : gCreatingVst2;
        switch (opcode) {
        case VST_HOST_OPCODE_PARAM_START_EDIT:
            if (self && index >= 0) {
                const bool hostOrigin = self->hostParameterCallOnCurrentThread();
                auto nativeCallback = self->m_parameterEdits.capture(!hostOrigin);
                const int previousDepth =
                    self->m_editDepth.fetch_add(1, std::memory_order_acq_rel);
                uint64_t gestureSerial =
                    self->m_gestureSerial.load(std::memory_order_acquire);
                bool outermost = false;
                if (!hostOrigin) {
                    const uint64_t freshSerial =
                        self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
                    gestureSerial = self->m_nativeGestureSerials.begin(
                        static_cast<uint32_t>(index), freshSerial, &outermost);
                    if (gestureSerial == 0)
                        self->m_parameterEdits.markOverflow(
                            static_cast<uint32_t>(index), freshSerial);
                } else if (previousDepth == 0) {
                    self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
                }
                if (!hostOrigin && outermost && gestureSerial != 0)
                    self->m_parameterEdits.push({PluginParameterEditPhase::Begin,
                        static_cast<uint32_t>(index), 0.0,
                        gestureSerial});
                return 1;
            }
            return 0;
        case VST_HOST_OPCODE_PARAM_UPDATE:
            if (self && index >= 0 && std::isfinite(option)) {
                const bool hostOrigin = self->hostParameterCallOnCurrentThread();
                auto nativeCallback = self->m_parameterEdits.capture(!hostOrigin);
                uint64_t gestureSerial = 0;
                bool suppressed = false;
                bool trackerOverflowed = false;
                if (!hostOrigin) {
                    gestureSerial = self->m_nativeGestureSerials.current(
                        static_cast<uint32_t>(index), 0, &suppressed,
                        &trackerOverflowed);
                    if (trackerOverflowed)
                        self->m_parameterEdits.markOverflow(
                            static_cast<uint32_t>(index),
                            self->m_gestureSerial.load(std::memory_order_acquire));
                    if (gestureSerial == 0 && !suppressed)
                        gestureSerial =
                            self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
                } else if (self->m_editDepth.load(std::memory_order_acquire) == 0) {
                    self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
                }
                self->m_automationInvalidation.fetch_add(1, std::memory_order_release);
                self->m_stateDirty.store(true, std::memory_order_release);
                if (!hostOrigin && !suppressed)
                    self->m_parameterEdits.push({PluginParameterEditPhase::Value,
                        static_cast<uint32_t>(index),
                        std::clamp(static_cast<double>(option), 0.0, 1.0),
                        gestureSerial});
                return 1;
            }
            return 0;
        case VST_HOST_OPCODE_PARAM_STOP_EDIT:
            if (self && index >= 0) {
                const bool hostOrigin = self->hostParameterCallOnCurrentThread();
                auto nativeCallback = self->m_parameterEdits.capture(!hostOrigin);
                bool terminal = false;
                bool trackerOverflowed = false;
                bool trackerConsumed = false;
                const uint64_t gestureSerial = !hostOrigin
                    ? self->m_nativeGestureSerials.end(
                        static_cast<uint32_t>(index),
                        self->m_gestureSerial.load(std::memory_order_acquire),
                        &terminal, &trackerOverflowed, &trackerConsumed)
                    : 0;
                if (trackerOverflowed)
                    self->m_parameterEdits.markOverflow(
                        static_cast<uint32_t>(index),
                        self->m_gestureSerial.load(std::memory_order_acquire));
                if (hostOrigin || trackerConsumed)
                    decrementPositive(self->m_editDepth);
                self->m_stateDirty.store(true, std::memory_order_release);
                if (!hostOrigin && terminal)
                    self->m_parameterEdits.push({PluginParameterEditPhase::End,
                        static_cast<uint32_t>(index), 0.0,
                        gestureSerial});
                return 1;
            }
            return 0;
        case VST_HOST_OPCODE_VST_VERSION: return 2400;
        case VST_HOST_OPCODE_CURRENT_EFFECT_ID: return 0;
        case VST_HOST_OPCODE_07:
            return self ? reinterpret_cast<intptr_t>(&self->m_timeInfo) : 0;
        case VST_HOST_OPCODE_GET_SAMPLE_RATE: return self ? static_cast<intptr_t>(self->m_sampleRate) : 44100;
        case VST_HOST_OPCODE_GET_BLOCK_SIZE: return kDefaultBlockSize;
        // Opcode 0x17 is audioMasterGetCurrentProcessLevel in VST 2.4.
        case VST_HOST_OPCODE_GET_ACTIVE_THREAD: return self && self->m_offline ? 4 : 2;
        case VST_HOST_OPCODE_VENDOR_VERSION: return 1000;
        case VST_HOST_OPCODE_SUPPORTS:
            if (data && (strcmp(data, "sendVstEvents") == 0 ||
                         strcmp(data, "sendVstMidiEvent") == 0 ||
                         strcmp(data, "receiveVstEvents") == 0 ||
                         strcmp(data, "receiveVstMidiEvent") == 0 ||
                         strcmp(data, "startStopProcess") == 0)) {
                return 1;
            }
            return 0;
        case VST_HOST_OPCODE_VENDOR_NAME:
        case VST_HOST_OPCODE_PRODUCT_NAME:
            if (data) {
                strcpy_s(const_cast<char*>(data), 64, "VeraCore");
                return 1;
            }
            return 0;
        default: return 0;
        }
    }

    static bool processPlugin(vst_effect_t* effect, const float* const* inputs, float** outputs, int frames) noexcept
    {
        __try {
            if (effect->process_float) {
                effect->process_float(effect, inputs, outputs, frames);
            } else if (effect->process) {
                effect->process(effect, inputs, outputs, frames);
            } else {
                return false;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void prepareBuffers(int frames)
    {
        const int inputCount = std::clamp(m_effect->num_inputs, 0, kMaxPluginChannels);
        const int outputCount = std::clamp(m_effect->num_outputs, 0, kMaxPluginChannels);
        m_inputs.resize(inputCount);
        m_outputs.resize(outputCount);
        m_inputPointers.resize(inputCount);
        m_outputPointers.resize(outputCount);
        for (int channel = 0; channel < inputCount; ++channel) {
            if (static_cast<int>(m_inputs[channel].size()) < frames) {
                m_inputs[channel].resize(frames);
            }
            std::fill_n(m_inputs[channel].begin(), frames, 0.0f);
            m_inputPointers[channel] = m_inputs[channel].data();
        }
        for (int channel = 0; channel < outputCount; ++channel) {
            if (static_cast<int>(m_outputs[channel].size()) < frames) {
                m_outputs[channel].resize(frames);
            }
            std::fill_n(m_outputs[channel].begin(), frames, 0.0f);
            m_outputPointers[channel] = m_outputs[channel].data();
        }
    }

    bool clearBuffers(int frames) noexcept
    {
        if (frames <= 0 || frames > kDefaultBlockSize || !m_effect) return false;
        const int inputCount = std::clamp(m_effect->num_inputs, 0, kMaxPluginChannels);
        const int outputCount = std::clamp(m_effect->num_outputs, 0, kMaxPluginChannels);
        if (m_inputs.size() != static_cast<size_t>(inputCount) ||
            m_outputs.size() != static_cast<size_t>(outputCount) ||
            m_inputPointers.size() != static_cast<size_t>(inputCount) ||
            m_outputPointers.size() != static_cast<size_t>(outputCount)) return false;
        for (int channel = 0; channel < inputCount; ++channel) {
            if (m_inputs[channel].size() < static_cast<size_t>(frames) ||
                m_inputPointers[channel] != m_inputs[channel].data()) return false;
            std::fill_n(m_inputs[channel].begin(), frames, 0.0f);
        }
        for (int channel = 0; channel < outputCount; ++channel) {
            if (m_outputs[channel].size() < static_cast<size_t>(frames) ||
                m_outputPointers[channel] != m_outputs[channel].data()) return false;
            std::fill_n(m_outputPointers[channel], frames, 0.0f);
        }
        return true;
    }

    void updateTimeInfo(const PluginTransportState& transport, int frames)
    {
        const Vst2TimeInfo previous = m_timeInfo;
        const bool hadPrevious = m_hasTimeInfo;
        const int previousFrames = m_previousBlockFrames;
        const double safeBpm = std::isfinite(transport.bpm) && transport.bpm > 0.0
            ? transport.bpm : 120.0;
        const double safeBeat = std::isfinite(transport.projectBeat) && transport.projectBeat >= 0.0
            ? transport.projectBeat : 0.0;
        const double safeSeconds = boundedPluginTimelineSeconds(
            transport.projectSeconds, safeBeat * 60.0 / safeBpm);
        const double beatsPerBar = transportBeatsPerBar(transport);
        const double safeBarStart = std::isfinite(transport.barStartBeat) &&
            transport.barStartBeat >= 0.0 && transport.barStartBeat <= safeBeat + 1.0e-9
            ? transport.barStartBeat
            : std::floor(safeBeat / beatsPerBar) * beatsPerBar;
        const bool validCycle = transport.cycleActive &&
            std::isfinite(transport.cycleStartBeat) &&
            std::isfinite(transport.cycleEndBeat) &&
            transport.cycleStartBeat >= 0.0 &&
            transport.cycleEndBeat > transport.cycleStartBeat;
        m_timeInfo = {};
        m_timeInfo.sampleRate = m_sampleRate;
        m_timeInfo.samplePos = safeSeconds * m_sampleRate;
        m_timeInfo.ppqPos = safeBeat;
        m_timeInfo.tempo = safeBpm;
        m_timeInfo.barStartPos = safeBarStart;
        m_timeInfo.flags = kVst2PpqPosValid | kVst2TempoValid |
            kVst2BarsValid | kVst2TimeSigValid;
        if (validCycle) {
            m_timeInfo.cycleStartPos = transport.cycleStartBeat;
            m_timeInfo.cycleEndPos = transport.cycleEndBeat;
            m_timeInfo.flags |= kVst2TransportCycleActive | kVst2CyclePosValid;
        }
        m_timeInfo.timeSigNumerator = transport.numerator;
        m_timeInfo.timeSigDenominator = transport.denominator;
        if (transport.playing) m_timeInfo.flags |= kVst2TransportPlaying;
        if (transport.recording) m_timeInfo.flags |= kVst2TransportRecording;
        const bool previousPlaying = hadPrevious &&
            (previous.flags & kVst2TransportPlaying) != 0;
        const bool previousRecording = hadPrevious &&
            (previous.flags & kVst2TransportRecording) != 0;
        const bool previousCycle = hadPrevious &&
            (previous.flags & kVst2TransportCycleActive) != 0;
        const double expectedSamplePosition = previous.samplePos +
            (previousPlaying ? static_cast<double>(previousFrames) : 0.0);
        const bool positionDiscontinuity = hadPrevious &&
            std::fabs(m_timeInfo.samplePos - expectedSamplePosition) > 32.0;
        const bool stateChanged = !hadPrevious ||
            previousPlaying != transport.playing ||
            previousRecording != transport.recording ||
            previousCycle != validCycle ||
            std::fabs(previous.tempo - safeBpm) > 1.0e-9 ||
            previous.timeSigNumerator != transport.numerator ||
            previous.timeSigDenominator != transport.denominator ||
            (validCycle && (std::fabs(previous.cycleStartPos - transport.cycleStartBeat) > 1.0e-9 ||
                            std::fabs(previous.cycleEndPos - transport.cycleEndBeat) > 1.0e-9));
        if (stateChanged || positionDiscontinuity)
            m_timeInfo.flags |= kVst2TransportChanged;
        m_hasTimeInfo = true;
        m_previousBlockFrames = std::max(0, frames);
    }

    void sendMidi(const std::vector<VeraCoreMidiEvent>& source, int layerId, int frames)
    {
        if (!m_effect->control) {
            return;
        }
        m_midiEvents.clear();
        if (m_panicPending) {
            for (int channel = 0; channel < 16; ++channel) {
                for (const uint8_t controller : {uint8_t(120), uint8_t(123)}) {
                    vst_event_midi_t panicEvent{};
                    panicEvent.event.type = VST_EVENT_TYPE_MIDI;
                    panicEvent.event.size = static_cast<int32_t>(sizeof(vst_event_midi_t));
                    panicEvent.midi.data[0] = static_cast<char>(0xB0 | channel);
                    panicEvent.midi.data[1] = static_cast<char>(controller);
                    m_midiEvents.push_back(panicEvent);
                }
            }
            m_panicPending = false;
        }
        for (const VeraCoreMidiEvent& event : source) {
            if (m_midiEvents.size() >= kMaxMidiEventsPerBlock) {
                break;
            }
            if (!event.externalPlugin || event.layerId != layerId) {
                continue;
            }
            vst_event_midi_t vstEvent{};
            vstEvent.event.type = VST_EVENT_TYPE_MIDI;
            vstEvent.event.size = static_cast<int32_t>(sizeof(vst_event_midi_t));
            vstEvent.event.offset = std::clamp(event.sampleOffset, 0, std::max(0, frames - 1));
            vstEvent.midi.data[0] = static_cast<char>(event.status);
            vstEvent.midi.data[1] = static_cast<char>(event.data1);
            vstEvent.midi.data[2] = static_cast<char>(event.data2);
            m_midiEvents.push_back(vstEvent);
        }
        if (m_midiEvents.empty()) {
            return;
        }
        m_eventBatch.count = static_cast<int32_t>(m_midiEvents.size());
        m_eventBatch.padding = 0;
        m_eventBatch.reserved = 0;
        for (size_t index = 0; index < m_midiEvents.size(); ++index) {
            m_eventBatch.events[index] = &m_midiEvents[index].event;
        }
        controlVst2Seh(m_effect, VST_EFFECT_OPCODE_EVENT, 0, 0, &m_eventBatch, 0.0f);
    }

    HMODULE m_module = nullptr;
    vst_effect_t* m_effect = nullptr;
    QString m_path;
    double m_sampleRate = 44100.0;
    std::atomic<bool> m_crashed{false};
    std::atomic<bool> m_panicPending{false};
    bool m_offline = false;
    std::atomic<bool> m_stateDirty{false};
    std::atomic<int> m_editDepth{0};
    std::atomic<uint64_t> m_gestureSerial{0};
    std::atomic<int> m_hostParameterCallDepth{0};
    std::atomic<DWORD> m_hostParameterThreadId{0};
    BoundedParameterEditQueue<kNativeParameterEditQueueCapacity> m_parameterEdits;
    NativeParameterGestureSerials m_nativeGestureSerials;
    Vst2TimeInfo m_timeInfo;
    bool m_hasTimeInfo = false;
    int m_previousBlockFrames = 0;
    std::vector<std::vector<float>> m_inputs;
    std::vector<std::vector<float>> m_outputs;
    std::vector<const float*> m_inputPointers;
    std::vector<float*> m_outputPointers;
    std::vector<vst_event_midi_t> m_midiEvents;
    std::vector<float> m_parameterDefaults;
    std::array<uint32_t, kMaxPluginAutomationLanes> m_automationCacheIds{};
    std::array<float, kMaxPluginAutomationLanes> m_automationCacheValues{};
    size_t m_automationCacheCount = 0;
    std::atomic<uint64_t> m_automationInvalidation{0};
    uint64_t m_observedAutomationInvalidation = 0;
    struct alignas(8) VstEventBatch {
        int32_t count = 0;
        int32_t padding = 0;
        intptr_t reserved = 0;
        std::array<vst_event_t*, kMaxMidiEventsPerBlock> events{};
    };
    static_assert(sizeof(intptr_t) == 8, "VeraCore plugin host requires x64.");
    static_assert(offsetof(VstEventBatch, events) == 16, "Unexpected VST2 x64 event ABI layout.");
    VstEventBatch m_eventBatch;
};

class ClapInstance
{
public:
    ~ClapInstance() { close(); }

    bool open(const QString& path, const QString& pluginId, double sampleRate,
               const QByteArray& state = {}, bool offline = false,
               AraDocumentRegistry* araDocuments = nullptr,
               int araTrackIndex = -1, bool araEditorOwner = false,
			   VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo)
    {
        close();
        m_offline = offline;
        m_araTrackIndex = araTrackIndex;
        m_araEditorOwner = araEditorOwner;
		m_speakerLayout = speakerLayout;
        m_binaryPath = resolveClapBinary(path);
        m_pluginId = pluginId;
        m_sampleRate = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
            ? sampleRate
            : 44100.0;
        if (m_binaryPath.isEmpty() || m_pluginId.isEmpty()) {
            return false;
        }
        const std::wstring widePath = m_binaryPath.toStdWString();
        m_module = LoadLibraryExW(widePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_module) {
            m_module = LoadLibraryExW(widePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (!m_module) {
            return false;
        }
        m_entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(m_module, "clap_entry"));
        if (!m_entry || !m_entry->init || !m_entry->get_factory || !m_entry->deinit) {
            close();
            return false;
        }
        const QByteArray utf8Path = m_binaryPath.toUtf8();
        if (!clapEntryInitSeh(m_entry, utf8Path.constData())) {
            m_entry = nullptr;
            close();
            return false;
        }
        m_entryInitialized = true;
        const auto* factory = static_cast<const clap_plugin_factory_t*>(
            clapGetFactorySeh(m_entry, CLAP_PLUGIN_FACTORY_ID));
        if (!factory || !factory->create_plugin) {
            close();
            return false;
        }
        initializeHost();
        const QByteArray utf8Id = m_pluginId.toUtf8();
        m_plugin = clapCreatePluginSeh(factory, &m_host, utf8Id.constData());
        if (!m_plugin || !m_plugin->init || !m_plugin->destroy || !m_plugin->activate ||
            !m_plugin->deactivate || !m_plugin->start_processing || !m_plugin->stop_processing ||
            !m_plugin->process || !m_plugin->get_extension) {
            close();
            return false;
        }
        if (!clapPluginInitSeh(m_plugin)) {
            close();
            return false;
        }
        m_pluginInitialized = true;
        discoverCapabilities();
        scanParameters();
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        bindAraDocument(araDocuments);
#endif
        const auto* render = static_cast<const clap_plugin_render_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_RENDER));
        if (render) {
            clapSetRenderModeSeh(render, m_plugin,
                m_offline ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME);
        }
        if (!state.isEmpty()) restoreState(state);
		selectAudioPortConfiguration(m_speakerLayout);
        scanAudioPorts();
        scanNotePort();
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!assignAraRegions()) {
            close();
            return false;
        }
#endif
        if (m_outputChannelCounts.empty() ||
            !clapPluginActivateSeh(m_plugin, m_sampleRate, 1, kDefaultBlockSize)) {
            close();
            return false;
        }
        m_active = true;
        m_events.reserve(kMaxMidiEventsPerBlock);
        m_eventSortScratch.resize(kMaxMidiEventsPerBlock);
        initializeEventInterfaces();
        preparePorts(kDefaultBlockSize);
        m_stateDirty.store(false, std::memory_order_relaxed);
        m_editDepth.store(0, std::memory_order_relaxed);
        m_gestureSerial.store(0, std::memory_order_relaxed);
        m_parameterEdits.reset();
        m_nativeGestureSerials.reset();
        if (!clapPluginStartSeh(m_plugin)) {
            close();
            return false;
        }
        m_processing = true;
        return true;
    }

    void close()
    {
        if (m_plugin) {
            if (m_processing && m_plugin->stop_processing) {
                clapPluginStopSeh(m_plugin);
            }
            m_processing = false;
            if (m_active && m_plugin->deactivate) {
                clapPluginDeactivateSeh(m_plugin);
            }
            m_active = false;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
            clearAraRegionAssignments();
#endif
            if (m_plugin->destroy) {
                clapPluginDestroySeh(m_plugin);
            }
        }
        m_plugin = nullptr;
        m_pluginInitialized = false;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        m_araPluginExtension = nullptr;
        m_araExtensionInstance = nullptr;
        m_araFactory = nullptr;
        m_araBinding = {};
        m_araDocument.reset();
#endif
        if (m_entryInitialized && m_entry && m_entry->deinit) {
            clapEntryDeinitSeh(m_entry);
        }
        m_entryInitialized = false;
        m_entry = nullptr;
        if (m_module) {
            FreeLibrary(m_module);
            m_module = nullptr;
        }
        m_inputPorts.clear();
        m_outputPorts.clear();
        m_inputChannelCounts.clear();
        m_outputChannelCounts.clear();
        m_outputPortIds.clear();
        m_outputPortNames.clear();
        m_parameterInfos.clear();
        m_parameterLookup.clear();
        m_readOnlyParameterIds.clear();
        m_params = nullptr;
        m_mainOutputPort = -1;
		m_speakerLayout = VeraCoreSpeakerLayout::Stereo;
        m_events.clear();
        m_hostParameterRead.store(0, std::memory_order_relaxed);
        m_hostParameterWrite.store(0, std::memory_order_relaxed);
        m_hostParameterEventsInBlock = false;
        m_crashed = false;
        m_panicPending = false;
        m_noteEventsSupported = true;
        m_useClapNoteDialect = true;
        m_notePortIndex = 0;
        m_steadyTime = 0;
        m_offline = false;
        m_araTrackIndex = -1;
        m_araEditorOwner = false;
        m_araModelUpdateSuspended.store(false, std::memory_order_release);
        m_capabilities = {};
        m_restartRequested.store(false, std::memory_order_relaxed);
        m_mainThreadCallbackRequested.store(false, std::memory_order_relaxed);
        m_parameterFlushRequested.store(false, std::memory_order_relaxed);
        m_pluginCallGate.clear(std::memory_order_relaxed);
        m_capabilityRescanRequested.store(false, std::memory_order_relaxed);
        m_stateDirty.store(false, std::memory_order_relaxed);
        m_metricsDirty.store(false, std::memory_order_relaxed);
        m_editDepth.store(0, std::memory_order_relaxed);
        m_gestureSerial.store(0, std::memory_order_relaxed);
        m_parameterEdits.reset();
        m_nativeGestureSerials.reset();
        m_processRequestGeneration.store(0, std::memory_order_relaxed);
        m_observedProcessRequestGeneration.store(0, std::memory_order_relaxed);
    }

    bool matches(const QString& path, const QString& pluginId, double sampleRate, bool offline,
				 VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo) const
    {
        return m_plugin && !m_crashed && resolveClapBinary(path) == m_binaryPath &&
            pluginId == m_pluginId && std::fabs(m_sampleRate - sampleRate) < 0.5 &&
            m_offline == offline && m_speakerLayout == speakerLayout;
    }

    bool healthy() const { return m_plugin && !m_crashed; }
    bool reusable() const
    {
        return healthy() && !m_restartRequested.load(std::memory_order_acquire);
    }

    PluginTechnologyCapabilities capabilities() const { return m_capabilities; }
    std::vector<PluginAudioOutputBus> outputBuses() const
    {
        std::vector<PluginAudioOutputBus> buses;
        buses.reserve(m_outputChannelCounts.size());
        for (size_t index = 0; index < m_outputChannelCounts.size(); ++index) {
            const QString id = index < m_outputPortIds.size()
                ? QString("clap-%1").arg(m_outputPortIds[index])
                : QString("clap-index-%1").arg(index);
            const QString name = index < m_outputPortNames.size() && !m_outputPortNames[index].isEmpty()
                ? m_outputPortNames[index] : QString("Output %1").arg(index + 1);
            buses.push_back({id, name, static_cast<int>(index),
                             static_cast<int>(m_outputChannelCounts[index]),
                             static_cast<int>(index) == m_mainOutputPort});
        }
        return buses;
    }
    std::vector<PluginParameterInfo> parameters() const
    {
        std::vector<PluginParameterInfo> result;
        if (!m_plugin || m_crashed) return result;
        const auto* params = m_params;
        result.reserve(m_parameterInfos.size());
        for (const clap_param_info_t& source : m_parameterInfos) {
            double plain = source.default_value;
            if (!clapParamValueSeh(params, m_plugin, source.id, plain) || !std::isfinite(plain))
                plain = source.default_value;
            const double range = source.max_value - source.min_value;
            const double normalized = range > 0.0
                ? std::clamp((plain - source.min_value) / range, 0.0, 1.0) : 0.0;
            char text[CLAP_NAME_SIZE]{};
            clapParamTextSeh(params, m_plugin, source.id, plain, text, sizeof(text));
            PluginParameterInfo info;
            info.stableId = source.id;
            info.name = QString::fromUtf8(
                source.name, static_cast<qsizetype>(strnlen_s(source.name, sizeof(source.name)))).trimmed();
            if (info.name.isEmpty()) info.name = QString("Parameter %1").arg(result.size() + 1);
            info.module = QString::fromUtf8(
                source.module, static_cast<qsizetype>(strnlen_s(source.module, sizeof(source.module)))).trimmed();
            info.displayValue = QString::fromUtf8(
                text, static_cast<qsizetype>(strnlen_s(text, sizeof(text)))).trimmed();
            info.normalizedValue = normalized;
            info.defaultNormalizedValue = range > 0.0
                ? std::clamp((source.default_value - source.min_value) / range, 0.0, 1.0) : 0.0;
            if ((source.flags & CLAP_PARAM_IS_STEPPED) != 0 && range > 0.0)
                info.stepCount = static_cast<int>(std::clamp(
                    std::llround(range), 1ll, static_cast<long long>(INT32_MAX)));
            info.automatable = (source.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
            info.readOnly = (source.flags & CLAP_PARAM_IS_READONLY) != 0;
            info.hidden = (source.flags & CLAP_PARAM_IS_HIDDEN) != 0;
            info.bypass = (source.flags & CLAP_PARAM_IS_BYPASS) != 0;
            result.push_back(std::move(info));
        }
        return result;
    }

    bool beginParameterGesture(uint32_t parameterId) noexcept
    {
        const clap_param_info_t* info = parameterInfo(parameterId);
        if (!info || (info->flags & CLAP_PARAM_IS_READONLY) != 0) return false;
        if (m_editDepth.fetch_add(1, std::memory_order_acq_rel) == 0)
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
        ClapInputEvent event{};
        event.gesture.header.size = sizeof(clap_event_param_gesture_t);
        event.gesture.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.gesture.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
        event.gesture.header.flags = CLAP_EVENT_DONT_RECORD;
        event.gesture.param_id = parameterId;
        if (enqueueHostParameterEvent(event)) return true;
        m_editDepth.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    bool setParameterNormalized(uint32_t parameterId, double value) noexcept
    {
        const clap_param_info_t* info = parameterInfo(parameterId);
        if (!info || (info->flags & CLAP_PARAM_IS_READONLY) != 0 || !std::isfinite(value)) return false;
        if (m_editDepth.load(std::memory_order_acquire) == 0)
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
        const double normalized = std::clamp(value, 0.0, 1.0);
        double plain = info->min_value + normalized * (info->max_value - info->min_value);
        if ((info->flags & CLAP_PARAM_IS_STEPPED) != 0) plain = std::round(plain);
        ClapInputEvent event{};
        event.parameter.header.size = sizeof(clap_event_param_value_t);
        event.parameter.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.parameter.header.type = CLAP_EVENT_PARAM_VALUE;
        event.parameter.header.flags = CLAP_EVENT_DONT_RECORD;
        event.parameter.param_id = parameterId;
        event.parameter.cookie = info->cookie;
        event.parameter.note_id = -1;
        event.parameter.port_index = -1;
        event.parameter.channel = -1;
        event.parameter.key = -1;
        event.parameter.value = std::clamp(plain, info->min_value, info->max_value);
        return enqueueHostParameterEvent(event);
    }
    bool endParameterGesture(uint32_t parameterId) noexcept
    {
        const clap_param_info_t* info = parameterInfo(parameterId);
        if (!info || (info->flags & CLAP_PARAM_IS_READONLY) != 0) return false;
        ClapInputEvent event{};
        event.gesture.header.size = sizeof(clap_event_param_gesture_t);
        event.gesture.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        event.gesture.header.flags = CLAP_EVENT_DONT_RECORD;
        event.gesture.param_id = parameterId;
        const bool queued = enqueueHostParameterEvent(event);
        decrementPositive(m_editDepth);
        return queued;
    }

    bool hasAraRenderer() const noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        return m_plugin && !m_crashed.load(std::memory_order_acquire) &&
            m_araDocument && m_araBinding.playbackRenderer;
#else
        return false;
#endif
    }

    void requestAraModelUpdate() noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (m_araDocument && m_araBinding.playbackRenderer)
            m_araModelUpdateSuspended.store(true, std::memory_order_release);
#endif
    }

    void cancelAraModelUpdate() noexcept
    {
        m_araModelUpdateSuspended.store(false, std::memory_order_release);
    }

    void beginAraModelUpdate() noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!m_araModelUpdateSuspended.load(std::memory_order_acquire)) return;
        m_araResumeProcessing = m_processing;
        m_araResumeActive = m_active;
        if (m_processing) clapPluginStopSeh(m_plugin);
        m_processing = false;
        if (m_active) clapPluginDeactivateSeh(m_plugin);
        m_active = false;
        clearAraRegionAssignments();
#endif
    }

    void endAraModelUpdate() noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!m_araModelUpdateSuspended.load(std::memory_order_acquire)) return;
        bool ready = assignAraRegions();
        if (ready && m_araResumeActive) {
            ready = clapPluginActivateSeh(m_plugin, m_sampleRate, 1, kDefaultBlockSize);
            m_active = ready;
        }
        if (ready && m_araResumeProcessing) {
            ready = clapPluginStartSeh(m_plugin);
            m_processing = ready;
        }
        m_araResumeActive = false;
        m_araResumeProcessing = false;
        if (!ready) m_crashed.store(true, std::memory_order_release);
        m_araModelUpdateSuspended.store(false, std::memory_order_release);
#endif
    }

    void setAraEditorOwner(bool owner) noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (m_araEditorOwner == owner) return;
        if (owner) {
            size_t added = 0;
            for (const auto region : m_araAssignedRegions) {
                if (!addAraEditorRegionSeh(m_araExtensionInstance, region)) {
                    while (added > 0)
                        removeAraEditorRegionSeh(m_araExtensionInstance,
                            m_araAssignedRegions[--added]);
                    return;
                }
                ++added;
            }
        } else {
            for (auto it = m_araAssignedRegions.rbegin(); it != m_araAssignedRegions.rend(); ++it)
                removeAraEditorRegionSeh(m_araExtensionInstance, *it);
        }
        m_araEditorOwner = owner;
#else
        (void)owner;
#endif
    }

    void notifyAraEditorSelection(
        int selectedTrackIndex,
        const std::vector<std::string>& clipIds) noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!m_araEditorOwner) return;
        try {
            std::vector<ARA::ARAPlaybackRegionRef> regions;
            if (selectedTrackIndex == m_araTrackIndex && m_araDocument)
                regions = m_araDocument->playbackRegionRefsForPersistentIds(
                    m_araTrackIndex, clipIds);
            notifyAraSelectionSeh(m_araExtensionInstance, regions);
        } catch (...) {
            const std::vector<ARA::ARAPlaybackRegionRef> emptySelection;
            notifyAraSelectionSeh(m_araExtensionInstance, emptySelection);
        }
#else
        (void)selectedTrackIndex;
        (void)clipIds;
#endif
    }

    void serviceControlThreadRequests()
    {
        if (m_mainThreadCallbackRequested.exchange(false, std::memory_order_acq_rel))
            clapPluginOnMainThreadSeh(m_plugin);
        if (!m_processing) flushRequestedParameterEvents();
        if (m_capabilityRescanRequested.exchange(false, std::memory_order_acq_rel))
            discoverCapabilities();
    }

    bool consumeMetricsDirty()
    {
        return m_metricsDirty.exchange(false, std::memory_order_acq_rel);
    }

    bool consumeStateDirty(uint64_t& gestureSerial)
    {
        if (m_editDepth.load(std::memory_order_acquire) > 0) return false;
        if (!m_stateDirty.exchange(false, std::memory_order_acq_rel)) return false;
        gestureSerial = m_gestureSerial.load(std::memory_order_acquire);
        return true;
    }
    bool popParameterEdit(NativePluginParameterEdit& event) noexcept
    {
        return m_parameterEdits.pop(event);
    }
    NativeParameterEditOverflow consumeParameterEditOverflow() noexcept
    {
        return m_parameterEdits.consumeOverflowDescriptor();
    }
    bool parameterEditsQuiescent() const noexcept
    {
        return m_parameterEdits.quiescent();
    }
    bool parameterEditsQuarantined() const noexcept
    {
        return m_parameterEdits.quarantined();
    }
    bool tryClearParameterEditQuarantine() noexcept
    {
        return m_parameterEdits.tryClearQuarantine([this]() noexcept {
            return m_nativeGestureSerials.activeDepth() == 0;
        });
    }
    int nativeEditDepth() noexcept
    {
        return m_nativeGestureSerials.activeDepth();
    }

    void requestPanic() { m_panicPending = true; }
    bool hasPendingPanic() const { return m_panicPending; }
    bool hasPendingParameterFlush() const noexcept
    {
        return m_parameterFlushRequested.load(std::memory_order_acquire);
    }
    void serviceAudioThreadParameterFlush() noexcept
    {
        flushRequestedParameterEvents();
    }
    uint32_t tailSamples() const
    {
        const auto* tail = static_cast<const clap_plugin_tail_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_TAIL));
        return clapTailSamplesSeh(tail, m_plugin);
    }
    uint32_t latencySamples() const
    {
        const auto* latency = static_cast<const clap_plugin_latency_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_LATENCY));
        return clapLatencySamplesSeh(latency, m_plugin);
    }

    std::vector<MidiExpressionMapEntry> expressionMap(int midiChannel) const
    {
        std::vector<MidiExpressionMapEntry> entries;
        if (!m_plugin || m_crashed) return entries;
        const auto* names = static_cast<const clap_plugin_note_name_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_NOTE_NAME));
        const uint32_t count = std::min<uint32_t>(clapNoteNameCountSeh(names, m_plugin), 512u);
        const int channel = std::clamp(midiChannel, 0, 15);
        for (uint32_t index = 0; index < count; ++index) {
            clap_note_name_t noteName{};
            if (!clapNoteNameGetSeh(names, m_plugin, index, &noteName) ||
                noteName.key < 0 || noteName.key > 127 ||
                (noteName.port >= 0 && noteName.port != 0) ||
                (noteName.channel >= 0 && noteName.channel != channel)) {
                continue;
            }
            int length = 0;
            while (length < CLAP_NAME_SIZE && noteName.name[length] != '\0') ++length;
            const QString name = QString::fromUtf8(noteName.name, length).trimmed();
            if (name.isEmpty()) continue;
            entries.push_back({noteName.key,
                               noteName.channel >= 0 ? noteName.channel : channel,
                               -1,
                               name,
                               name});
        }
        return entries;
    }

    QByteArray captureState() const
    {
        if (!m_plugin || m_crashed) return {};
        const auto* state = static_cast<const clap_plugin_state_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_STATE));
        ClapStateWriter writer;
        clap_ostream_t stream{&writer, &clapStateWrite};
        return clapStateSaveSeh(state, m_plugin, &stream) ? writer.bytes : QByteArray{};
    }

    void process(float* mix, int frames, const std::vector<VeraCoreMidiEvent>& source,
                 int layerId, float gain, float pan, const PluginTransportState& transport,
                 const std::vector<PluginAutomationLane>* automation = nullptr,
                 bool effectMode = false,
                 const std::vector<PluginOutputRoute>* routes = nullptr,
                  VeraCorePluginOutputSink sink = nullptr, void* sinkContext = nullptr,
                  int effectChannelCount = 2)
    {
        if (m_araModelUpdateSuspended.load(std::memory_order_acquire)) return;
        if (!mix || frames <= 0 || !m_plugin || !m_active || m_crashed) {
            return;
        }
        // activate() advertises kDefaultBlockSize as the hard maximum. Reject a
        // contract-violating block instead of resizing audio buffers on the RT thread.
        if (frames > kDefaultBlockSize) return;
        if (!m_processing) return;
        if (m_pluginCallGate.test_and_set(std::memory_order_acquire)) return;
        struct GateRelease {
            std::atomic_flag* gate = nullptr;
            ~GateRelease() { if (gate) gate->clear(std::memory_order_release); }
        } gateRelease{&m_pluginCallGate};
        if (!clearPorts(frames)) return;
        m_observedProcessRequestGeneration.store(
            m_processRequestGeneration.load(std::memory_order_acquire),
            std::memory_order_release);
        buildEvents(source, layerId, frames, automation, transport);
        if (effectMode) {
            if (m_inputStorage.empty() || m_inputStorage[0].channels.empty()) return;
            const int hostChannels = std::clamp(effectChannelCount, 1, kMaxPluginChannels);
            const int inputChannels = static_cast<int>(m_inputStorage[0].channels.size());
			const std::vector<int>& canonicalMap = m_inputStorage[0].canonicalMap;
            for (int frame = 0; frame < frames; ++frame) {
                const float* sourceFrame = mix + static_cast<size_t>(frame) * hostChannels;
                if (inputChannels == 1) {
                    const float right = hostChannels > 1 ? sourceFrame[1] : sourceFrame[0];
                    m_inputStorage[0].channels[0][frame] = finiteAudioSample(
                        0.5f * finiteAudioSample(sourceFrame[0]) + 0.5f * finiteAudioSample(right));
                } else {
					for (int channel = 0; channel < inputChannels; ++channel) {
						const int canonical = channel < static_cast<int>(canonicalMap.size())
							? canonicalMap[static_cast<size_t>(channel)] : channel;
                        m_inputStorage[0].channels[static_cast<size_t>(channel)][frame] =
							canonical >= 0 && canonical < hostChannels
								? finiteAudioSample(sourceFrame[canonical]) : 0.0f;
					}
                }
            }
        }
        updateTransport(transport);
        clap_process_t processData{};
        processData.steady_time = m_steadyTime;
        processData.frames_count = static_cast<uint32_t>(frames);
        processData.transport = &m_transport;
        processData.audio_inputs = m_inputPorts.empty() ? nullptr : m_inputPorts.data();
        processData.audio_outputs = m_outputPorts.empty() ? nullptr : m_outputPorts.data();
        processData.audio_inputs_count = static_cast<uint32_t>(m_inputPorts.size());
        processData.audio_outputs_count = static_cast<uint32_t>(m_outputPorts.size());
        processData.in_events = &m_inputEvents;
        processData.out_events = &m_outputEvents;
        const double captureBeat = std::isfinite(transport.projectBeat) &&
            transport.projectBeat >= 0.0 ? transport.projectBeat : 0.0;
        const double captureBpm = std::isfinite(transport.bpm) && transport.bpm > 0.0
            ? transport.bpm : 120.0;
        m_captureBlockBeat.store(captureBeat, std::memory_order_release);
        m_captureBlockBpm.store(captureBpm, std::memory_order_release);
        m_outputEventFrameLimit.store(static_cast<uint32_t>(frames), std::memory_order_release);
        m_parameterFlushRequested.store(false, std::memory_order_release);
        const clap_process_status status = processClapSeh(m_plugin, &processData);
        m_outputEventFrameLimit.store(kInactiveOutputEventFrameLimit,
                                      std::memory_order_release);
        if (status != CLAP_PROCESS_ERROR && m_hostParameterEventsInBlock)
            m_stateDirty.store(true, std::memory_order_release);
        m_steadyTime += frames;
        if (status == CLAP_PROCESS_ERROR || m_mainOutputPort < 0 ||
            m_mainOutputPort >= static_cast<int>(m_outputPorts.size()) ||
            m_outputPorts[static_cast<size_t>(m_mainOutputPort)].channel_count == 0) {
            if (status == CLAP_PROCESS_ERROR) {
                m_crashed = true;
            }
            return;
        }
        const size_t firstBus = effectMode ? static_cast<size_t>(m_mainOutputPort) : 0u;
        const size_t busEnd = effectMode ? firstBus + 1u : m_outputPorts.size();
        for (size_t bus = firstBus; bus < busEnd; ++bus) {
            clap_audio_buffer_t& output = m_outputPorts[bus];
            if (output.channel_count == 0 || !output.data32 || !output.data32[0]) continue;
			if (effectMode && effectChannelCount > 2) {
				const int hostChannels = std::clamp(effectChannelCount, 1, kMaxPluginChannels);
				const int pluginChannels = static_cast<int>(output.channel_count);
				for (int frame = 0; frame < frames; ++frame) {
					float* destination = mix + static_cast<size_t>(frame) * hostChannels;
					if (pluginChannels == 1) {
						const float mono = finiteAudioSample(output.data32[0][frame]);
						destination[0] = mono;
						if (hostChannels > 1) destination[1] = mono;
					} else {
						const std::vector<int>& canonicalMap =
							m_outputStorage[bus].canonicalMap;
						for (int channel = 0; channel < pluginChannels; ++channel) {
							const int canonical = channel < static_cast<int>(canonicalMap.size())
								? canonicalMap[static_cast<size_t>(channel)] : channel;
							if (canonical >= 0 && canonical < hostChannels)
								destination[canonical] = finiteAudioSample(output.data32[channel][frame]);
						}
					}
				}
				break;
			}
            float* left = output.data32[0];
            float* right = output.channel_count > 1 ? output.data32[1] : left;
            PortStorage& storage = m_outputStorage[bus];
            const bool projected = output.channel_count > 2 ||
                storage.downmixCoefficients.requiresProjection(
                    static_cast<int>(output.channel_count));
            if (projected) {
                downmixPlanarPluginBus(output.data32,
                    static_cast<int>(output.channel_count), frames,
                    storage.downmixLeft.data(), storage.downmixRight.data(),
                    &storage.downmixCoefficients);
                left = storage.downmixLeft.data();
                right = storage.downmixRight.data();
            }
            deliverPluginOutput(mix, frames, left, right,
                                output.channel_count == 1 && !projected,
                                layerId, static_cast<int>(bus),
                                static_cast<int>(bus) == m_mainOutputPort, gain, pan,
                                transport.panLawDb,
                                effectMode ? nullptr : routes,
                                effectMode ? nullptr : sink, sinkContext, effectMode);
            if (effectMode) break;
        }
    }

private:
    bool restoreState(const QByteArray& bytes)
    {
        if (!m_plugin || bytes.isEmpty() || bytes.size() > kMaxPluginStateBytes) return false;
        const auto* state = static_cast<const clap_plugin_state_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_STATE));
        ClapStateReader reader{&bytes, 0};
        clap_istream_t stream{&reader, &clapStateRead};
        return clapStateLoadSeh(state, m_plugin, &stream);
    }

    const clap_param_info_t* parameterInfo(clap_id id) const noexcept
    {
        const auto found = std::lower_bound(
            m_parameterLookup.begin(), m_parameterLookup.end(), id,
            [](const auto& item, clap_id target) { return item.first < target; });
        return found != m_parameterLookup.end() && found->first == id &&
            found->second < m_parameterInfos.size()
            ? &m_parameterInfos[found->second] : nullptr;
    }

    bool enqueueHostParameterEvent(const ClapInputEvent& event) noexcept
    {
        const size_t write = m_hostParameterWrite.load(std::memory_order_relaxed);
        const size_t next = (write + 1u) % m_hostParameterEvents.size();
        if (next == m_hostParameterRead.load(std::memory_order_acquire)) return false;
        m_hostParameterEvents[write] = event;
        m_hostParameterWrite.store(next, std::memory_order_release);
        m_processRequestGeneration.fetch_add(1, std::memory_order_release);
        return true;
    }

    void drainHostParameterEvents() noexcept
    {
        size_t read = m_hostParameterRead.load(std::memory_order_relaxed);
        const size_t write = m_hostParameterWrite.load(std::memory_order_acquire);
        while (read != write && m_events.size() < kMaxMidiEventsPerBlock) {
            m_events.push_back(m_hostParameterEvents[read]);
            read = (read + 1u) % m_hostParameterEvents.size();
            m_hostParameterEventsInBlock = true;
        }
        m_hostParameterRead.store(read, std::memory_order_release);
    }

    void flushRequestedParameterEvents() noexcept
    {
        if (!m_parameterFlushRequested.load(std::memory_order_acquire) || !m_plugin ||
            m_pluginCallGate.test_and_set(std::memory_order_acquire)) return;
        if (!m_parameterFlushRequested.exchange(false, std::memory_order_acq_rel)) {
            m_pluginCallGate.clear(std::memory_order_release);
            return;
        }
        m_events.clear();
        m_hostParameterEventsInBlock = false;
        drainHostParameterEvents();
        m_outputEventFrameLimit.store(0, std::memory_order_release);
        clapParamFlushSeh(m_params, m_plugin, &m_inputEvents, &m_outputEvents);
        m_outputEventFrameLimit.store(kInactiveOutputEventFrameLimit,
                                      std::memory_order_release);
        if (m_hostParameterEventsInBlock)
            m_stateDirty.store(true, std::memory_order_release);
        m_pluginCallGate.clear(std::memory_order_release);
    }

    struct PortStorage {
        std::vector<std::vector<float>> channels;
        std::vector<float*> pointers;
        std::vector<float> downmixLeft;
        std::vector<float> downmixRight;
        StereoDownmixCoefficients downmixCoefficients;
		std::vector<int> canonicalMap;
    };

    static ClapInstance* fromHost(const clap_host_t* host)
    {
        return host ? static_cast<ClapInstance*>(host->host_data) : nullptr;
    }

    static const void* CLAP_ABI hostGetExtension(const clap_host_t*, const char* extensionId)
    {
        static const clap_host_note_ports_t notePorts = {
            &hostSupportedNoteDialects,
            &hostRescanNotePorts
        };
        static const clap_host_audio_ports_t audioPorts = {
            &hostAudioPortRescanSupported,
            &hostRescanAudioPorts
        };
        static const clap_host_state_t state = {&hostMarkStateDirty};
        static const clap_host_params_t params = {
            &hostRescanParams, &hostClearParam, &hostRequestParamFlush
        };
        static const clap_host_latency_t latency = {&hostLatencyChanged};
        static const clap_host_tail_t tail = {&hostTailChanged};
        static const clap_host_surround_t surround = {&hostSurroundChanged};
        if (!extensionId) return nullptr;
        if (std::strcmp(extensionId, CLAP_EXT_NOTE_PORTS) == 0) return &notePorts;
        if (std::strcmp(extensionId, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPorts;
        if (std::strcmp(extensionId, CLAP_EXT_STATE) == 0) return &state;
        if (std::strcmp(extensionId, CLAP_EXT_PARAMS) == 0) return &params;
        if (std::strcmp(extensionId, CLAP_EXT_LATENCY) == 0) return &latency;
        if (std::strcmp(extensionId, CLAP_EXT_TAIL) == 0) return &tail;
        if (std::strcmp(extensionId, CLAP_EXT_SURROUND) == 0 ||
            std::strcmp(extensionId, CLAP_EXT_SURROUND_COMPAT) == 0) return &surround;
        return nullptr;
    }
    static void CLAP_ABI hostRequestRestart(const clap_host_t* host)
    {
        if (auto* self = fromHost(host)) self->m_restartRequested.store(true, std::memory_order_release);
    }
    static void CLAP_ABI hostRequestProcess(const clap_host_t* host)
    {
        if (auto* self = fromHost(host))
            self->m_processRequestGeneration.fetch_add(1, std::memory_order_release);
    }
    static void CLAP_ABI hostSurroundChanged(const clap_host_t* host)
    {
        if (auto* self = fromHost(host)) {
            self->m_capabilityRescanRequested.store(true, std::memory_order_release);
            self->m_restartRequested.store(true, std::memory_order_release);
        }
    }
    static void CLAP_ABI hostRequestCallback(const clap_host_t* host)
    {
        if (auto* self = fromHost(host))
            self->m_mainThreadCallbackRequested.store(true, std::memory_order_release);
    }
    static uint32_t CLAP_ABI hostSupportedNoteDialects(const clap_host_t*)
    {
        return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
    }
    static void CLAP_ABI hostRescanNotePorts(const clap_host_t* host, uint32_t)
    {
        if (auto* self = fromHost(host)) {
            self->m_capabilityRescanRequested.store(true, std::memory_order_release);
            self->m_restartRequested.store(true, std::memory_order_release);
        }
    }
    static bool CLAP_ABI hostAudioPortRescanSupported(const clap_host_t*, uint32_t flags)
    {
        constexpr uint32_t supported = CLAP_AUDIO_PORTS_RESCAN_NAMES |
            CLAP_AUDIO_PORTS_RESCAN_FLAGS | CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT |
            CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE | CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR |
            CLAP_AUDIO_PORTS_RESCAN_LIST;
        return flags != 0 && (flags & ~supported) == 0;
    }
    static void CLAP_ABI hostRescanAudioPorts(const clap_host_t* host, uint32_t)
    {
        if (auto* self = fromHost(host)) {
            self->m_capabilityRescanRequested.store(true, std::memory_order_release);
            self->m_restartRequested.store(true, std::memory_order_release);
        }
    }
    static void CLAP_ABI hostMarkStateDirty(const clap_host_t* host)
    {
        if (auto* self = fromHost(host)) {
            if (self->m_editDepth.load(std::memory_order_acquire) == 0)
                self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
            self->m_stateDirty.store(true, std::memory_order_release);
        }
    }
    static void CLAP_ABI hostRescanParams(const clap_host_t* host, uint32_t flags)
    {
        if (auto* self = fromHost(host)) {
            if ((flags & CLAP_PARAM_RESCAN_ALL) != 0)
                self->m_restartRequested.store(true, std::memory_order_release);
            if ((flags & CLAP_PARAM_RESCAN_VALUES) != 0) {
                self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
                self->m_stateDirty.store(true, std::memory_order_release);
            }
        }
    }
    static void CLAP_ABI hostClearParam(const clap_host_t*, clap_id, uint32_t) {}
    static void CLAP_ABI hostRequestParamFlush(const clap_host_t* host)
    {
        if (auto* self = fromHost(host)) {
            self->m_parameterFlushRequested.store(true, std::memory_order_release);
            self->m_processRequestGeneration.fetch_add(1, std::memory_order_release);
        }
    }

    static void CLAP_ABI hostLatencyChanged(const clap_host_t* host)
    {
        if (auto* self = fromHost(host)) self->m_metricsDirty.store(true, std::memory_order_release);
    }
    static void CLAP_ABI hostTailChanged(const clap_host_t* host)
    {
        if (auto* self = fromHost(host)) self->m_metricsDirty.store(true, std::memory_order_release);
    }

    static uint32_t CLAP_ABI inputEventCount(const clap_input_events_t* list)
    {
        const auto* self = static_cast<const ClapInstance*>(list->ctx);
        return self ? static_cast<uint32_t>(self->m_events.size()) : 0;
    }

    static const clap_event_header_t* CLAP_ABI inputEventGet(const clap_input_events_t* list, uint32_t index)
    {
        const auto* self = static_cast<const ClapInstance*>(list->ctx);
        return self && index < self->m_events.size() ? &self->m_events[index].header : nullptr;
    }

    static bool CLAP_ABI outputEventPush(const clap_output_events_t* list,
                                         const clap_event_header_t* event)
    {
        auto* self = list ? static_cast<ClapInstance*>(list->ctx) : nullptr;
        if (!self || !event || event->size < sizeof(clap_event_header_t) ||
            event->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
        const uint32_t frameLimit = self->m_outputEventFrameLimit.load(
            std::memory_order_acquire);
        if (frameLimit == kInactiveOutputEventFrameLimit ||
            (frameLimit == 0 ? event->time != 0 : event->time >= frameLimit)) return false;
        switch (event->type) {
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:
            if (event->size < sizeof(clap_event_param_gesture_t)) return false;
            {
            const uint32_t parameterId =
                reinterpret_cast<const clap_event_param_gesture_t*>(event)->param_id;
            if (parameterId == CLAP_INVALID_ID || !self->parameterInfo(parameterId)) return false;
            const bool recordable = (event->flags & CLAP_EVENT_DONT_RECORD) == 0;
            auto nativeCallback = self->m_parameterEdits.capture(recordable);
            const int previousDepth =
                self->m_editDepth.fetch_add(1, std::memory_order_acq_rel);
            uint64_t gestureSerial =
                self->m_gestureSerial.load(std::memory_order_acquire);
            bool outermost = false;
            if (recordable) {
                const uint64_t freshSerial =
                    self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
                gestureSerial = self->m_nativeGestureSerials.begin(
                    parameterId,
                    freshSerial, &outermost);
                if (gestureSerial == 0)
                    self->m_parameterEdits.markOverflow(
                        parameterId,
                        freshSerial);
            } else if (previousDepth == 0) {
                self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
            }
            if (recordable && outermost && gestureSerial != 0)
                self->m_parameterEdits.push({PluginParameterEditPhase::Begin,
                    parameterId,
                    0.0, gestureSerial,
                    self->captureCounterForOffset(event->time),
                    self->captureBeatForOffset(event->time)});
            return true;
            }
        case CLAP_EVENT_PARAM_VALUE: {
            if (event->size < sizeof(clap_event_param_value_t)) return false;
            const auto* value = reinterpret_cast<const clap_event_param_value_t*>(event);
            if (value->param_id == CLAP_INVALID_ID || !std::isfinite(value->value)) return false;
            const clap_param_info_t* info = self->parameterInfo(value->param_id);
            if (!info) return false;
            if ((info->flags & CLAP_PARAM_IS_READONLY) != 0) return true;
            const bool recordable = (event->flags & CLAP_EVENT_DONT_RECORD) == 0 &&
                value->note_id == -1 && value->port_index == -1 &&
                value->channel == -1 && value->key == -1;
            auto nativeCallback = self->m_parameterEdits.capture(recordable);
            uint64_t gestureSerial = 0;
            bool suppressed = false;
            bool trackerOverflowed = false;
            if (recordable) {
                gestureSerial = self->m_nativeGestureSerials.current(
                    value->param_id, 0, &suppressed, &trackerOverflowed);
                if (trackerOverflowed)
                    self->m_parameterEdits.markOverflow(
                        value->param_id,
                        self->m_gestureSerial.load(std::memory_order_acquire));
                if (gestureSerial == 0 && !suppressed)
                    gestureSerial =
                        self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
            } else if (self->m_editDepth.load(std::memory_order_acquire) == 0) {
                self->m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
            }
            self->m_stateDirty.store(true, std::memory_order_release);
            if (recordable && !suppressed) {
                const double range = info->max_value - info->min_value;
                const double normalized = range > 0.0
                    ? std::clamp((value->value - info->min_value) / range, 0.0, 1.0)
                    : 0.0;
                self->m_parameterEdits.push({PluginParameterEditPhase::Value,
                    value->param_id, normalized,
                    gestureSerial,
                    self->captureCounterForOffset(event->time),
                    self->captureBeatForOffset(event->time)});
            }
            return true;
        }
        case CLAP_EVENT_PARAM_MOD:
            return event->size >= sizeof(clap_event_param_mod_t);
        case CLAP_EVENT_PARAM_GESTURE_END: {
            if (event->size < sizeof(clap_event_param_gesture_t)) return false;
            const bool recordable = (event->flags & CLAP_EVENT_DONT_RECORD) == 0;
            auto nativeCallback = self->m_parameterEdits.capture(recordable);
            const uint32_t parameterId =
                reinterpret_cast<const clap_event_param_gesture_t*>(event)->param_id;
            if (parameterId == CLAP_INVALID_ID || !self->parameterInfo(parameterId)) return false;
            bool terminal = false;
            bool trackerOverflowed = false;
            bool trackerConsumed = false;
            const uint64_t gestureSerial = recordable
                ? self->m_nativeGestureSerials.end(
                    parameterId, self->m_gestureSerial.load(std::memory_order_acquire),
                    &terminal, &trackerOverflowed, &trackerConsumed)
                : 0;
            if (trackerOverflowed)
                self->m_parameterEdits.markOverflow(
                    parameterId,
                    self->m_gestureSerial.load(std::memory_order_acquire));
            if (!recordable || trackerConsumed)
                decrementPositive(self->m_editDepth);
            self->m_stateDirty.store(true, std::memory_order_release);
            if (recordable && terminal)
                self->m_parameterEdits.push({PluginParameterEditPhase::End,
                    parameterId, 0.0, gestureSerial,
                    self->captureCounterForOffset(event->time),
                    self->captureBeatForOffset(event->time)});
            return true;
        }
        default:
            return false;
        }
    }

    static clap_process_status processClapSeh(const clap_plugin_t* plugin, const clap_process_t* process) noexcept
    {
        __try {
            return plugin && plugin->process ? plugin->process(plugin, process) : CLAP_PROCESS_ERROR;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return CLAP_PROCESS_ERROR;
        }
    }

    uint64_t captureCounterForOffset(uint32_t sampleOffset) const noexcept
    {
        (void)sampleOffset;
        return nativeEditCounterNow();
    }

    double captureBeatForOffset(uint32_t sampleOffset) const noexcept
    {
        const double blockBeat = m_captureBlockBeat.load(std::memory_order_acquire);
        const double bpm = m_captureBlockBpm.load(std::memory_order_acquire);
        if (!std::isfinite(blockBeat) || blockBeat < 0.0 ||
            !std::isfinite(bpm) || bpm <= 0.0 || m_sampleRate <= 0.0) return -1.0;
        return blockBeat + static_cast<double>(sampleOffset) * bpm /
            (60.0 * m_sampleRate);
    }

    void initializeHost()
    {
        m_host = {};
        m_host.clap_version = CLAP_VERSION;
        m_host.host_data = this;
        m_host.name = "VeraCore";
        m_host.vendor = "VeraCore";
        m_host.url = "https://veracore.local";
        m_host.version = "0.1.0";
        m_host.get_extension = &hostGetExtension;
        m_host.request_restart = &hostRequestRestart;
        m_host.request_process = &hostRequestProcess;
        m_host.request_callback = &hostRequestCallback;
    }

    void discoverCapabilities()
    {
        auto has = [this](const char* id) {
            return m_plugin && id && clapGetExtensionSeh(m_plugin, id) != nullptr;
        };
        m_capabilities = {};
        m_capabilities.format = "CLAP";
        m_capabilities.audioPorts = has(CLAP_EXT_AUDIO_PORTS);
        m_capabilities.notePorts = has(CLAP_EXT_NOTE_PORTS);
        m_capabilities.parameters = has(CLAP_EXT_PARAMS);
        m_capabilities.persistentState = has(CLAP_EXT_STATE);
        m_capabilities.nativeGui = has(CLAP_EXT_GUI);
        m_capabilities.latency = has(CLAP_EXT_LATENCY);
        m_capabilities.tail = has(CLAP_EXT_TAIL);
        m_capabilities.offlineRender = has(CLAP_EXT_RENDER);
        m_capabilities.noteNames = has(CLAP_EXT_NOTE_NAME);
        // CLAP note-name labels must not be promoted to articulations. CLAP has
        // no standardized keyswitch discovery extension.
        m_capabilities.keyswitches = false;
        m_capabilities.voiceInfo = has(CLAP_EXT_VOICE_INFO);
        m_capabilities.remoteControls = has(CLAP_EXT_REMOTE_CONTROLS);
        m_capabilities.trackInfo = has(CLAP_EXT_TRACK_INFO);
        m_capabilities.contextMenu = has(CLAP_EXT_CONTEXT_MENU);
        m_capabilities.presetLoad = has(CLAP_EXT_PRESET_LOAD);
        m_capabilities.timerSupport = has(CLAP_EXT_TIMER_SUPPORT);
        m_capabilities.threadPool = has(CLAP_EXT_THREAD_POOL);
        m_capabilities.araSdkCompiled = kAraSdkCompiled;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        m_araPluginExtension = static_cast<const clap_ara_plugin_extension_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_ARA_PLUGINEXTENSION));
        if (!m_araPluginExtension) {
            m_araPluginExtension = static_cast<const clap_ara_plugin_extension_t*>(
                clapGetExtensionSeh(m_plugin, CLAP_EXT_ARA_PLUGINEXTENSION_COMPAT));
        }
        m_araFactory = clapAraFactorySeh(m_araPluginExtension, m_plugin);
        if (!validAraFactory(m_araFactory)) m_araFactory = nullptr;
        m_capabilities.araFactory = m_araFactory != nullptr;
#endif
    }

    void scanParameters()
    {
        m_parameterInfos.clear();
        m_parameterLookup.clear();
        m_readOnlyParameterIds.clear();
        m_params = static_cast<const clap_plugin_params_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_PARAMS));
        const auto* params = m_params;
        const uint32_t count = std::min<uint32_t>(clapParamCountSeh(params, m_plugin), 65536u);
        m_parameterInfos.reserve(count);
        m_parameterLookup.reserve(count);
        m_readOnlyParameterIds.reserve(count);
        std::set<clap_id> seenIds;
        for (uint32_t index = 0; index < count; ++index) {
            clap_param_info_t info{};
            if (!clapParamInfoSeh(params, m_plugin, index, info) ||
                info.id == CLAP_INVALID_ID || !std::isfinite(info.min_value) ||
                !std::isfinite(info.max_value) || !std::isfinite(info.default_value) ||
                info.max_value < info.min_value || !seenIds.insert(info.id).second) continue;
            m_parameterLookup.push_back({info.id, m_parameterInfos.size()});
            m_parameterInfos.push_back(info);
            if ((info.flags & CLAP_PARAM_IS_READONLY) != 0)
                m_readOnlyParameterIds.push_back(info.id);
        }
        std::sort(m_parameterLookup.begin(), m_parameterLookup.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        std::sort(m_readOnlyParameterIds.begin(), m_readOnlyParameterIds.end());
        m_readOnlyParameterIds.erase(
            std::unique(m_readOnlyParameterIds.begin(), m_readOnlyParameterIds.end()),
            m_readOnlyParameterIds.end());
    }

#if defined(VERACORE_ARA_SDK_AVAILABLE)
    void bindAraDocument(AraDocumentRegistry* documents)
    {
        if (!documents || !m_araFactory || !m_araPluginExtension) return;
        try {
            m_araDocument = documents->acquire(m_araFactory);
        } catch (...) {
            m_araDocument.reset();
            return;
        }
        if (!m_araDocument || !m_araDocument->ready()) {
            m_araDocument.reset();
            return;
        }
        m_araExtensionInstance = clapAraBindSeh(
            m_araPluginExtension, m_plugin, m_araDocument->documentControllerRef(),
            kAraRenderAndEditorRoles, kAraRenderAndEditorRoles);
        m_araBinding = validateAraBinding(
            m_araExtensionInstance, kAraRenderAndEditorRoles, m_araDocument);
        m_capabilities.araDocumentController = m_araBinding.documentController;
        m_capabilities.araPlaybackRenderer = m_araBinding.playbackRenderer;
        m_capabilities.araEditorRenderer = m_araBinding.editorRenderer;
        m_capabilities.araEditorView = m_araBinding.editorView;
        m_capabilities.araContentAccess = m_araBinding.contentAccess;
    }

    bool assignAraRegions() noexcept
    {
        if (!m_araDocument || !m_araExtensionInstance || !m_araBinding.playbackRenderer ||
            m_araTrackIndex < 0) return true;
        try {
            const auto regions = m_araDocument->playbackRegionRefsForTrack(m_araTrackIndex);
            m_araAssignedRegions.reserve(regions.size());
            for (const auto region : regions) {
                if (!addAraRendererRegionSeh(m_araExtensionInstance, region, m_araEditorOwner)) {
                    clearAraRegionAssignments();
                    return false;
                }
                m_araAssignedRegions.push_back(region);
            }
        } catch (...) {
            clearAraRegionAssignments();
            return false;
        }
        return true;
    }

    void clearAraRegionAssignments() noexcept
    {
        for (auto it = m_araAssignedRegions.rbegin(); it != m_araAssignedRegions.rend(); ++it)
            removeAraRendererRegionSeh(m_araExtensionInstance, *it, m_araEditorOwner);
        m_araAssignedRegions.clear();
    }
#endif

    void initializeEventInterfaces()
    {
        m_inputEvents = {this, &inputEventCount, &inputEventGet};
        m_outputEvents = {this, &outputEventPush};
    }

    void updateTransport(const PluginTransportState& transport)
    {
        const double safeBpm = std::isfinite(transport.bpm) && transport.bpm > 0.0
            ? transport.bpm : 120.0;
        const double safeBeat = std::isfinite(transport.projectBeat) && transport.projectBeat >= 0.0
            ? transport.projectBeat : 0.0;
        const double safeSeconds = boundedPluginTimelineSeconds(
            transport.projectSeconds, safeBeat * 60.0 / safeBpm);
        const double beatsPerBar = transportBeatsPerBar(transport);
        const double safeBarStart = std::isfinite(transport.barStartBeat) &&
            transport.barStartBeat >= 0.0 && transport.barStartBeat <= safeBeat + 1.0e-9
            ? transport.barStartBeat
            : std::floor(safeBeat / beatsPerBar) * beatsPerBar;
        const bool validCycle = transport.cycleActive &&
            std::isfinite(transport.cycleStartBeat) &&
            std::isfinite(transport.cycleEndBeat) &&
            std::isfinite(transport.cycleStartSeconds) &&
            std::isfinite(transport.cycleEndSeconds) &&
            transport.cycleStartBeat >= 0.0 &&
            transport.cycleEndBeat > transport.cycleStartBeat &&
            transport.cycleStartSeconds >= 0.0 &&
            transport.cycleEndSeconds > transport.cycleStartSeconds;
        auto fixedPoint = [](double value, int64_t factor) {
            const long double scaled = static_cast<long double>(value) * factor;
            if (!std::isfinite(scaled)) return int64_t(0);
            if (scaled >= static_cast<long double>(std::numeric_limits<int64_t>::max()))
                return std::numeric_limits<int64_t>::max();
            if (scaled <= static_cast<long double>(std::numeric_limits<int64_t>::min()))
                return std::numeric_limits<int64_t>::min();
            return static_cast<int64_t>(std::llround(scaled));
        };
        m_transport = {};
        m_transport.header.size = sizeof(clap_event_transport_t);
        m_transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        m_transport.header.type = CLAP_EVENT_TRANSPORT;
        m_transport.flags = CLAP_TRANSPORT_HAS_TEMPO |
            CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
            CLAP_TRANSPORT_HAS_SECONDS_TIMELINE |
            CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        if (transport.playing) m_transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
        if (transport.recording) m_transport.flags |= CLAP_TRANSPORT_IS_RECORDING;
        if (validCycle) m_transport.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
        m_transport.song_pos_beats = fixedPoint(safeBeat, CLAP_BEATTIME_FACTOR);
        m_transport.song_pos_seconds = fixedPoint(safeSeconds, CLAP_SECTIME_FACTOR);
        m_transport.tempo = safeBpm;
        if (validCycle) {
            m_transport.loop_start_beats = fixedPoint(
                transport.cycleStartBeat, CLAP_BEATTIME_FACTOR);
            m_transport.loop_end_beats = fixedPoint(
                transport.cycleEndBeat, CLAP_BEATTIME_FACTOR);
            m_transport.loop_start_seconds = fixedPoint(
                transport.cycleStartSeconds, CLAP_SECTIME_FACTOR);
            m_transport.loop_end_seconds = fixedPoint(
                transport.cycleEndSeconds, CLAP_SECTIME_FACTOR);
        }
        m_transport.bar_start = fixedPoint(safeBarStart, CLAP_BEATTIME_FACTOR);
        m_transport.bar_number = std::clamp(transport.barNumber, 0, INT32_MAX);
        m_transport.tsig_num = static_cast<uint16_t>(transport.numerator);
        m_transport.tsig_denom = static_cast<uint16_t>(transport.denominator);
    }

    void selectAudioPortConfiguration(VeraCoreSpeakerLayout layout)
    {
        const auto* configs = static_cast<const clap_plugin_audio_ports_config_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_AUDIO_PORTS_CONFIG));
        if (!configs) return;
        const uint32_t targetChannels = static_cast<uint32_t>(
            std::clamp(VeraCoreSpeakerChannelCount(layout), 1, kVeraCoreMaxSpatialChannels));
        clap_id selected = CLAP_INVALID_ID;
        int selectedScore = -1;
        const uint32_t count = std::min<uint32_t>(
            clapAudioPortsConfigCountSeh(configs, m_plugin), 256u);
        for (uint32_t index = 0; index < count; ++index) {
            clap_audio_ports_config_t info{};
            if (!clapAudioPortsConfigGetSeh(configs, m_plugin, index, &info) ||
                !info.has_main_output || info.main_output_channel_count != targetChannels ||
                (info.has_main_input && info.main_input_channel_count != targetChannels)) {
                continue;
            }
            int score = info.has_main_input ? 4 : 2;
            if (targetChannels <= 2) {
                if (info.main_output_port_type &&
                    std::strcmp(info.main_output_port_type,
                        targetChannels == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO) == 0) ++score;
            } else if (info.main_output_port_type &&
                       std::strcmp(info.main_output_port_type, CLAP_PORT_SURROUND) == 0) {
                score += 2;
            }
            if (score > selectedScore) {
                selected = info.id;
                selectedScore = score;
            }
        }
        if (selected != CLAP_INVALID_ID)
            clapAudioPortsConfigSelectSeh(configs, m_plugin, selected);
    }

    void scanAudioPorts()
    {
        const auto* extension = static_cast<const clap_plugin_audio_ports_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_AUDIO_PORTS));
        if (!extension || !extension->count || !extension->get) {
            return;
        }
        const auto* surround = static_cast<const clap_plugin_surround_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_SURROUND));
        if (!surround) {
            surround = static_cast<const clap_plugin_surround_t*>(
                clapGetExtensionSeh(m_plugin, CLAP_EXT_SURROUND_COMPAT));
        }
        std::vector<StereoDownmixCoefficients> outputCoefficients;
        std::vector<std::vector<int>> inputCanonicalMaps;
        std::vector<std::vector<int>> outputCanonicalMaps;
        auto scan = [&](bool input, std::vector<uint32_t>& counts,
                        std::vector<std::vector<int>>& canonicalMaps) {
            const uint32_t portCount = clapAudioPortCountSeh(extension, m_plugin, input);
            for (uint32_t port = 0; port < portCount; ++port) {
                clap_audio_port_info_t info{};
                if (clapAudioPortGetSeh(extension, m_plugin, port, input, &info) && info.channel_count > 0) {
                    const uint32_t channelCount =
                        std::min<uint32_t>(info.channel_count, kMaxPluginChannels);
                    counts.push_back(channelCount);
                    std::vector<int> canonicalMap(channelCount, -1);
                    if (surround && info.channel_count <= kMaxPluginChannels) {
                        std::array<uint8_t, kMaxPluginChannels> channelMap{};
                        const uint32_t mapped = clapSurroundChannelMapSeh(
                            surround, m_plugin, input, port, channelMap.data(), info.channel_count);
                        if (mapped == info.channel_count) {
                            for (uint32_t channel = 0; channel < channelCount; ++channel)
                                canonicalMap[channel] = canonicalIndexForClapSurround(channelMap[channel]);
                        }
                    }
                    for (uint32_t channel = 0; channel < channelCount; ++channel)
                        if (canonicalMap[channel] < 0) canonicalMap[channel] = static_cast<int>(channel);
                    canonicalMaps.push_back(std::move(canonicalMap));
                    if (!input) {
                        StereoDownmixCoefficients coefficients;
                        if (surround && info.channel_count <= kMaxPluginChannels) {
                            std::array<uint8_t, kMaxPluginChannels> channelMap{};
                            const uint32_t mapped = clapSurroundChannelMapSeh(
                                surround, m_plugin, false, port, channelMap.data(),
                                info.channel_count);
                            if (mapped == info.channel_count) {
                                buildClapStereoDownmix(channelMap.data(),
                                    static_cast<int>(info.channel_count), coefficients);
                            }
                        }
                        outputCoefficients.push_back(std::move(coefficients));
                        m_outputPortIds.push_back(info.id);
                        m_outputPortNames.push_back(QString::fromUtf8(
                            info.name, static_cast<qsizetype>(strnlen_s(info.name, sizeof(info.name)))).trimmed());
                        if ((info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0 && m_mainOutputPort < 0)
                            m_mainOutputPort = static_cast<int>(counts.size()) - 1;
                    }
                }
            }
        };
        scan(true, m_inputChannelCounts, inputCanonicalMaps);
        scan(false, m_outputChannelCounts, outputCanonicalMaps);
        if (m_mainOutputPort < 0 && !m_outputChannelCounts.empty()) m_mainOutputPort = 0;
        m_inputStorage.resize(m_inputChannelCounts.size());
        m_outputStorage.resize(m_outputChannelCounts.size());
		for (size_t port = 0; port < m_inputStorage.size() &&
			 port < inputCanonicalMaps.size(); ++port)
			m_inputStorage[port].canonicalMap = std::move(inputCanonicalMaps[port]);
        for (size_t port = 0; port < m_outputStorage.size() &&
             port < outputCoefficients.size(); ++port) {
            m_outputStorage[port].downmixCoefficients = std::move(outputCoefficients[port]);
			if (port < outputCanonicalMaps.size())
				m_outputStorage[port].canonicalMap = std::move(outputCanonicalMaps[port]);
        }
        m_inputPorts.resize(m_inputChannelCounts.size());
        m_outputPorts.resize(m_outputChannelCounts.size());
    }

    void scanNotePort()
    {
        m_noteEventsSupported = true;
        m_useClapNoteDialect = true;
        m_notePortIndex = 0;
        const auto* extension = static_cast<const clap_plugin_note_ports_t*>(
            clapGetExtensionSeh(m_plugin, CLAP_EXT_NOTE_PORTS));
        if (!extension) return;
        if (clapNotePortCountSeh(extension, m_plugin, true) == 0) {
            m_noteEventsSupported = false;
            return;
        }
        clap_note_port_info_t info{};
        if (!clapNotePortGetSeh(extension, m_plugin, 0, true, &info)) return;
        const uint32_t supported = info.supported_dialects;
        const bool clapSupported = (supported & CLAP_NOTE_DIALECT_CLAP) != 0;
        const bool midiSupported =
            (supported & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE)) != 0;
        if (info.preferred_dialect == CLAP_NOTE_DIALECT_CLAP && clapSupported) {
            m_useClapNoteDialect = true;
        } else if ((info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI ||
                    info.preferred_dialect == CLAP_NOTE_DIALECT_MIDI_MPE) && midiSupported) {
            m_useClapNoteDialect = false;
        } else if (clapSupported) {
            m_useClapNoteDialect = true;
        } else if (midiSupported) {
            m_useClapNoteDialect = false;
        } else {
            m_noteEventsSupported = false;
        }
    }

    void preparePorts(int frames)
    {
        auto prepare = [&](const std::vector<uint32_t>& counts, std::vector<PortStorage>& storage,
                           std::vector<clap_audio_buffer_t>& ports) {
            for (size_t port = 0; port < counts.size(); ++port) {
                storage[port].channels.resize(counts[port]);
                storage[port].pointers.resize(counts[port]);
                if (counts[port] > 2 || storage[port].downmixCoefficients.requiresProjection(
                        static_cast<int>(counts[port]))) {
                    storage[port].downmixLeft.resize(static_cast<size_t>(frames));
                    storage[port].downmixRight.resize(static_cast<size_t>(frames));
                }
                for (uint32_t channel = 0; channel < counts[port]; ++channel) {
                    auto& samples = storage[port].channels[channel];
                    if (static_cast<int>(samples.size()) < frames) {
                        samples.resize(frames);
                    }
                    std::fill_n(samples.begin(), frames, 0.0f);
                    storage[port].pointers[channel] = samples.data();
                }
                ports[port] = {};
                ports[port].data32 = storage[port].pointers.data();
                ports[port].channel_count = counts[port];
            }
        };
        prepare(m_inputChannelCounts, m_inputStorage, m_inputPorts);
        prepare(m_outputChannelCounts, m_outputStorage, m_outputPorts);
    }

    bool clearPorts(int frames) noexcept
    {
        if (frames <= 0 || frames > kDefaultBlockSize) return false;
        auto clear = [frames](const std::vector<uint32_t>& counts,
                              std::vector<PortStorage>& storage,
                              std::vector<clap_audio_buffer_t>& ports) noexcept {
            if (storage.size() != counts.size() || ports.size() != counts.size()) return false;
            for (size_t portIndex = 0; portIndex < counts.size(); ++portIndex) {
                PortStorage& port = storage[portIndex];
                if (port.channels.size() != counts[portIndex] ||
                    port.pointers.size() != counts[portIndex] ||
                    ((counts[portIndex] > 2 ||
                      port.downmixCoefficients.requiresProjection(
                          static_cast<int>(counts[portIndex]))) &&
                     (port.downmixLeft.size() < static_cast<size_t>(frames) ||
                      port.downmixRight.size() < static_cast<size_t>(frames))) ||
                    ports[portIndex].data32 != port.pointers.data() ||
                    ports[portIndex].channel_count != counts[portIndex]) return false;
                ports[portIndex].data64 = nullptr;
                ports[portIndex].latency = 0;
                ports[portIndex].constant_mask = 0;
                for (size_t channel = 0; channel < port.channels.size(); ++channel) {
                    auto& samples = port.channels[channel];
                    if (samples.size() < static_cast<size_t>(frames) ||
                        port.pointers[channel] != samples.data()) return false;
                    std::fill_n(samples.begin(), frames, 0.0f);
                }
            }
            return true;
        };
        return clear(m_inputChannelCounts, m_inputStorage, m_inputPorts) &&
            clear(m_outputChannelCounts, m_outputStorage, m_outputPorts);
    }

    void appendEvent(uint32_t time, uint8_t status, uint8_t data1, uint8_t data2)
    {
        if (m_events.size() >= kMaxMidiEventsPerBlock) {
            return;
        }
        ClapInputEvent event{};
        const uint8_t eventType = status & 0xF0;
        const bool noteOn = eventType == 0x90 && data2 > 0;
        const bool noteOff = eventType == 0x80 || (eventType == 0x90 && data2 == 0);
        if ((noteOn || noteOff) && !m_noteEventsSupported) return;
        if ((noteOn || noteOff) && m_useClapNoteDialect) {
            event.note.header.size = sizeof(clap_event_note_t);
            event.note.header.time = time;
            event.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.note.header.type = noteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
            event.note.note_id = -1;
            event.note.port_index = m_notePortIndex;
            event.note.channel = status & 0x0F;
            event.note.key = data1;
            event.note.velocity = static_cast<double>(data2) / 127.0;
        } else {
            event.midi.header.size = sizeof(clap_event_midi_t);
            event.midi.header.time = time;
            event.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.midi.header.type = CLAP_EVENT_MIDI;
            event.midi.port_index = 0;
            event.midi.data[0] = status;
            event.midi.data[1] = data1;
            event.midi.data[2] = data2;
        }
        m_events.push_back(event);
    }

    void appendAutomation(const std::vector<PluginAutomationLane>* automation,
                          const PluginTransportState& transport, int frames)
    {
        if (!automation || m_editDepth.load(std::memory_order_acquire) > 0) return;
        size_t emitted = 0;
        const size_t laneCount = std::min(automation->size(), kMaxPluginAutomationLanes);
        size_t eligibleRemaining = 0;
        for (size_t laneIndex = 0; laneIndex < laneCount; ++laneIndex) {
            const PluginAutomationLane& lane = (*automation)[laneIndex];
            const clap_param_info_t* info = parameterInfo(lane.parameterId);
            if (lane.enabled && !lane.points.empty() && info &&
                (info->flags & CLAP_PARAM_IS_READONLY) == 0) ++eligibleRemaining;
        }
        for (size_t laneIndex = 0; laneIndex < laneCount &&
             emitted < kMaxAutomationEventsPerBlock; ++laneIndex) {
            const PluginAutomationLane& lane = (*automation)[laneIndex];
            const clap_param_info_t* info = parameterInfo(lane.parameterId);
            if (!lane.enabled || lane.points.empty() || !info ||
                (info->flags & CLAP_PARAM_IS_READONLY) != 0) continue;
            const size_t remainingBudget = kMaxAutomationEventsPerBlock - emitted;
            const int laneBudget = static_cast<int>(std::max<size_t>(
                1, remainingBudget / std::max<size_t>(1, eligibleRemaining)));
            --eligibleRemaining;
            emitPluginAutomationPoints(lane, transport, frames, laneBudget,
                [&](int offset, double normalized) {
                if (emitted >= kMaxAutomationEventsPerBlock ||
                    m_events.size() >= kMaxMidiEventsPerBlock) return;
                double plain = info->min_value + normalized * (info->max_value - info->min_value);
                if ((info->flags & CLAP_PARAM_IS_STEPPED) != 0) plain = std::round(plain);
                ClapInputEvent event{};
                event.parameter.header.size = sizeof(clap_event_param_value_t);
                event.parameter.header.time = static_cast<uint32_t>(offset);
                event.parameter.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                event.parameter.header.type = CLAP_EVENT_PARAM_VALUE;
                event.parameter.header.flags = CLAP_EVENT_DONT_RECORD;
                event.parameter.param_id = lane.parameterId;
                event.parameter.cookie = info->cookie;
                event.parameter.note_id = -1;
                event.parameter.port_index = -1;
                event.parameter.channel = -1;
                event.parameter.key = -1;
                event.parameter.value = std::clamp(plain, info->min_value, info->max_value);
                m_events.push_back(event);
                ++emitted;
                });
        }
    }

    void buildEvents(const std::vector<VeraCoreMidiEvent>& source, int layerId, int frames,
                     const std::vector<PluginAutomationLane>* automation,
                     const PluginTransportState& transport)
    {
        m_events.clear();
        m_hostParameterEventsInBlock = false;
        drainHostParameterEvents();
        if (m_panicPending) {
            for (int channel = 0; channel < 16; ++channel) {
                if (m_noteEventsSupported && m_useClapNoteDialect) {
                    if (m_events.size() >= kMaxMidiEventsPerBlock) break;
                    ClapInputEvent choke{};
                    choke.note.header.size = sizeof(clap_event_note_t);
                    choke.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                    choke.note.header.type = CLAP_EVENT_NOTE_CHOKE;
                    choke.note.note_id = -1;
                    choke.note.port_index = m_notePortIndex;
                    choke.note.channel = static_cast<int16_t>(channel);
                    choke.note.key = -1;
                    m_events.push_back(choke);
                }
                appendEvent(0, static_cast<uint8_t>(0xB0 | channel), 120, 0);
                appendEvent(0, static_cast<uint8_t>(0xB0 | channel), 123, 0);
            }
            m_panicPending = false;
        }
        for (const VeraCoreMidiEvent& event : source) {
            if (event.externalPlugin && event.layerId == layerId) {
                appendEvent(static_cast<uint32_t>(std::clamp(event.sampleOffset, 0, std::max(0, frames - 1))),
                            event.status, event.data1, event.data2);
            }
        }
        // Musical and panic events have capacity priority. Read automation uses
        // the remaining bounded deviceSlots and cannot starve note-off/note-on data.
        appendAutomation(automation, transport, frames);
        const auto eventLess = [](const ClapInputEvent& a, const ClapInputEvent& b) {
            if (a.header.time != b.header.time) return a.header.time < b.header.time;
            // Host gestures/values arrive first; read automation then resumes at
            // the same sample after Touch is released.
            return (a.header.flags & CLAP_EVENT_DONT_RECORD) == 0 &&
                   (b.header.flags & CLAP_EVENT_DONT_RECORD) != 0;
        };
        // Scratch is sized during open(). This stable merge sort is bounded,
        // O(n log n), and performs no allocation in the real-time callback.
        const size_t eventCount = m_events.size();
        for (size_t width = 1; width < eventCount; width *= 2) {
            for (size_t first = 0; first < eventCount; first += width * 2) {
                const size_t middle = std::min(first + width, eventCount);
                const size_t last = std::min(first + width * 2, eventCount);
                size_t left = first;
                size_t right = middle;
                size_t output = first;
                while (left < middle && right < last) {
                    if (eventLess(m_events[right], m_events[left]))
                        m_eventSortScratch[output++] = m_events[right++];
                    else
                        m_eventSortScratch[output++] = m_events[left++];
                }
                while (left < middle) m_eventSortScratch[output++] = m_events[left++];
                while (right < last) m_eventSortScratch[output++] = m_events[right++];
            }
            std::copy_n(m_eventSortScratch.begin(), eventCount, m_events.begin());
        }
    }

    HMODULE m_module = nullptr;
    const clap_plugin_entry_t* m_entry = nullptr;
    const clap_plugin_t* m_plugin = nullptr;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    const clap_ara_plugin_extension_t* m_araPluginExtension = nullptr;
    const ARA::ARAPlugInExtensionInstance* m_araExtensionInstance = nullptr;
    const ARA::ARAFactory* m_araFactory = nullptr;
    std::shared_ptr<AraDocumentContext> m_araDocument;
    AraBindingStatus m_araBinding;
    std::vector<ARA::ARAPlaybackRegionRef> m_araAssignedRegions;
#endif
    clap_host_t m_host{};
    clap_input_events_t m_inputEvents{};
    clap_output_events_t m_outputEvents{};
    const clap_plugin_params_t* m_params = nullptr;
    clap_event_transport_t m_transport{};
    QString m_binaryPath;
    QString m_pluginId;
    double m_sampleRate = 44100.0;
	VeraCoreSpeakerLayout m_speakerLayout = VeraCoreSpeakerLayout::Stereo;
    bool m_entryInitialized = false;
    bool m_pluginInitialized = false;
    bool m_active = false;
    bool m_processing = false;
    std::atomic<bool> m_crashed{false};
    std::atomic<bool> m_panicPending{false};
    std::atomic<bool> m_restartRequested{false};
    std::atomic<bool> m_mainThreadCallbackRequested{false};
    std::atomic<bool> m_parameterFlushRequested{false};
    std::atomic_flag m_pluginCallGate = ATOMIC_FLAG_INIT;
    std::atomic<double> m_captureBlockBeat{0.0};
    std::atomic<double> m_captureBlockBpm{120.0};
    static constexpr uint32_t kInactiveOutputEventFrameLimit =
        std::numeric_limits<uint32_t>::max();
    std::atomic<uint32_t> m_outputEventFrameLimit{kInactiveOutputEventFrameLimit};
    std::atomic<bool> m_capabilityRescanRequested{false};
    std::atomic<bool> m_stateDirty{false};
    std::atomic<bool> m_metricsDirty{false};
    std::atomic<int> m_editDepth{0};
    std::atomic<uint64_t> m_gestureSerial{0};
    BoundedParameterEditQueue<kNativeParameterEditQueueCapacity> m_parameterEdits;
    NativeParameterGestureSerials m_nativeGestureSerials;
    std::atomic<bool> m_araModelUpdateSuspended{false};
    std::atomic<uint64_t> m_processRequestGeneration{0};
    std::atomic<uint64_t> m_observedProcessRequestGeneration{0};
    bool m_offline = false;
    bool m_araEditorOwner = false;
    bool m_araResumeActive = false;
    bool m_araResumeProcessing = false;
    int m_araTrackIndex = -1;
    bool m_noteEventsSupported = true;
    bool m_useClapNoteDialect = true;
    int16_t m_notePortIndex = 0;
    int m_mainOutputPort = -1;
    int64_t m_steadyTime = 0;
    std::vector<uint32_t> m_inputChannelCounts;
    std::vector<uint32_t> m_outputChannelCounts;
    std::vector<clap_id> m_outputPortIds;
    std::vector<clap_param_info_t> m_parameterInfos;
    std::vector<std::pair<clap_id, size_t>> m_parameterLookup;
    std::vector<clap_id> m_readOnlyParameterIds;
    std::array<ClapInputEvent, 1024> m_hostParameterEvents{};
    std::atomic<size_t> m_hostParameterRead{0};
    std::atomic<size_t> m_hostParameterWrite{0};
    bool m_hostParameterEventsInBlock = false;
    std::vector<QString> m_outputPortNames;
    std::vector<PortStorage> m_inputStorage;
    std::vector<PortStorage> m_outputStorage;
    std::vector<clap_audio_buffer_t> m_inputPorts;
    std::vector<clap_audio_buffer_t> m_outputPorts;
    std::vector<ClapInputEvent> m_events;
    std::vector<ClapInputEvent> m_eventSortScratch;
    PluginTechnologyCapabilities m_capabilities;
};

using Vst3GetFactoryProc = Steinberg::IPluginFactory* (PLUGIN_API*)();
using Vst3InitExitProc = bool (PLUGIN_API*)();

Steinberg::IPluginFactory* getVst3FactorySeh(Vst3GetFactoryProc getFactory) noexcept
{
    __try { return getFactory ? getFactory() : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool callVst3ModuleSeh(Vst3InitExitProc function) noexcept
{
    __try { return !function || function(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Steinberg::tresult createVst3ComponentSeh(Steinberg::IPluginFactory* factory,
                                           const Steinberg::TUID classId,
                                           const Steinberg::TUID interfaceId,
                                           void** object) noexcept
{
    __try { return factory ? factory->createInstance(classId, interfaceId, object) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::int32 vst3ClassCountSeh(Steinberg::IPluginFactory* factory) noexcept
{
    __try { return factory ? factory->countClasses() : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool vst3ClassInfoSeh(Steinberg::IPluginFactory* factory, Steinberg::int32 index,
                      Steinberg::PClassInfo& info) noexcept
{
    __try { return factory && factory->getClassInfo(index, &info) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Steinberg::tresult initializeVst3Seh(Steinberg::Vst::IComponent* component,
                                      Steinberg::FUnknown* host) noexcept
{
    __try { return component ? component->initialize(host) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::tresult initializeVst3ControllerSeh(Steinberg::Vst::IEditController* controller,
                                                 Steinberg::FUnknown* host) noexcept
{
    __try { return controller ? controller->initialize(host) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::tresult setVst3ComponentHandlerSeh(
    Steinberg::Vst::IEditController* controller,
    Steinberg::Vst::IComponentHandler* handler) noexcept
{
    __try { return controller ? controller->setComponentHandler(handler) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::int32 vst3ParameterCountSeh(
    Steinberg::Vst::IEditController* controller) noexcept
{
    __try { return controller ? controller->getParameterCount() : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool vst3ParameterInfoSeh(Steinberg::Vst::IEditController* controller,
                          Steinberg::int32 index,
                          Steinberg::Vst::ParameterInfo& info) noexcept
{
    __try {
        return controller && controller->getParameterInfo(index, info) == Steinberg::kResultOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

double vst3ParameterValueSeh(Steinberg::Vst::IEditController* controller,
                             Steinberg::Vst::ParamID id) noexcept
{
    __try { return controller ? controller->getParamNormalized(id) : 0.0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0; }
}

bool vst3ParameterTextSeh(Steinberg::Vst::IEditController* controller,
                          Steinberg::Vst::ParamID id, double value,
                          Steinberg::Vst::String128 text) noexcept
{
    __try {
        return controller && controller->getParamStringByValue(id, value, text) ==
            Steinberg::kResultOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool setVst3ParameterSeh(Steinberg::Vst::IEditController* controller,
                         Steinberg::Vst::ParamID id, double value) noexcept
{
    __try {
        return controller && controller->setParamNormalized(id, value) == Steinberg::kResultOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool beginVst3HostEditSeh(Steinberg::Vst::IEditControllerHostEditing* editing,
                          Steinberg::Vst::ParamID id) noexcept
{
    __try { return !editing || editing->beginEditFromHost(id) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool endVst3HostEditSeh(Steinberg::Vst::IEditControllerHostEditing* editing,
                        Steinberg::Vst::ParamID id) noexcept
{
    __try { return !editing || editing->endEditFromHost(id) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Steinberg::tresult getVst3ControllerClassIdSeh(Steinberg::Vst::IComponent* component,
                                                Steinberg::TUID classId) noexcept
{
    __try { return component ? component->getControllerClassId(classId) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::tresult queryVst3Seh(Steinberg::FUnknown* object, const Steinberg::TUID iid,
                                 void** result) noexcept
{
    __try { return object ? object->queryInterface(iid, result) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::uint32 releaseVst3Seh(Steinberg::FUnknown* object) noexcept
{
    __try { return object ? object->release() : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

#if defined(VERACORE_ARA_SDK_AVAILABLE)
const ARA::ARAFactory* vst3AraFactorySeh(ARA::IPlugInEntryPoint* entryPoint) noexcept
{
    __try { return entryPoint ? entryPoint->getFactory() : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}


const ARA::ARAPlugInExtensionInstance* vst3AraBindSeh(
    ARA::IPlugInEntryPoint2* entryPoint,
    ARA::ARADocumentControllerRef documentController,
    ARA::ARAPlugInInstanceRoleFlags knownRoles,
    ARA::ARAPlugInInstanceRoleFlags assignedRoles) noexcept
{
    __try {
        return entryPoint ? entryPoint->bindToDocumentControllerWithRoles(
            documentController, knownRoles, assignedRoles) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
#endif

Steinberg::tresult terminateVst3Seh(Steinberg::Vst::IComponent* component) noexcept
{
    __try { return component ? component->terminate() : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::tresult terminateVst3ControllerSeh(Steinberg::Vst::IEditController* controller) noexcept
{
    __try { return controller ? controller->terminate() : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

bool connectVst3PointsSeh(Steinberg::Vst::IConnectionPoint* source,
                          Steinberg::Vst::IConnectionPoint* target) noexcept
{
    __try { return source && target && source->connect(target) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void disconnectVst3PointsSeh(Steinberg::Vst::IConnectionPoint* source,
                              Steinberg::Vst::IConnectionPoint* target) noexcept
{
    __try { if (source && target) source->disconnect(target); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

class Vst3StateStream final : public Steinberg::IBStream
{
public:
    explicit Vst3StateStream(const QByteArray& bytes = {}) : m_bytes(bytes) {}

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IBStream::iid)) {
            *object = static_cast<Steinberg::IBStream*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }
    Steinberg::uint32 PLUGIN_API release() override { return m_refs > 0 ? --m_refs : 0; }
    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes,
                                      Steinberg::int32* numBytesRead) override
    {
        if (numBytes < 0 || (!buffer && numBytes > 0)) return Steinberg::kInvalidArgument;
        const int available = m_position < m_bytes.size()
            ? static_cast<int>(m_bytes.size() - m_position)
            : 0;
        const int count = std::min(static_cast<int>(numBytes), available);
        if (count > 0) std::memcpy(buffer, m_bytes.constData() + m_position, count);
        m_position += count;
        if (numBytesRead) *numBytesRead = count;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes,
                                       Steinberg::int32* numBytesWritten) override
    {
        if (numBytes < 0 || (!buffer && numBytes > 0) || m_position > kMaxPluginStateBytes - numBytes) {
            m_valid = false;
            if (numBytesWritten) *numBytesWritten = 0;
            return Steinberg::kOutOfMemory;
        }
        if (m_position + numBytes > m_bytes.size()) m_bytes.resize(m_position + numBytes);
        if (numBytes > 0) std::memcpy(m_bytes.data() + m_position, buffer, numBytes);
        m_position += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                      Steinberg::int64* result) override
    {
        Steinberg::int64 base = 0;
        if (mode == kIBSeekCur) base = m_position;
        else if (mode == kIBSeekEnd) base = m_bytes.size();
        else if (mode != kIBSeekSet) return Steinberg::kInvalidArgument;
        const Steinberg::int64 next = base + pos;
        if (next < 0 || next > kMaxPluginStateBytes) return Steinberg::kInvalidArgument;
        m_position = static_cast<int>(next);
        if (result) *result = next;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override
    {
        if (!pos) return Steinberg::kInvalidArgument;
        *pos = m_position;
        return Steinberg::kResultOk;
    }
    const QByteArray& bytes() const { return m_bytes; }
    bool valid() const { return m_valid; }

private:
    std::atomic<Steinberg::uint32> m_refs{1};
    QByteArray m_bytes;
    int m_position = 0;
    bool m_valid = true;
};

Steinberg::tresult getVst3StateSeh(Steinberg::Vst::IComponent* component,
                                    Steinberg::IBStream* stream) noexcept
{
    __try { return component ? component->getState(stream) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::tresult setVst3StateSeh(Steinberg::Vst::IComponent* component,
                                    Steinberg::IBStream* stream) noexcept
{
    __try { return component ? component->setState(stream) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::tresult getVst3ControllerStateSeh(Steinberg::Vst::IEditController* controller,
                                              Steinberg::IBStream* stream) noexcept
{
    __try { return controller ? controller->getState(stream) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::tresult setVst3ControllerStateSeh(Steinberg::Vst::IEditController* controller,
                                              Steinberg::IBStream* stream) noexcept
{
    __try { return controller ? controller->setState(stream) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::tresult setVst3ComponentStateOnControllerSeh(
    Steinberg::Vst::IEditController* controller, Steinberg::IBStream* stream) noexcept
{
    __try { return controller ? controller->setComponentState(stream) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

bool getVst3MidiAssignmentSeh(Steinberg::Vst::IMidiMapping* mapping,
                              Steinberg::int32 bus, Steinberg::int16 channel,
                              Steinberg::Vst::CtrlNumber controller,
                              Steinberg::Vst::ParamID& parameter) noexcept
{
    __try {
        return mapping && mapping->getMidiControllerAssignment(
            bus, channel, controller, parameter) == Steinberg::kResultTrue;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Steinberg::int32 getVst3KeyswitchCountSeh(Steinberg::Vst::IKeyswitchController* controller,
                                           Steinberg::int32 bus,
                                           Steinberg::int16 channel) noexcept
{
    __try { return controller ? controller->getKeyswitchCount(bus, channel) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool getVst3KeyswitchInfoSeh(Steinberg::Vst::IKeyswitchController* controller,
                             Steinberg::int32 bus, Steinberg::int16 channel,
                             Steinberg::int32 index, Steinberg::Vst::KeyswitchInfo& info) noexcept
{
    __try {
        return controller && controller->getKeyswitchInfo(bus, channel, index, info) == Steinberg::kResultTrue;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

QString vst3String(const Steinberg::Vst::TChar* text, int capacity)
{
    if (!text || capacity <= 0) return {};
    int length = 0;
    while (length < capacity && text[length] != 0) ++length;
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(text), length).trimmed();
}

Steinberg::tresult setVst3IoModeSeh(Steinberg::Vst::IComponent* component,
                                     Steinberg::Vst::IoMode mode) noexcept
{
    __try { return component ? component->setIoMode(mode) : Steinberg::kResultFalse; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return Steinberg::kResultFalse; }
}

Steinberg::int32 vst3BusCountSeh(Steinberg::Vst::IComponent* component,
                                  Steinberg::Vst::MediaType type,
                                  Steinberg::Vst::BusDirection direction) noexcept
{
    __try { return component ? component->getBusCount(type, direction) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool vst3BusInfoSeh(Steinberg::Vst::IComponent* component, Steinberg::Vst::MediaType type,
                     Steinberg::Vst::BusDirection direction, Steinberg::int32 index,
                     Steinberg::Vst::BusInfo& info) noexcept
{
    __try { return component && component->getBusInfo(type, direction, index, info) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool vst3BusArrangementSeh(Steinberg::Vst::IAudioProcessor* processor,
                           Steinberg::Vst::BusDirection direction,
                           Steinberg::int32 index,
                           Steinberg::Vst::SpeakerArrangement& arrangement) noexcept
{
    __try {
        arrangement = 0;
        return processor && processor->getBusArrangement(direction, index, arrangement) ==
            Steinberg::kResultOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        arrangement = 0;
        return false;
    }
}

bool setVst3BusArrangementsSeh(
    Steinberg::Vst::IAudioProcessor* processor,
    std::vector<Steinberg::Vst::SpeakerArrangement>& inputs,
    std::vector<Steinberg::Vst::SpeakerArrangement>& outputs) noexcept
{
    __try {
        return processor && processor->setBusArrangements(
            inputs.empty() ? nullptr : inputs.data(),
            static_cast<Steinberg::int32>(inputs.size()),
            outputs.empty() ? nullptr : outputs.data(),
            static_cast<Steinberg::int32>(outputs.size())) == Steinberg::kResultOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool activateVst3BusSeh(Steinberg::Vst::IComponent* component, Steinberg::Vst::MediaType type,
                         Steinberg::Vst::BusDirection direction, Steinberg::int32 index,
                         bool state) noexcept
{
    __try { return component && component->activateBus(type, direction, index, state) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool setupVst3Seh(Steinberg::Vst::IAudioProcessor* processor,
                   Steinberg::Vst::ProcessSetup& setup) noexcept
{
    __try {
        return processor && processor->canProcessSampleSize(Steinberg::Vst::kSample32) == Steinberg::kResultOk &&
            processor->setupProcessing(setup) == Steinberg::kResultOk;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool setVst3ActiveSeh(Steinberg::Vst::IComponent* component, bool active) noexcept
{
    __try { return component && component->setActive(active) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool setVst3ProcessingSeh(Steinberg::Vst::IAudioProcessor* processor, bool processing) noexcept
{
    __try { return processor && processor->setProcessing(processing) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool processVst3Seh(Steinberg::Vst::IAudioProcessor* processor,
                     Steinberg::Vst::ProcessData& data) noexcept
{
    __try { return processor && processor->process(data) == Steinberg::kResultOk; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Steinberg::uint32 vst3TailSamplesSeh(Steinberg::Vst::IAudioProcessor* processor) noexcept
{
    __try { return processor ? processor->getTailSamples() : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

Steinberg::uint32 vst3LatencySamplesSeh(Steinberg::Vst::IAudioProcessor* processor) noexcept
{
    __try { return processor ? processor->getLatencySamples() : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

class Vst3HostAttributeList final : public Steinberg::Vst::IAttributeList
{
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IAttributeList::iid)) {
            *object = static_cast<Steinberg::Vst::IAttributeList*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }
    Steinberg::uint32 PLUGIN_API release() override
    {
        const Steinberg::uint32 previous = m_refs.fetch_sub(1);
        if (previous == 1) {
            delete this;
            return 0;
        }
        return previous > 0 ? previous - 1 : 0;
    }

    Steinberg::tresult PLUGIN_API setInt(AttrID id, Steinberg::int64 value) override
    {
        if (!id) return Steinberg::kInvalidArgument;
        m_values[std::string(id)] = value;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API getInt(AttrID id, Steinberg::int64& value) override
    {
        const Value* stored = find(id);
        const auto* integer = stored ? std::get_if<Steinberg::int64>(stored) : nullptr;
        if (!integer) return Steinberg::kResultFalse;
        value = *integer;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API setFloat(AttrID id, double value) override
    {
        if (!id) return Steinberg::kInvalidArgument;
        m_values[std::string(id)] = value;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API getFloat(AttrID id, double& value) override
    {
        const Value* stored = find(id);
        const auto* number = stored ? std::get_if<double>(stored) : nullptr;
        if (!number) return Steinberg::kResultFalse;
        value = *number;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API setString(AttrID id, const Steinberg::Vst::TChar* value) override
    {
        if (!id || !value) return Steinberg::kInvalidArgument;
        constexpr size_t kMaxChars = static_cast<size_t>(kMaxPluginStateBytes) / sizeof(Steinberg::Vst::TChar);
        size_t length = 0;
        while (length < kMaxChars && value[length] != 0) ++length;
        if (length == kMaxChars) return Steinberg::kInvalidArgument;
        m_values[std::string(id)] = Text(value, value + length + 1u);
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API getString(AttrID id, Steinberg::Vst::TChar* value,
                                             Steinberg::uint32 sizeInBytes) override
    {
        if (!value || sizeInBytes < sizeof(Steinberg::Vst::TChar)) return Steinberg::kInvalidArgument;
        const Value* stored = find(id);
        const auto* text = stored ? std::get_if<Text>(stored) : nullptr;
        if (!text || text->empty()) return Steinberg::kResultFalse;
        const size_t capacity = sizeInBytes / sizeof(Steinberg::Vst::TChar);
        const size_t count = std::min(capacity - 1u, text->size() - 1u);
        std::copy_n(text->data(), count, value);
        value[count] = 0;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API setBinary(AttrID id, const void* data,
                                             Steinberg::uint32 sizeInBytes) override
    {
        if (!id || (!data && sizeInBytes > 0) || sizeInBytes > kMaxPluginStateBytes)
            return Steinberg::kInvalidArgument;
        Binary bytes;
        if (sizeInBytes > 0) {
            const auto* first = static_cast<const uint8_t*>(data);
            bytes.assign(first, first + sizeInBytes);
        }
        m_values[std::string(id)] = std::move(bytes);
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API getBinary(AttrID id, const void*& data,
                                             Steinberg::uint32& sizeInBytes) override
    {
        const Value* stored = find(id);
        const auto* bytes = stored ? std::get_if<Binary>(stored) : nullptr;
        if (!bytes) {
            data = nullptr;
            sizeInBytes = 0;
            return Steinberg::kResultFalse;
        }
        data = bytes->empty() ? nullptr : bytes->data();
        sizeInBytes = static_cast<Steinberg::uint32>(bytes->size());
        return Steinberg::kResultTrue;
    }

private:
    using Text = std::vector<Steinberg::Vst::TChar>;
    using Binary = std::vector<uint8_t>;
    using Value = std::variant<Steinberg::int64, double, Text, Binary>;

    const Value* find(AttrID id) const
    {
        if (!id) return nullptr;
        const auto found = m_values.find(std::string(id));
        return found == m_values.end() ? nullptr : &found->second;
    }

    std::atomic<Steinberg::uint32> m_refs{1};
    std::map<std::string, Value> m_values;
};

class Vst3HostMessage final : public Steinberg::Vst::IMessage
{
public:
    ~Vst3HostMessage()
    {
        if (m_attributes) m_attributes->release();
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IMessage::iid)) {
            *object = static_cast<Steinberg::Vst::IMessage*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }
    Steinberg::uint32 PLUGIN_API release() override
    {
        const Steinberg::uint32 previous = m_refs.fetch_sub(1);
        if (previous == 1) {
            delete this;
            return 0;
        }
        return previous > 0 ? previous - 1 : 0;
    }

    Steinberg::FIDString PLUGIN_API getMessageID() override
    {
        return m_messageId.empty() ? nullptr : m_messageId.c_str();
    }

    void PLUGIN_API setMessageID(Steinberg::FIDString id) override
    {
        m_messageId = id ? id : "";
    }

    Steinberg::Vst::IAttributeList* PLUGIN_API getAttributes() override
    {
        if (!m_attributes) m_attributes = new (std::nothrow) Vst3HostAttributeList();
        return m_attributes;
    }

private:
    std::atomic<Steinberg::uint32> m_refs{1};
    std::string m_messageId;
    Vst3HostAttributeList* m_attributes = nullptr;
};

class Vst3HostApplication final : public Steinberg::Vst::IHostApplication,
                                  public Steinberg::Vst::IPlugInterfaceSupport
{
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid)) {
            *object = static_cast<Steinberg::Vst::IHostApplication*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IPlugInterfaceSupport::iid)) {
            *object = static_cast<Steinberg::Vst::IPlugInterfaceSupport*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }
    Steinberg::uint32 PLUGIN_API release() override { return m_refs > 0 ? --m_refs : 0; }

    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override
    {
        if (!name) return Steinberg::kInvalidArgument;
        std::fill_n(name, 128, Steinberg::Vst::TChar(0));
        constexpr char hostName[] = "VeraCore";
        for (size_t i = 0; i + 1 < sizeof(hostName); ++i) name[i] = hostName[i];
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID classId,
                                                  Steinberg::TUID interfaceId,
                                                  void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(classId, Steinberg::Vst::IMessage::iid) &&
            Steinberg::FUnknownPrivate::iidEqual(interfaceId, Steinberg::Vst::IMessage::iid)) {
            *object = new (std::nothrow) Vst3HostMessage();
            return *object ? Steinberg::kResultOk : Steinberg::kOutOfMemory;
        }
        if (Steinberg::FUnknownPrivate::iidEqual(classId, Steinberg::Vst::IAttributeList::iid) &&
            Steinberg::FUnknownPrivate::iidEqual(interfaceId, Steinberg::Vst::IAttributeList::iid)) {
            *object = new (std::nothrow) Vst3HostAttributeList();
            return *object ? Steinberg::kResultOk : Steinberg::kOutOfMemory;
        }
        return Steinberg::kNoInterface;
    }

    Steinberg::tresult PLUGIN_API isPlugInterfaceSupported(const Steinberg::TUID iid) override
    {
        return (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IMidiMapping::iid) ||
                Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IKeyswitchController::iid))
            ? Steinberg::kResultTrue
            : Steinberg::kResultFalse;
    }

private:
    std::atomic<Steinberg::uint32> m_refs{1};
};

class Vst3EventList final : public Steinberg::Vst::IEventList
{
public:
    void clear() { m_events.clear(); }
    void reserve(size_t count) { m_events.reserve(count); }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IEventList::iid)) {
            *object = static_cast<Steinberg::Vst::IEventList*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }
    Steinberg::uint32 PLUGIN_API release() override { return m_refs > 0 ? --m_refs : 0; }
    Steinberg::int32 PLUGIN_API getEventCount() override { return static_cast<Steinberg::int32>(m_events.size()); }
    Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, Steinberg::Vst::Event& event) override
    {
        if (index < 0 || static_cast<size_t>(index) >= m_events.size()) return Steinberg::kInvalidArgument;
        event = m_events[static_cast<size_t>(index)];
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& event) override
    {
        if (m_events.size() >= kMaxMidiEventsPerBlock) return Steinberg::kOutOfMemory;
        m_events.push_back(event);
        return Steinberg::kResultOk;
    }

private:
    std::atomic<Steinberg::uint32> m_refs{1};
    std::vector<Steinberg::Vst::Event> m_events;
};

class Vst3ParamValueQueue final : public Steinberg::Vst::IParamValueQueue
{
public:
    void reset(Steinberg::Vst::ParamID id)
    {
        m_id = id;
        m_pointCount = 0;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IParamValueQueue::iid)) {
            *object = static_cast<Steinberg::Vst::IParamValueQueue*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }
    Steinberg::uint32 PLUGIN_API release() override { return m_refs > 0 ? --m_refs : 0; }
    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return m_id; }
    Steinberg::int32 PLUGIN_API getPointCount() override { return static_cast<Steinberg::int32>(m_pointCount); }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& offset,
                                           Steinberg::Vst::ParamValue& value) override
    {
        if (index < 0 || static_cast<size_t>(index) >= m_pointCount) return Steinberg::kInvalidArgument;
        offset = m_points[static_cast<size_t>(index)].first;
        value = m_points[static_cast<size_t>(index)].second;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 offset, Steinberg::Vst::ParamValue value,
                                           Steinberg::int32& index) override
    {
        if (!std::isfinite(value)) {
            index = -1;
            return Steinberg::kInvalidArgument;
        }
        const Steinberg::int32 safeOffset = std::max<Steinberg::int32>(0, offset);
        const Steinberg::Vst::ParamValue safeValue = std::clamp(value, 0.0, 1.0);
        size_t position = 0;
        while (position < m_pointCount && m_points[position].first < safeOffset) ++position;
        if (position < m_pointCount && m_points[position].first == safeOffset) {
            m_points[position].second = safeValue; // Last source at this sample wins.
            index = static_cast<Steinberg::int32>(position);
            return Steinberg::kResultOk;
        }
        if (m_pointCount >= m_points.size()) {
            index = -1;
            return Steinberg::kOutOfMemory;
        }
        for (size_t move = m_pointCount; move > position; --move)
            m_points[move] = m_points[move - 1];
        m_points[position] = {safeOffset, safeValue};
        ++m_pointCount;
        index = static_cast<Steinberg::int32>(position);
        return Steinberg::kResultOk;
    }

private:
    std::atomic<Steinberg::uint32> m_refs{1};
    Steinberg::Vst::ParamID m_id = Steinberg::Vst::kNoParamId;
    size_t m_pointCount = 0;
    std::array<std::pair<Steinberg::int32, Steinberg::Vst::ParamValue>,
               kMaxVst3PointsPerQueue> m_points{};
};

class Vst3ParameterChanges final : public Steinberg::Vst::IParameterChanges
{
public:
    void clear() { m_queueCount = 0; }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IParameterChanges::iid)) {
            *object = static_cast<Steinberg::Vst::IParameterChanges*>(this);
            addRef();
            return Steinberg::kResultOk;
        }
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }
    Steinberg::uint32 PLUGIN_API release() override { return m_refs > 0 ? --m_refs : 0; }
    Steinberg::int32 PLUGIN_API getParameterCount() override { return static_cast<Steinberg::int32>(m_queueCount); }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override
    {
        return index >= 0 && static_cast<size_t>(index) < m_queueCount
            ? &m_queues[static_cast<size_t>(index)] : nullptr;
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id,
                                                                   Steinberg::int32& index) override
    {
        for (size_t i = 0; i < m_queueCount; ++i) {
            if (m_queues[i].getParameterId() == id) {
                index = static_cast<Steinberg::int32>(i);
                return &m_queues[i];
            }
        }
        if (m_queueCount >= m_queues.size()) {
            index = -1;
            return nullptr;
        }
        m_queues[m_queueCount].reset(id);
        index = static_cast<Steinberg::int32>(m_queueCount);
        return &m_queues[m_queueCount++];
    }

private:
    std::atomic<Steinberg::uint32> m_refs{1};
    size_t m_queueCount = 0;
    std::array<Vst3ParamValueQueue, kMaxVst3ParameterQueues> m_queues;
};

// VST3 UI callbacks arrive on the control thread. Parameter values cross to
// process() through a bounded SPSC queue; state-dirty/gesture metadata crosses
// back to the control timer through atomics. No allocation or lock is used by
// either the callback or audio-thread drain.
class Vst3ComponentHandler final : public Steinberg::Vst::IComponentHandler,
                                   public Steinberg::Vst::IComponentHandler2
{
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (!object) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid))
            *object = static_cast<Steinberg::Vst::IComponentHandler*>(this);
        else if (Steinberg::FUnknownPrivate::iidEqual(
                     iid, Steinberg::Vst::IComponentHandler2::iid))
            *object = static_cast<Steinberg::Vst::IComponentHandler2*>(this);
        else
            return Steinberg::kNoInterface;
        addRef();
        return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++m_refs; }
    Steinberg::uint32 PLUGIN_API release() override { return m_refs > 0 ? --m_refs : 0; }

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override
    {
        const bool hostOrigin = hostOriginOnCurrentThread();
        auto nativeCallback = m_parameterEdits.capture(!hostOrigin);
        const int previousDepth = m_editDepth.fetch_add(1, std::memory_order_acq_rel);
        uint64_t gestureSerial = m_gestureSerial.load(std::memory_order_acquire);
        bool outermost = false;
        if (!hostOrigin && id != Steinberg::Vst::kNoParamId) {
            const uint64_t freshSerial =
                m_gestureSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
            gestureSerial = m_nativeGestureSerials.begin(id, freshSerial, &outermost);
            if (gestureSerial == 0)
                m_parameterEdits.markOverflow(id, freshSerial);
        } else if (previousDepth == 0) {
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
        }
        if (id != Steinberg::Vst::kNoParamId && !hostOrigin &&
            outermost && gestureSerial != 0)
            m_parameterEdits.push({PluginParameterEditPhase::Begin, id, 0.0,
                gestureSerial});
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(
        Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override
    {
        if (id == Steinberg::Vst::kNoParamId || !std::isfinite(value))
            return Steinberg::kInvalidArgument;
        const bool hostOrigin = hostOriginOnCurrentThread();
        auto nativeCallback = m_parameterEdits.capture(!hostOrigin);
        uint64_t gestureSerial = 0;
        bool suppressed = false;
        bool trackerOverflowed = false;
        if (!hostOrigin) {
            gestureSerial = m_nativeGestureSerials.current(
                id, 0, &suppressed, &trackerOverflowed);
            if (trackerOverflowed)
                m_parameterEdits.markOverflow(
                    id, m_gestureSerial.load(std::memory_order_acquire));
            if (gestureSerial == 0 && !suppressed)
                gestureSerial = m_gestureSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
        } else if (m_editDepth.load(std::memory_order_acquire) == 0) {
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
        }
        const double normalized = std::clamp(value, 0.0, 1.0);
        const size_t write = m_write.load(std::memory_order_relaxed);
        const size_t next = (write + 1u) % m_events.size();
        if (next == m_read.load(std::memory_order_acquire)) return Steinberg::kOutOfMemory;
        m_events[write] = {id, normalized};
        m_write.store(next, std::memory_order_release);
        m_stateDirty.store(true, std::memory_order_release);
        if (!hostOrigin && !suppressed)
            m_parameterEdits.push({PluginParameterEditPhase::Value, id,
                normalized, gestureSerial});
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override
    {
        const bool hostOrigin = hostOriginOnCurrentThread();
        auto nativeCallback = m_parameterEdits.capture(!hostOrigin);
        bool terminal = false;
        bool trackerOverflowed = false;
        bool trackerConsumed = false;
        const uint64_t gestureSerial =
            id != Steinberg::Vst::kNoParamId && !hostOrigin
            ? m_nativeGestureSerials.end(
                id, m_gestureSerial.load(std::memory_order_acquire),
                &terminal, &trackerOverflowed, &trackerConsumed)
            : 0;
        if (trackerOverflowed)
            m_parameterEdits.markOverflow(
                id, m_gestureSerial.load(std::memory_order_acquire));
        if (hostOrigin || id == Steinberg::Vst::kNoParamId || trackerConsumed)
            decrementPositive(m_editDepth);
        m_stateDirty.store(true, std::memory_order_release);
        if (id != Steinberg::Vst::kNoParamId && !hostOrigin && terminal)
            m_parameterEdits.push({PluginParameterEditPhase::End, id, 0.0,
                gestureSerial});
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override
    {
        if (flags != 0) {
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
            m_stateDirty.store(true, std::memory_order_release);
        }
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API setDirty(Steinberg::TBool state) override
    {
        if (state) {
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
            m_stateDirty.store(true, std::memory_order_release);
        }
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API requestOpenEditor(Steinberg::FIDString) override
    {
        return Steinberg::kNotImplemented;
    }
    Steinberg::tresult PLUGIN_API startGroupEdit() override
    {
        if (m_groupDepth.fetch_add(1, std::memory_order_acq_rel) == 0 &&
            m_editDepth.load(std::memory_order_acquire) == 0)
            m_gestureSerial.fetch_add(1, std::memory_order_acq_rel);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API finishGroupEdit() override
    {
        decrementPositive(m_groupDepth);
        m_stateDirty.store(true, std::memory_order_release);
        return Steinberg::kResultOk;
    }

    void drainParameterChanges(Vst3ParameterChanges& destination) noexcept
    {
        size_t read = m_read.load(std::memory_order_relaxed);
        const size_t write = m_write.load(std::memory_order_acquire);
        while (read != write) {
            const ParameterEvent event = m_events[read];
            Steinberg::int32 queueIndex = -1;
            auto* queue = destination.addParameterData(event.id, queueIndex);
            Steinberg::int32 pointIndex = -1;
            if (!queue || queue->addPoint(0, event.value, pointIndex) != Steinberg::kResultOk)
                break;
            read = (read + 1u) % m_events.size();
        }
        m_read.store(read, std::memory_order_release);
    }
    bool consumeStateDirty(uint64_t& gestureSerial) noexcept
    {
        if (m_editDepth.load(std::memory_order_acquire) > 0 ||
            m_groupDepth.load(std::memory_order_acquire) > 0) return false;
        if (!m_stateDirty.exchange(false, std::memory_order_acq_rel)) return false;
        gestureSerial = m_gestureSerial.load(std::memory_order_acquire);
        return true;
    }
    bool editing() const noexcept
    {
        return m_editDepth.load(std::memory_order_acquire) > 0 ||
            m_groupDepth.load(std::memory_order_acquire) > 0;
    }
    void beginHostOrigin() noexcept
    {
        if (m_hostOriginDepth.fetch_add(1, std::memory_order_acq_rel) == 0)
            m_hostOriginThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    }
    void endHostOrigin() noexcept
    {
        const int previous = m_hostOriginDepth.fetch_sub(1, std::memory_order_acq_rel);
        if (previous <= 1) {
            m_hostOriginDepth.store(0, std::memory_order_release);
            m_hostOriginThreadId.store(0, std::memory_order_release);
        }
    }
    bool hostOriginOnCurrentThread() const noexcept
    {
        return m_hostOriginDepth.load(std::memory_order_acquire) > 0 &&
            m_hostOriginThreadId.load(std::memory_order_acquire) == GetCurrentThreadId();
    }
    bool popParameterEdit(NativePluginParameterEdit& event) noexcept
    {
        return m_parameterEdits.pop(event);
    }
    NativeParameterEditOverflow consumeParameterEditOverflow() noexcept
    {
        return m_parameterEdits.consumeOverflowDescriptor();
    }
    bool parameterEditsQuiescent() const noexcept
    {
        return m_parameterEdits.quiescent();
    }
    bool parameterEditsQuarantined() const noexcept
    {
        return m_parameterEdits.quarantined();
    }
    bool tryClearParameterEditQuarantine() noexcept
    {
        return m_parameterEdits.tryClearQuarantine([this]() noexcept {
            return m_nativeGestureSerials.activeDepth() == 0;
        });
    }
    int nativeEditDepth() noexcept
    {
        return m_nativeGestureSerials.activeDepth();
    }
    void reset() noexcept
    {
        m_read.store(0, std::memory_order_relaxed);
        m_write.store(0, std::memory_order_relaxed);
        m_editDepth.store(0, std::memory_order_relaxed);
        m_groupDepth.store(0, std::memory_order_relaxed);
        m_stateDirty.store(false, std::memory_order_relaxed);
        m_gestureSerial.store(0, std::memory_order_relaxed);
        m_hostOriginDepth.store(0, std::memory_order_relaxed);
        m_hostOriginThreadId.store(0, std::memory_order_relaxed);
        m_parameterEdits.reset();
        m_nativeGestureSerials.reset();
    }

private:
    struct ParameterEvent {
        Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;
        Steinberg::Vst::ParamValue value = 0.0;
    };
    std::atomic<Steinberg::uint32> m_refs{1};
    std::array<ParameterEvent, 1024> m_events{};
    std::atomic<size_t> m_read{0};
    std::atomic<size_t> m_write{0};
    std::atomic<int> m_editDepth{0};
    std::atomic<int> m_groupDepth{0};
    std::atomic<bool> m_stateDirty{false};
    std::atomic<uint64_t> m_gestureSerial{0};
    std::atomic<int> m_hostOriginDepth{0};
    std::atomic<DWORD> m_hostOriginThreadId{0};
    BoundedParameterEditQueue<kNativeParameterEditQueueCapacity> m_parameterEdits;
    NativeParameterGestureSerials m_nativeGestureSerials;
};

class Vst3Instance
{
public:
    ~Vst3Instance() { close(); }

    bool open(const QString& path, const QString& pluginUid, double sampleRate,
               const QByteArray& state = {}, bool offline = false,
               AraDocumentRegistry* araDocuments = nullptr,
               int araTrackIndex = -1, bool araEditorOwner = false,
			   VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo)
    {
        close();
        m_offline = offline;
        m_araTrackIndex = araTrackIndex;
        m_araEditorOwner = araEditorOwner;
		m_speakerLayout = speakerLayout;
        m_binaryPath = PluginLoader::ResolveVst3Binary(path);
        m_pluginUid = pluginUid.trimmed().toUpper();
        m_sampleRate = validSampleRate(sampleRate);
        Steinberg::TUID classId{};
        if (m_binaryPath.isEmpty()) return false;
        const bool hasStoredClassId = parseUid(m_pluginUid, classId);

        const std::wstring widePath = m_binaryPath.toStdWString();
        m_module = LoadLibraryExW(widePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_module) m_module = LoadLibraryExW(widePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m_module) return false;

        auto getFactory = reinterpret_cast<Vst3GetFactoryProc>(GetProcAddress(m_module, "GetPluginFactory"));
        m_initDll = reinterpret_cast<Vst3InitExitProc>(GetProcAddress(m_module, "InitDll"));
        m_exitDll = reinterpret_cast<Vst3InitExitProc>(GetProcAddress(m_module, "ExitDll"));
        if (!getFactory || !callVst3ModuleSeh(m_initDll)) { close(); return false; }
        m_moduleInitialized = m_initDll != nullptr;
        m_factory = getVst3FactorySeh(getFactory);
        if (!m_factory) { close(); return false; }

        if (!hasStoredClassId && !findSingleAudioEffectClassId(m_factory, classId)) {
            close();
            return false;
        }

        Steinberg::TUID componentIid{};
        Steinberg::Vst::IComponent::iid.toTUID(componentIid);
        Steinberg::tresult createResult = createVst3ComponentSeh(
            m_factory, classId, componentIid, reinterpret_cast<void**>(&m_component));
        if ((createResult != Steinberg::kResultOk || !m_component) && hasStoredClassId) {
            if (m_component) releaseVst3Seh(m_component);
            m_component = nullptr;
            Steinberg::TUID fallbackClassId{};
            if (findSingleAudioEffectClassId(m_factory, fallbackClassId) &&
                std::memcmp(classId, fallbackClassId, sizeof(Steinberg::TUID)) != 0) {
                createResult = createVst3ComponentSeh(
                    m_factory, fallbackClassId, componentIid,
                    reinterpret_cast<void**>(&m_component));
            }
        }
        if (createResult != Steinberg::kResultOk || !m_component) {
            close();
            return false;
        }
        Steinberg::TUID controllerClassId{};
        const bool hasControllerClass =
            getVst3ControllerClassIdSeh(m_component, controllerClassId) == Steinberg::kResultOk;
        setVst3IoModeSeh(m_component, Steinberg::Vst::kSimple);
        auto* hostContext = static_cast<Steinberg::Vst::IHostApplication*>(&m_host);
        if (initializeVst3Seh(m_component, hostContext) != Steinberg::kResultOk) { close(); return false; }
        m_componentInitialized = true;

        Steinberg::TUID controllerIid{};
        Steinberg::Vst::IEditController::iid.toTUID(controllerIid);
        bool separateController = false;
        if (hasControllerClass) {
            createVst3ComponentSeh(m_factory, controllerClassId, controllerIid,
                                   reinterpret_cast<void**>(&m_controller));
            separateController = m_controller != nullptr;
        }
        if (!m_controller) {
            queryVst3Seh(m_component, controllerIid, reinterpret_cast<void**>(&m_controller));
        }
        if (m_controller) {
            if (!separateController ||
                initializeVst3ControllerSeh(m_controller, hostContext) == Steinberg::kResultOk) {
                m_controllerInitialized = separateController;
                if (separateController) {
                    Steinberg::TUID connectionIid{};
                    Steinberg::Vst::IConnectionPoint::iid.toTUID(connectionIid);
                    queryVst3Seh(m_component, connectionIid,
                                 reinterpret_cast<void**>(&m_componentConnection));
                    queryVst3Seh(m_controller, connectionIid,
                                 reinterpret_cast<void**>(&m_controllerConnection));
                    connectVst3PointsSeh(m_componentConnection, m_controllerConnection);
                    connectVst3PointsSeh(m_controllerConnection, m_componentConnection);
                }
                Steinberg::TUID mappingIid{};
                Steinberg::Vst::IMidiMapping::iid.toTUID(mappingIid);
                queryVst3Seh(m_controller, mappingIid, reinterpret_cast<void**>(&m_midiMapping));
                Steinberg::TUID hostEditingIid{};
                Steinberg::Vst::IEditControllerHostEditing::iid.toTUID(hostEditingIid);
                queryVst3Seh(m_controller, hostEditingIid,
                             reinterpret_cast<void**>(&m_hostEditing));
                if (setVst3ComponentHandlerSeh(
                        m_controller,
                        static_cast<Steinberg::Vst::IComponentHandler*>(&m_componentHandler)) !=
                    Steinberg::kResultOk) {
                    close();
                    return false;
                }
                cacheMidiAssignments();
            } else {
                releaseVst3Seh(m_controller);
                m_controller = nullptr;
            }
        }
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        discoverAraInterfaces();
        bindAraDocument(araDocuments);
#endif
        if (!state.isEmpty()) restoreState(state);
        else syncControllerFromComponent();

        Steinberg::TUID processorIid{};
        Steinberg::Vst::IAudioProcessor::iid.toTUID(processorIid);
        if (queryVst3Seh(m_component, processorIid,
                         reinterpret_cast<void**>(&m_processor)) != Steinberg::kResultOk || !m_processor) {
            close();
            return false;
        }
		negotiateSpeakerArrangement(m_speakerLayout);
        scanBuses();
        if (m_mainOutputBus < 0) { close(); return false; }

        Steinberg::Vst::ProcessSetup setup{};
        setup.processMode = m_offline ? Steinberg::Vst::kOffline : Steinberg::Vst::kRealtime;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = kDefaultBlockSize;
        setup.sampleRate = m_sampleRate;
        if (!setupVst3Seh(m_processor, setup)) { close(); return false; }
        activateBuses();
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!assignAraRegions()) { close(); return false; }
#endif
        if (!setVst3ActiveSeh(m_component, true)) { close(); return false; }
        m_active = true;
        m_inputEvents.reserve(kMaxMidiEventsPerBlock);
        m_outputEvents.reserve(kMaxMidiEventsPerBlock);
        prepareBuses(kDefaultBlockSize);
        m_componentHandler.reset();
        if (!setVst3ProcessingSeh(m_processor, true)) { close(); return false; }
        m_processing = true;
        return true;
    }

    void close()
    {
        if (m_processing && m_processor) setVst3ProcessingSeh(m_processor, false);
        m_processing = false;
        if (m_active && m_component) setVst3ActiveSeh(m_component, false);
        m_active = false;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        clearAraRegionAssignments();
#endif
        if (m_componentConnection && m_controllerConnection) {
            disconnectVst3PointsSeh(m_controllerConnection, m_componentConnection);
            disconnectVst3PointsSeh(m_componentConnection, m_controllerConnection);
        }
        if (m_midiMapping) releaseVst3Seh(m_midiMapping);
        m_midiMapping = nullptr;
        if (m_hostEditing) releaseVst3Seh(m_hostEditing);
        m_hostEditing = nullptr;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        m_araExtensionInstance = nullptr;
        m_araBinding = {};
        if (m_araEntryPoint2) releaseVst3Seh(m_araEntryPoint2);
        m_araEntryPoint2 = nullptr;
        if (m_araEntryPoint) releaseVst3Seh(m_araEntryPoint);
        m_araEntryPoint = nullptr;
        m_araFactory = nullptr;
#endif
        if (m_controllerConnection) releaseVst3Seh(m_controllerConnection);
        m_controllerConnection = nullptr;
        if (m_componentConnection) releaseVst3Seh(m_componentConnection);
        m_componentConnection = nullptr;
        if (m_controller)
            setVst3ComponentHandlerSeh(m_controller, nullptr);
        if (m_controllerInitialized && m_controller) terminateVst3ControllerSeh(m_controller);
        m_controllerInitialized = false;
        if (m_controller) releaseVst3Seh(m_controller);
        m_controller = nullptr;
        if (m_componentInitialized && m_component) terminateVst3Seh(m_component);
        m_componentInitialized = false;
        if (m_processor) releaseVst3Seh(m_processor);
        m_processor = nullptr;
        if (m_component) releaseVst3Seh(m_component);
        m_component = nullptr;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        m_araDocument.reset();
#endif
        if (m_factory) releaseVst3Seh(m_factory);
        m_factory = nullptr;
        if (m_moduleInitialized && m_exitDll) callVst3ModuleSeh(m_exitDll);
        m_moduleInitialized = false;
        m_initDll = nullptr;
        m_exitDll = nullptr;
        if (m_module) FreeLibrary(m_module);
        m_module = nullptr;
        m_inputChannelCounts.clear();
        m_outputChannelCounts.clear();
        m_outputBusNames.clear();
        m_inputStorage.clear();
        m_outputStorage.clear();
        m_inputBuses.clear();
        m_outputBuses.clear();
        m_mainOutputBus = -1;
		m_speakerLayout = VeraCoreSpeakerLayout::Stereo;
        m_inputEvents.clear();
        m_outputEvents.clear();
        for (auto& channel : m_midiAssignments) channel.fill(Steinberg::Vst::kNoParamId);
        for (auto& channel : m_activeNotes) channel.fill(0);
        m_crashed = false;
        m_panicPending = false;
        m_offline = false;
        m_araTrackIndex = -1;
        m_araEditorOwner = false;
        m_araResumeActive = false;
        m_araResumeProcessing = false;
        m_araModelUpdateSuspended.store(false, std::memory_order_release);
        m_componentHandler.reset();
        m_continuousTimeSamples = 0;
        m_continuousTimeInitialized = false;
        m_expectedProjectTimeSamples = 0;
        m_expectedProjectTimeInitialized = false;
    }

    bool matches(const QString& path, const QString& pluginUid, double sampleRate, bool offline,
				 VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo) const
    {
        return m_component && !m_crashed && PluginLoader::ResolveVst3Binary(path) == m_binaryPath &&
            pluginUid.trimmed().compare(m_pluginUid, Qt::CaseInsensitive) == 0 &&
            std::fabs(m_sampleRate - validSampleRate(sampleRate)) < 0.5 &&
			m_offline == offline && m_speakerLayout == speakerLayout;
    }

    bool healthy() const { return m_component && !m_crashed; }

    PluginTechnologyCapabilities capabilities() const
    {
        auto hasInterface = [](Steinberg::FUnknown* object, const Steinberg::FUID& interfaceId) {
            if (!object) return false;
            Steinberg::TUID iid{};
            interfaceId.toTUID(iid);
            void* queried = nullptr;
            if (queryVst3Seh(object, iid, &queried) != Steinberg::kResultOk || !queried)
                return false;
            releaseVst3Seh(static_cast<Steinberg::FUnknown*>(queried));
            return true;
        };

        PluginTechnologyCapabilities result;
        result.format = "VST3";
        result.audioPorts = m_component != nullptr;
        result.notePorts = m_component &&
            vst3BusCountSeh(m_component, Steinberg::Vst::kEvent, Steinberg::Vst::kInput) > 0;
        result.parameters = m_controller != nullptr;
        result.persistentState = m_component != nullptr;
        result.latency = m_processor != nullptr;
        result.tail = m_processor != nullptr;
        result.offlineRender = m_processor != nullptr;
        result.noteExpression = hasInterface(
            m_controller, Steinberg::Vst::INoteExpressionController::iid);
        result.keyswitches = hasInterface(
            m_controller, Steinberg::Vst::IKeyswitchController::iid);
        result.noteNames = result.keyswitches;
        result.physicalUiMapping = hasInterface(
            m_controller, Steinberg::Vst::INoteExpressionPhysicalUIMapping::iid);
        result.unitPrograms = hasInterface(m_controller, Steinberg::Vst::IUnitInfo::iid) ||
            hasInterface(m_controller, Steinberg::Vst::IProgramListData::iid);
        result.processContextRequirements = hasInterface(
            m_processor, Steinberg::Vst::IProcessContextRequirements::iid);
        result.prefetchableSupport = hasInterface(
            m_component, Steinberg::Vst::IPrefetchableSupport::iid);
        result.araSdkCompiled = kAraSdkCompiled;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        result.araFactory = m_araFactory != nullptr && m_araEntryPoint2 != nullptr;
        result.araDocumentController = m_araBinding.documentController;
        result.araPlaybackRenderer = m_araBinding.playbackRenderer;
        result.araEditorRenderer = m_araBinding.editorRenderer;
        result.araEditorView = m_araBinding.editorView;
        result.araContentAccess = m_araBinding.contentAccess;
#endif
        return result;
    }

    std::vector<PluginAudioOutputBus> outputBuses() const
    {
        std::vector<PluginAudioOutputBus> buses;
        buses.reserve(m_outputChannelCounts.size());
        for (size_t index = 0; index < m_outputChannelCounts.size(); ++index) {
            const QString name = index < m_outputBusNames.size() && !m_outputBusNames[index].isEmpty()
                ? m_outputBusNames[index] : QString("Output %1").arg(index + 1);
            buses.push_back({QString("vst3-output-%1").arg(index), name,
                             static_cast<int>(index), m_outputChannelCounts[index],
                             static_cast<int>(index) == m_mainOutputBus});
        }
        return buses;
    }

    bool hasAraRenderer() const noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        return m_component && !m_crashed.load(std::memory_order_acquire) &&
            m_araDocument && m_araBinding.playbackRenderer;
#else
        return false;
#endif
    }

    void requestAraModelUpdate() noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (m_araDocument && m_araBinding.playbackRenderer)
            m_araModelUpdateSuspended.store(true, std::memory_order_release);
#endif
    }

    void cancelAraModelUpdate() noexcept
    {
        m_araModelUpdateSuspended.store(false, std::memory_order_release);
    }

    void beginAraModelUpdate() noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!m_araModelUpdateSuspended.load(std::memory_order_acquire)) return;
        m_araResumeProcessing = m_processing;
        m_araResumeActive = m_active;
        if (m_processing && m_processor) setVst3ProcessingSeh(m_processor, false);
        m_processing = false;
        if (m_active && m_component) setVst3ActiveSeh(m_component, false);
        m_active = false;
        clearAraRegionAssignments();
#endif
    }

    void endAraModelUpdate() noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!m_araModelUpdateSuspended.load(std::memory_order_acquire)) return;
        bool ready = assignAraRegions();
        if (ready && m_araResumeActive) {
            ready = setVst3ActiveSeh(m_component, true);
            m_active = ready;
        }
        if (ready && m_araResumeProcessing) {
            ready = setVst3ProcessingSeh(m_processor, true);
            m_processing = ready;
        }
        m_araResumeActive = false;
        m_araResumeProcessing = false;
        if (!ready) m_crashed.store(true, std::memory_order_release);
        m_araModelUpdateSuspended.store(false, std::memory_order_release);
#endif
    }

    void setAraEditorOwner(bool owner) noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (m_araEditorOwner == owner) return;
        if (owner) {
            size_t added = 0;
            for (const auto region : m_araAssignedRegions) {
                if (!addAraEditorRegionSeh(m_araExtensionInstance, region)) {
                    while (added > 0)
                        removeAraEditorRegionSeh(m_araExtensionInstance,
                            m_araAssignedRegions[--added]);
                    return;
                }
                ++added;
            }
        } else {
            for (auto it = m_araAssignedRegions.rbegin(); it != m_araAssignedRegions.rend(); ++it)
                removeAraEditorRegionSeh(m_araExtensionInstance, *it);
        }
        m_araEditorOwner = owner;
#else
        (void)owner;
#endif
    }

    void notifyAraEditorSelection(
        int selectedTrackIndex,
        const std::vector<std::string>& clipIds) noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!m_araEditorOwner) return;
        try {
            std::vector<ARA::ARAPlaybackRegionRef> regions;
            if (selectedTrackIndex == m_araTrackIndex && m_araDocument)
                regions = m_araDocument->playbackRegionRefsForPersistentIds(
                    m_araTrackIndex, clipIds);
            notifyAraSelectionSeh(m_araExtensionInstance, regions);
        } catch (...) {
            const std::vector<ARA::ARAPlaybackRegionRef> emptySelection;
            notifyAraSelectionSeh(m_araExtensionInstance, emptySelection);
        }
#else
        (void)selectedTrackIndex;
        (void)clipIds;
#endif
    }

    void requestPanic() { m_panicPending = true; }
    bool hasPendingPanic() const { return m_panicPending; }
    uint32_t tailSamples() const { return vst3TailSamplesSeh(m_processor); }
    uint32_t latencySamples() const { return vst3LatencySamplesSeh(m_processor); }
    bool consumeStateDirty(uint64_t& gestureSerial) noexcept
    {
        return m_componentHandler.consumeStateDirty(gestureSerial);
    }
    std::vector<PluginParameterInfo> parameters() const
    {
        std::vector<PluginParameterInfo> result;
        if (!m_controller || m_crashed) return result;
        const Steinberg::int32 count = std::clamp<Steinberg::int32>(
            vst3ParameterCountSeh(m_controller), 0, 65536);
        result.reserve(static_cast<size_t>(count));
        for (Steinberg::int32 index = 0; index < count; ++index) {
            Steinberg::Vst::ParameterInfo source{};
            if (!vst3ParameterInfoSeh(m_controller, index, source) ||
                source.id == Steinberg::Vst::kNoParamId) continue;
            const double value = std::clamp(
                vst3ParameterValueSeh(m_controller, source.id), 0.0, 1.0);
            Steinberg::Vst::String128 display{};
            vst3ParameterTextSeh(m_controller, source.id, value, display);
            PluginParameterInfo info;
            info.stableId = source.id;
            info.name = vst3String(source.title, 128).trimmed();
            if (info.name.isEmpty()) info.name = QString("Parameter %1").arg(index + 1);
            info.unit = vst3String(source.units, 128).trimmed();
            info.displayValue = vst3String(display, 128).trimmed();
            info.normalizedValue = value;
            info.defaultNormalizedValue = std::clamp(source.defaultNormalizedValue, 0.0, 1.0);
            info.stepCount = std::max(0, source.stepCount);
            info.automatable = (source.flags & Steinberg::Vst::ParameterInfo::kCanAutomate) != 0;
            info.readOnly = (source.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0;
            info.hidden = (source.flags & Steinberg::Vst::ParameterInfo::kIsHidden) != 0;
            info.bypass = (source.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0;
            result.push_back(std::move(info));
        }
        return result;
    }
    bool beginParameterGesture(uint32_t parameterId) noexcept
    {
        if (!m_controller || m_crashed) return false;
        m_componentHandler.beginHostOrigin();
        const bool editorBegan = beginVst3HostEditSeh(m_hostEditing, parameterId);
        const bool handled = editorBegan &&
            m_componentHandler.beginEdit(parameterId) == Steinberg::kResultOk;
        m_componentHandler.endHostOrigin();
        return handled;
    }
    bool setParameterNormalized(uint32_t parameterId, double value) noexcept
    {
        if (!m_controller || m_crashed || !std::isfinite(value)) return false;
        const double normalized = std::clamp(value, 0.0, 1.0);
        m_componentHandler.beginHostOrigin();
        const bool changed = setVst3ParameterSeh(m_controller, parameterId, normalized) &&
            m_componentHandler.performEdit(parameterId, normalized) == Steinberg::kResultOk;
        m_componentHandler.endHostOrigin();
        return changed;
    }
    bool endParameterGesture(uint32_t parameterId) noexcept
    {
        if (!m_controller) return false;
        m_componentHandler.beginHostOrigin();
        const bool handled = m_componentHandler.endEdit(parameterId) == Steinberg::kResultOk;
        const bool editorEnded = endVst3HostEditSeh(m_hostEditing, parameterId);
        m_componentHandler.endHostOrigin();
        return editorEnded && handled;
    }
    bool popParameterEdit(NativePluginParameterEdit& event) noexcept
    {
        return m_componentHandler.popParameterEdit(event);
    }
    NativeParameterEditOverflow consumeParameterEditOverflow() noexcept
    {
        return m_componentHandler.consumeParameterEditOverflow();
    }
    bool parameterEditsQuiescent() const noexcept
    {
        return m_componentHandler.parameterEditsQuiescent();
    }
    bool parameterEditsQuarantined() const noexcept
    {
        return m_componentHandler.parameterEditsQuarantined();
    }
    bool tryClearParameterEditQuarantine() noexcept
    {
        return m_componentHandler.tryClearParameterEditQuarantine();
    }
    int nativeEditDepth() noexcept
    {
        return m_componentHandler.nativeEditDepth();
    }
    std::vector<MidiExpressionMapEntry> expressionMap(int midiChannel) const
    {
        std::vector<MidiExpressionMapEntry> entries;
        if (!m_controller || m_crashed) return entries;
        Steinberg::TUID iid{};
        Steinberg::Vst::IKeyswitchController::iid.toTUID(iid);
        Steinberg::Vst::IKeyswitchController* keyswitches = nullptr;
        if (queryVst3Seh(m_controller, iid, reinterpret_cast<void**>(&keyswitches)) != Steinberg::kResultOk ||
            !keyswitches) {
            return entries;
        }

        const int channel = std::clamp(midiChannel, 0, 15);
        const Steinberg::int32 count = std::clamp<Steinberg::int32>(
            getVst3KeyswitchCountSeh(keyswitches, 0, static_cast<Steinberg::int16>(channel)), 0, 512);
        for (Steinberg::int32 index = 0; index < count; ++index) {
            Steinberg::Vst::KeyswitchInfo info{};
            if (!getVst3KeyswitchInfoSeh(keyswitches, 0, static_cast<Steinberg::int16>(channel), index, info)) {
                continue;
            }
            const QString name = vst3String(info.title, 128);
            const QString shortName = vst3String(info.shortTitle, 128);
            const int firstPitch = info.keyRemapped >= 0 && info.keyRemapped <= 127
                ? info.keyRemapped : std::clamp(info.keyswitchMin, 0, 127);
            const int lastPitch = info.keyRemapped >= 0 && info.keyRemapped <= 127
                ? info.keyRemapped : std::clamp(info.keyswitchMax, firstPitch, 127);
            for (int pitch = firstPitch; pitch <= lastPitch; ++pitch) {
                const QString display = name.isEmpty() ? QString("Articulation %1").arg(index + 1) : name;
                entries.push_back({pitch, channel, -1, display,
                                   shortName.isEmpty() ? display : shortName});
            }
        }
        releaseVst3Seh(keyswitches);
        return entries;
    }
    QByteArray captureState() const
    {
        if (!m_component || m_crashed) return {};
        Vst3StateStream componentStream;
        if (getVst3StateSeh(m_component, &componentStream) != Steinberg::kResultOk ||
            !componentStream.valid()) return {};
        QByteArray controllerState;
        if (m_controller) {
            Vst3StateStream controllerStream;
            if (getVst3ControllerStateSeh(m_controller, &controllerStream) == Steinberg::kResultOk &&
                controllerStream.valid()) controllerState = controllerStream.bytes();
        }
        const QByteArray& componentState = componentStream.bytes();
        if (componentState.size() + controllerState.size() + 14 > kMaxPluginStateBytes) return {};
        QByteArray state("VCV3S1", 6);
        const uint32_t componentSize = static_cast<uint32_t>(componentState.size());
        const uint32_t controllerSize = static_cast<uint32_t>(controllerState.size());
        state.append(reinterpret_cast<const char*>(&componentSize), sizeof(componentSize));
        state.append(reinterpret_cast<const char*>(&controllerSize), sizeof(controllerSize));
        state.append(componentState);
        state.append(controllerState);
        return state;
    }

    void process(float* mix, int frames, const std::vector<VeraCoreMidiEvent>& source,
                 int layerId, float gain, float pan, const PluginTransportState& transport,
                 const std::vector<PluginAutomationLane>* automation = nullptr,
                 bool effectMode = false,
                 const std::vector<PluginOutputRoute>* routes = nullptr,
                  VeraCorePluginOutputSink sink = nullptr, void* sinkContext = nullptr,
                  int effectChannelCount = 2)
    {
        if (m_araModelUpdateSuspended.load(std::memory_order_acquire)) return;
        if (!mix || frames <= 0 || !m_processor || !m_active || m_crashed) return;
        if (!m_processing) return;
        // setupProcessing() advertises this hard maximum. Storage is fully
        // built in open(); callback only clears verified fixed buffers.
        if (!clearBuses(frames)) return;
        updateProcessContext(transport);
        buildEvents(source, layerId, frames, automation, transport);
        if (m_mainOutputBus < 0 || m_mainOutputBus >= static_cast<int>(m_outputBuses.size()) ||
            m_outputBuses[static_cast<size_t>(m_mainOutputBus)].numChannels <= 0 ||
            !m_outputBuses[static_cast<size_t>(m_mainOutputBus)].channelBuffers32) return;
        if (effectMode) {
            if (m_inputStorage.empty() || m_inputStorage[0].channels.empty()) return;
            const int hostChannels = std::clamp(effectChannelCount, 1, kMaxPluginChannels);
            const int inputChannels = static_cast<int>(m_inputStorage[0].channels.size());
			const std::vector<int>& canonicalMap = m_inputStorage[0].canonicalMap;
            for (int frame = 0; frame < frames; ++frame) {
                const float* sourceFrame = mix + static_cast<size_t>(frame) * hostChannels;
                if (inputChannels == 1) {
                    const float right = hostChannels > 1 ? sourceFrame[1] : sourceFrame[0];
                    m_inputStorage[0].channels[0][frame] = finiteAudioSample(
                        0.5f * finiteAudioSample(sourceFrame[0]) + 0.5f * finiteAudioSample(right));
                } else {
					for (int channel = 0; channel < inputChannels; ++channel) {
						const int canonical = channel < static_cast<int>(canonicalMap.size())
							? canonicalMap[static_cast<size_t>(channel)] : channel;
                        m_inputStorage[0].channels[static_cast<size_t>(channel)][frame] =
							canonical >= 0 && canonical < hostChannels
								? finiteAudioSample(sourceFrame[canonical]) : 0.0f;
					}
                }
            }
            // Host populated this bus after prepareBuses marked zeroed inputs silent.
            m_inputBuses[0].silenceFlags = 0;
        }
        m_outputEvents.clear();

        Steinberg::Vst::ProcessData data;
        data.processMode = m_offline ? Steinberg::Vst::kOffline : Steinberg::Vst::kRealtime;
        data.symbolicSampleSize = Steinberg::Vst::kSample32;
        data.numSamples = frames;
        data.numInputs = static_cast<Steinberg::int32>(m_inputBuses.size());
        data.numOutputs = static_cast<Steinberg::int32>(m_outputBuses.size());
        data.inputs = m_inputBuses.empty() ? nullptr : m_inputBuses.data();
        data.outputs = m_outputBuses.empty() ? nullptr : m_outputBuses.data();
        data.inputParameterChanges = &m_inputParameters;
        data.outputParameterChanges = &m_outputParameters;
        data.inputEvents = &m_inputEvents;
        data.outputEvents = &m_outputEvents;
        data.processContext = &m_processContext;
        if (!processVst3Seh(m_processor, data)) { m_crashed = true; return; }
        if (transport.playing) {
            m_continuousTimeSamples = frames >
                std::numeric_limits<Steinberg::Vst::TSamples>::max() -
                    m_continuousTimeSamples
                ? std::numeric_limits<Steinberg::Vst::TSamples>::max()
                : m_continuousTimeSamples + frames;
        }
        const Steinberg::Vst::TSamples projectSamples = m_processContext.projectTimeSamples;
        m_expectedProjectTimeSamples = transport.playing && frames >
            std::numeric_limits<Steinberg::Vst::TSamples>::max() - projectSamples
            ? std::numeric_limits<Steinberg::Vst::TSamples>::max()
            : projectSamples + (transport.playing ? frames : 0);
        m_expectedProjectTimeInitialized = true;

        const size_t firstBus = effectMode ? static_cast<size_t>(m_mainOutputBus) : 0u;
        const size_t busEnd = effectMode ? firstBus + 1u : m_outputBuses.size();
        for (size_t bus = firstBus; bus < busEnd; ++bus) {
            Steinberg::Vst::AudioBusBuffers& output = m_outputBuses[bus];
            if (output.numChannels <= 0 || !output.channelBuffers32 || !output.channelBuffers32[0]) continue;
			if (effectMode && effectChannelCount > 2) {
				const int hostChannels = std::clamp(effectChannelCount, 1, kMaxPluginChannels);
				const int pluginChannels = output.numChannels;
				for (int frame = 0; frame < frames; ++frame) {
					float* destination = mix + static_cast<size_t>(frame) * hostChannels;
					if (pluginChannels == 1) {
						const float mono = finiteAudioSample(output.channelBuffers32[0][frame]);
						destination[0] = mono;
						if (hostChannels > 1) destination[1] = mono;
					} else {
						const std::vector<int>& canonicalMap =
							m_outputStorage[bus].canonicalMap;
						for (int channel = 0; channel < pluginChannels; ++channel) {
							const int canonical = channel < static_cast<int>(canonicalMap.size())
								? canonicalMap[static_cast<size_t>(channel)] : channel;
							if (canonical >= 0 && canonical < hostChannels)
								destination[canonical] = finiteAudioSample(
									output.channelBuffers32[channel][frame]);
						}
					}
				}
				break;
			}
            float* left = output.channelBuffers32[0];
            float* right = output.numChannels > 1 ? output.channelBuffers32[1] : left;
            BusStorage& storage = m_outputStorage[bus];
            const bool projected = output.numChannels > 2 ||
                storage.downmixCoefficients.requiresProjection(output.numChannels);
            if (projected) {
                downmixPlanarPluginBus(output.channelBuffers32, output.numChannels, frames,
                    storage.downmixLeft.data(), storage.downmixRight.data(),
                    &storage.downmixCoefficients);
                left = storage.downmixLeft.data();
                right = storage.downmixRight.data();
            }
            deliverPluginOutput(mix, frames, left, right,
                                output.numChannels == 1 && !projected,
                                layerId, static_cast<int>(bus),
                                static_cast<int>(bus) == m_mainOutputBus, gain, pan,
                                transport.panLawDb,
                                effectMode ? nullptr : routes,
                                effectMode ? nullptr : sink, sinkContext, effectMode);
            if (effectMode) break;
        }
    }

private:
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    void discoverAraInterfaces()
    {
        queryVst3Seh(m_component, ARA::IPlugInEntryPoint_iid,
                     reinterpret_cast<void**>(&m_araEntryPoint));
        queryVst3Seh(m_component, ARA::IPlugInEntryPoint2_iid,
                     reinterpret_cast<void**>(&m_araEntryPoint2));
        m_araFactory = vst3AraFactorySeh(m_araEntryPoint);
        if (!validAraFactory(m_araFactory)) m_araFactory = nullptr;
    }

    void bindAraDocument(AraDocumentRegistry* documents)
    {
        if (!documents || !m_araFactory || !m_araEntryPoint2) return;
        try {
            m_araDocument = documents->acquire(m_araFactory);
        } catch (...) {
            m_araDocument.reset();
            return;
        }
        if (!m_araDocument || !m_araDocument->ready()) {
            m_araDocument.reset();
            return;
        }
        m_araExtensionInstance = vst3AraBindSeh(
            m_araEntryPoint2, m_araDocument->documentControllerRef(),
            kAraRenderAndEditorRoles, kAraRenderAndEditorRoles);
        m_araBinding = validateAraBinding(
            m_araExtensionInstance, kAraRenderAndEditorRoles, m_araDocument);
    }

    bool assignAraRegions() noexcept
    {
        if (!m_araDocument || !m_araExtensionInstance || !m_araBinding.playbackRenderer ||
            m_araTrackIndex < 0) return true;
        try {
            const auto regions = m_araDocument->playbackRegionRefsForTrack(m_araTrackIndex);
            m_araAssignedRegions.reserve(regions.size());
            for (const auto region : regions) {
                if (!addAraRendererRegionSeh(m_araExtensionInstance, region, m_araEditorOwner)) {
                    clearAraRegionAssignments();
                    return false;
                }
                m_araAssignedRegions.push_back(region);
            }
        } catch (...) {
            clearAraRegionAssignments();
            return false;
        }
        return true;
    }

    void clearAraRegionAssignments() noexcept
    {
        for (auto it = m_araAssignedRegions.rbegin(); it != m_araAssignedRegions.rend(); ++it)
            removeAraRendererRegionSeh(m_araExtensionInstance, *it, m_araEditorOwner);
        m_araAssignedRegions.clear();
    }
#endif

    bool restoreState(const QByteArray& state)
    {
        if (!m_component || state.isEmpty() || state.size() > kMaxPluginStateBytes) return false;
        QByteArray componentState = state;
        QByteArray controllerState;
        if (state.startsWith("VCV3S1")) {
            if (state.size() < 14) return false;
            uint32_t componentSize = 0;
            uint32_t controllerSize = 0;
            std::memcpy(&componentSize, state.constData() + 6, sizeof(componentSize));
            std::memcpy(&controllerSize, state.constData() + 10, sizeof(controllerSize));
            const uint64_t payloadSize = static_cast<uint64_t>(componentSize) + controllerSize;
            if (payloadSize != static_cast<uint64_t>(state.size() - 14)) return false;
            componentState = state.mid(14, static_cast<int>(componentSize));
            controllerState = state.mid(14 + static_cast<int>(componentSize),
                                        static_cast<int>(controllerSize));
        }
        Vst3StateStream componentStream(componentState);
        if (setVst3StateSeh(m_component, &componentStream) != Steinberg::kResultOk) return false;
        if (m_controller) {
            Vst3StateStream syncStream(componentState);
            setVst3ComponentStateOnControllerSeh(m_controller, &syncStream);
            if (!controllerState.isEmpty()) {
                Vst3StateStream controllerStream(controllerState);
                setVst3ControllerStateSeh(m_controller, &controllerStream);
            }
        }
        return true;
    }

    void syncControllerFromComponent()
    {
        if (!m_component || !m_controller) return;
        Vst3StateStream state;
        if (getVst3StateSeh(m_component, &state) != Steinberg::kResultOk || !state.valid()) return;
        Vst3StateStream readback(state.bytes());
        setVst3ComponentStateOnControllerSeh(m_controller, &readback);
    }

    void cacheMidiAssignments()
    {
        for (auto& channel : m_midiAssignments) channel.fill(Steinberg::Vst::kNoParamId);
        if (!m_midiMapping) return;
        for (int channel = 0; channel < 16; ++channel) {
            for (int controller = 0; controller < kVst3MidiControllerSlots; ++controller) {
                Steinberg::Vst::ParamID parameter = Steinberg::Vst::kNoParamId;
                if (getVst3MidiAssignmentSeh(
                        m_midiMapping, 0, static_cast<Steinberg::int16>(channel),
                        static_cast<Steinberg::Vst::CtrlNumber>(controller), parameter)) {
                    m_midiAssignments[static_cast<size_t>(channel)][static_cast<size_t>(controller)] = parameter;
                }
            }
        }
    }

    struct BusStorage {
        std::vector<std::vector<float>> channels;
        std::vector<float*> pointers;
        std::vector<float> downmixLeft;
        std::vector<float> downmixRight;
        StereoDownmixCoefficients downmixCoefficients;
		std::vector<int> canonicalMap;
    };

    static double validSampleRate(double value)
    {
        return std::isfinite(value) && value >= 8000.0 && value <= 384000.0 ? value : 44100.0;
    }

    static bool parseUid(const QString& text, Steinberg::TUID uid)
    {
        if (text.size() != 32) return false;
        const QByteArray bytes = text.toLatin1();
        auto nibble = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            return -1;
        };
        for (int i = 0; i < 16; ++i) {
            const int high = nibble(bytes[i * 2]);
            const int low = nibble(bytes[i * 2 + 1]);
            if (high < 0 || low < 0) return false;
            uid[i] = static_cast<char>((high << 4) | low);
        }
        return true;
    }

    static bool findSingleAudioEffectClassId(Steinberg::IPluginFactory* factory,
                                             Steinberg::TUID classId)
    {
        if (!factory) return false;
        bool found = false;
        const Steinberg::int32 count = std::clamp<Steinberg::int32>(
            vst3ClassCountSeh(factory), 0, 4096);
        for (Steinberg::int32 index = 0; index < count; ++index) {
            Steinberg::PClassInfo info{};
            if (!vst3ClassInfoSeh(factory, index, info) ||
                std::strcmp(info.category, kVstAudioEffectClass) != 0) {
                continue;
            }
            if (found) return false;
            std::memcpy(classId, info.cid, sizeof(Steinberg::TUID));
            found = true;
        }
        return found;
    }

    void negotiateSpeakerArrangement(VeraCoreSpeakerLayout layout)
    {
        if (!m_component || !m_processor) return;
        const Steinberg::Vst::SpeakerArrangement requested =
            vst3ArrangementForLayout(layout);
        auto collect = [&](Steinberg::Vst::BusDirection direction,
                           std::vector<Steinberg::Vst::SpeakerArrangement>& arrangements,
                           int& mainBus) {
            const int count = std::max<int>(0,
                vst3BusCountSeh(m_component, Steinberg::Vst::kAudio, direction));
            arrangements.assign(static_cast<size_t>(count), Steinberg::Vst::SpeakerArr::kEmpty);
            for (int index = 0; index < count; ++index) {
                Steinberg::Vst::BusInfo info{};
                if (!vst3BusInfoSeh(m_component, Steinberg::Vst::kAudio,
                        direction, index, info)) continue;
                Steinberg::Vst::SpeakerArrangement current = 0;
                if (!vst3BusArrangementSeh(m_processor, direction, index, current)) {
                    if (info.channelCount == 1) current = Steinberg::Vst::SpeakerArr::kMono;
                    else if (info.channelCount == 2) current = Steinberg::Vst::SpeakerArr::kStereo;
                }
                arrangements[static_cast<size_t>(index)] = current;
                if (mainBus < 0 && info.busType == Steinberg::Vst::kMain &&
                    info.channelCount > 0) mainBus = index;
            }
        };
        std::vector<Steinberg::Vst::SpeakerArrangement> inputs;
        std::vector<Steinberg::Vst::SpeakerArrangement> outputs;
        int mainInput = -1;
        int mainOutput = -1;
        collect(Steinberg::Vst::kInput, inputs, mainInput);
        collect(Steinberg::Vst::kOutput, outputs, mainOutput);
        if (mainOutput < 0) return;
        outputs[static_cast<size_t>(mainOutput)] = requested;
        if (mainInput >= 0) inputs[static_cast<size_t>(mainInput)] = requested;
        setVst3BusArrangementsSeh(m_processor, inputs, outputs);
    }

    void scanBuses()
    {
        std::vector<StereoDownmixCoefficients> outputCoefficients;
        std::vector<std::vector<int>> inputCanonicalMaps;
        std::vector<std::vector<int>> outputCanonicalMaps;
        auto scan = [&](Steinberg::Vst::BusDirection direction, std::vector<int>& counts,
                        std::vector<std::vector<int>>& canonicalMaps) {
            const Steinberg::int32 count = std::max<Steinberg::int32>(0,
                vst3BusCountSeh(m_component, Steinberg::Vst::kAudio, direction));
            for (Steinberg::int32 index = 0; index < count; ++index) {
                Steinberg::Vst::BusInfo info{};
                const bool valid = vst3BusInfoSeh(
                    m_component, Steinberg::Vst::kAudio, direction, index, info);
				const int channelCount = valid
					? std::clamp<int>(info.channelCount, 0, kMaxPluginChannels) : 0;
                counts.push_back(channelCount);
				std::vector<int> canonicalMap(static_cast<size_t>(channelCount), -1);
				Steinberg::Vst::SpeakerArrangement arrangement = 0;
				if (valid && channelCount > 0 &&
					vst3BusArrangementSeh(m_processor, direction, index, arrangement))
					buildVst3CanonicalMap(arrangement, channelCount, canonicalMap);
				if (canonicalMap.size() != static_cast<size_t>(channelCount))
					canonicalMap.assign(static_cast<size_t>(channelCount), -1);
				for (int channel = 0; channel < channelCount; ++channel)
					if (canonicalMap[static_cast<size_t>(channel)] < 0)
						canonicalMap[static_cast<size_t>(channel)] = channel;
				canonicalMaps.push_back(std::move(canonicalMap));
                if (direction == Steinberg::Vst::kOutput) {
                    StereoDownmixCoefficients coefficients;
                    if (valid && info.channelCount > 0 &&
                        info.channelCount <= kMaxPluginChannels) {
                        if (arrangement != 0) {
                            buildVst3StereoDownmix(arrangement, info.channelCount, coefficients);
                        }
                    }
                    outputCoefficients.push_back(std::move(coefficients));
                    m_outputBusNames.push_back(valid ? vst3String(info.name, 128) : QString{});
                    if (valid && info.busType == Steinberg::Vst::kMain && info.channelCount > 0 &&
                        m_mainOutputBus < 0) m_mainOutputBus = index;
                }
            }
        };
        scan(Steinberg::Vst::kInput, m_inputChannelCounts, inputCanonicalMaps);
        scan(Steinberg::Vst::kOutput, m_outputChannelCounts, outputCanonicalMaps);
        for (int index = 0; index < static_cast<int>(m_outputChannelCounts.size()); ++index) {
            if (m_mainOutputBus < 0 && m_outputChannelCounts[index] > 0) {
                m_mainOutputBus = index;
                break;
            }
        }
        m_inputStorage.resize(m_inputChannelCounts.size());
        m_outputStorage.resize(m_outputChannelCounts.size());
		for (size_t bus = 0; bus < m_inputStorage.size() &&
			 bus < inputCanonicalMaps.size(); ++bus)
			m_inputStorage[bus].canonicalMap = std::move(inputCanonicalMaps[bus]);
        for (size_t bus = 0; bus < m_outputStorage.size() &&
             bus < outputCoefficients.size(); ++bus) {
            m_outputStorage[bus].downmixCoefficients = std::move(outputCoefficients[bus]);
			if (bus < outputCanonicalMaps.size())
				m_outputStorage[bus].canonicalMap = std::move(outputCanonicalMaps[bus]);
        }
        m_inputBuses.resize(m_inputChannelCounts.size());
        m_outputBuses.resize(m_outputChannelCounts.size());
    }

    void updateProcessContext(const PluginTransportState& transport)
    {
        const double safeBpm = std::isfinite(transport.bpm) && transport.bpm > 0.0
            ? transport.bpm : 120.0;
        const double safeBeat = std::isfinite(transport.projectBeat) && transport.projectBeat >= 0.0
            ? transport.projectBeat : 0.0;
        const double safeSeconds = boundedPluginTimelineSeconds(
            transport.projectSeconds, safeBeat * 60.0 / safeBpm);
        const double beatsPerBar = transportBeatsPerBar(transport);
        const double safeBarStart = std::isfinite(transport.barStartBeat) &&
            transport.barStartBeat >= 0.0 && transport.barStartBeat <= safeBeat + 1.0e-9
            ? transport.barStartBeat
            : std::floor(safeBeat / beatsPerBar) * beatsPerBar;
        const bool validCycle = transport.cycleActive &&
            std::isfinite(transport.cycleStartBeat) &&
            std::isfinite(transport.cycleEndBeat) &&
            transport.cycleStartBeat >= 0.0 &&
            transport.cycleEndBeat > transport.cycleStartBeat;
        m_processContext = {};
        m_processContext.state = Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
            Steinberg::Vst::ProcessContext::kBarPositionValid |
            Steinberg::Vst::ProcessContext::kTempoValid |
            Steinberg::Vst::ProcessContext::kTimeSigValid |
            Steinberg::Vst::ProcessContext::kContTimeValid;
        if (transport.playing) m_processContext.state |= Steinberg::Vst::ProcessContext::kPlaying;
        if (transport.recording) m_processContext.state |= Steinberg::Vst::ProcessContext::kRecording;
        if (validCycle) {
            m_processContext.state |= Steinberg::Vst::ProcessContext::kCycleValid |
                Steinberg::Vst::ProcessContext::kCycleActive;
        }
        m_processContext.sampleRate = m_sampleRate;
        const Steinberg::Vst::TSamples projectTimeSamples =
            static_cast<Steinberg::Vst::TSamples>(
                saturatedPluginTimelineSamples(safeSeconds, m_sampleRate));
        m_processContext.projectTimeSamples = projectTimeSamples;
        const auto differsByMoreThanOne = [](Steinberg::Vst::TSamples left,
                                             Steinberg::Vst::TSamples right) noexcept {
            return left > right ? left - right > 1 : right - left > 1;
        };
        const Steinberg::Vst::TSamples cycleStartSamples = validCycle
            ? static_cast<Steinberg::Vst::TSamples>(saturatedPluginTimelineSamples(
                transport.cycleStartSeconds, m_sampleRate)) : 0;
        const Steinberg::Vst::TSamples cycleEndSamples = validCycle
            ? static_cast<Steinberg::Vst::TSamples>(saturatedPluginTimelineSamples(
                transport.cycleEndSeconds, m_sampleRate)) : 0;
        const bool expectedCycleWrap = validCycle && transport.cycleWrappedAtStart &&
            m_expectedProjectTimeInitialized &&
            !differsByMoreThanOne(projectTimeSamples, cycleStartSamples) &&
            m_expectedProjectTimeSamples >= cycleEndSamples -
                std::min<Steinberg::Vst::TSamples>(1, cycleEndSamples);
        const bool rollingSeek = transport.playing && m_expectedProjectTimeInitialized &&
            differsByMoreThanOne(projectTimeSamples, m_expectedProjectTimeSamples) &&
            !expectedCycleWrap;
        if (!m_continuousTimeInitialized || !transport.playing || rollingSeek) {
            m_continuousTimeSamples = projectTimeSamples;
            m_continuousTimeInitialized = true;
        }
        m_processContext.continousTimeSamples = m_continuousTimeSamples;
        m_processContext.projectTimeMusic = safeBeat;
        m_processContext.barPositionMusic = safeBarStart;
        if (validCycle) {
            m_processContext.cycleStartMusic = transport.cycleStartBeat;
            m_processContext.cycleEndMusic = transport.cycleEndBeat;
        }
        m_processContext.tempo = safeBpm;
        m_processContext.timeSigNumerator = transport.numerator;
        m_processContext.timeSigDenominator = transport.denominator;
    }

    void activateBuses()
    {
        for (int index = 0; index < static_cast<int>(m_inputChannelCounts.size()); ++index)
            activateVst3BusSeh(m_component, Steinberg::Vst::kAudio, Steinberg::Vst::kInput, index,
                               m_inputChannelCounts[index] > 0);
        for (int index = 0; index < static_cast<int>(m_outputChannelCounts.size()); ++index)
            activateVst3BusSeh(m_component, Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, index,
                               m_outputChannelCounts[index] > 0);
        const int eventInputs = std::max<int>(0,
            vst3BusCountSeh(m_component, Steinberg::Vst::kEvent, Steinberg::Vst::kInput));
        for (int index = 0; index < eventInputs; ++index)
            activateVst3BusSeh(m_component, Steinberg::Vst::kEvent, Steinberg::Vst::kInput, index, true);
    }

    void prepareBuses(int frames)
    {
        auto prepare = [&](const std::vector<int>& counts, std::vector<BusStorage>& storage,
                           std::vector<Steinberg::Vst::AudioBusBuffers>& buses, bool silent) {
            for (size_t bus = 0; bus < counts.size(); ++bus) {
                storage[bus].channels.resize(static_cast<size_t>(counts[bus]));
                storage[bus].pointers.resize(static_cast<size_t>(counts[bus]));
                if (counts[bus] > 2 ||
                    storage[bus].downmixCoefficients.requiresProjection(counts[bus])) {
                    storage[bus].downmixLeft.resize(static_cast<size_t>(frames));
                    storage[bus].downmixRight.resize(static_cast<size_t>(frames));
                }
                for (int channel = 0; channel < counts[bus]; ++channel) {
                    auto& samples = storage[bus].channels[static_cast<size_t>(channel)];
                    if (static_cast<int>(samples.size()) < frames) samples.resize(frames);
                    std::fill_n(samples.begin(), frames, 0.0f);
                    storage[bus].pointers[static_cast<size_t>(channel)] = samples.data();
                }
                buses[bus].numChannels = counts[bus];
                buses[bus].channelBuffers32 = storage[bus].pointers.empty() ? nullptr : storage[bus].pointers.data();
                buses[bus].silenceFlags = silent && counts[bus] > 0
                    ? ((Steinberg::uint64(1) << counts[bus]) - 1) : 0;
            }
        };
        prepare(m_inputChannelCounts, m_inputStorage, m_inputBuses, true);
        prepare(m_outputChannelCounts, m_outputStorage, m_outputBuses, false);
    }

    bool clearBuses(int frames) noexcept
    {
        if (frames <= 0 || frames > kDefaultBlockSize) return false;
        auto clear = [frames](const std::vector<int>& counts,
                              std::vector<BusStorage>& storage,
                              std::vector<Steinberg::Vst::AudioBusBuffers>& buses,
                              bool silent) noexcept {
            if (storage.size() != counts.size() || buses.size() != counts.size()) return false;
            for (size_t busIndex = 0; busIndex < counts.size(); ++busIndex) {
                const int channelCount = counts[busIndex];
                BusStorage& bus = storage[busIndex];
                if (channelCount < 0 ||
                    bus.channels.size() != static_cast<size_t>(channelCount) ||
                    bus.pointers.size() != static_cast<size_t>(channelCount) ||
                    ((channelCount > 2 ||
                      bus.downmixCoefficients.requiresProjection(channelCount)) &&
                     (bus.downmixLeft.size() < static_cast<size_t>(frames) ||
                      bus.downmixRight.size() < static_cast<size_t>(frames))) ||
                    buses[busIndex].numChannels != channelCount ||
                    buses[busIndex].channelBuffers32 !=
                        (bus.pointers.empty() ? nullptr : bus.pointers.data())) return false;
                for (int channel = 0; channel < channelCount; ++channel) {
                    auto& samples = bus.channels[static_cast<size_t>(channel)];
                    if (samples.size() < static_cast<size_t>(frames) ||
                        bus.pointers[static_cast<size_t>(channel)] != samples.data()) return false;
                    std::fill_n(samples.begin(), frames, 0.0f);
                }
                buses[busIndex].silenceFlags = silent && channelCount > 0
                    ? ((Steinberg::uint64(1) << channelCount) - 1) : 0;
            }
            return true;
        };
        return clear(m_inputChannelCounts, m_inputStorage, m_inputBuses, true) &&
            clear(m_outputChannelCounts, m_outputStorage, m_outputBuses, false);
    }

    bool appendNote(int offset, uint8_t status, uint8_t pitch, uint8_t velocity)
    {
        const uint8_t type = status & 0xF0;
        const bool noteOn = type == 0x90 && velocity > 0;
        const bool noteOff = type == 0x80 || (type == 0x90 && velocity == 0);
        const bool polyPressure = type == 0xA0;
        if ((!noteOn && !noteOff && !polyPressure) || pitch > 127) return false;
        Steinberg::Vst::Event event{};
        event.busIndex = 0;
        event.sampleOffset = offset;
        event.ppqPosition = m_processContext.projectTimeMusic +
            static_cast<double>(offset) * m_processContext.tempo /
            (60.0 * m_processContext.sampleRate);
        event.flags = 0;
        event.type = noteOn ? Steinberg::Vst::Event::kNoteOnEvent
            : noteOff ? Steinberg::Vst::Event::kNoteOffEvent
            : Steinberg::Vst::Event::kPolyPressureEvent;
        if (noteOn) {
            event.noteOn.channel = status & 0x0F;
            event.noteOn.pitch = pitch;
            event.noteOn.tuning = 0.0f;
            event.noteOn.velocity = static_cast<float>(velocity) / 127.0f;
            event.noteOn.length = 0;
            event.noteOn.noteId = -1;
        } else if (noteOff) {
            event.noteOff.channel = status & 0x0F;
            event.noteOff.pitch = pitch;
            event.noteOff.velocity = static_cast<float>(velocity) / 127.0f;
            event.noteOff.noteId = -1;
            event.noteOff.tuning = 0.0f;
        } else {
            event.polyPressure.channel = status & 0x0F;
            event.polyPressure.pitch = pitch;
            event.polyPressure.pressure = static_cast<float>(velocity) / 127.0f;
            event.polyPressure.noteId = -1;
        }
        if (m_inputEvents.addEvent(event) != Steinberg::kResultOk) return false;
        int& activeCount = m_activeNotes[status & 0x0F][pitch];
        if (noteOn && activeCount < std::numeric_limits<int>::max()) {
            ++activeCount;
        } else if (noteOff && activeCount > 0) {
            --activeCount;
        }
        return true;
    }

    bool appendAllNotesOff(int offset, int channel)
    {
        const int safeChannel = std::clamp(channel, 0, 15);
        for (int pitch = 0; pitch < 128; ++pitch) {
            while (m_activeNotes[safeChannel][pitch] > 0) {
                if (!appendNote(offset, static_cast<uint8_t>(0x80 | safeChannel),
                                static_cast<uint8_t>(pitch), 0)) return false;
            }
        }
        return true;
    }

    void appendMappedController(int offset, uint8_t status, uint8_t data1, uint8_t data2)
    {
        const uint8_t type = status & 0xF0;
        Steinberg::Vst::CtrlNumber controller = 0;
        Steinberg::Vst::ParamValue value = 0.0;
        if (type == 0xB0) {
            controller = static_cast<Steinberg::Vst::CtrlNumber>(data1);
            value = static_cast<double>(data2) / 127.0;
        } else if (type == 0xD0) {
            controller = Steinberg::Vst::kAfterTouch;
            value = static_cast<double>(data1) / 127.0;
        } else if (type == 0xE0) {
            controller = Steinberg::Vst::kPitchBend;
            value = static_cast<double>((static_cast<int>(data2) << 7) | data1) / 16383.0;
        } else if (type == 0xC0) {
            controller = Steinberg::Vst::kCtrlProgramChange;
            value = static_cast<double>(data1) / 127.0;
        } else {
            return;
        }
        if (controller < 0 || controller >= kVst3MidiControllerSlots) return;
        const Steinberg::Vst::ParamID parameter =
            m_midiAssignments[status & 0x0F][static_cast<size_t>(controller)];
        if (parameter == Steinberg::Vst::kNoParamId) return;
        Steinberg::int32 queueIndex = -1;
        Steinberg::Vst::IParamValueQueue* queue =
            m_inputParameters.addParameterData(parameter, queueIndex);
        Steinberg::int32 pointIndex = -1;
        if (queue) queue->addPoint(offset, std::clamp(value, 0.0, 1.0), pointIndex);
    }

    void appendAutomation(const std::vector<PluginAutomationLane>* automation,
                          const PluginTransportState& transport, int frames)
    {
        if (!automation || m_componentHandler.editing()) return;
        const size_t laneLimit = kMaxVst3ParameterQueues > 4
            ? kMaxVst3ParameterQueues - 4 : 0;
        const size_t laneCount = std::min(automation->size(), laneLimit);
        size_t emitted = 0;
        size_t eligibleRemaining = 0;
        for (size_t laneIndex = 0; laneIndex < laneCount; ++laneIndex) {
            const PluginAutomationLane& lane = (*automation)[laneIndex];
            if (lane.enabled && !lane.points.empty() &&
                lane.parameterId != Steinberg::Vst::kNoParamId) ++eligibleRemaining;
        }
        for (size_t laneIndex = 0; laneIndex < laneCount &&
             emitted < kMaxAutomationEventsPerBlock; ++laneIndex) {
            const PluginAutomationLane& lane = (*automation)[laneIndex];
            if (!lane.enabled || lane.points.empty() ||
                lane.parameterId == Steinberg::Vst::kNoParamId) continue;
            Steinberg::int32 queueIndex = -1;
            Steinberg::Vst::IParamValueQueue* queue =
                m_inputParameters.addParameterData(lane.parameterId, queueIndex);
            if (!queue) break;
            const size_t remainingBudget = kMaxAutomationEventsPerBlock > emitted
                ? kMaxAutomationEventsPerBlock - emitted : 0;
            const int laneBudget = static_cast<int>(std::max<size_t>(
                1, remainingBudget / std::max<size_t>(1, eligibleRemaining)));
            --eligibleRemaining;
            emitPluginAutomationPoints(lane, transport, frames, laneBudget,
                [&](int offset, double value) {
                Steinberg::int32 pointIndex = -1;
                if (queue->addPoint(offset, value, pointIndex) == Steinberg::kResultOk)
                    ++emitted;
                });
        }
    }

    void buildEvents(const std::vector<VeraCoreMidiEvent>& source, int layerId, int frames,
                     const std::vector<PluginAutomationLane>* automation,
                     const PluginTransportState& transport)
    {
        m_inputEvents.clear();
        m_inputParameters.clear();
        m_outputParameters.clear();
        m_componentHandler.drainParameterChanges(m_inputParameters);
        appendAutomation(automation, transport, frames);
        if (m_panicPending) {
            bool complete = true;
            for (int channel = 0; channel < 16; ++channel) {
                if (!appendAllNotesOff(0, channel)) {
                    complete = false;
                    break;
                }
            }
            m_panicPending = !complete;
        }
        for (const VeraCoreMidiEvent& event : source) {
            if (event.externalPlugin && event.layerId == layerId) {
                const int offset = std::clamp(event.sampleOffset, 0, std::max(0, frames - 1));
                if ((event.status & 0xF0) == 0xB0 && (event.data1 == 120 || event.data1 == 123) &&
                    !appendAllNotesOff(offset, event.status & 0x0F)) {
                    m_panicPending = true;
                }
                appendNote(offset, event.status, event.data1, event.data2);
                appendMappedController(offset, event.status, event.data1, event.data2);
            }
        }
    }

    HMODULE m_module = nullptr;
    Vst3InitExitProc m_initDll = nullptr;
    Vst3InitExitProc m_exitDll = nullptr;
    Steinberg::IPluginFactory* m_factory = nullptr;
    Steinberg::Vst::IComponent* m_component = nullptr;
    Steinberg::Vst::IAudioProcessor* m_processor = nullptr;
    Steinberg::Vst::IEditController* m_controller = nullptr;
    Steinberg::Vst::IMidiMapping* m_midiMapping = nullptr;
    Steinberg::Vst::IEditControllerHostEditing* m_hostEditing = nullptr;
    Steinberg::Vst::IConnectionPoint* m_componentConnection = nullptr;
    Steinberg::Vst::IConnectionPoint* m_controllerConnection = nullptr;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    ARA::IPlugInEntryPoint* m_araEntryPoint = nullptr;
    ARA::IPlugInEntryPoint2* m_araEntryPoint2 = nullptr;
    const ARA::ARAPlugInExtensionInstance* m_araExtensionInstance = nullptr;
    const ARA::ARAFactory* m_araFactory = nullptr;
    std::shared_ptr<AraDocumentContext> m_araDocument;
    AraBindingStatus m_araBinding;
    std::vector<ARA::ARAPlaybackRegionRef> m_araAssignedRegions;
#endif
    Vst3HostApplication m_host;
    Vst3ComponentHandler m_componentHandler;
    Vst3EventList m_inputEvents;
    Vst3EventList m_outputEvents;
    Vst3ParameterChanges m_inputParameters;
    Vst3ParameterChanges m_outputParameters;
    Steinberg::Vst::ProcessContext m_processContext{};
    Steinberg::Vst::TSamples m_continuousTimeSamples = 0;
    bool m_continuousTimeInitialized = false;
    Steinberg::Vst::TSamples m_expectedProjectTimeSamples = 0;
    bool m_expectedProjectTimeInitialized = false;
    QString m_binaryPath;
    QString m_pluginUid;
    double m_sampleRate = 44100.0;
	VeraCoreSpeakerLayout m_speakerLayout = VeraCoreSpeakerLayout::Stereo;
    bool m_moduleInitialized = false;
    bool m_componentInitialized = false;
    bool m_controllerInitialized = false;
    bool m_active = false;
    bool m_processing = false;
    std::atomic<bool> m_crashed{false};
    std::atomic<bool> m_panicPending{false};
    std::atomic<bool> m_araModelUpdateSuspended{false};
    bool m_offline = false;
    bool m_araEditorOwner = false;
    bool m_araResumeActive = false;
    bool m_araResumeProcessing = false;
    int m_araTrackIndex = -1;
    int m_mainOutputBus = -1;
    std::array<std::array<int, 128>, 16> m_activeNotes{};
    std::array<std::array<Steinberg::Vst::ParamID, kVst3MidiControllerSlots>, 16> m_midiAssignments{};
    std::vector<int> m_inputChannelCounts;
    std::vector<int> m_outputChannelCounts;
    std::vector<QString> m_outputBusNames;
    std::vector<BusStorage> m_inputStorage;
    std::vector<BusStorage> m_outputStorage;
    std::vector<Steinberg::Vst::AudioBusBuffers> m_inputBuses;
    std::vector<Steinberg::Vst::AudioBusBuffers> m_outputBuses;
};

struct HostedLayer
{
    QString path;
    QString pluginType;
    QString pluginUid;
    double sampleRate = 44100.0;
    bool offline = false;
	VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo;
    float gain = 1.0f;
    float pan = 0.0f;
    bool enabled = true;
    uint32_t latencySamples = 0;
    uint32_t tailSamples = 0;
    uint64_t instanceEpoch = 0;
    uint32_t parameterEditQuarantinedId = std::numeric_limits<uint32_t>::max();
    uint32_t parameterEditQuarantinedSerial = 0;
    bool parameterEditStreamQuarantined = false;
    QByteArray configuredState;
    QByteArray state;
    PluginTechnologyCapabilities capabilities;
    std::vector<PluginAudioOutputBus> outputBuses;
    std::vector<PluginOutputRoute> outputRoutes;
    std::vector<PluginAutomationLane> automation;
    std::array<std::vector<MidiExpressionMapEntry>, 16> expressionMaps;
    std::shared_ptr<Vst2Instance> vst2;
    std::shared_ptr<Vst3Instance> vst3;
    std::shared_ptr<ClapInstance> clap;
};

struct HostedEffect
{
    QString path;
    QString pluginType;
    QString pluginUid;
    double sampleRate = 44100.0;
    bool offline = false;
	VeraCoreSpeakerLayout speakerLayout = VeraCoreSpeakerLayout::Stereo;
    bool enabled = true;
    uint32_t latencySamples = 0;
    uint32_t tailSamples = 0;
    uint64_t instanceEpoch = 0;
    uint32_t parameterEditQuarantinedId = std::numeric_limits<uint32_t>::max();
    uint32_t parameterEditQuarantinedSerial = 0;
    bool parameterEditStreamQuarantined = false;
    QByteArray configuredState;
    QByteArray state;
    PluginTechnologyCapabilities capabilities;
    std::vector<PluginAutomationLane> automation;
    std::shared_ptr<Vst2Instance> vst2;
    std::shared_ptr<Vst3Instance> vst3;
    std::shared_ptr<ClapInstance> clap;
};
} // namespace

struct ExternalPluginHost::Impl
{
    struct PdcDelayLine
    {
        std::vector<float> ring;
        std::vector<uint64_t> frameGenerations;
        size_t writeFrame = 0;
        uint32_t delayFrames = 0;
        uint64_t appliedResetGeneration = 0;
    };

    struct RenderScratch
    {
        std::vector<VeraCoreMidiEvent> trackEvents;
        std::vector<VeraCoreMidiEvent> chunkEvents;

        RenderScratch()
        {
            trackEvents.reserve(8192);
            chunkEvents.reserve(8192);
        }
    };

    struct PluginGraph
    {
        std::map<int, HostedLayer> layers;
        std::map<int, std::map<int, HostedEffect>> effectsByTrack;
        std::map<int, uint32_t> latencyByTrack;
        std::map<int, std::shared_ptr<PdcDelayLine>> pdcDelayLines;
        std::map<int, RenderScratch> trackScratch;
        RenderScratch globalScratch;
		// Fixed stereo adapter for legacy VST2 and stereo-only effects in a
		// multichannel chain. It is graph-owned, preallocated, and RT allocation-free.
		mutable std::array<float, static_cast<size_t>(kDefaultBlockSize) * 2u>
			multichannelStereoScratch{};
        uint32_t maximumLatency = 0;
    };

    mutable std::mutex mutex;
    mutable std::mutex transportWriteMutex;
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    AraDocumentRegistry araDocuments;
#endif
    int araSelectedTrackIndex = -1;
    std::vector<std::string> araSelectedClipIds;
    std::shared_ptr<PluginGraph> currentGraph;
    std::array<std::shared_ptr<PluginGraph>, 2> graphBindings;
    std::array<std::atomic<uint32_t>, 2> graphReaders{};
    std::array<PluginTransportState, 2> renderBatchTransports;
    std::atomic<int> activeGraphBinding{0};
    const std::vector<VeraCoreMidiEvent> noEvents;
    std::atomic<bool> requestedPlaying{false};
    std::atomic<bool> requestedRecording{false};
    std::atomic<double> requestedBpm{120.0};
    std::atomic<double> requestedProjectBeat{0.0};
    std::atomic<double> requestedProjectSeconds{0.0};
    std::atomic<double> requestedBarStartBeat{0.0};
    std::atomic<int> requestedBarNumber{0};
    std::atomic<bool> requestedCycleActive{false};
    std::atomic<double> requestedCycleStartBeat{0.0};
    std::atomic<double> requestedCycleEndBeat{0.0};
    std::atomic<double> requestedCycleStartSeconds{0.0};
    std::atomic<double> requestedCycleEndSeconds{0.0};
    std::atomic<double> requestedCycleStartBpm{120.0};
    std::atomic<double> requestedCycleStartBarStartBeat{0.0};
    std::atomic<int> requestedCycleStartBarNumber{0};
    std::atomic<int> requestedCycleStartNumerator{4};
    std::atomic<int> requestedCycleStartDenominator{4};
    std::atomic<double> requestedSampleRate{44100.0};
    std::atomic<double> requestedPanLawDb{-3.0};
    std::atomic<int> requestedNumerator{4};
    std::atomic<int> requestedDenominator{4};
    std::atomic<uint64_t> transportGeneration{0};
    std::atomic<uint64_t> pdcResetGeneration{0};
    bool offlineProcessing = false;
    uint64_t nextPluginInstanceEpoch = 1;

    AraDocumentRegistry* araRegistry() noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        return &araDocuments;
#else
        return nullptr;
#endif
    }

    Impl()
    {
        static_assert(std::atomic<double>::is_always_lock_free,
                      "ExternalPluginHost requires lock-free transport atomics.");
        currentGraph = std::make_shared<PluginGraph>();
        graphBindings[0] = currentGraph;
    }

    int acquireGraph() noexcept
    {
        const int index = activeGraphBinding.load(std::memory_order_seq_cst);
        graphReaders[static_cast<size_t>(index)].fetch_add(1, std::memory_order_seq_cst);
        if (index != activeGraphBinding.load(std::memory_order_seq_cst)) {
            graphReaders[static_cast<size_t>(index)].fetch_sub(1, std::memory_order_seq_cst);
            return -1;
        }
        return index;
    }

    void releaseGraph(int index) noexcept
    {
        if (index < 0 || index >= static_cast<int>(graphReaders.size())) return;
        graphReaders[static_cast<size_t>(index)].fetch_sub(1, std::memory_order_seq_cst);
    }

    void publishGraph(std::shared_ptr<PluginGraph> graph)
    {
        const int previousIndex = activeGraphBinding.load(std::memory_order_seq_cst);
        const int nextIndex = 1 - previousIndex;
        while (graphReaders[static_cast<size_t>(nextIndex)].load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
        graphBindings[static_cast<size_t>(nextIndex)] = graph;
        activeGraphBinding.store(nextIndex, std::memory_order_seq_cst);
        while (graphReaders[static_cast<size_t>(previousIndex)].load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
        graphBindings[static_cast<size_t>(previousIndex)].reset();
        currentGraph = std::move(graph);
    }

    struct AraInstances
    {
        std::vector<std::shared_ptr<ClapInstance>> clap;
        std::vector<std::shared_ptr<Vst3Instance>> vst3;
    };

    AraInstances araInstancesForModelUpdate(const std::shared_ptr<PluginGraph>& graph) const
    {
        AraInstances result;
        if (!graph) return result;
        std::set<const ClapInstance*> seenClap;
        std::set<const Vst3Instance*> seenVst3;
        const auto append = [&](const auto& hosted) {
            if (hosted.clap && hosted.clap->hasAraRenderer() &&
                seenClap.insert(hosted.clap.get()).second)
                result.clap.push_back(hosted.clap);
            if (hosted.vst3 && hosted.vst3->hasAraRenderer() &&
                seenVst3.insert(hosted.vst3.get()).second)
                result.vst3.push_back(hosted.vst3);
        };
        for (const auto& entry : graph->layers) append(entry.second);
        for (const auto& track : graph->effectsByTrack)
            for (const auto& entry : track.second) append(entry.second);
        return result;
    }

    static void requestAraModelUpdates(const AraInstances& instances) noexcept
    {
        for (const auto& instance : instances.clap) instance->requestAraModelUpdate();
        for (const auto& instance : instances.vst3) instance->requestAraModelUpdate();
    }

    bool waitForGraphReaders() noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (graphReaders[0].load(std::memory_order_seq_cst) != 0 ||
               graphReaders[1].load(std::memory_order_seq_cst) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }


    static void cancelAraModelUpdates(const AraInstances& instances) noexcept
    {
        for (const auto& instance : instances.clap) instance->cancelAraModelUpdate();
        for (const auto& instance : instances.vst3) instance->cancelAraModelUpdate();
    }

    static void beginAraModelUpdates(const AraInstances& instances) noexcept
    {
        for (const auto& instance : instances.clap) instance->beginAraModelUpdate();
        for (const auto& instance : instances.vst3) instance->beginAraModelUpdate();
    }

    static void endAraModelUpdates(const AraInstances& instances) noexcept
    {
        for (auto it = instances.vst3.rbegin(); it != instances.vst3.rend(); ++it)
            (*it)->endAraModelUpdate();
        for (auto it = instances.clap.rbegin(); it != instances.clap.rend(); ++it)
            (*it)->endAraModelUpdate();
    }

    void applyAraEditorSelection(const std::shared_ptr<PluginGraph>& graph) noexcept
    {
#if defined(VERACORE_ARA_SDK_AVAILABLE)
        if (!graph) return;
        const auto apply = [this](const auto& hosted) noexcept {
            if (hosted.clap)
                hosted.clap->notifyAraEditorSelection(
                    araSelectedTrackIndex, araSelectedClipIds);
            if (hosted.vst3)
                hosted.vst3->notifyAraEditorSelection(
                    araSelectedTrackIndex, araSelectedClipIds);
        };
        for (const auto& entry : graph->layers) apply(entry.second);
        for (const auto& track : graph->effectsByTrack)
            for (const auto& entry : track.second) apply(entry.second);
#else
        (void)graph;
#endif
    }

    PluginTransportState loadTransport() noexcept
    {
        thread_local PluginTransportState cached;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint64_t generation = transportGeneration.load(std::memory_order_acquire);
            if ((generation & 1u) != 0u) continue;
            PluginTransportState next;
            next.playing = requestedPlaying.load(std::memory_order_relaxed);
            next.recording = requestedRecording.load(std::memory_order_relaxed);
            next.bpm = requestedBpm.load(std::memory_order_relaxed);
            next.projectBeat = requestedProjectBeat.load(std::memory_order_relaxed);
            next.projectSeconds = requestedProjectSeconds.load(std::memory_order_relaxed);
            next.barStartBeat = requestedBarStartBeat.load(std::memory_order_relaxed);
            next.barNumber = requestedBarNumber.load(std::memory_order_relaxed);
            next.cycleActive = requestedCycleActive.load(std::memory_order_relaxed);
            next.cycleStartBeat = requestedCycleStartBeat.load(std::memory_order_relaxed);
            next.cycleEndBeat = requestedCycleEndBeat.load(std::memory_order_relaxed);
            next.cycleStartSeconds = requestedCycleStartSeconds.load(std::memory_order_relaxed);
            next.cycleEndSeconds = requestedCycleEndSeconds.load(std::memory_order_relaxed);
            next.cycleStartBpm = requestedCycleStartBpm.load(std::memory_order_relaxed);
            next.cycleStartBarStartBeat = requestedCycleStartBarStartBeat.load(std::memory_order_relaxed);
            next.cycleStartBarNumber = requestedCycleStartBarNumber.load(std::memory_order_relaxed);
            next.cycleStartNumerator = requestedCycleStartNumerator.load(std::memory_order_relaxed);
            next.cycleStartDenominator = requestedCycleStartDenominator.load(std::memory_order_relaxed);
            next.sampleRate = requestedSampleRate.load(std::memory_order_relaxed);
            next.panLawDb = requestedPanLawDb.load(std::memory_order_relaxed);
            next.numerator = requestedNumerator.load(std::memory_order_relaxed);
            next.denominator = requestedDenominator.load(std::memory_order_relaxed);
            if (generation != transportGeneration.load(std::memory_order_acquire)) continue;
            cached = next;
            return cached;
        }
        return cached;
    }

    uint32_t latencyCap(double sampleRate) const
    {
        const double rate = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 44100.0;
        return static_cast<uint32_t>(std::min<double>(
            std::numeric_limits<uint32_t>::max(), std::ceil(rate * kMaxPdcSeconds)));
    }

    void rebuildTrackLatencies(PluginGraph& graph, const PluginGraph& previous,
                               size_t trackCount, double sampleRate)
    {
        const uint32_t cap = latencyCap(sampleRate);
        std::map<int, uint32_t> updated;
        for (size_t index = 0; index < trackCount && index <= static_cast<size_t>(INT32_MAX); ++index)
            updated.emplace(static_cast<int>(index), 0);

        for (const auto& entry : graph.layers) {
            if (entry.first < 0 || !entry.second.enabled) continue;
            uint32_t& trackLatency = updated[entry.first / kLayerIdStride];
            trackLatency = std::max(trackLatency, std::min(entry.second.latencySamples, cap));
            for (const PluginOutputRoute& route : entry.second.outputRoutes) {
                if (!route.enabled || route.destinationTrackIndex < 0 ||
                    route.destinationTrackIndex >= static_cast<int>(trackCount)) continue;
                uint32_t& destinationLatency = updated[route.destinationTrackIndex];
                destinationLatency = std::max(
                    destinationLatency, std::min(entry.second.latencySamples, cap));
            }
        }
        for (const auto& track : graph.effectsByTrack) {
            if (track.first < 0) continue;
            uint64_t latency = updated[track.first];
            for (const auto& entry : track.second) {
                if (!entry.second.enabled) continue;
                latency = std::min<uint64_t>(cap,
                    latency + std::min(entry.second.latencySamples, cap));
            }
            updated[track.first] = static_cast<uint32_t>(latency);
        }
        uint32_t trackStageMaximumLatency = 0;
        for (const auto& entry : updated)
            trackStageMaximumLatency = std::max(trackStageMaximumLatency, entry.second);

        // Stage two models the real master topology: every aligned track feeds
        // both the direct path and Return A/B, then each return adds its own
        // serial insert latency. Compensate these endpoints independently so a
        // parallel dry/send signal remains phase aligned.
        std::array<uint32_t, kReturnBusCount> returnStageLatencies{};
        const uint32_t routeLatencyBudget = cap > trackStageMaximumLatency
            ? cap - trackStageMaximumLatency : 0;
        uint32_t routeStageMaximumLatency = 0;
        for (int returnBus = 0; returnBus < kReturnBusCount; ++returnBus) {
            const int returnPathId = VeraCoreReturnAudioFxTrackId(returnBus);
            uint64_t returnLatency = 0;
            const auto effects = graph.effectsByTrack.find(returnPathId);
            if (effects != graph.effectsByTrack.end()) {
                for (const auto& entry : effects->second) {
                    if (!entry.second.enabled) continue;
                    returnLatency = std::min<uint64_t>(routeLatencyBudget,
                        returnLatency + std::min(entry.second.latencySamples,
                            routeLatencyBudget));
                }
            }
            returnStageLatencies[static_cast<std::size_t>(returnBus)] =
                static_cast<uint32_t>(returnLatency);
            routeStageMaximumLatency = std::max(
                routeStageMaximumLatency, static_cast<uint32_t>(returnLatency));
        }

        const uint32_t preMasterLatency = std::min<uint32_t>(
            cap, trackStageMaximumLatency + routeStageMaximumLatency);
        uint64_t masterInsertLatency = 0;
        const auto masterEffects = graph.effectsByTrack.find(VeraCoreMasterAudioFxTrackId);
        if (masterEffects != graph.effectsByTrack.end()) {
            for (const auto& entry : masterEffects->second) {
                if (!entry.second.enabled) continue;
                masterInsertLatency = std::min<uint64_t>(
                    cap - preMasterLatency,
                    masterInsertLatency + std::min(entry.second.latencySamples,
                                                   cap - preMasterLatency));
            }
        }
        graph.latencyByTrack = updated;
        graph.maximumLatency = preMasterLatency + static_cast<uint32_t>(masterInsertLatency);
        graph.latencyByTrack[VeraCoreMasterPdcPathId] = preMasterLatency;
        graph.latencyByTrack[VeraCoreMasterAudioFxTrackId] = graph.maximumLatency;
        for (int returnBus = 0; returnBus < kReturnBusCount; ++returnBus) {
            graph.latencyByTrack[VeraCoreReturnAudioFxTrackId(returnBus)] =
                trackStageMaximumLatency +
                returnStageLatencies[static_cast<std::size_t>(returnBus)];
        }

        const auto addDelayLine = [&](int pathId, uint32_t delay) {
            if (delay == 0) return;
            const auto existing = previous.pdcDelayLines.find(pathId);
            if (existing != previous.pdcDelayLines.end() && existing->second &&
                existing->second->delayFrames == delay) {
                graph.pdcDelayLines.emplace(pathId, existing->second);
                return;
            }
            try {
                auto line = std::make_shared<PdcDelayLine>();
                line->ring.assign(static_cast<size_t>(delay) * 2u, 0.0f);
                line->frameGenerations.assign(static_cast<size_t>(delay), 0);
                line->delayFrames = delay;
                graph.pdcDelayLines.emplace(pathId, std::move(line));
            } catch (const std::bad_alloc&) {
            }
        };
        for (const auto& entry : updated) {
            const uint32_t delay = trackStageMaximumLatency > entry.second
                ? trackStageMaximumLatency - entry.second : 0;
            addDelayLine(entry.first, delay);
        }
        addDelayLine(VeraCoreMasterPdcPathId, routeStageMaximumLatency);
        for (int returnBus = 0; returnBus < kReturnBusCount; ++returnBus) {
            const uint32_t latency =
                returnStageLatencies[static_cast<std::size_t>(returnBus)];
            const uint32_t delay = routeStageMaximumLatency > latency
                ? routeStageMaximumLatency - latency : 0;
            addDelayLine(VeraCoreReturnAudioFxTrackId(returnBus), delay);
        }
    }

    void applyPdc(const PluginGraph& graph, int trackId, float* stereo, int frames) noexcept
    {
        if (!stereo || frames <= 0) return;
        const auto found = graph.pdcDelayLines.find(trackId);
        if (found == graph.pdcDelayLines.end() || !found->second) return;
        PdcDelayLine& line = *found->second;
        const uint64_t resetGeneration = pdcResetGeneration.load(std::memory_order_acquire);
        if (line.appliedResetGeneration != resetGeneration) {
            line.writeFrame = 0;
            line.appliedResetGeneration = resetGeneration;
        }
        for (int frame = 0; frame < frames; ++frame) {
            const size_t ringIndex = line.writeFrame * 2u;
            const size_t sampleIndex = static_cast<size_t>(frame) * 2u;
            const float inputLeft = stereo[sampleIndex];
            const float inputRight = stereo[sampleIndex + 1u];
            const bool valid = line.frameGenerations[line.writeFrame] == resetGeneration;
            stereo[sampleIndex] = valid ? line.ring[ringIndex] : 0.0f;
            stereo[sampleIndex + 1u] = valid ? line.ring[ringIndex + 1u] : 0.0f;
            line.ring[ringIndex] = inputLeft;
            line.ring[ringIndex + 1u] = inputRight;
            line.frameGenerations[line.writeFrame] = resetGeneration;
            if (++line.writeFrame >= line.delayFrames) line.writeFrame = 0;
        }
    }
};

ExternalPluginHost::ExternalPluginHost() : m_impl(std::make_unique<Impl>())
{
}
ExternalPluginHost::~ExternalPluginHost() = default;

void ExternalPluginHost::syncAraModelGraph(
    const std::vector<AraAudioSourceDescriptor>& sources,
    const std::vector<AraPlaybackRegionDescriptor>& regions,
    const std::vector<AraTempoEntryDescriptor>& tempoEntries,
    const std::vector<AraBarSignatureDescriptor>& barSignatures)
{
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    if (!m_impl) return;
    try {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Impl::AraInstances instances =
        m_impl->araInstancesForModelUpdate(m_impl->currentGraph);
    if (!instances.clap.empty() || !instances.vst3.empty()) {
        Impl::requestAraModelUpdates(instances);
        if (!m_impl->waitForGraphReaders()) {
            Impl::cancelAraModelUpdates(instances);
            return;
        }
        Impl::beginAraModelUpdates(instances);
    }
    try {
        m_impl->araDocuments.syncModelGraph(sources, regions, tempoEntries, barSignatures);
    } catch (...) {}
    if (!instances.clap.empty() || !instances.vst3.empty())
        Impl::endAraModelUpdates(instances);
    m_impl->applyAraEditorSelection(m_impl->currentGraph);
    } catch (...) {}
#else
    (void)sources;
    (void)regions;
    (void)tempoEntries;
    (void)barSignatures;
#endif
}

void ExternalPluginHost::setAraEditorSelection(
    int trackIndex, const std::vector<std::string>& clipIds)
{
    if (!m_impl) return;
    try {
        std::vector<std::string> stableIds;
        if (trackIndex >= 0) {
            stableIds.reserve(clipIds.size());
            for (const std::string& clipId : clipIds)
                if (!clipId.empty()) stableIds.push_back(clipId);
        }
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->araSelectedClipIds.swap(stableIds);
        m_impl->araSelectedTrackIndex =
            m_impl->araSelectedClipIds.empty() ? -1 : trackIndex;
        m_impl->applyAraEditorSelection(m_impl->currentGraph);
    } catch (...) {}
}

bool ExternalPluginHost::captureAraArchives(
    std::vector<AraDocumentArchive>& archives)
{
    archives.clear();
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    if (!m_impl) return false;
    try {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const Impl::AraInstances instances =
            m_impl->araInstancesForModelUpdate(m_impl->currentGraph);
        if (!instances.clap.empty() || !instances.vst3.empty()) {
            Impl::requestAraModelUpdates(instances);
            if (!m_impl->waitForGraphReaders()) {
                Impl::cancelAraModelUpdates(instances);
                return false;
            }
            Impl::beginAraModelUpdates(instances);
        }
        const bool captured = m_impl->araDocuments.captureArchives(archives);
        if (!instances.clap.empty() || !instances.vst3.empty())
            Impl::endAraModelUpdates(instances);
        m_impl->applyAraEditorSelection(m_impl->currentGraph);
        if (!captured) archives.clear();
        return captured;
    } catch (...) {
        archives.clear();
        return false;
    }
#else
    return true;
#endif
}

bool ExternalPluginHost::restoreAraArchives(
    const std::vector<AraDocumentArchive>& archives)
{
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    if (!m_impl) return false;
    if (archives.empty()) return true;
    try {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const Impl::AraInstances instances =
            m_impl->araInstancesForModelUpdate(m_impl->currentGraph);
        if (instances.clap.empty() && instances.vst3.empty()) return false;
        Impl::requestAraModelUpdates(instances);
        if (!m_impl->waitForGraphReaders()) {
            Impl::cancelAraModelUpdates(instances);
            return false;
        }
        Impl::beginAraModelUpdates(instances);
        const bool restored = m_impl->araDocuments.restoreArchives(archives);
        Impl::endAraModelUpdates(instances);
        m_impl->applyAraEditorSelection(m_impl->currentGraph);
        return restored;
    } catch (...) { return false; }
#else
    return archives.empty();
#endif
}

bool ExternalPluginHost::consumeAraTransportRequest(
    AraTransportRequest& request) noexcept
{
    request = {};
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    if (!m_impl) return false;
    try {
        std::unique_lock<std::mutex> lock(m_impl->mutex, std::try_to_lock);
        return lock.owns_lock() && m_impl->araDocuments.consumeTransportRequest(request);
    } catch (...) {
        request = {};
        return false;
    }
#else
    return false;
#endif
}

bool ExternalPluginHost::pollAraStatus(AraHostStatus& status) noexcept
{
    status = {};
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    if (!m_impl) return true;
    try {
        // Status fields are atomic and the registry uses its own try-lock.
        // Do not take the graph mutex: archive callbacks can report progress
        // while capture/restore owns that mutex on another control thread.
        return m_impl->araDocuments.pollStatus(status);
    } catch (...) {
        status = {};
        return false;
    }
#else
    return true;
#endif
}

bool ExternalPluginHost::selectedAraProcessingAlgorithms(
    AraProcessingAlgorithmSelection& selection)
{
    selection = {};
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    if (!m_impl) return false;
    try {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->araDocuments.processingAlgorithmSelection(
            m_impl->araSelectedClipIds, selection);
    } catch (...) {
        selection = {};
        return false;
    }
#else
    return false;
#endif
}

bool ExternalPluginHost::setSelectedAraProcessingAlgorithm(int algorithmIndex)
{
#if defined(VERACORE_ARA_SDK_AVAILABLE)
    if (!m_impl || algorithmIndex < 0) return false;
    try {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        AraProcessingAlgorithmSelection selection;
        if (!m_impl->araDocuments.processingAlgorithmSelection(
                m_impl->araSelectedClipIds, selection)) return false;
        return m_impl->araDocuments.requestProcessingAlgorithm(
            selection.factoryPersistentId, m_impl->araSelectedClipIds,
            algorithmIndex);
    } catch (...) { return false; }
#else
    (void)algorithmIndex;
    return false;
#endif
}

void ExternalPluginHost::setOfflineProcessing(bool offline)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->offlineProcessing == offline) return;
    m_impl->offlineProcessing = offline;
    m_impl->publishGraph(std::make_shared<Impl::PluginGraph>());
    m_impl->pdcResetGeneration.fetch_add(1, std::memory_order_release);
}

void ExternalPluginHost::setPanLawDb(double centerAttenuationDb) noexcept
{
    const double safeLaw = std::isfinite(centerAttenuationDb)
        ? std::clamp(centerAttenuationDb, -12.0, 0.0) : -3.0;
    std::lock_guard<std::mutex> lock(m_impl->transportWriteMutex);
    m_impl->transportGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_impl->requestedPanLawDb.store(safeLaw, std::memory_order_relaxed);
    m_impl->transportGeneration.fetch_add(1, std::memory_order_release);
}

void ExternalPluginHost::setTransport(bool playing, bool recording, double bpm, double projectBeat,
                                      int timeSignatureNumerator, int timeSignatureDenominator,
                                      double projectSeconds, double barStartBeat,
                                      int barNumber, bool cycleActive,
                                      double cycleStartBeat, double cycleEndBeat,
                                      double cycleStartSeconds, double cycleEndSeconds,
                                      double cycleStartBpm,
                                      double cycleStartBarStartBeat,
                                      int cycleStartBarNumber,
                                      int cycleStartNumerator,
                                      int cycleStartDenominator)
{
    std::lock_guard<std::mutex> lock(m_impl->transportWriteMutex);
    const bool wasPlaying = m_impl->requestedPlaying.load(std::memory_order_relaxed);
    const bool denominatorValid = timeSignatureDenominator >= 1 &&
        timeSignatureDenominator <= 64 &&
        (timeSignatureDenominator & (timeSignatureDenominator - 1)) == 0;
    const int denominator = denominatorValid ? timeSignatureDenominator : 4;
    m_impl->transportGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_impl->requestedPlaying.store(playing, std::memory_order_relaxed);
    m_impl->requestedRecording.store(recording, std::memory_order_relaxed);
    m_impl->requestedBpm.store(
        std::isfinite(bpm) && bpm > 0.0
            ? std::clamp(bpm, kMinPluginTempoBpm, kMaxPluginTempoBpm) : 120.0,
        std::memory_order_relaxed);
    m_impl->requestedProjectBeat.store(
        std::isfinite(projectBeat) && projectBeat >= 0.0 ? projectBeat : 0.0,
        std::memory_order_relaxed);
    const double safeBpm = std::isfinite(bpm) && bpm > 0.0
        ? std::clamp(bpm, kMinPluginTempoBpm, kMaxPluginTempoBpm) : 120.0;
    const double safeBeat = std::isfinite(projectBeat) && projectBeat >= 0.0
        ? projectBeat : 0.0;
    const double safeProjectSeconds = std::clamp(
        std::isfinite(projectSeconds) && projectSeconds >= 0.0
            ? projectSeconds : safeBeat * 60.0 / safeBpm,
        0.0, kMaxPluginTimelineSeconds);
    m_impl->requestedProjectSeconds.store(safeProjectSeconds, std::memory_order_relaxed);
    const double beatsPerBar = static_cast<double>(
        std::clamp(timeSignatureNumerator, 1, 32)) * 4.0 /
        static_cast<double>(denominator);
    const double safeBarStart = std::isfinite(barStartBeat) &&
        barStartBeat >= 0.0 && barStartBeat <= safeBeat + 1.0e-9
        ? barStartBeat : std::floor(safeBeat / beatsPerBar) * beatsPerBar;
    m_impl->requestedBarStartBeat.store(safeBarStart, std::memory_order_relaxed);
    const int safeBarNumber = barNumber >= 0
        ? std::clamp(barNumber, 0, INT32_MAX)
        : static_cast<int>(std::min<double>(
            INT32_MAX, std::floor(safeBeat / beatsPerBar)));
    m_impl->requestedBarNumber.store(safeBarNumber, std::memory_order_relaxed);
    const double safeCycleStartSeconds = std::clamp(
        std::isfinite(cycleStartSeconds) ? cycleStartSeconds : 0.0,
        0.0, kMaxPluginTimelineSeconds);
    const double safeCycleEndSeconds = std::clamp(
        std::isfinite(cycleEndSeconds) ? cycleEndSeconds : 0.0,
        0.0, kMaxPluginTimelineSeconds);
    const bool validCycle = cycleActive && std::isfinite(cycleStartBeat) &&
        std::isfinite(cycleEndBeat) && std::isfinite(cycleStartSeconds) &&
        std::isfinite(cycleEndSeconds) && cycleStartBeat >= 0.0 &&
        cycleEndBeat > cycleStartBeat && cycleStartSeconds >= 0.0 &&
        safeCycleEndSeconds > safeCycleStartSeconds;
    m_impl->requestedCycleActive.store(validCycle, std::memory_order_relaxed);
    m_impl->requestedCycleStartBeat.store(
        validCycle ? cycleStartBeat : 0.0, std::memory_order_relaxed);
    m_impl->requestedCycleEndBeat.store(
        validCycle ? cycleEndBeat : 0.0, std::memory_order_relaxed);
    m_impl->requestedCycleStartSeconds.store(
        validCycle ? safeCycleStartSeconds : 0.0, std::memory_order_relaxed);
    m_impl->requestedCycleEndSeconds.store(
        validCycle ? safeCycleEndSeconds : 0.0, std::memory_order_relaxed);
    const bool cycleDenominatorValid = cycleStartDenominator >= 1 &&
        cycleStartDenominator <= 64 &&
        (cycleStartDenominator & (cycleStartDenominator - 1)) == 0;
    const int safeCycleNumerator = std::clamp(cycleStartNumerator, 1, 32);
    const int safeCycleDenominator = cycleDenominatorValid ? cycleStartDenominator : 4;
    const double safeCycleBpm = std::isfinite(cycleStartBpm) && cycleStartBpm > 0.0
        ? std::clamp(cycleStartBpm, kMinPluginTempoBpm, kMaxPluginTempoBpm) : safeBpm;
    const double cycleBeatsPerBar = static_cast<double>(safeCycleNumerator) * 4.0 /
        static_cast<double>(safeCycleDenominator);
    const double safeCycleBarStart = std::isfinite(cycleStartBarStartBeat) &&
        cycleStartBarStartBeat >= 0.0 && cycleStartBarStartBeat <= cycleStartBeat + 1.0e-9
        ? cycleStartBarStartBeat
        : std::floor(std::max(0.0, cycleStartBeat) / cycleBeatsPerBar) * cycleBeatsPerBar;
    m_impl->requestedCycleStartBpm.store(
        validCycle ? safeCycleBpm : safeBpm, std::memory_order_relaxed);
    m_impl->requestedCycleStartBarStartBeat.store(
        validCycle ? safeCycleBarStart : safeBarStart, std::memory_order_relaxed);
    m_impl->requestedCycleStartBarNumber.store(
        validCycle ? std::clamp(cycleStartBarNumber, 0, INT32_MAX) : safeBarNumber,
        std::memory_order_relaxed);
    m_impl->requestedCycleStartNumerator.store(
        validCycle ? safeCycleNumerator : std::clamp(timeSignatureNumerator, 1, 32),
        std::memory_order_relaxed);
    m_impl->requestedCycleStartDenominator.store(
        validCycle ? safeCycleDenominator : denominator, std::memory_order_relaxed);
    m_impl->requestedNumerator.store(std::clamp(timeSignatureNumerator, 1, 32),
                                     std::memory_order_relaxed);
    m_impl->requestedDenominator.store(denominator, std::memory_order_relaxed);
    m_impl->transportGeneration.fetch_add(1, std::memory_order_release);
    if (wasPlaying != playing)
        m_impl->pdcResetGeneration.fetch_add(1, std::memory_order_release);
}

void ExternalPluginHost::syncLayers(
    const std::vector<std::shared_ptr<Track>>& tracks,
    const std::array<ReturnBusState, kReturnBusCount>& returns,
    const MasterBusState& master,
    double sampleRate,
	VeraCoreSpeakerLayout speakerLayout)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const double safeSampleRate = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
        ? sampleRate : 44100.0;
	const VeraCoreSpeakerLayout requestedSpeakerLayout =
		VeraCoreSpeakerChannelCount(speakerLayout) >= 2 ? speakerLayout : VeraCoreSpeakerLayout::Stereo;
    {
        std::lock_guard<std::mutex> transportLock(m_impl->transportWriteMutex);
        m_impl->transportGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_impl->requestedSampleRate.store(safeSampleRate, std::memory_order_relaxed);
        m_impl->transportGeneration.fetch_add(1, std::memory_order_release);
    }

    const std::shared_ptr<Impl::PluginGraph> previous = m_impl->currentGraph;
    auto next = std::make_shared<Impl::PluginGraph>();
    for (size_t index = 0; index < tracks.size() && index <= static_cast<size_t>(INT32_MAX); ++index)
        next->trackScratch.try_emplace(static_cast<int>(index));
    const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const std::shared_ptr<Track>& track) {
        return track && track->IsSoloed();
    });
    std::set<int> araEditorTracks;
    for (int trackIndex = 0; trackIndex < static_cast<int>(tracks.size()); ++trackIndex) {
        const Track* track = tracks[trackIndex].get();
        const MidiTrack* midiTrack = dynamic_cast<const MidiTrack*>(track);
        if (!track) {
            continue;
        }
        const auto& fxSlots = track->GetAudioFxSlots();
        for (int slotIndex = 0; slotIndex < static_cast<int>(fxSlots.size()); ++slotIndex) {
            const AudioFxSlot& slot = fxSlots[slotIndex];
            const bool isVst2 = slot.pluginType.compare("VST2", Qt::CaseInsensitive) == 0;
            const bool isVst3 = slot.pluginType.compare("VST3", Qt::CaseInsensitive) == 0;
            const bool isClap = slot.pluginType.compare("CLAP", Qt::CaseInsensitive) == 0;
            if ((!isVst2 && !isVst3 && !isClap) || slot.pluginPath.trimmed().isEmpty()) continue;

            const HostedEffect* existing = nullptr;
            const auto trackEffects = previous->effectsByTrack.find(trackIndex);
            if (trackEffects != previous->effectsByTrack.end()) {
                auto found = trackEffects->second.find(slotIndex);
                if (found != trackEffects->second.end()) existing = &found->second;
            }
            if (existing && existing->clap) existing->clap->serviceControlThreadRequests();
            const bool samePlugin = existing &&
                existing->pluginType.compare(slot.pluginType, Qt::CaseInsensitive) == 0 &&
                existing->path == slot.pluginPath &&
                existing->pluginUid.trimmed().compare(slot.pluginUid.trimmed(), Qt::CaseInsensitive) == 0 &&
                std::fabs(existing->sampleRate - safeSampleRate) < 0.5 &&
                existing->offline == m_impl->offlineProcessing &&
				existing->speakerLayout == VeraCoreSpeakerLayout::Stereo &&
                existing->configuredState == slot.pluginState;
            const bool reusePlugin = samePlugin &&
                ((isVst2 && existing->vst2 && existing->vst2->healthy()) ||
                 (isVst3 && existing->vst3 && existing->vst3->healthy()) ||
                 (isClap && existing->clap && existing->clap->reusable()));

            HostedEffect hosted;
            if (reusePlugin) {
                hosted = *existing;
                if (hosted.clap) {
                    hosted.capabilities = hosted.clap->capabilities();
                    if (hosted.clap->consumeMetricsDirty()) {
                        hosted.latencySamples = hosted.clap->latencySamples();
                        hosted.tailSamples = hosted.clap->tailSamples();
                    }
                }
            } else if (isVst2) {
                auto instance = std::make_shared<Vst2Instance>();
                if (!instance->open(slot.pluginPath, safeSampleRate, slot.pluginState,
                                    m_impl->offlineProcessing)) continue;
                hosted.vst2 = std::move(instance);
            } else if (isVst3) {
                auto instance = std::make_shared<Vst3Instance>();
                if (!instance->open(slot.pluginPath, slot.pluginUid, safeSampleRate, slot.pluginState,
                                    m_impl->offlineProcessing, m_impl->araRegistry(), trackIndex, false,
									VeraCoreSpeakerLayout::Stereo)) continue;
                hosted.vst3 = std::move(instance);
            } else {
                auto instance = std::make_shared<ClapInstance>();
                if (!instance->open(slot.pluginPath, slot.pluginUid, safeSampleRate, slot.pluginState,
                                    m_impl->offlineProcessing, m_impl->araRegistry(), trackIndex, false,
									VeraCoreSpeakerLayout::Stereo)) continue;
                hosted.clap = std::move(instance);
            }
            hosted.path = slot.pluginPath;
            hosted.pluginType = slot.pluginType;
            hosted.pluginUid = slot.pluginUid;
            hosted.sampleRate = safeSampleRate;
            hosted.offline = m_impl->offlineProcessing;
			hosted.speakerLayout = VeraCoreSpeakerLayout::Stereo;
            if (!reusePlugin) hosted.instanceEpoch = m_impl->nextPluginInstanceEpoch++;
            hosted.configuredState = slot.pluginState;
            hosted.enabled = slot.enabled && !track->IsMuted() && (!anySolo || track->IsSoloed());
            hosted.automation = slot.automation;
            if (!reusePlugin) {
                if (hosted.vst2) {
                    hosted.capabilities = hosted.vst2->capabilities();
                    hosted.latencySamples = hosted.vst2->latencySamples();
                    hosted.tailSamples = hosted.vst2->tailSamples();
                    hosted.state = hosted.vst2->captureState();
                } else if (hosted.vst3) {
                    hosted.capabilities = hosted.vst3->capabilities();
                    hosted.latencySamples = hosted.vst3->latencySamples();
                    hosted.tailSamples = hosted.vst3->tailSamples();
                    hosted.state = hosted.vst3->captureState();
                } else if (hosted.clap) {
                    hosted.capabilities = hosted.clap->capabilities();
                    hosted.latencySamples = hosted.clap->latencySamples();
                    hosted.tailSamples = hosted.clap->tailSamples();
                    hosted.state = hosted.clap->captureState();
                }
                if (hosted.state.isEmpty()) hosted.state = slot.pluginState;
            }
            const bool editorOwner = hosted.capabilities.araEditorRenderer &&
                araEditorTracks.insert(trackIndex).second;
            if (hosted.vst3) hosted.vst3->setAraEditorOwner(editorOwner);
            if (hosted.clap) hosted.clap->setAraEditorOwner(editorOwner);
            next->effectsByTrack[trackIndex].emplace(slotIndex, std::move(hosted));
        }
        if (!midiTrack) continue;
        const auto& deviceSlots = midiTrack->GetLayerSlots();
        for (int slotIndex = 0; slotIndex < static_cast<int>(deviceSlots.size()); ++slotIndex) {
            const LayerSlot& slot = deviceSlots[slotIndex];
            const bool isVst2 = slot.pluginType.compare("VST2", Qt::CaseInsensitive) == 0;
            const bool isVst3 = slot.pluginType.compare("VST3", Qt::CaseInsensitive) == 0;
            const bool isClap = slot.pluginType.compare("CLAP", Qt::CaseInsensitive) == 0;
            if ((!isVst2 && !isVst3 && !isClap) || slot.pluginPath.trimmed().isEmpty()) {
                continue;
            }
            const int layerId = trackIndex * kLayerIdStride + slotIndex;
            const auto found = previous->layers.find(layerId);
            const HostedLayer* existing = found != previous->layers.end() ? &found->second : nullptr;
            if (existing && existing->clap) existing->clap->serviceControlThreadRequests();
            const bool samePlugin = existing &&
                existing->pluginType.compare(slot.pluginType, Qt::CaseInsensitive) == 0 &&
                existing->path == slot.pluginPath &&
                existing->pluginUid.trimmed().compare(slot.pluginUid.trimmed(), Qt::CaseInsensitive) == 0 &&
                std::fabs(existing->sampleRate - safeSampleRate) < 0.5 &&
                existing->offline == m_impl->offlineProcessing &&
				existing->speakerLayout == VeraCoreSpeakerLayout::Stereo &&
                existing->configuredState == slot.pluginState;
            const bool reusePlugin = samePlugin &&
                ((isVst2 && existing->vst2 && existing->vst2->healthy()) ||
                 (isVst3 && existing->vst3 && existing->vst3->healthy()) ||
                 (isClap && existing->clap && existing->clap->reusable()));

            HostedLayer hosted;
            if (reusePlugin) {
                hosted = *existing;
                if (hosted.clap) {
                    hosted.capabilities = hosted.clap->capabilities();
                    if (hosted.clap->consumeMetricsDirty()) {
                        hosted.latencySamples = hosted.clap->latencySamples();
                        hosted.tailSamples = hosted.clap->tailSamples();
                    }
                }
            } else if (isVst2) {
                auto instance = std::make_shared<Vst2Instance>();
                if (!instance->open(slot.pluginPath, safeSampleRate, slot.pluginState,
                                    m_impl->offlineProcessing)) {
                    continue;
                }
                hosted.vst2 = std::move(instance);
            } else if (isVst3) {
                auto instance = std::make_shared<Vst3Instance>();
                if (!instance->open(slot.pluginPath, slot.pluginUid, safeSampleRate, slot.pluginState,
                                    m_impl->offlineProcessing, m_impl->araRegistry(), trackIndex, false,
									VeraCoreSpeakerLayout::Stereo)) {
                    continue;
                }
                hosted.vst3 = std::move(instance);
            } else {
                auto instance = std::make_shared<ClapInstance>();
                if (!instance->open(slot.pluginPath, slot.pluginUid, safeSampleRate, slot.pluginState,
                                    m_impl->offlineProcessing, m_impl->araRegistry(), trackIndex, false,
									VeraCoreSpeakerLayout::Stereo)) {
                    continue;
                }
                hosted.clap = std::move(instance);
            }
            hosted.path = slot.pluginPath;
            hosted.pluginType = slot.pluginType;
            hosted.pluginUid = slot.pluginUid;
            hosted.sampleRate = safeSampleRate;
            hosted.offline = m_impl->offlineProcessing;
			hosted.speakerLayout = VeraCoreSpeakerLayout::Stereo;
            if (!reusePlugin) hosted.instanceEpoch = m_impl->nextPluginInstanceEpoch++;
            hosted.configuredState = slot.pluginState;
            hosted.enabled = slot.enabled && !track->IsMuted() && (!anySolo || track->IsSoloed());
            hosted.gain = slot.gain;
            hosted.pan = slot.pan;
            hosted.outputRoutes = slot.outputRoutes;
            hosted.automation = slot.automation;
            if (!reusePlugin) {
                if (hosted.vst2) {
                    hosted.capabilities = hosted.vst2->capabilities();
                    hosted.latencySamples = hosted.vst2->latencySamples();
                    hosted.tailSamples = hosted.vst2->tailSamples();
                    hosted.state = hosted.vst2->captureState();
                } else if (hosted.vst3) {
                    hosted.capabilities = hosted.vst3->capabilities();
                    hosted.latencySamples = hosted.vst3->latencySamples();
                    hosted.tailSamples = hosted.vst3->tailSamples();
                    hosted.state = hosted.vst3->captureState();
                } else if (hosted.clap) {
                    hosted.capabilities = hosted.clap->capabilities();
                    hosted.latencySamples = hosted.clap->latencySamples();
                    hosted.tailSamples = hosted.clap->tailSamples();
                    hosted.state = hosted.clap->captureState();
                }
                if (hosted.state.isEmpty()) hosted.state = slot.pluginState;
                if (hosted.vst2) hosted.outputBuses = hosted.vst2->outputBuses();
                else if (hosted.vst3) hosted.outputBuses = hosted.vst3->outputBuses();
                else if (hosted.clap) hosted.outputBuses = hosted.clap->outputBuses();
                for (int channel = 0; channel < 16; ++channel) {
                    auto& entries = hosted.expressionMaps[static_cast<size_t>(channel)];
                    // Only VST3 IKeyswitchController is authoritative here.
                    // VST2 MIDI key names and CLAP note names remain pitch labels,
                    // never implicit articulation events.
                    if (hosted.vst3 && hosted.capabilities.keyswitches)
                        entries = hosted.vst3->expressionMap(channel);
                    for (MidiExpressionMapEntry& entry : entries) entry.layerId = layerId;
                    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                        if (a.pitch != b.pitch) return a.pitch < b.pitch;
                        if (a.channel != b.channel) return a.channel < b.channel;
                        return a.name < b.name;
                    });
                    entries.erase(std::unique(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                        return a.pitch == b.pitch && a.channel == b.channel && a.name == b.name;
                    }), entries.end());
                }
            }
            for (PluginOutputRoute& route : hosted.outputRoutes) {
                const auto bus = std::find_if(
                    hosted.outputBuses.begin(), hosted.outputBuses.end(),
                    [&](const PluginAudioOutputBus& candidate) {
                        return !route.stablePortId.isEmpty() &&
                            candidate.stablePortId == route.stablePortId;
                    });
                if (bus != hosted.outputBuses.end()) {
                    route.outputBusIndex = bus->busIndex;
                    route.displayName = bus->name;
                } else if (route.outputBusIndex < 0 ||
                           route.outputBusIndex >= static_cast<int>(hosted.outputBuses.size())) {
                    route.enabled = false;
                }
            }
            const bool editorOwner = hosted.capabilities.araEditorRenderer &&
                araEditorTracks.insert(trackIndex).second;
            if (hosted.vst3) hosted.vst3->setAraEditorOwner(editorOwner);
            if (hosted.clap) hosted.clap->setAraEditorOwner(editorOwner);
            next->layers.emplace(layerId, std::move(hosted));
        }
    }
    for (int returnBus = 0; returnBus < kReturnBusCount; ++returnBus) {
        const int effectTrackId = VeraCoreReturnAudioFxTrackId(returnBus);
        const auto& fxSlots = returns[static_cast<std::size_t>(returnBus)].audioFxSlots;
        for (int slotIndex = 0; slotIndex < static_cast<int>(fxSlots.size()); ++slotIndex) {
            const AudioFxSlot& slot = fxSlots[static_cast<std::size_t>(slotIndex)];
            const bool isVst2 = slot.pluginType.compare("VST2", Qt::CaseInsensitive) == 0;
            const bool isVst3 = slot.pluginType.compare("VST3", Qt::CaseInsensitive) == 0;
            const bool isClap = slot.pluginType.compare("CLAP", Qt::CaseInsensitive) == 0;
            if ((!isVst2 && !isVst3 && !isClap) || slot.pluginPath.trimmed().isEmpty()) continue;

            const HostedEffect* existing = nullptr;
            const auto previousTrack = previous->effectsByTrack.find(effectTrackId);
            if (previousTrack != previous->effectsByTrack.end()) {
                const auto found = previousTrack->second.find(slotIndex);
                if (found != previousTrack->second.end()) existing = &found->second;
            }
            if (existing && existing->clap) existing->clap->serviceControlThreadRequests();
            const bool samePlugin = existing &&
                existing->pluginType.compare(slot.pluginType, Qt::CaseInsensitive) == 0 &&
                existing->path == slot.pluginPath &&
                existing->pluginUid.trimmed().compare(
                    slot.pluginUid.trimmed(), Qt::CaseInsensitive) == 0 &&
                std::fabs(existing->sampleRate - safeSampleRate) < 0.5 &&
                existing->offline == m_impl->offlineProcessing &&
				existing->speakerLayout == VeraCoreSpeakerLayout::Stereo &&
                existing->configuredState == slot.pluginState;
            const bool reusePlugin = samePlugin &&
                ((isVst2 && existing->vst2 && existing->vst2->healthy()) ||
                 (isVst3 && existing->vst3 && existing->vst3->healthy()) ||
                 (isClap && existing->clap && existing->clap->reusable()));

            HostedEffect hosted;
            if (reusePlugin) {
                hosted = *existing;
                if (hosted.clap) {
                    hosted.capabilities = hosted.clap->capabilities();
                    if (hosted.clap->consumeMetricsDirty()) {
                        hosted.latencySamples = hosted.clap->latencySamples();
                        hosted.tailSamples = hosted.clap->tailSamples();
                    }
                }
            } else if (isVst2) {
                auto instance = std::make_shared<Vst2Instance>();
                if (!instance->open(slot.pluginPath, safeSampleRate, slot.pluginState,
                                    m_impl->offlineProcessing)) continue;
                hosted.vst2 = std::move(instance);
            } else if (isVst3) {
                auto instance = std::make_shared<Vst3Instance>();
                if (!instance->open(slot.pluginPath, slot.pluginUid, safeSampleRate,
                                    slot.pluginState, m_impl->offlineProcessing,
                                    m_impl->araRegistry(), effectTrackId, false,
									VeraCoreSpeakerLayout::Stereo)) continue;
                hosted.vst3 = std::move(instance);
            } else {
                auto instance = std::make_shared<ClapInstance>();
                if (!instance->open(slot.pluginPath, slot.pluginUid, safeSampleRate,
                                    slot.pluginState, m_impl->offlineProcessing,
                                    m_impl->araRegistry(), effectTrackId, false,
									VeraCoreSpeakerLayout::Stereo)) continue;
                hosted.clap = std::move(instance);
            }
            hosted.path = slot.pluginPath;
            hosted.pluginType = slot.pluginType;
            hosted.pluginUid = slot.pluginUid;
            hosted.sampleRate = safeSampleRate;
            hosted.offline = m_impl->offlineProcessing;
			hosted.speakerLayout = VeraCoreSpeakerLayout::Stereo;
            if (!reusePlugin) hosted.instanceEpoch = m_impl->nextPluginInstanceEpoch++;
            hosted.configuredState = slot.pluginState;
            hosted.enabled = slot.enabled;
            hosted.automation = slot.automation;
            if (!reusePlugin) {
                if (hosted.vst2) {
                    hosted.capabilities = hosted.vst2->capabilities();
                    hosted.latencySamples = hosted.vst2->latencySamples();
                    hosted.tailSamples = hosted.vst2->tailSamples();
                    hosted.state = hosted.vst2->captureState();
                } else if (hosted.vst3) {
                    hosted.capabilities = hosted.vst3->capabilities();
                    hosted.latencySamples = hosted.vst3->latencySamples();
                    hosted.tailSamples = hosted.vst3->tailSamples();
                    hosted.state = hosted.vst3->captureState();
                } else if (hosted.clap) {
                    hosted.capabilities = hosted.clap->capabilities();
                    hosted.latencySamples = hosted.clap->latencySamples();
                    hosted.tailSamples = hosted.clap->tailSamples();
                    hosted.state = hosted.clap->captureState();
                }
                if (hosted.state.isEmpty()) hosted.state = slot.pluginState;
            }
            if (hosted.vst3) hosted.vst3->setAraEditorOwner(false);
            if (hosted.clap) hosted.clap->setAraEditorOwner(false);
            next->effectsByTrack[effectTrackId].emplace(slotIndex, std::move(hosted));
        }
    }
    // Master inserts share the hardened effect host but run on a dedicated
    // final serial path after dry/return summing and before the master fader.
    const int masterEffectTrackId = VeraCoreMasterAudioFxTrackId;
    for (int slotIndex = 0; slotIndex < static_cast<int>(master.audioFxSlots.size()); ++slotIndex) {
        const AudioFxSlot& slot = master.audioFxSlots[static_cast<std::size_t>(slotIndex)];
        const bool isVst2 = slot.pluginType.compare("VST2", Qt::CaseInsensitive) == 0;
        const bool isVst3 = slot.pluginType.compare("VST3", Qt::CaseInsensitive) == 0;
        const bool isClap = slot.pluginType.compare("CLAP", Qt::CaseInsensitive) == 0;
        if ((!isVst2 && !isVst3 && !isClap) || slot.pluginPath.trimmed().isEmpty()) continue;

        const HostedEffect* existing = nullptr;
        const auto previousTrack = previous->effectsByTrack.find(masterEffectTrackId);
        if (previousTrack != previous->effectsByTrack.end()) {
            const auto found = previousTrack->second.find(slotIndex);
            if (found != previousTrack->second.end()) existing = &found->second;
        }
        if (existing && existing->clap) existing->clap->serviceControlThreadRequests();
        const bool samePlugin = existing &&
            existing->pluginType.compare(slot.pluginType, Qt::CaseInsensitive) == 0 &&
            existing->path == slot.pluginPath &&
            existing->pluginUid.trimmed().compare(
                slot.pluginUid.trimmed(), Qt::CaseInsensitive) == 0 &&
            std::fabs(existing->sampleRate - safeSampleRate) < 0.5 &&
            existing->offline == m_impl->offlineProcessing &&
			existing->speakerLayout == requestedSpeakerLayout &&
            existing->configuredState == slot.pluginState;
        const bool reusePlugin = samePlugin &&
            ((isVst2 && existing->vst2 && existing->vst2->healthy()) ||
             (isVst3 && existing->vst3 && existing->vst3->healthy()) ||
             (isClap && existing->clap && existing->clap->reusable()));

        HostedEffect hosted;
        if (reusePlugin) {
            hosted = *existing;
            if (hosted.clap) {
                hosted.capabilities = hosted.clap->capabilities();
                if (hosted.clap->consumeMetricsDirty()) {
                    hosted.latencySamples = hosted.clap->latencySamples();
                    hosted.tailSamples = hosted.clap->tailSamples();
                }
            }
        } else if (isVst2) {
            auto instance = std::make_shared<Vst2Instance>();
            if (!instance->open(slot.pluginPath, safeSampleRate, slot.pluginState,
                                m_impl->offlineProcessing)) continue;
            hosted.vst2 = std::move(instance);
        } else if (isVst3) {
            auto instance = std::make_shared<Vst3Instance>();
            if (!instance->open(slot.pluginPath, slot.pluginUid, safeSampleRate,
                                slot.pluginState, m_impl->offlineProcessing,
                                m_impl->araRegistry(), masterEffectTrackId, false,
								requestedSpeakerLayout)) continue;
            hosted.vst3 = std::move(instance);
        } else {
            auto instance = std::make_shared<ClapInstance>();
            if (!instance->open(slot.pluginPath, slot.pluginUid, safeSampleRate,
                                slot.pluginState, m_impl->offlineProcessing,
                                m_impl->araRegistry(), masterEffectTrackId, false,
								requestedSpeakerLayout)) continue;
            hosted.clap = std::move(instance);
        }
        hosted.path = slot.pluginPath;
        hosted.pluginType = slot.pluginType;
        hosted.pluginUid = slot.pluginUid;
        hosted.sampleRate = safeSampleRate;
        hosted.offline = m_impl->offlineProcessing;
		hosted.speakerLayout = requestedSpeakerLayout;
        if (!reusePlugin) hosted.instanceEpoch = m_impl->nextPluginInstanceEpoch++;
        hosted.configuredState = slot.pluginState;
        hosted.enabled = slot.enabled;
        hosted.automation = slot.automation;
        if (!reusePlugin) {
            if (hosted.vst2) {
                hosted.capabilities = hosted.vst2->capabilities();
                hosted.latencySamples = hosted.vst2->latencySamples();
                hosted.tailSamples = hosted.vst2->tailSamples();
                hosted.state = hosted.vst2->captureState();
            } else if (hosted.vst3) {
                hosted.capabilities = hosted.vst3->capabilities();
                hosted.latencySamples = hosted.vst3->latencySamples();
                hosted.tailSamples = hosted.vst3->tailSamples();
                hosted.state = hosted.vst3->captureState();
            } else if (hosted.clap) {
                hosted.capabilities = hosted.clap->capabilities();
                hosted.latencySamples = hosted.clap->latencySamples();
                hosted.tailSamples = hosted.clap->tailSamples();
                hosted.state = hosted.clap->captureState();
            }
            if (hosted.state.isEmpty()) hosted.state = slot.pluginState;
        }
        if (hosted.vst3) hosted.vst3->setAraEditorOwner(false);
        if (hosted.clap) hosted.clap->setAraEditorOwner(false);
        next->effectsByTrack[masterEffectTrackId].emplace(slotIndex, std::move(hosted));
    }
    m_impl->rebuildTrackLatencies(*next, *previous, tracks.size(), safeSampleRate);
    m_impl->publishGraph(std::move(next));
    m_impl->applyAraEditorSelection(m_impl->currentGraph);
}

bool ExternalPluginHost::isLayerHosted(int layerId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    return found != m_impl->currentGraph->layers.end() && found->second.enabled &&
        ((found->second.vst2 && found->second.vst2->healthy()) ||
         (found->second.vst3 && found->second.vst3->healthy()) ||
         (found->second.clap && found->second.clap->healthy()));
}

bool ExternalPluginHost::isLayerLoaded(int layerId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    return found != m_impl->currentGraph->layers.end() &&
        ((found->second.vst2 && found->second.vst2->healthy()) ||
         (found->second.vst3 && found->second.vst3->healthy()) ||
         (found->second.clap && found->second.clap->healthy()));
}

bool ExternalPluginHost::isLayerFaulted(int layerId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    if (found == m_impl->currentGraph->layers.end()) return false;
    return (found->second.vst2 && !found->second.vst2->healthy()) ||
           (found->second.vst3 && !found->second.vst3->healthy()) ||
           (found->second.clap && !found->second.clap->healthy());
}

bool ExternalPluginHost::isAudioFxLoaded(int trackIndex, int slotIndex) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return false;
    const auto found = track->second.find(slotIndex);
    return found != track->second.end() &&
        ((found->second.vst2 && found->second.vst2->healthy()) ||
         (found->second.vst3 && found->second.vst3->healthy()) ||
         (found->second.clap && found->second.clap->healthy()));
}

bool ExternalPluginHost::isAudioFxFaulted(int trackIndex, int slotIndex) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return false;
    const auto found = track->second.find(slotIndex);
    if (found == track->second.end()) return false;
    return (found->second.vst2 && !found->second.vst2->healthy()) ||
           (found->second.vst3 && !found->second.vst3->healthy()) ||
           (found->second.clap && !found->second.clap->healthy());
}

uint64_t ExternalPluginHost::instanceEpochForLayer(int layerId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    return found != m_impl->currentGraph->layers.end()
        ? found->second.instanceEpoch : 0;
}

uint64_t ExternalPluginHost::instanceEpochForAudioFx(int trackIndex, int slotIndex) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return 0;
    const auto found = track->second.find(slotIndex);
    return found != track->second.end() ? found->second.instanceEpoch : 0;
}

PluginTechnologyCapabilities ExternalPluginHost::capabilitiesForLayer(int layerId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    return found != m_impl->currentGraph->layers.end()
        ? found->second.capabilities
        : PluginTechnologyCapabilities{};
}

PluginTechnologyCapabilities ExternalPluginHost::capabilitiesForAudioFx(
    int trackIndex, int slotIndex) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return {};
    const auto found = track->second.find(slotIndex);
    return found != track->second.end()
        ? found->second.capabilities
        : PluginTechnologyCapabilities{};
}

std::vector<PluginAudioOutputBus> ExternalPluginHost::outputBusesForLayer(int layerId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    return found != m_impl->currentGraph->layers.end()
        ? found->second.outputBuses : std::vector<PluginAudioOutputBus>{};
}

std::vector<MidiExpressionMapEntry> ExternalPluginHost::expressionMapForLayer(
    int layerId, int midiChannel) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    if (found == m_impl->currentGraph->layers.end() || !found->second.enabled) return {};
    return found->second.expressionMaps[static_cast<size_t>(std::clamp(midiChannel, 0, 15))];
}

std::vector<PluginParameterInfo> ExternalPluginHost::parametersForLayer(int layerId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    if (found == m_impl->currentGraph->layers.end()) return {};
    if (found->second.vst2) return found->second.vst2->parameters();
    if (found->second.vst3) return found->second.vst3->parameters();
    if (found->second.clap) return found->second.clap->parameters();
    return {};
}

std::vector<PluginParameterInfo> ExternalPluginHost::parametersForAudioFx(
    int trackIndex, int slotIndex) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return {};
    const auto found = track->second.find(slotIndex);
    if (found == track->second.end()) return {};
    if (found->second.vst2) return found->second.vst2->parameters();
    if (found->second.vst3) return found->second.vst3->parameters();
    if (found->second.clap) return found->second.clap->parameters();
    return {};
}

bool ExternalPluginHost::beginLayerParameterGesture(int layerId, uint32_t parameterId)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    if (found == m_impl->currentGraph->layers.end()) return false;
    if (found->second.vst2) return found->second.vst2->beginParameterGesture(parameterId);
    if (found->second.vst3) return found->second.vst3->beginParameterGesture(parameterId);
    return found->second.clap && found->second.clap->beginParameterGesture(parameterId);
}

bool ExternalPluginHost::setLayerParameterNormalized(
    int layerId, uint32_t parameterId, double value)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    if (found == m_impl->currentGraph->layers.end()) return false;
    if (found->second.vst2) return found->second.vst2->setParameterNormalized(parameterId, value);
    if (found->second.vst3) return found->second.vst3->setParameterNormalized(parameterId, value);
    return found->second.clap && found->second.clap->setParameterNormalized(parameterId, value);
}

bool ExternalPluginHost::endLayerParameterGesture(int layerId, uint32_t parameterId)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    if (found == m_impl->currentGraph->layers.end()) return false;
    if (found->second.vst2) return found->second.vst2->endParameterGesture(parameterId);
    if (found->second.vst3) return found->second.vst3->endParameterGesture(parameterId);
    return found->second.clap && found->second.clap->endParameterGesture(parameterId);
}

bool ExternalPluginHost::beginAudioFxParameterGesture(
    int trackIndex, int slotIndex, uint32_t parameterId)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return false;
    const auto found = track->second.find(slotIndex);
    if (found == track->second.end()) return false;
    if (found->second.vst2) return found->second.vst2->beginParameterGesture(parameterId);
    if (found->second.vst3) return found->second.vst3->beginParameterGesture(parameterId);
    return found->second.clap && found->second.clap->beginParameterGesture(parameterId);
}

bool ExternalPluginHost::setAudioFxParameterNormalized(
    int trackIndex, int slotIndex, uint32_t parameterId, double value)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return false;
    const auto found = track->second.find(slotIndex);
    if (found == track->second.end()) return false;
    if (found->second.vst2) return found->second.vst2->setParameterNormalized(parameterId, value);
    if (found->second.vst3) return found->second.vst3->setParameterNormalized(parameterId, value);
    return found->second.clap && found->second.clap->setParameterNormalized(parameterId, value);
}

bool ExternalPluginHost::endAudioFxParameterGesture(
    int trackIndex, int slotIndex, uint32_t parameterId)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return false;
    const auto found = track->second.find(slotIndex);
    if (found == track->second.end()) return false;
    if (found->second.vst2) return found->second.vst2->endParameterGesture(parameterId);
    if (found->second.vst3) return found->second.vst3->endParameterGesture(parameterId);
    return found->second.clap && found->second.clap->endParameterGesture(parameterId);
}

QByteArray ExternalPluginHost::captureLayerState(int layerId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto found = m_impl->currentGraph->layers.find(layerId);
    return found != m_impl->currentGraph->layers.end() ? found->second.state : QByteArray{};
}

QByteArray ExternalPluginHost::captureAudioFxState(int trackIndex, int slotIndex) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto track = m_impl->currentGraph->effectsByTrack.find(trackIndex);
    if (track == m_impl->currentGraph->effectsByTrack.end()) return {};
    const auto found = track->second.find(slotIndex);
    return found != track->second.end() ? found->second.state : QByteArray{};
}

std::vector<PluginStateChange> ExternalPluginHost::consumePluginStateChanges()
{
    std::vector<PluginStateChange> changes;
    if (!m_impl) return changes;
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    const auto consume = [&changes](auto& hosted, PluginStateChange change) {
        uint64_t serial = 0;
        bool dirty = false;
        QByteArray refreshed;
        if (hosted.vst2) {
            dirty = hosted.vst2->consumeStateDirty(serial);
            if (dirty) refreshed = hosted.vst2->captureState();
        } else if (hosted.vst3) {
            dirty = hosted.vst3->consumeStateDirty(serial);
            if (dirty) refreshed = hosted.vst3->captureState();
        } else if (hosted.clap) {
            hosted.clap->serviceControlThreadRequests();
            dirty = hosted.clap->consumeStateDirty(serial);
            if (dirty) refreshed = hosted.clap->captureState();
        }
        if (!dirty || refreshed.isEmpty() || refreshed == hosted.state) return;
        change.gestureSerial = serial;
        change.instanceEpoch = hosted.instanceEpoch;
        change.beforeState = hosted.state.isEmpty() ? hosted.configuredState : hosted.state;
        change.afterState = refreshed;
        hosted.state = refreshed;
        hosted.configuredState = refreshed;
        changes.push_back(std::move(change));
    };

    for (auto& entry : m_impl->currentGraph->layers) {
        PluginStateChange change;
        change.target = PluginStateTarget::Layer;
        change.layerId = entry.first;
        change.trackIndex = entry.first / kLayerIdStride;
        change.slotIndex = entry.first % kLayerIdStride;
        consume(entry.second, std::move(change));
    }
    for (auto& track : m_impl->currentGraph->effectsByTrack) {
        for (auto& entry : track.second) {
            PluginStateChange change;
            change.target = PluginStateTarget::AudioFx;
            change.trackIndex = track.first;
            change.slotIndex = entry.first;
            consume(entry.second, std::move(change));
        }
    }
    return changes;
}

std::vector<PluginParameterEditEvent> ExternalPluginHost::consumePluginParameterEdits(
    bool* overflowed)
{
    std::vector<PluginParameterEditEvent> edits;
    if (overflowed) *overflowed = false;
    if (!m_impl) return edits;
    edits.reserve(256);
    bool anyOverflow = false;
    LARGE_INTEGER counterFrequency{};
    const bool counterReady = QueryPerformanceFrequency(&counterFrequency) &&
        counterFrequency.QuadPart > 0;
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    const auto consume = [&](auto& hosted, PluginParameterEditEvent mapped) {
        mapped.instanceEpoch = hosted.instanceEpoch;
        const auto pop = [&](NativePluginParameterEdit& native) {
            if (hosted.vst2) return hosted.vst2->popParameterEdit(native);
            if (hosted.vst3) return hosted.vst3->popParameterEdit(native);
            return hosted.clap && hosted.clap->popParameterEdit(native);
        };
        const auto parameterEditsQuiescent = [&]() {
            if (hosted.vst2) return hosted.vst2->parameterEditsQuiescent();
            if (hosted.vst3) return hosted.vst3->parameterEditsQuiescent();
            return !hosted.clap || hosted.clap->parameterEditsQuiescent();
        };
        const auto parameterEditsQuarantined = [&]() {
            if (hosted.vst2) return hosted.vst2->parameterEditsQuarantined();
            if (hosted.vst3) return hosted.vst3->parameterEditsQuarantined();
            return hosted.clap && hosted.clap->parameterEditsQuarantined();
        };
        const auto tryClearParameterEditQuarantine = [&]() {
            if (hosted.vst2) return hosted.vst2->tryClearParameterEditQuarantine();
            if (hosted.vst3) return hosted.vst3->tryClearParameterEditQuarantine();
            return !hosted.clap || hosted.clap->tryClearParameterEditQuarantine();
        };
        NativeParameterEditOverflow overflowDescriptor;
        if (hosted.vst2) overflowDescriptor = hosted.vst2->consumeParameterEditOverflow();
        else if (hosted.vst3) overflowDescriptor = hosted.vst3->consumeParameterEditOverflow();
        else if (hosted.clap) overflowDescriptor = hosted.clap->consumeParameterEditOverflow();
        if (!overflowDescriptor && !hosted.parameterEditStreamQuarantined &&
            parameterEditsQuarantined()) {
            hosted.parameterEditStreamQuarantined = true;
            return;
        }
        if (overflowDescriptor) {
            anyOverflow = true;
            hosted.parameterEditStreamQuarantined = true;
            hosted.parameterEditQuarantinedId = overflowDescriptor.parameterId;
            hosted.parameterEditQuarantinedSerial = overflowDescriptor.gestureSerial;
            mapped.streamOverflowed = true;
            mapped.parameterId = hosted.parameterEditQuarantinedId;
            mapped.gestureSerial = hosted.parameterEditQuarantinedSerial;
            edits.push_back(mapped);
        }
        NativePluginParameterEdit native;
        if (hosted.parameterEditStreamQuarantined) {
            size_t discarded = 0;
            while (discarded < kMaxNativeParameterEditsPerPoll && pop(native))
                ++discarded;
            const bool queueObservedEmpty =
                discarded < kMaxNativeParameterEditsPerPoll;
            if (queueObservedEmpty && parameterEditsQuiescent() &&
                tryClearParameterEditQuarantine()) {
                hosted.parameterEditStreamQuarantined = false;
                hosted.parameterEditQuarantinedId =
                    std::numeric_limits<uint32_t>::max();
                hosted.parameterEditQuarantinedSerial = 0;
            }
            return;
        }
        while (edits.size() < kMaxNativeParameterEditsPerPoll && pop(native)) {
            if (native.parameterId == std::numeric_limits<uint32_t>::max() ||
                (native.phase == PluginParameterEditPhase::Value &&
                 !std::isfinite(native.normalizedValue))) continue;
            mapped.phase = native.phase;
            mapped.gestureSerial = native.gestureSerial;
            mapped.captureCounter = native.captureCounter;
            mapped.capturedProjectBeat = native.capturedProjectBeat;
            mapped.parameterId = native.parameterId;
            mapped.normalizedValue = native.phase == PluginParameterEditPhase::Value
                ? std::clamp(native.normalizedValue, 0.0, 1.0) : 0.0;
            edits.push_back(mapped);
        }
    };

    for (auto& entry : m_impl->currentGraph->layers) {
        PluginParameterEditEvent mapped;
        mapped.target = PluginStateTarget::Layer;
        mapped.layerId = entry.first;
        mapped.trackIndex = entry.first / kLayerIdStride;
        mapped.slotIndex = entry.first % kLayerIdStride;
        consume(entry.second, mapped);
    }
    for (auto& track : m_impl->currentGraph->effectsByTrack) {
        for (auto& entry : track.second) {
            PluginParameterEditEvent mapped;
            mapped.target = PluginStateTarget::AudioFx;
            mapped.trackIndex = track.first;
            mapped.slotIndex = entry.first;
            consume(entry.second, mapped);
        }
    }
    LARGE_INTEGER pollCounter{};
    const bool pollCounterReady = counterReady && QueryPerformanceCounter(&pollCounter);
    for (PluginParameterEditEvent& edit : edits) {
        edit.captureAgeSeconds = pollCounterReady && edit.captureCounter > 0 &&
            pollCounter.QuadPart >= static_cast<LONGLONG>(edit.captureCounter)
            ? static_cast<double>(pollCounter.QuadPart -
                static_cast<LONGLONG>(edit.captureCounter)) /
                static_cast<double>(counterFrequency.QuadPart)
            : 0.0;
    }
    std::stable_sort(edits.begin(), edits.end(), [](const auto& left, const auto& right) {
        return left.captureAgeSeconds > right.captureAgeSeconds;
    });
    if (overflowed) *overflowed = anyOverflow;
    return edits;
}

std::map<int, uint32_t> ExternalPluginHost::trackLatencySamples() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->currentGraph->latencyByTrack;
}

uint32_t ExternalPluginHost::maxTailSamples(double maxTailSeconds) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const double safeSeconds = std::isfinite(maxTailSeconds) && maxTailSeconds > 0.0
        ? std::clamp(maxTailSeconds, 0.1, 300.0) : 30.0;
    const double requestedRate = m_impl->requestedSampleRate.load(std::memory_order_relaxed);
    const double sampleRate = std::isfinite(requestedRate) && requestedRate > 0.0 ? requestedRate : 44100.0;
    const uint32_t cap = static_cast<uint32_t>(std::min<double>(
        std::numeric_limits<uint32_t>::max(), std::ceil(safeSeconds * sampleRate)));
    std::map<int, uint64_t> tailsByTrack;
    for (const auto& entry : m_impl->currentGraph->layers) {
        const HostedLayer& layer = entry.second;
        if (!layer.enabled) continue;
        const uint32_t tail = layer.tailSamples;
        if (entry.first >= 0) {
            uint64_t& trackTail = tailsByTrack[entry.first / kLayerIdStride];
            trackTail = std::max<uint64_t>(trackTail, std::min(tail, cap));
        }
    }
    for (const auto& track : m_impl->currentGraph->effectsByTrack) {
        for (const auto& entry : track.second) {
            const HostedEffect& effect = entry.second;
            if (!effect.enabled) continue;
            const uint32_t tail = effect.tailSamples;
            tailsByTrack[track.first] = std::min<uint64_t>(
                cap, tailsByTrack[track.first] + std::min(tail, cap));
        }
    }
    const uint64_t masterTail = tailsByTrack[VeraCoreMasterAudioFxTrackId];
    uint64_t upstreamMaximum = 0;
    for (const auto& track : tailsByTrack) {
        if (track.first == VeraCoreMasterAudioFxTrackId) continue;
        upstreamMaximum = std::max(upstreamMaximum, track.second);
    }
    return static_cast<uint32_t>(std::min<uint64_t>(
        cap, upstreamMaximum + std::min<uint64_t>(masterTail, cap)));
}

void ExternalPluginHost::panic()
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->pdcResetGeneration.fetch_add(1, std::memory_order_release);
    for (const auto& entry : m_impl->currentGraph->layers) {
        if (entry.second.vst2) {
            entry.second.vst2->requestPanic();
        }
        if (entry.second.vst3) {
            entry.second.vst3->requestPanic();
        }
        if (entry.second.clap) {
            entry.second.clap->requestPanic();
        }
    }
}

void ExternalPluginHost::render(float* interleavedStereo, unsigned long framesCount,
                                const std::vector<VeraCoreMidiEvent>& midiEvents, double sampleRate)
{
    if (!interleavedStereo || framesCount == 0 || framesCount > static_cast<unsigned long>(INT32_MAX)) {
        return;
    }
    int graphIndex = -1;
    for (int attempt = 0; attempt < 3 && graphIndex < 0; ++attempt)
        graphIndex = m_impl->acquireGraph();
    if (graphIndex < 0) return;
    Impl::PluginGraph* graph = m_impl->graphBindings[static_cast<size_t>(graphIndex)].get();
    if (!graph) {
        m_impl->releaseGraph(graphIndex);
        return;
    }
    const int totalFrames = static_cast<int>(framesCount);
    const double safeSampleRate = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
        ? sampleRate : 44100.0;
    PluginTransportState baseTransport = m_impl->loadTransport();
    baseTransport.sampleRate = safeSampleRate;
    std::vector<VeraCoreMidiEvent>& chunkEvents = graph->globalScratch.chunkEvents;
    chunkEvents.clear();
    PluginTransportState chunkTransport = baseTransport;
    for (int chunkStart = 0; chunkStart < totalFrames;) {
        if (pluginFramesUntilCycleEnd(chunkTransport, safeSampleRate) == 0)
            resetPluginTransportToCycleStart(chunkTransport);
        const int chunkFrames = std::min({kDefaultBlockSize, totalFrames - chunkStart,
            pluginFramesUntilCycleEnd(chunkTransport, safeSampleRate)});
        if (chunkFrames <= 0) break;
        const std::vector<VeraCoreMidiEvent>* events = &midiEvents;
        if (chunkStart != 0 || chunkFrames != totalFrames) {
            chunkEvents.clear();
            for (const VeraCoreMidiEvent& source : midiEvents) {
                const int offset = std::clamp(source.sampleOffset, 0, totalFrames - 1);
                if (offset >= chunkStart && offset < chunkStart + chunkFrames) {
                    if (chunkEvents.size() >= chunkEvents.capacity()) break;
                    VeraCoreMidiEvent event = source;
                    event.sampleOffset = offset - chunkStart;
                    chunkEvents.push_back(event);
                }
            }
            events = &chunkEvents;
        }
        float* chunkMix = interleavedStereo + static_cast<size_t>(chunkStart) * 2u;
        for (const auto& entry : graph->layers) {
            const HostedLayer& layer = entry.second;
            if (layer.vst2 && (layer.enabled || layer.vst2->hasPendingPanic())) {
                layer.vst2->process(chunkMix, chunkFrames, *events, entry.first,
                                    layer.enabled ? layer.gain : 0.0f, layer.pan, chunkTransport,
                                    &layer.automation);
            }
            if (layer.clap && (layer.enabled || layer.clap->hasPendingPanic())) {
                layer.clap->process(chunkMix, chunkFrames, *events, entry.first,
                                    layer.enabled ? layer.gain : 0.0f, layer.pan, chunkTransport,
                                    &layer.automation);
            } else if (layer.clap && layer.clap->hasPendingParameterFlush())
                layer.clap->serviceAudioThreadParameterFlush();
            if (layer.vst3 && (layer.enabled || layer.vst3->hasPendingPanic())) {
                layer.vst3->process(chunkMix, chunkFrames, *events, entry.first,
                                    layer.enabled ? layer.gain : 0.0f, layer.pan, chunkTransport,
                                    &layer.automation);
            }
        }
        advancePluginTransportBySamples(chunkTransport, chunkFrames, safeSampleRate);
        chunkStart += chunkFrames;
    }
    m_impl->releaseGraph(graphIndex);
}

void ExternalPluginHost::renderTrack(int trackId, float* interleavedStereo, unsigned long framesCount,
                                     const std::vector<VeraCoreMidiEvent>& midiEvents, double sampleRate,
                                     double projectBeat, double projectSeconds,
                                     double projectBpm, double barStartBeat, int barNumber,
                                     int timeSignatureNumerator, int timeSignatureDenominator,
                                     bool cycleWrappedAtStart,
                                     int sourceFrameOffset,
                                     VeraCorePluginOutputSink outputSink,
                                     void* outputSinkContext)

{
	renderTrack(trackId, interleavedStereo, framesCount, midiEvents.data(), midiEvents.size(),
		sampleRate, projectBeat, projectSeconds, projectBpm, barStartBeat, barNumber,
		timeSignatureNumerator, timeSignatureDenominator, cycleWrappedAtStart,
		sourceFrameOffset, outputSink, outputSinkContext);
}

void ExternalPluginHost::renderTrack(int trackId, float* interleavedStereo, unsigned long framesCount,
									 const VeraCoreMidiEvent* midiEvents, std::size_t midiEventCount,
									 double sampleRate, double projectBeat, double projectSeconds,
									 double projectBpm, double barStartBeat, int barNumber,
									 int timeSignatureNumerator, int timeSignatureDenominator,
									 bool cycleWrappedAtStart, int sourceFrameOffset,
									 VeraCorePluginOutputSink outputSink,
									 void* outputSinkContext,
									 int renderBatchToken)
{
    if (trackId < 0 || !interleavedStereo || framesCount == 0 ||
        framesCount > static_cast<unsigned long>(INT32_MAX)) return;
    const int64_t firstLayerId64 = static_cast<int64_t>(trackId) * kLayerIdStride;
    if (firstLayerId64 > INT32_MAX) return;
	const bool ownsGraph = renderBatchToken < 0;
	int graphIndex = renderBatchToken;
	for (int attempt = 0; ownsGraph && attempt < 3 && graphIndex < 0; ++attempt)
		graphIndex = m_impl->acquireGraph();
	if (graphIndex < 0 || graphIndex >= static_cast<int>(m_impl->graphBindings.size())) return;
    Impl::PluginGraph* graph = m_impl->graphBindings[static_cast<size_t>(graphIndex)].get();
    if (!graph) {
		if (ownsGraph) m_impl->releaseGraph(graphIndex);
        return;
    }
    const int totalFrames = static_cast<int>(framesCount);
    const auto scratchEntry = graph->trackScratch.find(trackId);
    if (scratchEntry == graph->trackScratch.end()) {
		if (ownsGraph) m_impl->releaseGraph(graphIndex);
        return;
    }
    Impl::RenderScratch& scratch = scratchEntry->second;
    const int safeSourceOffset = std::max(0, sourceFrameOffset);
    const int64_t sourceEnd = static_cast<int64_t>(safeSourceOffset) + totalFrames;
    scratch.trackEvents.clear();
	for (std::size_t eventIndex = 0; eventIndex < midiEventCount; ++eventIndex) {
		const VeraCoreMidiEvent& source = midiEvents[eventIndex];
        if (source.layerId < 0 || source.layerId / kLayerIdStride != trackId ||
            source.sampleOffset < safeSourceOffset ||
            static_cast<int64_t>(source.sampleOffset) >= sourceEnd) continue;
        if (scratch.trackEvents.size() >= scratch.trackEvents.capacity()) break;
        VeraCoreMidiEvent event = source;
        event.sampleOffset -= safeSourceOffset;
        scratch.trackEvents.push_back(event);
    }
    const std::vector<VeraCoreMidiEvent>& renderEvents = scratch.trackEvents;
    const double safeSampleRate = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
        ? sampleRate : 44100.0;
    PluginTransportState baseTransport = ownsGraph
        ? m_impl->loadTransport()
        : m_impl->renderBatchTransports[static_cast<size_t>(graphIndex)];
    baseTransport.sampleRate = safeSampleRate;
    if (std::isfinite(projectBeat) && projectBeat >= 0.0) {
        const double beatDelta = projectBeat - baseTransport.projectBeat;
        baseTransport.projectBeat = projectBeat;
        baseTransport.projectSeconds = boundedPluginTimelineSeconds(
            std::isfinite(projectSeconds) && projectSeconds >= 0.0
                ? projectSeconds
                : baseTransport.projectSeconds + beatDelta * 60.0 /
                    std::max(1.0, baseTransport.bpm), 0.0);
    }
    if (std::isfinite(projectBpm) && projectBpm > 0.0)
        baseTransport.bpm = std::clamp(
            projectBpm, kMinPluginTempoBpm, kMaxPluginTempoBpm);
    const bool denominatorValid = timeSignatureDenominator >= 1 &&
        timeSignatureDenominator <= 64 &&
        (timeSignatureDenominator & (timeSignatureDenominator - 1)) == 0;
    if (timeSignatureNumerator >= 1 && timeSignatureNumerator <= 32 && denominatorValid) {
        baseTransport.numerator = timeSignatureNumerator;
        baseTransport.denominator = timeSignatureDenominator;
    }
    if (std::isfinite(barStartBeat) && barStartBeat >= 0.0 &&
        barStartBeat <= baseTransport.projectBeat + 1.0e-9)
        baseTransport.barStartBeat = barStartBeat;
    if (barNumber >= 0) baseTransport.barNumber = std::clamp(barNumber, 0, INT32_MAX);
    baseTransport.cycleWrappedAtStart = cycleWrappedAtStart;
    alignPluginTransportBarToBeat(baseTransport);
    std::vector<VeraCoreMidiEvent>& chunkEvents = scratch.chunkEvents;
    chunkEvents.clear();
    PluginTransportState chunkTransport = baseTransport;
    for (int chunkStart = 0; chunkStart < totalFrames;) {
        if (pluginFramesUntilCycleEnd(chunkTransport, safeSampleRate) == 0)
            resetPluginTransportToCycleStart(chunkTransport);
        const int chunkFrames = std::min({kDefaultBlockSize, totalFrames - chunkStart,
            pluginFramesUntilCycleEnd(chunkTransport, safeSampleRate)});
        if (chunkFrames <= 0) break;
        const std::vector<VeraCoreMidiEvent>* events = &renderEvents;
        if (chunkStart != 0 || chunkFrames != totalFrames) {
            chunkEvents.clear();
            for (const VeraCoreMidiEvent& source : renderEvents) {
                const int offset = std::clamp(source.sampleOffset, 0, totalFrames - 1);
                if (offset >= chunkStart && offset < chunkStart + chunkFrames) {
                    if (chunkEvents.size() >= chunkEvents.capacity()) break;
                    VeraCoreMidiEvent event = source;
                    event.sampleOffset = offset - chunkStart;
                    chunkEvents.push_back(event);
                }
            }
            events = &chunkEvents;
        }
        float* chunkMix = interleavedStereo + static_cast<size_t>(chunkStart) * 2u;
        const int firstLayerId = static_cast<int>(firstLayerId64);
        const int64_t nextLayerId64 = firstLayerId64 + kLayerIdStride;
        const auto begin = graph->layers.lower_bound(firstLayerId);
        const auto end = nextLayerId64 <= INT32_MAX
            ? graph->layers.lower_bound(static_cast<int>(nextLayerId64)) : graph->layers.end();
        for (auto entry = begin; entry != end; ++entry) {
            const HostedLayer& layer = entry->second;
            if (layer.vst2 && (layer.enabled || layer.vst2->hasPendingPanic()))
                layer.vst2->process(chunkMix, chunkFrames, *events, entry->first,
                    layer.enabled ? layer.gain : 0.0f, layer.pan, chunkTransport,
                    &layer.automation, false,
                    &layer.outputRoutes, outputSink, outputSinkContext);
            if (layer.clap && (layer.enabled || layer.clap->hasPendingPanic()))
                layer.clap->process(chunkMix, chunkFrames, *events, entry->first,
                    layer.enabled ? layer.gain : 0.0f, layer.pan, chunkTransport,
                    &layer.automation, false,
                    &layer.outputRoutes, outputSink, outputSinkContext);
            else if (layer.clap && layer.clap->hasPendingParameterFlush())
                layer.clap->serviceAudioThreadParameterFlush();
            if (layer.vst3 && (layer.enabled || layer.vst3->hasPendingPanic()))
                layer.vst3->process(chunkMix, chunkFrames, *events, entry->first,
                    layer.enabled ? layer.gain : 0.0f, layer.pan, chunkTransport,
                    &layer.automation, false,
                    &layer.outputRoutes, outputSink, outputSinkContext);
        }
        advancePluginTransportBySamples(chunkTransport, chunkFrames, safeSampleRate);
        chunkStart += chunkFrames;
    }
	if (ownsGraph) m_impl->releaseGraph(graphIndex);
}

void ExternalPluginHost::audioCallback(void* context, int trackId, float* interleavedStereo,
                                       unsigned long framesCount,
									   const VeraCoreMidiEvent* midiEvents,
									   std::size_t midiEventCount,
                                       double sampleRate, double projectBeat, double projectSeconds,
                                       double projectBpm, double barStartBeat, int barNumber,
                                       int timeSignatureNumerator, int timeSignatureDenominator,
                                       bool cycleWrappedAtStart,
                                       int sourceFrameOffset,
                                       VeraCorePluginOutputSink outputSink,
									   void* outputSinkContext,
									   int renderBatchToken)
{
    if (context) {
        static_cast<ExternalPluginHost*>(context)->renderTrack(
			trackId, interleavedStereo, framesCount, midiEvents, midiEventCount, sampleRate,
            projectBeat, projectSeconds, projectBpm, barStartBeat, barNumber,
            timeSignatureNumerator, timeSignatureDenominator, cycleWrappedAtStart,
            sourceFrameOffset,
			outputSink, outputSinkContext, renderBatchToken);
    }
}

void ExternalPluginHost::renderAudioFx(int trackId, float* interleavedStereo,
                                       unsigned long framesCount, double sampleRate,
                                       double projectBeat, double projectSeconds,
                                       double projectBpm, double barStartBeat, int barNumber,
                                       int timeSignatureNumerator, int timeSignatureDenominator,
									   bool cycleWrappedAtStart,
									   int renderBatchToken)
{
	renderAudioFxMultichannel(trackId, interleavedStereo, framesCount, 2,
		VeraCoreSpeakerLayout::Stereo, sampleRate, projectBeat, projectSeconds,
		projectBpm, barStartBeat, barNumber, timeSignatureNumerator,
		timeSignatureDenominator, cycleWrappedAtStart, renderBatchToken);
}

void ExternalPluginHost::renderAudioFxMultichannel(
	int trackId, float* interleavedAudio, unsigned long framesCount, int channelCount,
	VeraCoreSpeakerLayout speakerLayout, double sampleRate, double projectBeat,
	double projectSeconds, double projectBpm, double barStartBeat, int barNumber,
	int timeSignatureNumerator, int timeSignatureDenominator,
	bool cycleWrappedAtStart, int renderBatchToken)
{
    if (!interleavedAudio || framesCount == 0 ||
		framesCount > static_cast<unsigned long>(INT32_MAX) ||
		channelCount < 1 || channelCount > kVeraCoreMaxSpatialChannels ||
		VeraCoreSpeakerChannelCount(speakerLayout) != channelCount) return;
	const bool ownsGraph = renderBatchToken < 0;
	int graphIndex = renderBatchToken;
	for (int attempt = 0; ownsGraph && attempt < 3 && graphIndex < 0; ++attempt)
		graphIndex = m_impl->acquireGraph();
	if (graphIndex < 0 || graphIndex >= static_cast<int>(m_impl->graphBindings.size())) return;
    const Impl::PluginGraph* graph = m_impl->graphBindings[static_cast<size_t>(graphIndex)].get();
    if (!graph) {
		if (ownsGraph) m_impl->releaseGraph(graphIndex);
        return;
    }
    const auto track = graph->effectsByTrack.find(trackId);
    const int totalFrames = static_cast<int>(framesCount);
    const double safeRate = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
        ? sampleRate : 44100.0;
    PluginTransportState baseTransport = ownsGraph
        ? m_impl->loadTransport()
        : m_impl->renderBatchTransports[static_cast<size_t>(graphIndex)];
    baseTransport.sampleRate = safeRate;
    if (std::isfinite(projectBeat) && projectBeat >= 0.0) {
        const double beatDelta = projectBeat - baseTransport.projectBeat;
        baseTransport.projectBeat = projectBeat;
        baseTransport.projectSeconds = boundedPluginTimelineSeconds(
            std::isfinite(projectSeconds) && projectSeconds >= 0.0
                ? projectSeconds
                : baseTransport.projectSeconds + beatDelta * 60.0 /
                    std::max(1.0, baseTransport.bpm), 0.0);
    }
    if (std::isfinite(projectBpm) && projectBpm > 0.0)
        baseTransport.bpm = std::clamp(
            projectBpm, kMinPluginTempoBpm, kMaxPluginTempoBpm);
    const bool denominatorValid = timeSignatureDenominator >= 1 &&
        timeSignatureDenominator <= 64 &&
        (timeSignatureDenominator & (timeSignatureDenominator - 1)) == 0;
    if (timeSignatureNumerator >= 1 && timeSignatureNumerator <= 32 && denominatorValid) {
        baseTransport.numerator = timeSignatureNumerator;
        baseTransport.denominator = timeSignatureDenominator;
    }
    if (std::isfinite(barStartBeat) && barStartBeat >= 0.0 &&
        barStartBeat <= baseTransport.projectBeat + 1.0e-9)
        baseTransport.barStartBeat = barStartBeat;
    if (barNumber >= 0) baseTransport.barNumber = std::clamp(barNumber, 0, INT32_MAX);
    baseTransport.cycleWrappedAtStart = cycleWrappedAtStart;
    alignPluginTransportBarToBeat(baseTransport);
    PluginTransportState transport = baseTransport;
    for (int chunkStart = 0; chunkStart < totalFrames;) {
        if (pluginFramesUntilCycleEnd(transport, safeRate) == 0)
            resetPluginTransportToCycleStart(transport);
        const int chunkFrames = std::min({kDefaultBlockSize, totalFrames - chunkStart,
            pluginFramesUntilCycleEnd(transport, safeRate)});
        if (chunkFrames <= 0) break;
        float* chunk = interleavedAudio + static_cast<size_t>(chunkStart) *
			static_cast<size_t>(channelCount);
        if (track != graph->effectsByTrack.end()) {
            for (const auto& entry : track->second) {
                const HostedEffect& effect = entry.second;
                if (!effect.enabled) {
                    if (effect.clap && effect.clap->hasPendingParameterFlush())
                        effect.clap->serviceAudioThreadParameterFlush();
                    continue;
                }
                if (effect.vst2) {
					if (channelCount == 2) {
						effect.vst2->process(chunk, chunkFrames, m_impl->noEvents,
							-1, 1.0f, 0.0f, transport, &effect.automation, true);
					} else {
						float* stereo = graph->multichannelStereoScratch.data();
						for (int frame = 0; frame < chunkFrames; ++frame) {
							const float* source = chunk + static_cast<size_t>(frame) * channelCount;
							stereo[static_cast<size_t>(frame) * 2u] = source[0];
							stereo[static_cast<size_t>(frame) * 2u + 1u] =
								channelCount > 1 ? source[1] : source[0];
						}
						effect.vst2->process(stereo, chunkFrames, m_impl->noEvents,
							-1, 1.0f, 0.0f, transport, &effect.automation, true);
						for (int frame = 0; frame < chunkFrames; ++frame) {
							float* destination = chunk + static_cast<size_t>(frame) * channelCount;
							destination[0] = stereo[static_cast<size_t>(frame) * 2u];
							if (channelCount > 1)
								destination[1] = stereo[static_cast<size_t>(frame) * 2u + 1u];
						}
					}
				}
                if (effect.clap) effect.clap->process(chunk, chunkFrames, m_impl->noEvents,
                    -1, 1.0f, 0.0f, transport, &effect.automation, true,
					nullptr, nullptr, nullptr, channelCount);
                if (effect.vst3) effect.vst3->process(chunk, chunkFrames, m_impl->noEvents,
                    -1, 1.0f, 0.0f, transport, &effect.automation, true,
					nullptr, nullptr, nullptr, channelCount);
            }
        }
        advancePluginTransportBySamples(transport, chunkFrames, safeRate);
        chunkStart += chunkFrames;
    }
	// Existing PDC storage is stereo by design. Multichannel master inserts are
	// serial and keep their intrinsic latency; bed-aware parallel PDC follows in
	// the dedicated routing phase rather than corrupting non-front channels.
	if (channelCount == 2)
		m_impl->applyPdc(*graph, trackId, interleavedAudio, totalFrames);
	if (ownsGraph) m_impl->releaseGraph(graphIndex);
}

void ExternalPluginHost::audioFxCallback(void* context, int trackId, float* interleavedAudio,
                                         unsigned long framesCount, int channelCount,
										 VeraCoreSpeakerLayout speakerLayout, double sampleRate,
                                         double projectBeat, double projectSeconds,
                                         double projectBpm, double barStartBeat, int barNumber,
                                         int timeSignatureNumerator, int timeSignatureDenominator,
										 bool cycleWrappedAtStart,
										 int renderBatchToken)
{
    if (context) static_cast<ExternalPluginHost*>(context)->renderAudioFxMultichannel(
        trackId, interleavedAudio, framesCount, channelCount, speakerLayout,
		sampleRate, projectBeat, projectSeconds,
        projectBpm, barStartBeat, barNumber,
		timeSignatureNumerator, timeSignatureDenominator, cycleWrappedAtStart,
		renderBatchToken);
}

int ExternalPluginHost::beginRenderBatch() noexcept
{
	int graphIndex = -1;
	for (int attempt = 0; attempt < 3 && graphIndex < 0; ++attempt)
		graphIndex = m_impl->acquireGraph();
	if (graphIndex >= 0)
		m_impl->renderBatchTransports[static_cast<size_t>(graphIndex)] = m_impl->loadTransport();
	return graphIndex;
}

void ExternalPluginHost::endRenderBatch(int renderBatchToken) noexcept
{
	m_impl->releaseGraph(renderBatchToken);
}

int ExternalPluginHost::beginRenderBatchCallback(void* context) noexcept
{
	return context ? static_cast<ExternalPluginHost*>(context)->beginRenderBatch() : -1;
}

void ExternalPluginHost::endRenderBatchCallback(void* context, int renderBatchToken) noexcept
{
	if (context) static_cast<ExternalPluginHost*>(context)->endRenderBatch(renderBatchToken);
}

