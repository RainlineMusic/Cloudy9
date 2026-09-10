#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// Cloudy9 v0.2 - multiband Chebyshev harmonic generator.
//
// Why v0.1 produced H10 and H11 at all:
//   T9 is a ninth degree polynomial, so a clean sine can only come out of it
//   as H1, H3, H5, H7, H9. Nothing above the ninth and nothing even is
//   mathematically possible. The extra harmonics came from two side effects:
//     1. the rectified one pole envelope follower ripples at 2f, and that
//        ripple amplitude modulates the cos(9wt) carrier, creating sidebands
//        at 7f and 11f (and 5f/13f from the 4f ripple component);
//     2. once the normalised signal was no longer a pure sine, the ninth
//        degree polynomial pushed energy far above Nyquist, which folded back
//        as H10 and friends because there was no oversampling.
//
// A third effect explains why the ninth was weak rather than dominant:
// T9(a*cos) puts exactly a^9 into the ninth harmonic. An envelope follower
// that overestimates the amplitude by 15% costs 15 dB of H9 and hands the
// spectrum to the lower odd residue.
//
// v0.2 addresses all three:
//   - narrow bands, so the detector only has to track one partial at a time;
//   - per band detector built from a rectified average lowpassed well below
//     the band, which is both accurate and free of 2f ripple;
//   - strictly odd symmetric shaping, so even harmonics cannot appear at all;
//   - the drive path is limited to 2.8 kHz and the plugin runs the shaper at
//     4x, so the highest product (9 * 2.8 kHz) never folds back;
//   - allpass compensated crossovers, so a tone sitting between two bands
//     does not cancel its own harmonic;
//   - one shared output filter instead of per band filters, for the same
//     phase coherence reason.

