#pragma once

#include <QString>

struct AudioStretchRequest
{
    QString sourcePath;
    QString destinationPath;
    double sourceTempoBpm = 120.0;
    double targetTempoBpm = 120.0;
    double tempoRatioOverride = 0.0;
    double pitchShiftSemitones = 0.0;
    int sampleRate = 44100;
};

struct AudioStretchResult
{
    bool success = false;
    QString message;
};

class AudioStretchBackend
{
public:
    static bool isElastiqueAvailable();
    static double tempoRatio(double sourceTempoBpm, double targetTempoBpm);
    static AudioStretchResult processForProjectSync(const AudioStretchRequest& request);
};
