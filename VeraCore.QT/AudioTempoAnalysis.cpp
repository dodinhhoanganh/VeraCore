#include "AudioTempoAnalysis.h"

#include <QtGlobal>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {
constexpr double kDefaultTempo = 120.0;
constexpr size_t kMaximumTransientMarkers = 4096u;

int safeNumerator(int value) { return qBound(1, value, 32); }

int safeDenominator(int value)
{
    return value >= 1 && value <= 64 && (value & (value - 1)) == 0 ? value : 4;
}

double safeProjectTempo(double value)
{
    return std::isfinite(value) && value > 0.0 ? qBound(20.0, value, 300.0) : kDefaultTempo;
}

double normalizedCorrelation(const std::vector<double>& signal, int lag)
{
    if (lag <= 0 || lag >= static_cast<int>(signal.size())) return 0.0;
    double dot = 0.0;
    double leftPower = 0.0;
    double rightPower = 0.0;
    for (int index = lag; index < static_cast<int>(signal.size()); ++index) {
        const double left = signal[static_cast<size_t>(index)];
        const double right = signal[static_cast<size_t>(index - lag)];
        dot += left * right;
        leftPower += left * left;
        rightPower += right * right;
    }
    const double denominator = std::sqrt(leftPower * rightPower);
    return denominator > 1.0e-12 ? dot / denominator : 0.0;
}

struct MeterCandidate { int numerator; int denominator; };

