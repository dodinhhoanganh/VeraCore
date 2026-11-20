#include "AudioStretchBackend.h"
#include "AudioNativeBackendConfig.h"

#include <QFile>
#include <QFileInfo>
#include <QtGlobal>
#include <cmath>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if VC_AUDIO_WITH_ELASTIQUE != 0 && VC_AUDIO_ELASTIQUE_PROCESSOR_READY != 0 && __has_include("VeraCoreElastiqueAdapter.h")
#include "VeraCoreElastiqueAdapter.h"
#define VC_AUDIO_HAS_ELASTIQUE_ADAPTER 1
#else
#define VC_AUDIO_HAS_ELASTIQUE_ADAPTER 0
#endif

bool AudioStretchBackend::isElastiqueAvailable()
{
    return VC_AUDIO_WITH_ELASTIQUE != 0 &&
        VC_AUDIO_ELASTIQUE_PROCESSOR_READY != 0 &&
        VC_AUDIO_HAS_ELASTIQUE_ADAPTER != 0;
}

double AudioStretchBackend::tempoRatio(double sourceTempoBpm, double targetTempoBpm)
{
    const double safeSource = std::isfinite(sourceTempoBpm) && sourceTempoBpm > 0.0
        ? qBound(20.0, sourceTempoBpm, 300.0)
        : 120.0;
    const double safeTarget = std::isfinite(targetTempoBpm) && targetTempoBpm > 0.0
        ? qBound(20.0, targetTempoBpm, 300.0)
        : 120.0;
    return safeTarget / safeSource;
}

AudioStretchResult AudioStretchBackend::processForProjectSync(const AudioStretchRequest& request)
{
    if (request.sourcePath.trimmed().isEmpty() || request.destinationPath.trimmed().isEmpty()) {
        return {false, "Audio sync needs valid source and destination paths."};
    }

    const double ratio = std::isfinite(request.tempoRatioOverride) && request.tempoRatioOverride > 0.0
        ? request.tempoRatioOverride
        : tempoRatio(request.sourceTempoBpm, request.targetTempoBpm);
    if (!std::isfinite(ratio) || ratio <= 0.0) {
        return {false, "Audio sync tempo ratio is invalid."};
    }

    const double pitch = std::isfinite(request.pitchShiftSemitones)
        ? qBound(-48.0, request.pitchShiftSemitones, 48.0)
        : 0.0;

    const bool noStretchNeeded = std::abs(ratio - 1.0) <= 0.0001 && std::abs(pitch) <= 0.0001;
    if (noStretchNeeded) {
        if (QFileInfo(request.sourcePath).absoluteFilePath() ==
            QFileInfo(request.destinationPath).absoluteFilePath()) {
            return {true, "Audio sync skipped because source already matches destination."};
        }
        QFile::remove(request.destinationPath);
        if (!QFile::copy(request.sourcePath, request.destinationPath)) {
            return {false, "Audio sync no-op copy failed."};
        }
        return {true, "Audio sync copied source because stretch/pitch changes were not needed."};
    }

#if VC_AUDIO_WITH_ELASTIQUE != 0 && VC_AUDIO_ELASTIQUE_PROCESSOR_READY != 0 && VC_AUDIO_HAS_ELASTIQUE_ADAPTER != 0
    const bool ok = VeraCoreElastiqueProcessFile(
        request.sourcePath,
        request.destinationPath,
        ratio,
        pitch,
        request.sampleRate);
    if (!ok) {
        QFile::remove(request.destinationPath);
        return {false, "Elastique Pro processing failed."};
    }
    return {true, "Audio sync processed with Elastique Pro."};
#else
    if (VC_AUDIO_WITH_ELASTIQUE == 0 || VC_AUDIO_ELASTIQUE_PROCESSOR_READY == 0) {
        return {false, "Audio sync needs a native Elastique Pro backend linked for time-stretch and pitch-shift."};
    }
    return {false, "Elastique Pro adapter header is missing; expected VeraCoreElastiqueAdapter.h."};
#endif
}
