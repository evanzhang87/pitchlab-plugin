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
        std::vector<double> t;   // 秒(宿主时间，若可用)
        std::vector<double> f;   // Hz, <=0 无声
        double sampleRate = 0.0;
        // 宿主播放头(传输同步)
        bool hasHost = false;
        bool hostPlaying = false;
        double hostTime = 0.0;          // 宿主时间(秒)
        juce::int64 hostBar = 0;        // 当前小节
        double hostBpm = 120.0;
        int hostNum = 4, hostDenom = 4;
        double hostPpq = 0.0;           // 当前四分音符位置
        double hostLastBarStartPpq = 0.0;
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

    // 宿主播放头状态(音频线程写入)
    bool hasHost_ = false;
    bool hostPlaying_ = false;
    double hostTimeSec_ = 0.0, hostBpm_ = 120.0;
    double hostPpq_ = 0.0, hostLastBarStartPpq_ = 0.0;
    juce::int64 hostBar_ = 0;
    int hostNum_ = 4, hostDenom_ = 4;
    double lastHostTimeSec_ = -1.0;      // 用于检测 seek 跳变
};