void detectMeter(const std::vector<double>& onset, double envelopeRate, double tempo,
                 int projectNumerator, int projectDenominator, AudioTempoAnalysis& result)
{
    static constexpr std::array<MeterCandidate, 8> candidates{{
        {2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}
    }};
    const int quarterBins = qMax(1, static_cast<int>(std::lround(
        envelopeRate * 60.0 / tempo)));
    const int quarterCount = static_cast<int>(onset.size()) / quarterBins;
    if (quarterCount >= 12) {
        std::vector<double> beatStrength(static_cast<size_t>(quarterCount), 0.0);
        double mean = 0.0;
        for (int beat = 0; beat < quarterCount; ++beat) {
            const int center = beat * quarterBins;
            const int radius = qMax(1, quarterBins / 8);
            double peak = 0.0;
            for (int bin = qMax(0, center - radius);
                 bin <= qMin(static_cast<int>(onset.size()) - 1, center + radius); ++bin)
                peak = qMax(peak, onset[static_cast<size_t>(bin)]);
            beatStrength[static_cast<size_t>(beat)] = peak;
            mean += peak;
        }
        mean /= quarterCount;
        for (double& value : beatStrength) value -= mean;
        double bestAccentCorrelation = 0.0;
        int bestAccentPeriod = 0;
        for (int period = 2; period <= 5; ++period) {
            const double correlation = normalizedCorrelation(beatStrength, period);
            if (correlation > bestAccentCorrelation + 0.02) {
                bestAccentCorrelation = correlation;
                bestAccentPeriod = period;
            }
        }
        if (bestAccentCorrelation >= 0.20) {
            result.timeSignatureNumerator = bestAccentPeriod;
            result.timeSignatureDenominator = 4;
            result.usedProjectTimeSignature = false;
            return;
        }
    }
    std::array<double, candidates.size()> scores{};
    scores.fill(-1.0);
    for (size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
        const MeterCandidate candidate = candidates[candidateIndex];
        const double pulseSeconds = 60.0 / tempo * (4.0 / candidate.denominator);
        const int pulseBins = qMax(1, static_cast<int>(std::lround(envelopeRate * pulseSeconds)));
        const int pulseCount = static_cast<int>(onset.size()) / pulseBins;
        if (pulseCount < candidate.numerator * 3) continue;
        std::vector<double> phaseEnergy(static_cast<size_t>(candidate.numerator), 0.0);
        std::vector<int> phaseCount(static_cast<size_t>(candidate.numerator), 0);
        for (int pulse = 0; pulse < pulseCount; ++pulse) {
            const int center = pulse * pulseBins;
            double localPeak = 0.0;
            const int radius = qMax(1, pulseBins / 8);
            for (int bin = qMax(0, center - radius);
                 bin <= qMin(static_cast<int>(onset.size()) - 1, center + radius); ++bin)
                localPeak = qMax(localPeak, onset[static_cast<size_t>(bin)]);
            const size_t phase = static_cast<size_t>(pulse % candidate.numerator);
            phaseEnergy[phase] += localPeak;
            ++phaseCount[phase];
        }
        double maximum = 0.0;
        double mean = 0.0;
        for (size_t phase = 0; phase < phaseEnergy.size(); ++phase) {
            const double value = phaseCount[phase] > 0 ? phaseEnergy[phase] / phaseCount[phase] : 0.0;
            maximum = qMax(maximum, value);
            mean += value;
        }
        mean /= qMax<size_t>(1u, phaseEnergy.size());
        const double contrast = maximum > 1.0e-12 ? qBound(0.0, (maximum - mean) / maximum, 1.0) : 0.0;
        const int barLag = pulseBins * candidate.numerator;
        const double barCorrelation = qMax(0.0, normalizedCorrelation(onset, barLag));
        const bool projectMatch = candidate.numerator == safeNumerator(projectNumerator) &&
                                  candidate.denominator == safeDenominator(projectDenominator);
        scores[candidateIndex] = 0.65 * contrast + 0.35 * barCorrelation +
            (projectMatch ? 0.015 : 0.0);
    }
    size_t bestIndex = static_cast<size_t>(std::distance(
        scores.begin(), std::max_element(scores.begin(), scores.end())));
    double bestScore = scores[bestIndex];
    double bestBarQuarterBeats = candidates[bestIndex].numerator *
        (4.0 / candidates[bestIndex].denominator);
    // 3/4 and 6/8 share a bar duration. Prefer the notation family already
    // used by the project when their evidence is effectively tied.
    const int preferredDenominator = safeDenominator(projectDenominator);
    for (size_t index = 0; index < candidates.size(); ++index) {
        const double barQuarterBeats = candidates[index].numerator *
            (4.0 / candidates[index].denominator);
        if (std::fabs(barQuarterBeats - bestBarQuarterBeats) > 0.001 ||
            scores[index] + 0.03 < bestScore ||
            candidates[index].denominator != preferredDenominator) continue;
        bestIndex = index;
        bestScore = scores[index];
        break;
    }
    const MeterCandidate best = candidates[bestIndex];
    bestBarQuarterBeats = best.numerator * (4.0 / best.denominator);
    double secondScore = -1.0;
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (index == bestIndex) continue;
        const double barQuarterBeats = candidates[index].numerator *
            (4.0 / candidates[index].denominator);
        if (std::fabs(barQuarterBeats - bestBarQuarterBeats) <= 0.001) continue;
        secondScore = qMax(secondScore, scores[index]);
    }
    const double separation = qMax(0.0, bestScore - qMax(0.0, secondScore));
    if (bestScore >= 0.18 && separation >= 0.025) {
        result.timeSignatureNumerator = best.numerator;
        result.timeSignatureDenominator = best.denominator;
        result.usedProjectTimeSignature = false;
    } else {
        result.timeSignatureNumerator = safeNumerator(projectNumerator);
        result.timeSignatureDenominator = safeDenominator(projectDenominator);
        result.usedProjectTimeSignature = true;
    }
}

