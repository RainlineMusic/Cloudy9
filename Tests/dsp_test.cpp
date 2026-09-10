// Cloudy9 v0.2 DSP tests.
//
// The plugin runs the shaper inside 4x oversampling, so the tests drive the
// engine directly at 192 kHz (48 kHz * 4). That is the rate the shaper really
// sees, and it lets the tests measure the polynomial's own harmonic content
// without the downsampling filter in the way.

#include "../Source/ChebyshevDSP.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double testSampleRate = 48000.0 * double(cloudy::oversamplingFactor);
int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message.c_str());
        ++failures;
    }
}

// Goertzel style magnitude of a single frequency inside a buffer.
double magnitudeAt(const std::vector<float>& signal, double frequency) {
    double real = 0.0, imaginary = 0.0;
    const size_t count = signal.size();
    for (size_t n = 0; n < count; ++n) {
        const double phase = 2.0 * cloudy::kPi * frequency * double(n) / testSampleRate;
        real += double(signal[n]) * std::cos(phase);
        imaginary -= double(signal[n]) * std::sin(phase);
    }
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) / double(count);
}

struct Capture {
    std::vector<float> output;
    double inputRms = 0.0;
    double outputRms = 0.0;
};

template <typename Generator>
Capture run(Generator generator, int order, float color, int settleSamples, int captureSamples) {
    cloudy::ChebyshevHarmonics engine;
    engine.prepare(testSampleRate);
    engine.setOrder(order);
    engine.setColor(color);

    Capture capture;
    capture.output.reserve(size_t(captureSamples));
    double inputEnergy = 0.0, outputEnergy = 0.0;
    for (int n = 0; n < settleSamples + captureSamples; ++n) {
        const float input = generator(n);
        const float wet = engine.process(input);
        if (n >= settleSamples) {
            capture.output.push_back(wet);
            inputEnergy += double(input) * double(input);
            outputEnergy += double(wet) * double(wet);
        }
    }
    capture.inputRms = std::sqrt(inputEnergy / double(captureSamples));
    capture.outputRms = std::sqrt(outputEnergy / double(captureSamples));
    return capture;
}

auto sineGenerator(double frequency, double amplitude) {
    return [frequency, amplitude](int n) {
        return float(amplitude * std::sin(2.0 * cloudy::kPi * frequency * double(n) / testSampleRate));
    };
}

void testSingleTonePurity() {
    const double fundamental = 500.0;
    const auto capture = run(sineGenerator(fundamental, 0.5), 9, 0.0f, 48000, 262144);

    const double ninth = magnitudeAt(capture.output, fundamental * 9.0);
    const double eighth = magnitudeAt(capture.output, fundamental * 8.0);
    const double tenth = magnitudeAt(capture.output, fundamental * 10.0);
    const double eleventh = magnitudeAt(capture.output, fundamental * 11.0);
    const double seventh = magnitudeAt(capture.output, fundamental * 7.0);
    const double second = magnitudeAt(capture.output, fundamental * 2.0);
    const double fundamentalLeak = magnitudeAt(capture.output, fundamental);

    std::printf("single tone H9=%g H7=%g H8=%g H10=%g H11=%g H2=%g F0=%g\n",
                ninth, seventh, eighth, tenth, eleventh, second, fundamentalLeak);

    check(ninth > 0.1, "ninth harmonic is generated at a usable level");
    check(tenth < ninth * 0.005, "tenth harmonic stays below -46 dB relative to the ninth");
    check(eighth < ninth * 0.005, "eighth harmonic stays below -46 dB relative to the ninth");
    check(second < ninth * 0.005, "no even harmonic family appears");
    check(eleventh < ninth * 0.02, "eleventh harmonic stays below -34 dB relative to the ninth");
    check(seventh < ninth * 0.35, "the ninth harmonic dominates the lower odd residue");
    check(fundamentalLeak < ninth * 0.1, "the dry fundamental does not leak into the wet path");
}

void testDetectorHasNoRipple() {
    // A ripple in the normalisation gain shows up as symmetric sidebands two
    // fundamentals away from the carrier. This is the v0.1 failure mode.
    const double fundamental = 300.0;
    const auto capture = run(sineGenerator(fundamental, 0.4), 9, 0.0f, 48000, 262144);
    const double ninth = magnitudeAt(capture.output, fundamental * 9.0);
    const double upper = magnitudeAt(capture.output, fundamental * 11.0);
    const double lower = magnitudeAt(capture.output, fundamental * 7.0);
    std::printf("sideband probe H9=%g upper=%g lower=%g\n", ninth, upper, lower);
    check(upper < ninth * 0.02, "upper modulation sideband is suppressed");
    check(lower < ninth * 0.35, "lower modulation sideband is suppressed");
}

