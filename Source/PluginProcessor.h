// PitchLab —— JUCE 插件处理器：音频直通 + 实时 YIN 音高检测。
#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "pitch/PitchDetector.h"

class PitchLabAudioProcessor : public juce::AudioProcessor,
                               public juce::AudioProcessorValueTreeState::Listener {
public:
    PitchLabAudioProcessor();
    ~PitchLabAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "PitchLab"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void parameterChanged(const juce::String&, float) override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    struct Snapshot {
        std::vector<double> t;   // 秒
        std::vector<double> f;   // Hz, <=0 无声
        double sampleRate = 0.0;
    };
    Snapshot grabSnapshot();
    void clearHistory();

private:
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::mutex mu_;
    pitchlab::PitchDetector det_;
    std::deque<pitchlab::Detection> hist_;
    double lastSr_ = 44100.0;
    float lastGateDb_ = -55.0f;
    std::atomic<bool> silent_ { true };
    static constexpr size_t kMaxHist = 24000; // ≈4.6 分钟 @86Hz
};
