#pragma once
#include <JuceHeader.h>
#include "SpectralHarmonic.h"

class Cloudy9AudioProcessor final : public juce::AudioProcessor {
public:
    Cloudy9AudioProcessor();
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override { processAudio(b, m, true); }
    juce::AudioProcessorEditor* createEditor() override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return parameters.getParameter("bypass"); }
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<bool> displayBypass{false};
    std::atomic<int> editorWidth{0};
private:
    std::array<cloudy::SpectralHarmonics, 2> engines;
    cloudy::DryDelay dryDelay;
    juce::SmoothedValue<float> amountSmooth, outputSmooth;
    std::atomic<float>* amount = nullptr;
    std::atomic<float>* color = nullptr;
    std::atomic<float>* output = nullptr;
    std::atomic<float>* harmonic = nullptr;
    std::atomic<float>* bypass = nullptr;
    void processAudio(juce::AudioBuffer<float>&, juce::MidiBuffer&, bool);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Cloudy9AudioProcessor)
};