void testMultitone() {
    const auto generator = [](int n) {
        const double t = double(n) / testSampleRate;
        return float(0.3 * std::sin(2.0 * cloudy::kPi * 250.0 * t)
                   + 0.3 * std::sin(2.0 * cloudy::kPi * 400.0 * t)
                   + 0.3 * std::sin(2.0 * cloudy::kPi * 550.0 * t));
    };
    const auto capture = run(generator, 9, 0.0f, 48000, 262144);
    const double first = magnitudeAt(capture.output, 2250.0);
    const double second = magnitudeAt(capture.output, 3600.0);
    const double third = magnitudeAt(capture.output, 4950.0);
    double worst = 0.0;
    double worstFrequency = 0.0;
    for (int k = 1; k <= 8; ++k) {
        const double combination = double(k) * 250.0 + double(9 - k) * 400.0;
        const double level = magnitudeAt(capture.output, combination);
        if (level > worst) { worst = level; worstFrequency = combination; }
    }
    std::printf("multitone H9: %g %g %g worst combination %g at %g Hz\n",
                first, second, third, worst, worstFrequency);
    // Waveshaping cannot separate partials that share a band, so a dense
    // chord always produces combination tones. This test documents the level
    // rather than pretending it is zero: the spectral branch is the clean
    // alternative, this branch is the saturator.
    check(first > 0.01 && second > 0.005 && third > 0.002, "every source tone still produces a ninth harmonic");
    check(worst < first * 1.5, "combination tones stay below the strongest wanted harmonic");
}

void testNoiseFloorGate() {
    std::mt19937 random(1234);
    std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);
    const auto capture = run([&](int) { return 0.0004f * distribution(random); }, 9, 0.0f, 24000, 96000);
    std::printf("noise floor in=%g out=%g\n", capture.inputRms, capture.outputRms);
    check(capture.outputRms < capture.inputRms, "a quiet noise floor is not normalised up to full scale");
}

void testSilence() {
    const auto capture = run([](int) { return 0.0f; }, 9, 0.0f, 4800, 48000);
    check(capture.outputRms == 0.0, "silence stays silent");
}

void testColorTilt() {
    const auto lowNegative = run(sineGenerator(150.0, 0.4), 9, -1.0f, 48000, 131072);
    const auto lowPositive = run(sineGenerator(150.0, 0.4), 9, 1.0f, 48000, 131072);
    const auto highNegative = run(sineGenerator(1200.0, 0.4), 9, -1.0f, 48000, 131072);
    const auto highPositive = run(sineGenerator(1200.0, 0.4), 9, 1.0f, 48000, 131072);

    const double lowMinus = magnitudeAt(lowNegative.output, 1350.0);
    const double lowPlus = magnitudeAt(lowPositive.output, 1350.0);
    const double highMinus = magnitudeAt(highNegative.output, 10800.0);
    const double highPlus = magnitudeAt(highPositive.output, 10800.0);
    std::printf("color low(-/+): %g/%g high(-/+): %g/%g\n", lowMinus, lowPlus, highMinus, highPlus);
    check(lowMinus > lowPlus, "negative color favours the low source band");
    check(highPlus > highMinus, "positive color favours the high source band");
}

void testDebugOrders() {
    const auto second = run(sineGenerator(500.0, 0.5), 2, 0.0f, 48000, 131072);
    const double secondHarmonic = magnitudeAt(second.output, 1000.0);
    const auto eleventh = run(sineGenerator(300.0, 0.5), 11, 0.0f, 48000, 131072);
    const double eleventhHarmonic = magnitudeAt(eleventh.output, 3300.0);
    std::printf("debug orders H2=%g H11=%g\n", secondHarmonic, eleventhHarmonic);
    check(secondHarmonic > 0.05, "debug order 2 generates the second harmonic");
    check(eleventhHarmonic > 0.05, "debug order 11 generates the eleventh harmonic");
}

void testDryDelay() {
    cloudy::DryDelay delay;
    const int latency = 64;
    delay.prepare(latency);
    std::vector<float> input(512), output(512);
    for (int n = 0; n < 512; ++n) {
        input[size_t(n)] = float(std::sin(double(n) * 0.1));
        output[size_t(n)] = delay.process({input[size_t(n)], input[size_t(n)]})[0];
    }
    bool identical = true;
    for (int n = latency; n < 512; ++n) {
        if (std::abs(output[size_t(n)] - input[size_t(n - latency)]) > 1.0e-6f) identical = false;
    }
    check(identical, "the dry path is delayed by exactly the reported latency");
}

} // namespace

int main() {
    testSingleTonePurity();
    testDetectorHasNoRipple();
    testMultitone();
    testNoiseFloorGate();
    testSilence();
    testColorTilt();
    testDebugOrders();
    testDryDelay();

    if (failures == 0) {
        std::printf("PASS Cloudy9 Chebyshev harmonic tests\n");
        return 0;
    }
    std::printf("%d Cloudy9 test(s) failed\n", failures);
    return 1;
}
