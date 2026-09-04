// PitchLab 编辑器实现。界面仿 Melodyne：深色半音格线 + 彩色音高带 + 音分仪表。
#include "PluginEditor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

// ------- 调色板(与 Python 原型一致) -------
const juce::Colour BG(0xff171a20), PANEL(0xff1e222b), STRIP(0xff111419);
const juce::Colour GRID(0xff333a46), GRID_HI(0xff3f4756), LABEL(0xff8b95a3);
const juce::Colour GOOD(0xff35d07f), WARN(0xffffb224), BAD(0xffff5a6a);
const juce::Colour WHITE(0xffdfe5ec), DIMMED(0xff5c6573);

inline juce::Colour devColor(double cents) {
    double a = std::fabs(cents);
    if (a <= 12.0) return GOOD;
    if (a <= 40.0) return WARN;
    return BAD;
}

inline const char* verdict(double dev, double span) {
    if (span >= 1.0) return "drift";
    if (std::fabs(dev) <= 12.0) return "OK";
    if (std::fabs(dev) <= 40.0) return "~";
    return "!!";
}

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline std::string fmtF(double v, int prec) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    return std::string(b);
}

// 中值平滑 + 八度归位(对一段连续 voiced 的 midi 值)。
// baseline 为"全局稳健八度基线"(窗口内所有有声帧的中位音高)：
// 把"同一音高类别却整八度跳置"的毛刺折叠回基线八度——对独立成句的
// 短尖刺同样生效(它们不再因"本句中值就是尖刺"而漏网)。|rel|<3 视为同音高类别。
std::vector<double> smoothRun(const std::vector<double>& mv, double baseline) {
    const size_t n = mv.size();
    const int hw = 4;                                // 窗口 9
    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i) {
        size_t a = (size_t)std::max(0, (int)i - hw);
        size_t b = std::min(n, i + hw + 1);
        std::vector<double> w(mv.begin() + (long)a, mv.begin() + (long)b);
        std::sort(w.begin(), w.end());
        out[i] = w[w.size() / 2];
    }
    if (baseline > -1e8) {
        for (size_t i = 0; i < n; ++i) {
            double rel = out[i] - 12.0 * std::lround((out[i] - baseline) / 12.0);
            if (std::fabs(rel) < 3.0)
                out[i] = baseline + rel;
        }
    }
    return out;
}

} // namespace

PitchLabAudioProcessorEditor::PitchLabAudioProcessorEditor(PitchLabAudioProcessor& p)
    : AudioProcessorEditor(p), proc_(p) {
    setSize(1080, 560);
    setResizeLimits(760, 400, 2400, 1400);

    gateSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    gateSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    gateSlider_.setColour(juce::Slider::trackColourId, GOOD.darker(0.4f));
    gateSlider_.setColour(juce::Slider::thumbColourId, WHITE);
    addAndMakeVisible(gateSlider_);
    gateAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        proc_.apvts, "gate", gateSlider_);

    startTimerHz(30);
    refreshSnapshot();
}

// ---------------------------------------------------------------- 几何 ----
PitchLabAudioProcessorEditor::Rect PitchLabAudioProcessorEditor::plot() const {
    float w = (float)getWidth(), h = (float)getHeight();
    return {8.0f, 40.0f, w - 8.0f - 34.0f, h - 40.0f - 66.0f};
}

PitchLabAudioProcessorEditor::Rect PitchLabAudioProcessorEditor::gauge() const {
    float w = (float)getWidth();
    float top = 40.0f;
    return {w - 26.0f, top, 18.0f, (float)getHeight() - 40.0f - 66.0f};
}

void PitchLabAudioProcessorEditor::updateMaps() {
    plotRect_ = plot();
    tToPx_ = plotRect_.w / (float)(x1_ - x0_);
    yToPx_ = plotRect_.h / (float)(m1_ - m0_);
}