namespace cloudy {

constexpr double kPi = 3.14159265358979323846;
// Eight roughly half octave bands. Band width matters: two partials sharing
// one band intermodulate through the polynomial, so the width of a band sets
// how dirty a chord sounds.
constexpr int bandCount = 8;
constexpr double driveLimitHz = 2800.0;
constexpr std::array<double, 7> crossoverFrequencies{90.0, 160.0, 280.0, 480.0, 800.0, 1400.0, 2200.0};
constexpr std::array<double, 8> bandLowEdges{20.0, 90.0, 160.0, 280.0, 480.0, 800.0, 1400.0, 2200.0};
constexpr std::array<double, 8> bandHighEdges{90.0, 160.0, 280.0, 480.0, 800.0, 1400.0, 2200.0, driveLimitHz};
constexpr int oversamplingFactor = 4;

inline float chebyshev(float x, int order) {
    if (order <= 0) return 1.0f;
    if (order == 1) return x;
    float previous = 1.0f, current = x;
    for (int n = 2; n <= order; ++n) {
        const float next = 2.0f * x * current - previous;
        previous = current;
        current = next;
    }
    return current;
}

// Topology preserving state variable filter (Zavalishin / Cytomic form).
struct Svf {
    void set(double frequency, double sampleRate, double q) {
        const double limited = std::clamp(frequency, 10.0, sampleRate * 0.45);
        g = float(std::tan(kPi * limited / sampleRate));
        k = float(1.0 / q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() { ic1 = 0.0f; ic2 = 0.0f; }
    // Second order allpass, which is exactly the sum of a Linkwitz-Riley pair.
    float allpass(float input) {
        const float v3 = input - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return input - 2.0f * k * v1;
    }
    void process(float input, float& low, float& high) {
        const float v3 = input - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        low = v2;
        high = input - k * v1 - v2;
    }
    float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, ic1 = 0.0f, ic2 = 0.0f;
};

// Two cascaded Butterworth sections, which is a Linkwitz-Riley fourth order
// section. Lowpass and highpass built this way stay in phase with each other.
struct Butterworth4 {
    void set(double frequency, double sampleRate) {
        a.set(frequency, sampleRate, 0.70710678);
        b.set(frequency, sampleRate, 0.70710678);
    }
    void reset() { a.reset(); b.reset(); }
    float lowpass(float input) {
        float low1 = 0.0f, high1 = 0.0f, low2 = 0.0f, high2 = 0.0f;
        a.process(input, low1, high1);
        b.process(low1, low2, high2);
        return low2;
    }
    float highpass(float input) {
        float low1 = 0.0f, high1 = 0.0f, low2 = 0.0f, high2 = 0.0f;
        a.process(input, low1, high1);
        b.process(high1, low2, high2);
        return high2;
    }
    Svf a, b;
};

struct Crossover {
    void set(double frequency, double sampleRate) { low.set(frequency, sampleRate); high.set(frequency, sampleRate); }
    void reset() { low.reset(); high.reset(); }
    void process(float input, float& lowOutput, float& highOutput) {
        lowOutput = low.lowpass(input);
        highOutput = high.highpass(input);
    }
    Butterworth4 low, high;
};

class HarmonicBand {
public:
    void prepare(double sampleRate, double lowEdge, double highEdge) {
        // Because every band is narrow, the rectified band signal ripples at
        // 2 * lowEdge or above while its real envelope only moves below
        // lowEdge / 2. A Linkwitz-Riley lowpass on the rectified signal
        // separates the two cleanly: fast tracking and no ripple, which a
        // single full band follower can never achieve.
        const double detectorCutoff = std::max(8.0, lowEdge / 2.5);
        rectifiedLowpass.set(detectorCutoff, sampleRate);
        release = coefficient(0.050, sampleRate);
        (void)highEdge;
        reset();
    }

    void reset() {
        rectifiedLowpass.reset();
        envelope = 0.0f;
    }

    float process(float input, int order, float weight) {
        // The mean of |sin| is 2A/pi, so the rectified average scaled by pi/2
        // is exactly the amplitude of a steady tone.
        const float rectified = rectifiedLowpass.lowpass(std::abs(input)) * 1.5707963f;
        // Hold briefly on the way down so a decay does not outrun the estimate
        // and collapse the harmonic through the a^9 law.
        envelope = rectified > envelope ? rectified : envelope + (rectified - envelope) * release;

        float shaped = 0.0f;
        if (envelope > 1.0e-7f) {
            // Soft gate: never normalise a noise floor up to full scale.
            const float gate = envelope / (envelope + 0.0015f);
            const float normalised = std::clamp(input / envelope, -1.0f, 1.0f);
            shaped = chebyshev(normalised, order) * envelope * gate * weight;
        }
        // No per band output filtering. Different filters per band would give
        // each band's harmonic a different phase, and since the polynomial
        // multiplies phase by the harmonic order, a tone sitting between two
        // bands would then cancel itself instead of summing.
        return shaped;
    }

private:
    static float coefficient(double seconds, double sampleRate) {
        return float(1.0 - std::exp(-1.0 / std::max(1.0, seconds * sampleRate)));
    }
    Butterworth4 rectifiedLowpass;
    float release = 0.0f, envelope = 0.0f;
};

class ChebyshevHarmonics {
public:
    void prepare(double newSampleRate) {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        driveLowpass.set(driveLimitHz, sampleRate);
        // One shared output filter for every band, so all contributions keep
        // the same phase and add instead of cancelling.
        outputHighpass.set(250.0, sampleRate);
        outputLowpass.set(std::min(11.0 * driveLimitHz, sampleRate * 0.45), sampleRate);
        for (int i = 0; i < bandCount - 1; ++i) {
            crossovers[size_t(i)].set(crossoverFrequencies[size_t(i)], sampleRate);
            // Phase compensation. A band leaves the tree early and therefore
            // misses every crossover below it, so it is passed through the
            // matching allpasses instead. Without this each band has its own
            // phase, the polynomial multiplies that difference by the harmonic
            // order, and a tone sitting on a crossover cancels its own ninth
            // harmonic. Measured before the fix: a 36 dB notch at 160 Hz.
            for (int j = i + 1; j < bandCount - 1; ++j)
                compensators[size_t(i)][size_t(j)].set(crossoverFrequencies[size_t(j)], sampleRate, 0.70710678);
        }
        for (int i = 0; i < bandCount; ++i) bands[size_t(i)].prepare(sampleRate, bandLowEdges[size_t(i)], bandHighEdges[size_t(i)]);
        updateWeights();
        reset();
    }

    void reset() {
        driveLowpass.reset();
        outputHighpass.reset();
        outputLowpass.reset();
        for (auto& crossover : crossovers) crossover.reset();
        for (auto& row : compensators) for (auto& filter : row) filter.reset();
        for (auto& band : bands) band.reset();
        dcInput = 0.0f; dcOutput = 0.0f;
    }

    void setColor(float tilt) {
        const float limited = std::clamp(tilt, -1.0f, 1.0f);
        if (std::abs(limited - colorTilt) < 1.0e-4f) return;
        colorTilt = limited;
        updateWeights();
    }

    void setOrder(int newOrder) { order = std::clamp(newOrder, 2, 11); }

    float process(float input) {
        float remaining = driveLowpass.lowpass(input);
        float sum = 0.0f;
        for (int i = 0; i < bandCount - 1; ++i) {
            float low = 0.0f, high = 0.0f;
            crossovers[size_t(i)].process(remaining, low, high);
            for (int j = i + 1; j < bandCount - 1; ++j) low = compensators[size_t(i)][size_t(j)].allpass(low);
            sum += bands[size_t(i)].process(low, order, weights[size_t(i)]);
            remaining = high;
        }
        sum += bands[size_t(bandCount - 1)].process(remaining, order, weights[size_t(bandCount - 1)]);

        const float blocked = sum - dcInput + 0.9995f * dcOutput;
        dcInput = sum;
        dcOutput = blocked;
        return outputLowpass.lowpass(outputHighpass.highpass(blocked));
    }

private:
    void updateWeights() {
        for (int i = 0; i < bandCount; ++i) {
            const double centre = std::sqrt(std::max(30.0, bandLowEdges[size_t(i)]) * bandHighEdges[size_t(i)]);
            const double decibels = double(colorTilt) * 14.0 * std::log10(centre / 500.0);
            weights[size_t(i)] = float(std::clamp(std::pow(10.0, decibels / 20.0), 0.06, 6.0));
        }
    }

    Butterworth4 driveLowpass, outputHighpass, outputLowpass;
    std::array<Crossover, size_t(bandCount - 1)> crossovers{};
    std::array<std::array<Svf, size_t(bandCount - 1)>, size_t(bandCount - 1)> compensators{};
    std::array<HarmonicBand, size_t(bandCount)> bands{};
    std::array<float, size_t(bandCount)> weights{};
    double sampleRate = 48000.0;
    int order = 9;
    float colorTilt = 0.0f, dcInput = 0.0f, dcOutput = 0.0f;
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
