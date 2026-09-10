#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// Cloudy9 spectral harmonic engine.
//
// The generator never distorts the signal. It analyses the input with an STFT,
// tracks every stable spectral peak, and synthesises one sine oscillator per
// peak at N * f. Because the wet signal is built from oscillators instead of a
// waveshaper, no intermodulation products can exist by construction: the only
// frequencies present in the wet path are the ones we explicitly create.

namespace cloudy {

constexpr double kPi = 3.14159265358979323846;
constexpr int fftOrder = 11;
constexpr int fftSize = 1 << fftOrder;     // 2048 -> 42.7 ms window at 48 kHz
constexpr int hopSize = fftSize / 8;       // 256  -> 5.3 ms analysis rate
constexpr int spectralLatency = fftSize / 2;
constexpr int maxPartials = 40;
constexpr int maxPeaks = 24;

class Fft {
public:
    void prepare() {
        if (prepared) return;
        reversed.resize(size_t(fftSize));
        cosTable.resize(size_t(fftSize / 2));
        sinTable.resize(size_t(fftSize / 2));
        for (int i = 0; i < fftSize / 2; ++i) {
            const double angle = -2.0 * kPi * double(i) / double(fftSize);
            cosTable[size_t(i)] = float(std::cos(angle));
            sinTable[size_t(i)] = float(std::sin(angle));
        }
        for (int i = 0; i < fftSize; ++i) {
            int value = i, result = 0;
            for (int bit = 0; bit < fftOrder; ++bit) { result = (result << 1) | (value & 1); value >>= 1; }
            reversed[size_t(i)] = result;
        }
        prepared = true;
    }

    void forward(std::vector<float>& re, std::vector<float>& im) const {
        for (int i = 0; i < fftSize; ++i) {
            const int j = reversed[size_t(i)];
            if (i < j) { std::swap(re[size_t(i)], re[size_t(j)]); std::swap(im[size_t(i)], im[size_t(j)]); }
        }
        for (int length = 2; length <= fftSize; length <<= 1) {
            const int half = length / 2;
            const int step = fftSize / length;
            for (int base = 0; base < fftSize; base += length) {
                for (int k = 0; k < half; ++k) {
                    const int twiddle = k * step;
                    const float wr = cosTable[size_t(twiddle)];
                    const float wi = sinTable[size_t(twiddle)];
                    const size_t a = size_t(base + k);
                    const size_t b = size_t(base + k + half);
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
            }
        }
    }

private:
    bool prepared = false;
    std::vector<int> reversed;
    std::vector<float> cosTable, sinTable;
};

struct Partial {
    bool active = false;
    float frequency = 0.0f, targetFrequency = 0.0f;
    float amplitude = 0.0f, targetAmplitude = 0.0f;
    float phase = 0.0f;
};

struct Candidate {
    float frequency = 0.0f;
    float amplitude = 0.0f;
    float phase = 0.0f;
    int bin = 0;
};

class SpectralHarmonics {
public:
    void prepare(double newSampleRate) {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        fft.prepare();
        ring.assign(size_t(fftSize), 0.0f);
        re.assign(size_t(fftSize), 0.0f);
        im.assign(size_t(fftSize), 0.0f);
        magnitude.assign(size_t(fftSize / 2), 0.0f);
        previous.assign(size_t(fftSize / 2), 0.0f);
        window.resize(size_t(fftSize));
        for (int i = 0; i < fftSize; ++i)
            window[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(fftSize)));
        smoothCoefficient = 1.0f - std::exp(-1.0f / (0.5f * float(hopSize)));
        attackCoefficient = 1.0f - std::exp(-1.0f / float(0.0006 * sampleRate));
        releaseCoefficient = 1.0f - std::exp(-1.0f / float(0.030 * sampleRate));
        phaseIncrement = float(2.0 * kPi / sampleRate);
        reset();
    }

    void reset() {
        std::fill(ring.begin(), ring.end(), 0.0f);
        std::fill(previous.begin(), previous.end(), 0.0f);
        partials.fill(Partial{});
        writeIndex = 0; hopCounter = 0;
        transientGain = 1.0f; transientTarget = 1.0f;
    }

    void setColor(float tilt) { colorTilt = std::clamp(tilt, -1.0f, 1.0f); }
    void setOrder(int newOrder) { order = std::clamp(newOrder, 2, 11); }

