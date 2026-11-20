#pragma once

#include "AudioFileDecoder.h"

#include <vector>

enum class AudioTempoSource
{
    Unknown,
    OnsetAutocorrelation,
    DurationFallback
};

struct AudioTempoAnalysis
{
    double tempoBpm = 0.0;
    double confidence = 0.0;
    int timeSignatureNumerator = 0;
    int timeSignatureDenominator = 0;
    AudioTempoSource source = AudioTempoSource::Unknown;
    bool usedProjectTimeSignature = true;
    std::vector<double> transientSeconds;
};

AudioTempoAnalysis analyzeAudioTempo(const DecodedAudioFile& audio,
                                     double projectTempoBpm,
                                     int projectNumerator,
                                     int projectDenominator);

AudioTempoAnalysis estimateAudioTempoFromDuration(double durationSeconds,
                                                  double projectTempoBpm,
                                                  int projectNumerator,
                                                  int projectDenominator);
