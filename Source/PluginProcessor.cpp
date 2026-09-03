#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <juce_core/juce_core.h>

#include <cmath>

namespace {
inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
} // namespace

PitchLabAudioProcessor::PitchLabAudioProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PitchLab", createParameterLayout()) {
    apvts.addParameterListener("gate", this);
}

juce::AudioProcessorValueTreeState::ParameterLayout
PitchLabAudioProcessor::createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "gate", "静音门限 (dB)", juce::NormalisableRange<float>(-70.0f, -30.0f, 0.1f), -55.0f));
    return layout;
}

void PitchLabAudioProcessor::parameterChanged(const juce::String&, float) {}

void PitchLabAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    juce::String text = xml->toString();
    destData.append(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

void PitchLabAudioProcessor::setStateInformation(const void* data, int sizeInBytes) {
    juce::String text(static_cast<const char*>(data), sizeInBytes);
    auto xml = juce::XmlDocument::parse(text);
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

void PitchLabAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/) {
    lastSr_ = sampleRate;
    det_.prepare(sampleRate);
    if (auto* g = apvts.getRawParameterValue("gate")) {
        lastGateDb_ = g->load();
        det_.setGate(dbToLin(lastGateDb_));
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        hist_.clear();
    }
}

void PitchLabAudioProcessor::releaseResources() {}

bool PitchLabAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    if (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::disabled())
        return false;
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void PitchLabAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& /*midi*/) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    // 参数刷新
    if (auto* g = apvts.getRawParameterValue("gate")) {
        float gd = g->load();
        if (gd != lastGateDb_) {
            lastGateDb_ = gd;
            det_.setGate(dbToLin(gd));
        }
    }

    // 直通（buffer 本身即输入输出）
    // 取第 1 声道送入检测器（立体声吉他轨两声道相同；若不同则建议用单声道轨）
    if (buffer.getNumChannels() > 0 && n > 0)
        det_.process(buffer.getReadPointer(0), n);

    bool anyVoice = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto outs = det_.take();
        for (auto& d : outs) {
            if (d.freq > 0.0f) anyVoice = true;
            hist_.push_back(d);
        }
        while (hist_.size() > kMaxHist)
            hist_.pop_front();
    }
    silent_.store(!anyVoice, std::memory_order_relaxed);
}

PitchLabAudioProcessor::Snapshot PitchLabAudioProcessor::grabSnapshot() {
    Snapshot s;
    s.sampleRate = lastSr_;
    {
        std::lock_guard<std::mutex> lk(mu_);
        s.t.reserve(hist_.size());
        s.f.reserve(hist_.size());
        for (auto& d : hist_) {
            s.t.push_back(d.time);
            s.f.push_back(d.freq);
        }
    }
    return s;
}

void PitchLabAudioProcessor::clearHistory() {
    std::lock_guard<std::mutex> lk(mu_);
    hist_.clear();
}

juce::AudioProcessorEditor* PitchLabAudioProcessor::createEditor() {
    return new PitchLabAudioProcessorEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new PitchLabAudioProcessor();
}
