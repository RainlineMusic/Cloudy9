#include "PluginProcessor.h"
#include "PluginEditor.h"

Cloudy9AudioProcessor::Cloudy9AudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", layout()) {
    amount = parameters.getRawParameterValue("amount");
    color = parameters.getRawParameterValue("color");
    output = parameters.getRawParameterValue("outputGain");
    harmonic = parameters.getRawParameterValue("harmonic");
    bypass = parameters.getRawParameterValue("bypass");
}

juce::AudioProcessorValueTreeState::ParameterLayout Cloudy9AudioProcessor::layout() {
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterFloat>("amount", "Harmonic Amount", juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("color", "Color", juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterFloat>("outputGain", "Output Gain", juce::NormalisableRange<float>(-100.0f, 6.0f, 0.01f, 4.94f), 0.0f));
    p.push_back(std::make_unique<juce::AudioParameterChoice>("harmonic", "Debug Harmonic", juce::StringArray{"2", "3", "4", "5", "6", "7", "8", "9", "10", "11"}, 7));
    p.push_back(std::make_unique<juce::AudioParameterBool>("bypass", "Bypass", false));
    return {p.begin(), p.end()};
}

void Cloudy9AudioProcessor::prepareToPlay(double sampleRate, int) {
    for (auto& engine : engines) engine.prepare(sampleRate);
    dryDelay.prepare(cloudy::spectralLatency);
    setLatencySamples(cloudy::spectralLatency);
    amountSmooth.reset(sampleRate, 0.020);
    outputSmooth.reset(sampleRate, 0.020);
    amountSmooth.setCurrentAndTargetValue(amount->load() * 0.01f);
    const float db = output->load();
    outputSmooth.setCurrentAndTargetValue(db <= -99.995f ? 0.0f : juce::Decibels::decibelsToGain(db));
}

void Cloudy9AudioProcessor::releaseResources() {}

void Cloudy9AudioProcessor::reset() {
    for (auto& engine : engines) engine.reset();
    dryDelay.reset();
}

bool Cloudy9AudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const {
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void Cloudy9AudioProcessor::processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& m) { processAudio(b, m, false); }

void Cloudy9AudioProcessor::processAudio(juce::AudioBuffer<float>& b, juce::MidiBuffer&, bool hostBypass) {
    juce::ScopedNoDenormals noDenormals;
    const int samples = b.getNumSamples();
    if (samples <= 0) return;
    displayBypass.store(hostBypass, std::memory_order_relaxed);
    const bool bypassed = hostBypass || bypass->load() > 0.5f;
    const int order = juce::jlimit(2, 11, 2 + juce::roundToInt(harmonic->load()));
    const float tilt = juce::jlimit(-1.0f, 1.0f, color->load() * 0.01f);
    for (auto& engine : engines) { engine.setOrder(order); engine.setColor(tilt); }

    amountSmooth.setTargetValue(amount->load() * 0.01f);
    const float db = output->load();
    outputSmooth.setTargetValue(db <= -99.995f ? 0.0f : juce::Decibels::decibelsToGain(db));

    const int channels = b.getNumChannels();
    for (int n = 0; n < samples; ++n) {
        const float left = b.getSample(0, n);
        const float right = channels > 1 ? b.getSample(1, n) : left;
        const float wetLeft = engines[0].process(left);
        const float wetRight = engines[1].process(right);
        const auto dry = dryDelay.process({left, right});
        const float mix = amountSmooth.getNextValue();
        const float wetGain = 0.75f * mix * mix;
        const float gain = outputSmooth.getNextValue();
        if (channels > 0) b.setSample(0, n, bypassed ? dry[0] : (dry[0] + wetLeft * wetGain) * gain);
        if (channels > 1) b.setSample(1, n, bypassed ? dry[1] : (dry[1] + wetRight * wetGain) * gain);
    }
}

void Cloudy9AudioProcessor::getStateInformation(juce::MemoryBlock& d) {
    auto state = parameters.copyState(); state.setProperty("uiWidth", editorWidth.load(), nullptr);
    if (auto x = state.createXml()) copyXmlToBinary(*x, d);
}
void Cloudy9AudioProcessor::setStateInformation(const void* d, int n) {
    if (auto x = getXmlFromBinary(d, n)) if (x->hasTagName(parameters.state.getType())) {
        auto state = juce::ValueTree::fromXml(*x); editorWidth.store(int(state.getProperty("uiWidth", 0))); parameters.replaceState(state);
    }
}
juce::AudioProcessorEditor* Cloudy9AudioProcessor::createEditor() { return new Cloudy9AudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new Cloudy9AudioProcessor(); }