std::vector<double> detectTransientSeconds(const std::vector<double>& onset,
                                           double envelopeRate)
{
    std::vector<double> markers;
    if (onset.size() < 3u || !std::isfinite(envelopeRate) || envelopeRate <= 0.0)
        return markers;

    double positiveMean = 0.0;
    double positivePower = 0.0;
    int positiveCount = 0;
    for (double value : onset) {
        if (value <= 0.0 || !std::isfinite(value)) continue;
        positiveMean += value;
        positivePower += value * value;
        ++positiveCount;
    }
    if (positiveCount == 0) return markers;
    positiveMean /= positiveCount;
    const double variance = qMax(0.0, positivePower / positiveCount - positiveMean * positiveMean);
    const double globalThreshold = qMax(positiveMean * 0.35, std::sqrt(variance) * 0.08);
    const int minimumSpacingBins = qMax(1, static_cast<int>(std::lround(envelopeRate * 0.040)));
    int lastMarkerBin = -minimumSpacingBins;
    markers.reserve(std::min(kMaximumTransientMarkers, onset.size() / 8u));
    for (int bin = 1; bin + 1 < static_cast<int>(onset.size()); ++bin) {
        const double value = onset[static_cast<size_t>(bin)];
        if (value < globalThreshold || value < onset[static_cast<size_t>(bin - 1)] ||
            value <= onset[static_cast<size_t>(bin + 1)]) continue;
        const int localRadius = qMax(2, static_cast<int>(std::lround(envelopeRate * 0.080)));
        double localMean = 0.0;
        int localCount = 0;
        for (int local = qMax(0, bin - localRadius);
             local <= qMin(static_cast<int>(onset.size()) - 1, bin + localRadius); ++local) {
            if (local == bin) continue;
            localMean += qMax(0.0, onset[static_cast<size_t>(local)]);
            ++localCount;
        }
        if (localCount > 0 && value < localMean / localCount * 1.6) continue;
        if (bin - lastMarkerBin < minimumSpacingBins) {
            if (!markers.empty() && value > onset[static_cast<size_t>(lastMarkerBin)])
                markers.back() = static_cast<double>(bin) / envelopeRate;
            else
                continue;
        } else {
            markers.push_back(static_cast<double>(bin) / envelopeRate);
        }
        lastMarkerBin = bin;
        if (markers.size() >= kMaximumTransientMarkers) break;
    }
    return markers;
}
}

AudioTempoAnalysis estimateAudioTempoFromDuration(double durationSeconds,
                                                  double projectTempoBpm,
                                                  int projectNumerator,
                                                  int projectDenominator)
{
    AudioTempoAnalysis result;
    result.timeSignatureNumerator = safeNumerator(projectNumerator);
    result.timeSignatureDenominator = safeDenominator(projectDenominator);
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0) return result;
    const double quarterBeatsPerBar = result.timeSignatureNumerator *
        (4.0 / result.timeSignatureDenominator);
    const double projectTempo = safeProjectTempo(projectTempoBpm);
    double bestTempo = 0.0;
    double bestScore = std::numeric_limits<double>::max();
    for (int bars : {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 64}) {
        const double tempo = bars * quarterBeatsPerBar * 60.0 / durationSeconds;
        if (!std::isfinite(tempo) || tempo < 40.0 || tempo > 240.0) continue;
        const double score = std::fabs(std::log(tempo / projectTempo));
        if (score < bestScore) { bestScore = score; bestTempo = tempo; }
    }
    if (bestTempo > 0.0 && bestScore <= std::log(1.5)) {
        result.tempoBpm = bestTempo;
        result.confidence = qBound(0.05, 0.35 * (1.0 - bestScore / std::log(1.5)), 0.35);
        result.source = AudioTempoSource::DurationFallback;
    }
    return result;
}

