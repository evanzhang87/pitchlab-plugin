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

    // 读取宿主播放头：传输状态 + 时间线(秒/小节/拍号/BPM)
    bool play = true;
    double hostStamp = -1.0;    // <0 => 无宿主时间，用内部时钟
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            hasHost_ = true;
            hostPlaying_ = pos->getIsPlaying() || pos->getIsRecording();
            if (auto v = pos->getTimeInSeconds())      hostTimeSec_ = *v;
            if (auto v = pos->getBarCount())           hostBar_ = *v;
            if (auto v = pos->getBpm())                hostBpm_ = *v;
            if (auto v = pos->getPpqPosition())        hostPpq_ = *v;
            if (auto v = pos->getPpqPositionOfLastBarStart()) hostLastBarStartPpq_ = *v;
            if (auto v = pos->getTimeSignature()) { hostNum_ = v->numerator; hostDenom_ = v->denominator; }
            play = hostPlaying_;
            hostStamp = hostTimeSec_;
            // 定位跳变(seek / 循环跳回)：清空历史，避免新旧时间混在一起
            if (hostPlaying_ && lastHostTimeSec_ >= 0.0
                && std::fabs(hostTimeSec_ - lastHostTimeSec_) > 1.0) {
                std::lock_guard<std::mutex> lk(mu_);
                hist_.clear();
            }
            lastHostTimeSec_ = hostTimeSec_;
        } else {
            hasHost_ = false;
            play = true;
        }
    } else {
        hasHost_ = false;
        play = true;
    }

    // 只在宿主播放/录音时喂检测器；停止时清空缓冲(时间轴随宿主走，不空转)
    bool anyVoice = false;
    if (play) {
        if (buffer.getNumChannels() > 0 && n > 0)
            det_.process(buffer.getReadPointer(0), n);
        std::lock_guard<std::mutex> lk(mu_);
        auto outs = det_.take();
        for (auto& d : outs) {
            if (d.freq > 0.0f) anyVoice = true;
            if (hostStamp >= 0.0) d.time = hostStamp;   // 用宿主时间打时间戳
            hist_.push_back(d);
        }
        while (hist_.size() > kMaxHist)
            hist_.pop_front();
    } else {
        det_.reset();
    }
    silent_.store(!anyVoice, std::memory_order_relaxed);
}

PitchLabAudioProcessor::Snapshot PitchLabAudioProcessor::grabSnapshot() {
    Snapshot s;
    s.sampleRate = lastSr_;
    s.hasHost = hasHost_;
    s.hostPlaying = hostPlaying_;
    s.hostTime = hostTimeSec_;
    s.hostBar = hostBar_;
    s.hostBpm = hostBpm_;
    s.hostNum = hostNum_;
    s.hostDenom = hostDenom_;
    s.hostPpq = hostPpq_;
    s.hostLastBarStartPpq = hostLastBarStartPpq_;
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