juce::Rectangle<int> PitchLabAudioProcessorEditor::btnRect(int idx) const {
    static const int ws[4] = {70, 70, 88, 70}; // Pause, Clear, Analyze, Fit
    const int gap = 6, top = 8, h = 24;
    int x = getWidth() - 10;
    for (int i = 3; i > idx; --i) x -= ws[i] + gap;
    x -= ws[idx];
    return {x, top, ws[idx], h};
}

// ---------------------------------------------------------------- 数据 ----
void PitchLabAudioProcessorEditor::refreshSnapshot() {
    auto s = proc_.grabSnapshot();
    t_.swap(s.t);
    f_.swap(s.f);
    hasHost_ = s.hasHost;
    hostPlaying_ = s.hostPlaying;
    hostTime_ = s.hostTime;
    hostBar_ = s.hostBar;
    hostBpm_ = s.hostBpm;
    hostPpq_ = s.hostPpq;
    hostLastBarStartPpq_ = s.hostLastBarStartPpq;
    hostNum_ = s.hostNum;
    hostDenom_ = s.hostDenom;
    if (t_.size() > 1) dt_ = t_[1] - t_[0];
    tEnd_ = t_.empty() ? 0.0 : t_.back();

    // x 窗口 & y 范围：
    //   AUTO(默认)：x 只在「未暂停」且「(无宿主 或 宿主正在播放/录音)」时滚动；宿主停止时冻结。
    //              y 做迟滞自动缩放(内容超出立即扩窗，收窄缓慢收缩)。
    //   手动(用户滚轮缩放 / Alt-拖动平移后)：x/y 完全由用户控制，不再被自动逻辑覆盖，
    //             以便静态放大查看某个区间的最高/最低点。Fit 按钮可回到 AUTO。
    if (autoFit_) {
        bool scroll = (!paused_) && (!hasHost_ || hostPlaying_);
        double anchor = hasHost_ ? hostTime_ : (t_.empty() ? 0.0 : t_.back());
        if (scroll) {
            x1_ = std::max(anchor + 0.15, 1.0);
            x0_ = std::max(0.0, x1_ - 10.0);
        } else if (paused_) {
            x1_ = pauseT_ + 0.15;
            x0_ = std::max(0.0, x1_ - 10.0);
        }
        // 宿主停止：保持当前 x0_/x1_ 不动(冻结)

        // y 范围：取当前窗口内有声部分，并做"迟滞自动缩放"——
        //   内容超出当前范围时立即扩窗(保证不裁切)，收窄时缓慢收缩(画面不抖)。
        //   上限放宽到 ±15 半音(2.5 个八度)，大跨度跳弦/滑音也不会跑出画面。
        double mn = 1e9, mx = -1e9;
        bool any = false;
        for (size_t i = 0; i < t_.size(); ++i) {
            if (f_[i] > 0.0 && t_[i] >= x0_ && t_[i] <= x1_) {
                double m = pitchlab::midiFromFreq(f_[i]);
                if (m < mn) mn = m;
                if (m > mx) mx = m;
                any = true;
            }
        }
        double tLo, tHi;
        if (any) {
            const double pad = 1.6;
            tLo = mn - pad;
            tHi = mx + pad;
            double span = tHi - tLo;
            if (span < 6.0) { double mid = (tLo + tHi) / 2.0; tLo = mid - 3.0; tHi = mid + 3.0; }
            if (span > 36.0) { double mid = (tLo + tHi) / 2.0; tLo = mid - 18.0; tHi = mid + 18.0; }
        } else if (!paused_) {
            tLo = 45.0; tHi = 57.0;
        } else {
            tLo = m0_; tHi = m1_;     // 暂停时冻结范围
        }
        // 迟滞：超出则立即扩到能容纳；否则按 20%/帧 向目标缓慢收缩
        m0_ = (tLo < m0_) ? tLo : m0_ + (tLo - m0_) * 0.20;
        m1_ = (tHi > m1_) ? tHi : m1_ + (tHi - m1_) * 0.20;
        if (m1_ - m0_ < 6.0) { double mid = (m0_ + m1_) / 2.0; m0_ = mid - 3.0; m1_ = mid + 3.0; }
    } else {
        // 手动模式：x/y 由用户控制；仅保证窗口合法
        if (x1_ <= x0_) x1_ = x0_ + 0.5;
        if (m1_ <= m0_) m1_ = m0_ + 3.0;
    }
    updateMaps();

    // ---- 实时 HUD：最近发声(0.6s 内) ----
    liveVoiced_ = false;
    int last = -1;
    for (int i = (int)t_.size() - 1; i >= 0 && t_[i] >= tEnd_ - 0.6; --i)
        if (f_[i] > 0.0f) { last = i; break; }
    if (last >= 0) {
        // 收集该句尾部连续 voiced(≤9 个) 取中值 → 平滑读数
        std::vector<double> tail;
        for (int i = last; i >= 0 && (int)tail.size() < 9 && f_[i] > 0.0f
             && (last - i) < 200; --i)
            tail.push_back(f_[i]);
        if (!tail.empty()) {
            std::sort(tail.begin(), tail.end());
            double fmed = tail[tail.size() / 2];
            double m = pitchlab::midiFromFreq(fmed);
            liveNoteMidi_ = m;
            liveCents_ = 100.0 * (m - std::round(m));
            liveVoiced_ = true;
        }
        // 揉弦：整段连续发声尾(≤0.9s)
        std::vector<double> runf;
        for (int i = last; i >= 0 && f_[i] > 0.0f && t_[last] - t_[i] < 0.9; --i)
            runf.push_back(f_[i]);
        if ((int)runf.size() >= 12) {
            auto vb = pitchlab::vibratoStats(runf, dt_);
            liveVibRate_ = vb.first;
            liveVibDepth_ = vb.second;
        } else {
            liveVibRate_ = liveVibDepth_ = -1.0;
        }
    } else {
        liveVibRate_ = liveVibDepth_ = -1.0;
    }
}