    float process(float input) {
        ring[size_t(writeIndex)] = input;
        if (++writeIndex >= fftSize) writeIndex = 0;
        if (++hopCounter >= hopSize) { hopCounter = 0; analyse(); }

        const float coefficient = transientTarget < transientGain ? attackCoefficient : releaseCoefficient;
        transientGain += (transientTarget - transientGain) * coefficient;

        float sum = 0.0f;
        for (auto& partial : partials) {
            if (!partial.active) continue;
            partial.amplitude += (partial.targetAmplitude - partial.amplitude) * smoothCoefficient;
            partial.frequency += (partial.targetFrequency - partial.frequency) * smoothCoefficient;
            partial.phase += partial.frequency * phaseIncrement;
            if (partial.phase >= float(2.0 * kPi)) partial.phase -= float(2.0 * kPi);
            sum += partial.amplitude * std::sin(partial.phase);
            if (partial.targetAmplitude <= 0.0f && partial.amplitude < 1.0e-6f) partial = Partial{};
        }
        return sum * transientGain;
    }

    static int latencySamples() { return spectralLatency; }

private:
    void analyse() {
        for (int i = 0; i < fftSize; ++i) {
            const int index = writeIndex + i;
            re[size_t(i)] = ring[size_t(index >= fftSize ? index - fftSize : index)] * window[size_t(i)];
            im[size_t(i)] = 0.0f;
        }
        fft.forward(re, im);

        const int bins = fftSize / 2;
        float flux = 0.0f, total = 0.0f, loudest = 0.0f;
        for (int k = 0; k < bins; ++k) {
            const float value = std::sqrt(re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)]);
            magnitude[size_t(k)] = value;
            const float difference = value - previous[size_t(k)];
            if (difference > 0.0f) flux += difference;
            total += value;
            loudest = std::max(loudest, value);
            previous[size_t(k)] = value;
        }
        const float fluxRatio = total > 1.0e-9f ? flux / total : 0.0f;
        transientTarget = std::clamp(1.0f - 2.2f * std::max(0.0f, fluxRatio - 0.15f), 0.18f, 1.0f);

        const float floorMagnitude = std::max(loudest * 0.0012f, 1.0e-3f);
        const float nyquistLimit = 0.47f * float(sampleRate);
        const float softLimit = 0.80f * nyquistLimit;

        int count = 0;
        for (int k = 2; k < bins - 2; ++k) {
            const float centre = magnitude[size_t(k)];
            if (centre < floorMagnitude) continue;
            if (centre <= magnitude[size_t(k - 1)] || centre < magnitude[size_t(k + 1)]) continue;

            const float a = std::log(std::max(magnitude[size_t(k - 1)], 1.0e-12f));
            const float b = std::log(std::max(centre, 1.0e-12f));
            const float c = std::log(std::max(magnitude[size_t(k + 1)], 1.0e-12f));
            const float denominator = a - 2.0f * b + c;
            if (denominator > -1.0e-9f) continue;
            const float delta = std::clamp(0.5f * (a - c) / denominator, -0.5f, 0.5f);
            const float peakMagnitude = std::exp(b - 0.25f * (a - c) * delta);
            const float frequency = (float(k) + delta) * float(sampleRate) / float(fftSize);
            if (frequency < 25.0f) continue;

            const float outputFrequency = frequency * float(order);
            if (outputFrequency > nyquistLimit) continue;

            // Median of the surrounding bins, not the mean: a mean would be
            // dominated by neighbouring tones and would gate out the middle
            // partial of a dense chord.
            std::array<float, 24> neighbourhood{}; int neighbours = 0;
            for (int j = k - 12; j <= k + 12; ++j) {
                if (j < 0 || j >= bins || std::abs(j - k) <= 2) continue;
                neighbourhood[size_t(neighbours++)] = magnitude[size_t(j)];
            }
            if (neighbours < 4) continue;
            std::nth_element(neighbourhood.begin(), neighbourhood.begin() + neighbours / 2, neighbourhood.begin() + neighbours);
            const float median = neighbourhood[size_t(neighbours / 2)];
            const float ratio = peakMagnitude / (median + 1.0e-12f);
            const float tonality = std::clamp((std::log10(std::max(ratio, 1.0e-12f)) - 0.8f) / 0.7f, 0.0f, 1.0f);
            if (tonality <= 0.0f) continue;

            float amplitude = 4.0f * peakMagnitude / float(fftSize);
            const float drive = std::clamp(std::pow(std::max(amplitude, 1.0e-6f) / 0.25f, 0.25f), 0.25f, 2.0f);
            amplitude *= tonality * drive;

            if (outputFrequency > softLimit)
                amplitude *= (nyquistLimit - outputFrequency) / (nyquistLimit - softLimit);

            const float tiltDecibels = colorTilt * 14.0f * std::log10(std::max(frequency, 20.0f) / 700.0f);
            amplitude *= std::clamp(std::pow(10.0f, tiltDecibels / 20.0f), 0.06f, 6.0f);
            if (amplitude < 1.0e-6f) continue;

            if (count > 0 && k - candidates[size_t(count - 1)].bin <= 2) {
                if (amplitude > candidates[size_t(count - 1)].amplitude)
                    candidates[size_t(count - 1)] = {frequency, amplitude, std::atan2(im[size_t(k)], re[size_t(k)]) * float(order), k};
                continue;
            }
            const Candidate candidate{frequency, amplitude, std::atan2(im[size_t(k)], re[size_t(k)]) * float(order), k};
            if (count < maxPeaks) { candidates[size_t(count++)] = candidate; continue; }
            int weakest = 0;
            for (int i = 1; i < count; ++i)
                if (candidates[size_t(i)].amplitude < candidates[size_t(weakest)].amplitude) weakest = i;
            if (candidate.amplitude > candidates[size_t(weakest)].amplitude) candidates[size_t(weakest)] = candidate;
        }

