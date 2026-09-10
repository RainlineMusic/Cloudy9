#include "../Source/SpectralHarmonic.h"
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <random>
#include <vector>

namespace {
constexpr double sr = 48000.0;
constexpr double twoPi = 2.0 * cloudy::kPi;

void check(bool value, const char* name) {
    if (!value) { std::cerr << "FAIL: " << name << '\n'; std::exit(1); }
}

// Block averaged magnitude: tolerant to small frequency errors of the
// resynthesised partials, unlike one long coherent projection.
double level(const std::vector<float>& x, double frequency, size_t begin) {
    constexpr size_t block = 4096;
    double total = 0.0; size_t blocks = 0;
    for (size_t start = begin; start + block <= x.size(); start += block) {
        double s = 0.0, c = 0.0;
        for (size_t n = 0; n < block; ++n) {
            const double phase = twoPi * frequency * double(start + n) / sr;
            s += x[start + n] * std::sin(phase);
            c += x[start + n] * std::cos(phase);
        }
        total += 2.0 * std::sqrt(s * s + c * c) / double(block);
        ++blocks;
    }
    return blocks > 0 ? total / double(blocks) : 0.0;
}

std::vector<float> render(std::initializer_list<double> tones, double amplitude, int harmonic,
                          float color = 0.0f, size_t length = 160000) {
    cloudy::SpectralHarmonics engine;
    engine.prepare(sr);
    engine.setOrder(harmonic);
    engine.setColor(color);
    std::vector<float> out(length);
    for (size_t n = 0; n < length; ++n) {
        double x = 0.0;
        for (double tone : tones) x += amplitude * std::sin(twoPi * tone * double(n) / sr);
        out[n] = engine.process(float(x));
    }
    return out;
}
} // namespace

int main() {
    constexpr size_t settle = 48000;

    {   // Single tone: only the selected harmonic exists in the wet path.
        const auto wet = render({500.0}, 0.5, 9);
        const double wanted = level(wet, 4500.0, settle);
        const double h8 = level(wet, 4000.0, settle);
        const double h10 = level(wet, 5000.0, settle);
        const double h11 = level(wet, 5500.0, settle);
        const double fundamental = level(wet, 500.0, settle);
        std::cout << "H9=" << wanted << " H8=" << h8 << " H10=" << h10
                  << " H11=" << h11 << " F0=" << fundamental << '\n';
        check(wanted > 0.2, "selected harmonic is generated");
        check(h8 < wanted * 0.01, "no H8");
        check(h10 < wanted * 0.01, "no H10");
        check(h11 < wanted * 0.01, "no H11");
        check(fundamental < wanted * 0.01, "wet path carries no dry signal");
    }

    {   // Three tones: every source partial is mapped, no combination tones.
        const auto wet = render({250.0, 400.0, 550.0}, 0.3, 9);
        const double a = level(wet, 2250.0, settle);
        const double b = level(wet, 3600.0, settle);
        const double c = level(wet, 4950.0, settle);
        const double weakest = std::min(a, std::min(b, c));
        std::cout << "multitone H9: " << a << ' ' << b << ' ' << c << '\n';
        check(weakest > 0.05, "all three source tones produce their ninth harmonic");
        double worst = 0.0; double worstFrequency = 0.0;
        for (int k = 1; k < 9; ++k) {
            for (double pair : {400.0, 550.0}) {
                const double intermodulation = double(k) * 250.0 + double(9 - k) * pair;
                if (intermodulation >= 0.47 * sr) continue;
                const double value = level(wet, intermodulation, settle);
                if (value > worst) { worst = value; worstFrequency = intermodulation; }
            }
        }
        std::cout << "worst intermodulation " << worst << " at " << worstFrequency << " Hz\n";
        check(worst < weakest * 0.05, "combination tones stay below -26 dB");
    }

    {   // Four tones: the old complex-power engine exploded here.
        const auto wet = render({220.0, 330.0, 470.0, 610.0}, 0.25, 9);
        const double wanted = level(wet, 1980.0, settle);
        const double intermodulation = level(wet, 2090.0, settle); // 8*220 + 330
        std::cout << "four tone H9=" << wanted << " IMD=" << intermodulation << '\n';
        check(wanted > 0.04, "four tone harmonic present");
        check(intermodulation < wanted * 0.05, "four tone combination tone suppressed");
    }

    {   // Debug harmonic selector.
        const auto h2 = render({500.0}, 0.5, 2);
        check(level(h2, 1000.0, settle) > 0.2, "debug H2");
        const auto h11 = render({300.0}, 0.5, 11);
        check(level(h11, 3300.0, settle) > 0.2, "debug H11");
    }

    {   // Color tilts the spectral weighting of the source partials.
        const auto lowMinus = render({120.0}, 0.5, 9, -1.0f);
        const auto lowPlus = render({120.0}, 0.5, 9, 1.0f);
        const double lowA = level(lowMinus, 1080.0, settle), lowB = level(lowPlus, 1080.0, settle);
        const auto highMinus = render({1500.0}, 0.5, 9, -1.0f);
        const auto highPlus = render({1500.0}, 0.5, 9, 1.0f);
        const double highA = level(highMinus, 13500.0, settle), highB = level(highPlus, 13500.0, settle);
        std::cout << "color low(-/+): " << lowA << '/' << lowB
                  << " high(-/+): " << highA << '/' << highB << '\n';
        check(lowA > lowB * 1.2, "negative color favours low source content");
        check(highB > highA * 1.2, "positive color favours high source content");
    }

    {   // Noise must not be converted into a harmonic cloud.
        cloudy::SpectralHarmonics engine; engine.prepare(sr); engine.setOrder(9);
        std::mt19937 rng(9); std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
        double energy = 0.0; size_t counted = 0;
        for (size_t n = 0; n < 96000; ++n) {
            const float value = engine.process(dist(rng));
            check(std::isfinite(value), "finite output on noise");
            if (n > 48000) { energy += double(value) * double(value); ++counted; }
        }
        const double rms = std::sqrt(energy / double(counted));
        std::cout << "noise wet rms=" << rms << '\n';
        check(rms < 0.02, "broadband noise is rejected by the tonality gate");
    }

    {   // Silence in, silence out; no self oscillation.
        cloudy::SpectralHarmonics engine; engine.prepare(sr); engine.setOrder(9);
        for (int n = 0; n < 20000; ++n) check(std::abs(engine.process(0.0f)) < 1.0e-9f, "silent input stays silent");
    }

    {   // Latency compensated dry path is sample accurate.
        cloudy::DryDelay delay; delay.prepare(cloudy::spectralLatency);
        std::vector<float> input(4096);
        std::mt19937 rng(3); std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (auto& value : input) value = dist(rng);
        std::vector<float> delayed(input.size());
        for (size_t n = 0; n < input.size(); ++n) delayed[n] = delay.process({input[n], input[n]})[0];
        for (size_t n = size_t(cloudy::spectralLatency); n < input.size(); ++n)
            check(std::abs(delayed[n] - input[n - size_t(cloudy::spectralLatency)]) < 1.0e-7f, "dry delay matches reported latency");
    }

    std::cout << "PASS Cloudy9 spectral harmonic tests\n";
}