void PitchLabAudioProcessorEditor::timerCallback() {
    refreshSnapshot();
    repaint();
}

// ---------------------------------------------------------------- 动作 ----
void PitchLabAudioProcessorEditor::doPauseResume() {
    paused_ = !paused_;
    if (paused_) pauseT_ = tEnd_;
}

void PitchLabAudioProcessorEditor::doClear() {
    proc_.clearHistory();
    t_.clear(); f_.clear();
    tEnd_ = 0.0;
    selA_ = selB_ = -1.0;
    analysis_.clear();
    repaint();
}

void PitchLabAudioProcessorEditor::doAnalyze() {
    double a = selA_, b = selB_;
    if (a < 0 || b < 0 || b - a < 0.05) {
        a = std::max(0.0, tEnd_ - 8.0);
        b = tEnd_;
    }
    if (a > b) std::swap(a, b);
    analysis_ = pitchlab::analyzeRegion(t_, f_, a, b, dt_);
    analysisT0_ = a;
    analysisT1_ = b;
    repaint();
}

void PitchLabAudioProcessorEditor::doFit() {
    autoFit_ = true;          // 回到自动滚动 + 自动缩放
    if (paused_) pauseT_ = tEnd_;
    repaint();
}

bool PitchLabAudioProcessorEditor::hitButton(const juce::MouseEvent& e,
                                             juce::Rectangle<int>& out) {
    for (int i = 0; i < 4; ++i) {
        auto r = btnRect(i);
        if (r.contains(e.getPosition())) { out = r; return true; }
    }
    return false;
}