AudioTempoAnalysis analyzeAudioTempo(const DecodedAudioFile& audio,
                                     double projectTempoBpm,
                                     int projectNumerator,
                                     int projectDenominator)
{
    AudioTempoAnalysis result;
    result.timeSignatureNumerator = safeNumerator(projectNumerator);
    result.timeSignatureDenominator = safeDenominator(projectDenominator);
    const int frameCount = audio.frameCount();
    if (audio.sampleRate <= 0 || frameCount < audio.sampleRate * 2 ||
        audio.stereoSamples.size() < static_cast<size_t>(frameCount) * 2u) return result;

    constexpr double targetEnvelopeRate = 200.0;
    const int hopFrames = qMax(1, static_cast<int>(std::lround(audio.sampleRate / targetEnvelopeRate)));
    const int envelopeCount = frameCount / hopFrames;
    if (envelopeCount < 400) return result;
    std::vector<double> onset(static_cast<size_t>(envelopeCount), 0.0);
    double previousEnergy = 0.0;
    double onsetSum = 0.0;
    for (int bin = 0; bin < envelopeCount; ++bin) {
        const int begin = bin * hopFrames;
        const int end = qMin(begin + hopFrames, frameCount);
        double energy = 0.0;
        for (int frame = begin; frame < end; ++frame) {
            const size_t sample = static_cast<size_t>(frame) * 2u;
            energy += 0.5 * (std::fabs(audio.stereoSamples[sample]) +
                             std::fabs(audio.stereoSamples[sample + 1u]));
        }
        energy /= qMax(1, end - begin);
        const double strength = qMax(0.0, energy - previousEnergy);
        onset[static_cast<size_t>(bin)] = strength;
        onsetSum += strength;
        previousEnergy = previousEnergy * 0.65 + energy * 0.35;
    }
    const double onsetMean = onsetSum / envelopeCount;
    double onsetPower = 0.0;
    for (double& strength : onset) { strength -= onsetMean; onsetPower += strength * strength; }
    if (!std::isfinite(onsetPower) || onsetPower <= 1.0e-12) return result;

    const double envelopeRate = static_cast<double>(audio.sampleRate) / hopFrames;
    result.transientSeconds = detectTransientSeconds(onset, envelopeRate);
    const int minimumLag = qMax(1, static_cast<int>(std::floor(envelopeRate * 60.0 / 240.0)));
    const int maximumLag = qMin(envelopeCount / 2,
        static_cast<int>(std::ceil(envelopeRate * 60.0 / 40.0)));
    const double projectTempo = safeProjectTempo(projectTempoBpm);
    double bestTempo = 0.0;
    double bestCorrelation = 0.0;
    double bestScore = -1.0;
    double secondScore = -1.0;
    int bestLag = 0;
    for (int lag = minimumLag; lag <= maximumLag; ++lag) {
        const double correlation = normalizedCorrelation(onset, lag);
        const double tempo = envelopeRate * 60.0 / lag;
        // Autocorrelation often promotes the bar/downbeat period (for example
        // 40 BPM for a 120 BPM 3/4 loop). The project tempo is the explicit
        // ambiguity prior, not a replacement for onset evidence.
        const double projectBias = std::exp(-std::fabs(std::log(tempo / projectTempo)) / 0.30);
        const double score = correlation + 0.16 * projectBias;
        if (score > bestScore) {
            secondScore = bestScore; bestScore = score; bestCorrelation = correlation;
            bestTempo = tempo; bestLag = lag;
        } else if (std::abs(lag - bestLag) > 2 && score > secondScore) {
            secondScore = score;
        }
    }
    // Resolve common bar-period/octave aliases using only integer-related
    // candidates that still carry meaningful onset correlation. This allows a
    // 3/4 loop with strong downbeats to resolve 40 -> 120 BPM without snapping
    // arbitrary material to the project tempo.
    const double currentProjectDistance = bestTempo > 0.0
        ? std::fabs(std::log(bestTempo / projectTempo)) : std::numeric_limits<double>::max();
    double selectedProjectDistance = currentProjectDistance;
    for (int divisor = 2; divisor <= 4 && bestLag > 0; ++divisor) {
        const int candidateLag = static_cast<int>(std::lround(
            static_cast<double>(bestLag) / divisor));
        if (candidateLag < minimumLag || candidateLag > maximumLag) continue;
        const double correlation = normalizedCorrelation(onset, candidateLag);
        if (correlation < 0.08 || correlation < bestCorrelation * 0.30) continue;
        const double tempo = envelopeRate * 60.0 / candidateLag;
        const double distance = std::fabs(std::log(tempo / projectTempo));
        if (distance + 0.08 >= selectedProjectDistance) continue;
        bestLag = candidateLag;
        bestTempo = tempo;
        bestCorrelation = correlation;
        selectedProjectDistance = distance;
    }
    if (bestCorrelation < 0.08 || !std::isfinite(bestTempo)) return result;
    const double strength = qBound(0.0, (bestCorrelation - 0.08) / 0.42, 1.0);
    const double separation = bestScore > 1.0e-9
        ? qBound(0.0, (bestScore - qMax(0.0, secondScore)) / bestScore, 1.0) : 0.0;
    result.tempoBpm = bestTempo;
    result.confidence = qBound(0.05, 0.8 * strength + 0.2 * separation, 1.0);
    result.source = AudioTempoSource::OnsetAutocorrelation;
    detectMeter(onset, envelopeRate, bestTempo, projectNumerator, projectDenominator, result);
    return result;
}
