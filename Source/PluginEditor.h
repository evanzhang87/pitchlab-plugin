// PitchLab —— 编辑器：Melodyne 式音高网格 + 选区分析(A/B 两功能)。
#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "pitch/Analysis.h"
#include "PluginProcessor.h"

class PitchLabAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer {
public:
    explicit PitchLabAudioProcessorEditor(PitchLabAudioProcessor&);
    ~PitchLabAudioProcessorEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;
    void refreshSnapshot();

    // ---- 坐标 ----
    struct Rect {
        float x, y, w, h;
        bool contains(float px, float py) const {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
    };
    Rect plot() const;                       // 主音高网格区
    Rect gauge() const;                      // 右侧音分仪表区
    void updateMaps();                       // 由当前窗口重算映射系数
    float xToPx(double t) const { return plotRect_.x + (float)(t - x0_) * tToPx_; }
    float yToPx(double m) const { return plotRect_.y + (float)(m1_ - m) * yToPx_; }

    // ---- 动作 ----
    void doPauseResume();
    void doClear();
    void doAnalyze();
    void doFit();                        // 恢复自动视图(Auto-fit)
    bool hitButton(const juce::MouseEvent&, juce::Rectangle<int>& out);
    juce::Rectangle<int> btnRect(int idx) const;

    // ---- 绘制子过程 ----
    void paintGrid(juce::Graphics&);
    void paintRibbons(juce::Graphics&);
    void paintSelection(juce::Graphics&);
    void paintAnalysis(juce::Graphics&);
    void paintHud(juce::Graphics&);
    void paintGauge(juce::Graphics&);
    void paintReport(juce::Graphics&);
    void paintButtons(juce::Graphics&);

    PitchLabAudioProcessor& proc_;

    // 快照数据(UI 线程本地副本)
    std::vector<double> t_, f_;
    double dt_ = 0.0116;
    double tEnd_ = 0.0;
    double x0_ = 0.0, x1_ = 10.0;   // 时间窗口(秒)
    double m0_ = 45.0, m1_ = 57.0;  // 音高窗口(半音 MIDI)
    float tToPx_ = 0.0f, yToPx_ = 0.0f;
    Rect plotRect_ {0, 0, 0, 0};

    bool paused_ = false;
    double pauseT_ = 0.0;
    double selA_ = -1.0, selB_ = -1.0; // 选区(秒)，<0 表示无
    bool dragging_ = false;

    // ---- 视图缩放/平移 ----
    bool autoFit_ = true;                // true=自动滚动/自动缩放；false=用户手动视图
    bool panning_ = false;               // 正在 Alt/中键拖动平移
    float panStartX_ = 0.0f, panStartY_ = 0.0f;
    double panStartX0_ = 0.0, panStartX1_ = 0.0;
    double panStartM0_ = 0.0, panStartM1_ = 0.0;

    std::vector<pitchlab::NoteStat> analysis_; // 选区分析结果
    double analysisT0_ = 0.0, analysisT1_ = 0.0;

    // 宿主播放头(传输同步)
    bool hasHost_ = false;
    bool hostPlaying_ = false;
    double hostTime_ = 0.0, hostBpm_ = 120.0;
    double hostPpq_ = 0.0, hostLastBarStartPpq_ = 0.0;
    juce::int64 hostBar_ = 0;
    int hostNum_ = 4, hostDenom_ = 4;

    // 实时 HUD
    double liveNoteMidi_ = -1.0;
    double liveCents_ = 0.0;
    double liveVibRate_ = -1.0, liveVibDepth_ = -1.0;
    bool liveVoiced_ = false;

    juce::Slider gateSlider_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gateAttach_;
};