void PitchLabAudioProcessorEditor::mouseDown(const juce::MouseEvent& e) {
    juce::Rectangle<int> btn;
    if (hitButton(e, btn)) {
        if (btn == btnRect(0)) doPauseResume();
        else if (btn == btnRect(1)) doClear();
        else if (btn == btnRect(2)) doAnalyze();
        else if (btn == btnRect(3)) doFit();
        return;
    }
    auto p = plot();
    float px = e.position.x, py = e.position.y;
    if (p.contains(px, py)) {
        // 平移视图：Alt+左键 / 中键 / 右键 拖动(左键单击仍用于选区)
        if (e.mods.isAltDown() || e.mods.isMiddleButtonDown() || e.mods.isRightButtonDown()) {
            autoFit_ = false;              // 进入手动视图
            panning_ = true;
            panStartX_ = px; panStartY_ = py;
            panStartX0_ = x0_; panStartX1_ = x1_;
            panStartM0_ = m0_; panStartM1_ = m1_;
            return;
        }
        dragging_ = true;
        double t = x0_ + (px - p.x) / p.w * (x1_ - x0_);
        selA_ = selB_ = clampd(t, 0.0, std::max(tEnd_, 10.0));
        analysis_.clear();
        repaint();
    }
}

void PitchLabAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e) {
    if (panning_) {
        auto p = plot();
        // 拖动平移：内容跟随鼠标移动(左拖看更早时间，下拖看更高音高)
        double dtw = (e.position.x - panStartX_) / p.w * (panStartX1_ - panStartX0_);
        double dm  = (e.position.y - panStartY_) / p.h * (panStartM1_ - panStartM0_);
        x0_ = panStartX0_ - dtw;
        x1_ = panStartX1_ - dtw;
        m0_ = panStartM0_ + dm;
        m1_ = panStartM1_ + dm;
        updateMaps();
        repaint();
        return;
    }
    if (!dragging_) return;
    auto p = plot();
    float px = e.position.x;
    double t = x0_ + (px - p.x) / p.w * (x1_ - x0_);
    selB_ = clampd(t, 0.0, std::max(tEnd_, 10.0));
    repaint();
}

void PitchLabAudioProcessorEditor::mouseUp(const juce::MouseEvent&) {
    panning_ = false;
    if (!dragging_) return;
    dragging_ = false;
    if (selA_ >= 0 && selB_ >= 0 && std::fabs(selB_ - selA_) < 0.04)
        selA_ = selB_ = -1.0; // 视为单击，取消选区
    repaint();
}

void PitchLabAudioProcessorEditor::mouseWheelMove(const juce::MouseEvent& e,
                                                  const juce::MouseWheelDetails& w) {
    auto p = plot();
    float px = e.position.x, py = e.position.y;
    if (!p.contains(px, py)) return;       // 只在网格区内缩放
    autoFit_ = false;                       // 进入手动视图
    const double factor = std::exp(-w.deltaY * 0.25); // 滚轮↑=放大(范围变小)，↓=缩小
    if (e.mods.isCtrlDown() || e.mods.isAltDown()) {
        // 缩放时间轴 X(以光标所在时刻为锚点)
        double refr = (px - p.x) / p.w;
        double tCur = x0_ + refr * (x1_ - x0_);
        double nd = clampd((x1_ - x0_) * factor, 0.2, 120.0);
        x0_ = tCur - refr * nd;
        x1_ = x0_ + nd;
    } else {
        // 缩放音高轴 Y(以光标处音高为锚点)
        double refr = (py - p.y) / p.h;
        double mCur = m1_ - refr * (m1_ - m0_);
        double nd = clampd((m1_ - m0_) * factor, 2.0, 60.0);
        m1_ = mCur + refr * nd;
        m0_ = m1_ - nd;
    }
    updateMaps();
    repaint();
}

void PitchLabAudioProcessorEditor::resized() {
    updateMaps();
    gateSlider_.setBounds(getWidth() - 200, getHeight() - 24, 170, 18);
}

// ---------------------------------------------------------------- 绘制 ----
void PitchLabAudioProcessorEditor::paint(juce::Graphics& g) {
    g.fillAll(BG);
    auto p = plot();
    g.setColour(PANEL);
    g.fillRect(juce::Rectangle<float>(p.x, p.y, p.w, p.h));
    paintGrid(g);
    paintRibbons(g);
    paintSelection(g);
    paintAnalysis(g);
    paintGauge(g);
    paintHud(g);
    paintReport(g);
    paintButtons(g);

    // 门限滑块标签
    g.setColour(LABEL);
    g.setFont(juce::Font(10.0f));
    g.drawText("gate dB", getWidth() - 200, getHeight() - 38, 120, 12,
               juce::Justification::centredLeft);
}