        float requested = 0.0f;
        for (int i = 0; i < count; ++i) requested += candidates[size_t(i)].amplitude;
        if (requested > 0.7f) {
            const float scale = 0.7f / requested;
            for (int i = 0; i < count; ++i) candidates[size_t(i)].amplitude *= scale;
        }

        std::array<bool, size_t(maxPeaks)> used{};
        for (auto& partial : partials) {
            if (!partial.active) continue;
            int best = -1; float bestDistance = 0.04f;
            for (int i = 0; i < count; ++i) {
                if (used[size_t(i)]) continue;
                const float target = candidates[size_t(i)].frequency * float(order);
                const float distance = std::abs(target - partial.targetFrequency) / std::max(1.0f, partial.targetFrequency);
                if (distance < bestDistance) { bestDistance = distance; best = i; }
            }
            if (best >= 0) {
                used[size_t(best)] = true;
                partial.targetFrequency = candidates[size_t(best)].frequency * float(order);
                partial.targetAmplitude = candidates[size_t(best)].amplitude;
            } else {
                partial.targetAmplitude = 0.0f;
            }
        }

        for (int i = 0; i < count; ++i) {
            if (used[size_t(i)]) continue;
            for (auto& partial : partials) {
                if (partial.active) continue;
                partial.active = true;
                partial.amplitude = 0.0f;
                partial.targetAmplitude = candidates[size_t(i)].amplitude;
                partial.frequency = candidates[size_t(i)].frequency * float(order);
                partial.targetFrequency = partial.frequency;
                partial.phase = std::fmod(candidates[size_t(i)].phase, float(2.0 * kPi));
                if (partial.phase < 0.0f) partial.phase += float(2.0 * kPi);
                break;
            }
        }
    }

    Fft fft;
    double sampleRate = 48000.0;
    std::vector<float> ring, re, im, magnitude, previous, window;
    std::array<Partial, size_t(maxPartials)> partials{};
    std::array<Candidate, size_t(maxPeaks)> candidates{};
    int writeIndex = 0, hopCounter = 0, order = 9;
    float colorTilt = 0.0f;
    float smoothCoefficient = 0.01f, attackCoefficient = 0.5f, releaseCoefficient = 0.001f;
    float transientGain = 1.0f, transientTarget = 1.0f, phaseIncrement = 0.0f;
};

class DryDelay {
public:
    void prepare(int delaySamples) {
        length = std::max(0, delaySamples);
        buffer.assign(size_t(std::max(length, 1)) * 2, 0.0f);
        index = 0;
    }
    void reset() { std::fill(buffer.begin(), buffer.end(), 0.0f); index = 0; }
    std::array<float, 2> process(std::array<float, 2> input) {
        if (length <= 0) return input;
        const size_t base = size_t(index) * 2;
        const std::array<float, 2> output{buffer[base], buffer[base + 1]};
        buffer[base] = input[0]; buffer[base + 1] = input[1];
        if (++index >= length) index = 0;
        return output;
    }
private:
    std::vector<float> buffer;
    int length = 0, index = 0;
};

} // namespace cloudy