void PitchLabAudioProcessorEditor::paintGrid(juce::Graphics& g) {
    auto p = plot();
    int lo = (int)std::ceil(m0_), hi = (int)std::floor(m1_);
    g.setFont(juce::Font(9.0f));
    for (int m = lo; m <= hi; ++m) {
        float y = yToPx((double)m);
        if (y < p.y || y > p.y + p.h) continue;
        bool strong = (m % 12 == 0 || m % 12 == 7);
        g.setColour(strong ? GRID_HI : GRID);
        g.drawHorizontalLine((int)y, p.x, p.x + p.w);
        g.setColour(LABEL);
        g.drawText(juce::String(pitchlab::noteName((double)m)),
                   p.x + 3, y - 6, 30, 12, juce::Justification::centredLeft);
    }
    // 秒刻度(图内底部)
    g.setColour(DIMMED);
    g.setFont(juce::Font(8.0f));
    g.drawText("time (s)", p.x + 4, p.y + p.h - 13, 52, 11,
               juce::Justification::centredLeft);
    for (int s = (int)std::ceil(x0_); s <= (int)x1_; ++s) {
        float px = xToPx((double)s);
        if (px < p.x + 8 || px > p.x + p.w - 8) continue;
        g.drawText(juce::String(s), px - 15, p.y + p.h - 12, 30, 11,
                   juce::Justification::centred);
    }

    // 小节刻度：与宿主时间轴同步(仅当宿主提供播放头 + BPM 时)
    if (hasHost_ && hostBpm_ > 1.0 && hostNum_ > 0 && hostDenom_ > 0) {
        double spb = 60.0 / hostBpm_;                          // 秒/拍
        double beatsPerBar = hostNum_ * 4.0 / (double)hostDenom_;
        double barDur = spb * beatsPerBar;
        if (barDur > 0.05) {
            double ppqInBar = hostPpq_ - hostLastBarStartPpq_; // 当前小节内经过的拍数
            double curBarStartTime = hostTime_ - ppqInBar * spb;
            g.setFont(juce::Font(8.5f).boldened());
            for (juce::int64 b = hostBar_ - 24; b <= hostBar_ + 1; ++b) {
                double bt = curBarStartTime + (double)(b - hostBar_) * barDur;
                if (bt < x0_ || bt > x1_) continue;
                float px = xToPx(bt);
                if (px < p.x + 8 || px > p.x + p.w - 8) continue;
                g.setColour(GRID_HI.withAlpha(0.7f));
                g.drawVerticalLine((int)px, p.y, p.y + p.h - 16);
                g.setColour(LABEL.withAlpha(0.85f));
                g.drawText(juce::String(b), px - 12, p.y + p.h - 15, 30, 12,
                           juce::Justification::centredLeft);
            }
        }
    }
}

void PitchLabAudioProcessorEditor::paintRibbons(juce::Graphics& g) {
    auto p = plot();
    float lw = clampd(p.h / (float)(m1_ - m0_) * 0.14f, 3.0f, 12.0f);
    juce::Path paths[3]; // good / warn / bad

    // 按"连续 voiced 段"分组，段内做窗口7平滑再上色
    size_t i = 0;
    const size_t n = t_.size();
    auto addSeg = [&](int col, double ta, double ma, double tb, double mb) {
        float xa = xToPx(ta), ya = yToPx(ma);
        float xb = xToPx(tb), yb = yToPx(mb);
        if (xa < p.x && xb < p.x) return;
        if (xa > p.x + p.w && xb > p.x + p.w) return;
        paths[col].addLineSegment(juce::Line<float>(xa, ya, xb, yb), 1.0f);
    };
    // 全局稳健八度基线 = 当前窗口内所有有声帧的中位音高(不被个别尖刺拖偏)
    double baseline = -1e9;
    {
        std::vector<double> all;
        for (size_t k = 0; k < n; ++k)
            if (f_[k] > 0.0f && t_[k] >= x0_ && t_[k] <= x1_)
                all.push_back(pitchlab::midiFromFreq(f_[k]));
        if (!all.empty()) {
            std::sort(all.begin(), all.end());
            baseline = all[all.size() / 2];
        }
    }

    while (i < n) {
        if (f_[i] <= 0.0f) { ++i; continue; }
        size_t j = i;
        while (j < n && f_[j] > 0.0f) ++j;
        // 段 [i, j)
        std::vector<double> mv;
        mv.reserve(j - i);
        for (size_t k = i; k < j; ++k) mv.push_back(pitchlab::midiFromFreq(f_[k]));
        auto sm = smoothRun(mv, baseline);
        for (size_t k = i; k + 1 < j; ++k) {
            double c0 = (sm[k - i] - std::round(sm[k - i])) * 100.0;
            int col = std::fabs(c0) <= 12.0 ? 0 : (std::fabs(c0) <= 40.0 ? 1 : 2);
            addSeg(col, t_[k], sm[k - i], t_[k + 1], sm[k - i + 1]);
        }
        i = j;
    }
    const juce::Colour cols[3] = {GOOD, WARN, BAD};
    for (int c = 0; c < 3; ++c) {
        if (paths[c].isEmpty()) continue;
        g.setColour(cols[c].withAlpha(0.92f));
        g.strokePath(paths[c], juce::PathStrokeType(lw, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }
}

void PitchLabAudioProcessorEditor::paintSelection(juce::Graphics& g) {
    if (selA_ < 0 || selB_ < 0) return;
    auto p = plot();
    double a = std::min(selA_, selB_), b = std::max(selA_, selB_);
    float xa = xToPx(a), xb = xToPx(b);
    if (xb < p.x || xa > p.x + p.w) return;
    g.setColour(WHITE.withAlpha(0.10f));
    g.fillRect(juce::Rectangle<float>(xa, p.y, xb - xa, p.h));
    g.setColour(WHITE.withAlpha(0.7f));
    g.drawRect(juce::Rectangle<float>(xa, p.y, xb - xa, p.h), 1.0f);
    g.setFont(juce::Font(10.0f));
    g.setColour(WHITE);
    juce::String label = juce::String(a, 2) + "-" + juce::String(b, 2) + " s  (drag)";
    g.drawText(label, juce::Rectangle<float>(std::max(p.x, xa), p.y + 2, 150, 14),
               juce::Justification::centredLeft);
}

void PitchLabAudioProcessorEditor::paintAnalysis(juce::Graphics& g) {
    if (analysis_.empty()) return;
    g.setFont(juce::Font(9.5f));
    for (auto& st : analysis_) {
        if (st.t1 < x0_ || st.t0 > x1_) continue;
        double m = st.midi;
        float ya = yToPx(m - 0.5);
        if (ya < plot().y || ya > plot().y + plot().h) continue;
        juce::Colour col = devColor(st.devCents);
        // 音符区间括号线
        float xa = std::max(plot().x, xToPx(st.t0));
        float xb = std::min(plot().x + plot().w, xToPx(st.t1));
        g.setColour(col.withAlpha(0.5f));
        g.drawHorizontalLine((int)ya, xa, xb);
        // 标签
        juce::String txt = pitchlab::noteName(m);
        txt << " " << juce::String(st.devCents, 0) << "\xc2\xa2";
        if (st.vibRate > 0)
            txt << "  " << juce::String(st.vibRate, 1) << "Hz \xc2\xb1" << juce::String(st.vibDepth, 0) << "\xc2\xa2";
        float tw = (float)txt.length() * 5.6f + 10.0f;
        float ty = yToPx(m + 0.5);
        juce::Rectangle<float> r(std::max(plot().x, xa), std::min(ty, plot().y + plot().h - 16),
                                 tw, 15);
        g.setColour(juce::Colour(0xff101318).withAlpha(0.92f));
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(col);
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        g.drawText(txt, r, juce::Justification::centred);
    }
}

void PitchLabAudioProcessorEditor::paintHud(juce::Graphics& g) {
    auto p = plot();
    juce::String main;
    juce::Colour col = WHITE;
    if (liveVoiced_) {
        main = pitchlab::noteName(liveNoteMidi_);
        main << "  " << juce::String(liveCents_, 0) << "\xc2\xa2";
        col = devColor(liveCents_);
        if (liveVibRate_ > 0)
            main << "   v " << juce::String(liveVibRate_, 1) << "Hz \xc2\xb1"
                 << juce::String(liveVibDepth_, 0) << "\xc2\xa2";
    } else {
        main = "—";
        col = DIMMED;
    }
    g.setColour(col);
    g.setFont(juce::Font(20.0f).boldened());
    g.drawText(main, juce::Rectangle<float>(p.x + 8, p.y + 6, p.w - 250, 22),
               juce::Justification::centredLeft);
    g.setColour(DIMMED);
    g.setFont(juce::Font(10.5f));
    juce::String sub = paused_ ? "PAUSED"
                      : (hasHost_ ? (hostPlaying_ ? "PLAYING" : "STOPPED") : "NO-HOST");
    if (hasHost_ && hostBpm_ > 1.0)
        sub << "  ·  bar " << (juce::int64)hostBar_ << "  " << (int)std::lround(hostBpm_) << "bpm";
    if (hasHost_)
        sub << "  ·  t=" << juce::String(hostTime_, 1) << "s";
    sub << (autoFit_ ? "  ·  [AUTO]" : "  ·  [ZOOM]");
    if (selA_ >= 0 && selB_ >= 0)
        sub << "  ·  sel " << juce::String(std::min(selA_, selB_), 2) << "-"
            << juce::String(std::max(selA_, selB_), 2) << "s";
    if (analysisT1_ > 0)
        sub << "  ·  analyzed " << (int)analysis_.size() << " note(s)";
    g.drawText(sub, juce::Rectangle<float>(p.x + 8, p.y + 28, p.w - 250, 13),
               juce::Justification::centredLeft);

    // 状态徽章(醒目，便于诊断)：绿=播放 琥珀=停止 灰=无宿主
    juce::String state;
    juce::Colour sc;
    if (paused_) { state = "PAUSED";  sc = WARN; }
    else if (!hasHost_) { state = "NO-HOST"; sc = DIMMED; }
    else if (hostPlaying_) { state = "PLAYING"; sc = GOOD; }
    else { state = "STOPPED"; sc = WARN; }
    float bw = (float)state.length() * 8.0f + 18.0f;
    juce::Rectangle<float> bg(p.x + p.w - bw - 6, p.y + 6, bw, 20);
    g.setColour(sc.withAlpha(0.16f));
    g.fillRoundedRectangle(bg, 4.0f);
    g.setColour(sc);
    g.drawRoundedRectangle(bg, 4.0f, 1.0f);
    g.setFont(juce::Font(12.0f).boldened());
    g.drawText(state, bg, juce::Justification::centred);
}

void PitchLabAudioProcessorEditor::paintGauge(juce::Graphics& g) {
    auto gr = gauge();
    struct Zone { double lo, hi; juce::Colour c; };
    Zone zones[] = {{-60, -40, BAD}, {-40, -12, WARN}, {-12, 12, GOOD},
                    {12, 40, WARN}, {40, 60, BAD}};
    for (auto& z : zones) {
        float y0 = gr.y + (float)((60.0 - z.hi) / 120.0) * gr.h;
        float y1 = gr.y + (float)((60.0 - z.lo) / 120.0) * gr.h;
        g.setColour(z.c.withAlpha(0.18f));
        g.fillRect(juce::Rectangle<float>(gr.x, y0, gr.w, y1 - y0));
    }
    // 刻度
    g.setColour(LABEL.withAlpha(0.6f));
    g.setFont(juce::Font(7.0f));
    for (double v : {-50.0, -25.0, 0.0, 25.0, 50.0}) {
        float y = gr.y + (float)((60.0 - v) / 120.0) * gr.h;
        g.drawHorizontalLine((int)y, gr.x, gr.x + gr.w * 0.35f);
        g.drawText(juce::String((int)v), gr.x - 2, y - 4, gr.w - 2, 8,
                   juce::Justification::centredRight);
    }
    double cents = liveVoiced_ ? liveCents_ : 0.0;
    float my = gr.y + (float)((60.0 - clampd(cents, -60, 60)) / 120.0) * gr.h;
    g.setColour(liveVoiced_ ? devColor(cents) : DIMMED);
    g.drawHorizontalLine((int)my, gr.x, gr.x + gr.w);
    g.fillRect(juce::Rectangle<float>(gr.x - 2, my - 2, gr.w + 4, 4));
}

void PitchLabAudioProcessorEditor::paintReport(juce::Graphics& g) {
    auto p = plot();
    float yTop = p.y + p.h + 6.0f;
    float w = (float)getWidth();
    g.setColour(STRIP);
    g.fillRect(0.0f, yTop - 3.0f, w, (float)getHeight() - yTop + 3.0f);
    g.setFont(juce::Font(11.0f));
    g.setColour(LABEL);
    g.drawText("drag=select · Alt+drag / 中键=pan · wheel=zoom pitch · Ctrl+wheel=zoom time · Fit=AUTO",
               p.x + 4, yTop, w - 220, 15, juce::Justification::centredLeft);
    float y = (float)getHeight() - 18.0f;
    if (analysis_.empty()) {
        g.setColour(DIMMED);
        g.drawText("—  drag to select a region and press Analyze  —",
                   p.x + 4, y - 4, w - 220, 14, juce::Justification::centredLeft);
    } else {
        int rows = 0;
        g.setFont(juce::Font(11.5f));
        for (int k = (int)analysis_.size() - 1; k >= 0 && rows < 8; --k, ++rows) {
            auto& st = analysis_[k];
            juce::String line;
            line << juce::String(st.t0, 2) << "-" << juce::String(st.t1, 2) << "s   "
                 << pitchlab::noteName(st.midi) << "  "
                 << juce::String(st.devCents, 0) << "\xc2\xa2";
            if (st.vibRate > 0)
                line << "   " << juce::String(st.vibRate, 1) << "Hz \xc2\xb1"
                     << juce::String(st.vibDepth, 0) << "\xc2\xa2";
            line << "   [" << verdict(st.devCents, st.spanSt) << "]";
            g.setColour(devColor(st.devCents));
            g.drawText(line, p.x + 4, y - 13, w - 240, 14,
                       juce::Justification::centredLeft);
            y -= 16.0f;
        }
    }
}

void PitchLabAudioProcessorEditor::paintButtons(juce::Graphics& g) {
    for (int i = 0; i < 4; ++i) {
        auto r = btnRect(i);
        bool accent = (i == 0 && paused_) || (i == 3 && !autoFit_);
        juce::Rectangle<float> rf = r.toFloat();
        g.setColour(accent ? WARN : juce::Colour(0xff262c38));
        g.fillRoundedRectangle(rf, 4.0f);
        g.setColour(accent ? juce::Colour(0xff3a3320) : GRID_HI);
        g.drawRoundedRectangle(rf, 4.0f, 1.0f);
        g.setColour(accent ? WARN : WHITE);
        g.setFont(juce::Font(11.5f).boldened());
        const char* label = i == 0 ? (paused_ ? "Resume" : "Pause")
                                   : (i == 1 ? "Clear"
                                      : (i == 2 ? "Analyze" : "Fit"));
        g.drawText(label, rf, juce::Justification::centred);
    }
}
