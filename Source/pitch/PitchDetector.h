// PitchLab —— 纯 C++ 音高检测核心（无 JUCE 依赖，可独立单元测试）。
// YIN 算法，与 Python 原型逐行对齐（已在合成吉他信号上验证 0¢ 误差）。
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace pitchlab {

inline constexpr double kA4Hz = 440.0;

inline double midiFromFreq(double f) { return 69.0 + 12.0 * std::log2(f / kA4Hz); }
inline double freqFromMidi(double m) { return kA4Hz * std::pow(2.0, (m - 69.0) / 12.0); }

struct Detection {
    double time = 0.0;   // 秒
    float  freq  = 0.0f; // Hz；<=0 表示无声/未发声
};

// ---------------------------------------------------------------------------
// 纯函数：对一帧(建议 >=2048 样本)做 YIN 基频检测。返回 Hz，失败返回 0。
// 与 Python 版语义一致：找 cmndf 第一个"局部极小且低于阈值"的谷底(而非下降沿
// 穿越)，无谷底时退化为全局最小；抛物线插值精化。
// ---------------------------------------------------------------------------
inline double yinFreq(const float* x, int n, double sr,
                      double fmin = 60.0, double fmax = 1500.0,
                      double thresh = 0.15, double* confOut = nullptr) {
    const int tauMin = std::max(2, (int)std::floor(sr / fmax));
    const int tauMax = std::min(n - 3, (int)std::ceil(sr / fmin));
    if (tauMax <= tauMin + 1) { if (confOut) *confOut = 1.0; return 0.0; }

    // 前缀和，用于快速算 Σx^2 的区间和
    std::vector<double> pref(n + 1, 0.0);
    for (int i = 0; i < n; ++i) pref[i + 1] = pref[i] + (double)x[i] * (double)x[i];

    // d(tau) = Σ_{i=0}^{n-tau-1} (x[i]-x[i+tau])^2 = S1+S2-2*dot
    std::vector<double> d(tauMax + 1, 0.0);
    std::vector<double> dot(tauMax + 1, 0.0);
    // 注意：d(1..tauMin-1) 也要正确(它们进入 cmndf 的累积和)，故点积从 tau=1 起算
    for (int i = 0; i < n; ++i) {
        const double xi = x[i];
        const int tmax = std::min(tauMax, n - 1 - i);
        for (int tau = 1; tau <= tmax; ++tau)
            dot[tau] += xi * (double)x[i + tau];
    }
    for (int tau = 1; tau <= tauMax; ++tau) {
        double s1 = pref[n - tau] - pref[0];
        double s2 = pref[n] - pref[tau];
        double v = s1 + s2 - 2.0 * dot[tau];
        d[tau] = v > 0.0 ? v : 0.0;
    }

    // cmndf(tau) = d(tau)*tau / Σ_{1..tau} d
    std::vector<double> cmn(tauMax + 1, 0.0);
    {
        double csum = 0.0;
        for (int tau = 1; tau <= tauMax; ++tau) {
            csum += d[tau];
            cmn[tau] = d[tau] * tau / (csum + 1e-9);
        }
    }

    int tauOpt = -1;
    double conf = 1.0;                   // cmndf 谷底深度：周期信号≈0，噪声≈0.3~1
    const int lo = tauMin;              // 候选范围 [tauMin, tauMax]
    for (int tau = lo; tau <= tauMax; ++tau) {
        if (cmn[tau] >= thresh) continue;
        bool loOk = (tau == lo) || (cmn[tau] <= cmn[tau - 1]);
        bool hiOk = (tau == tauMax) || (cmn[tau] < cmn[tau + 1]);
        if (loOk && hiOk) { tauOpt = tau; break; }
    }
    if (tauOpt < 0) {
        int best = lo;
        for (int tau = lo + 1; tau <= tauMax; ++tau)
            if (cmn[tau] < cmn[best]) best = tau;
        if (cmn[best] < 1.0) { tauOpt = best; conf = cmn[best]; }
        else { if (confOut) *confOut = 1.0; return 0.0; }
    } else {
        conf = cmn[tauOpt];
    }

    // 抛物线插值精化（在 d 曲线上，样本点 tau-1, tau, tau+1）→ 保留亚采样精度
    double tauF = (double)tauOpt;
    if (tauOpt >= 2 && tauOpt <= tauMax - 1) {
        double s0 = d[tauOpt - 1], s1 = d[tauOpt], s2 = d[tauOpt + 1];
        double denom = 2.0 * (2.0 * s1 - s2 - s0);
        if (std::fabs(denom) > 1e-12) {
            double adj = (s2 - s0) / denom;
            if (adj < -1.0) adj = -1.0;
            if (adj > 1.0) adj = 1.0;
            tauF += adj;
        }
    }
    if (confOut) *confOut = conf;
    if (tauF <= 1.0) return 0.0;
    double f = sr / tauF;
    if (f >= fmin && f <= fmax) return f;
    return 0.0;
}

// ---------------------------------------------------------------------------
// 流式跟踪器：push 音频，内部按 hop 步长滑动检测，产出 Detection 序列。
// ---------------------------------------------------------------------------
class PitchDetector {
public:
    void prepare(double sampleRate) {
        sr_ = sampleRate;
        hopN_ = std::max(64, (int)std::lround(sr_ * 0.0116));  // ~512 @44.1k
        bufN_ = std::max(256, (int)std::lround(sr_ * 0.0464)); // ~2048 @44.1k
        pending_.clear();
        pending_.reserve(bufN_ + hopN_ * 8);
        out_.clear();
        t_ = 0.0;
    }

    void setGate(double lin) { gate_ = lin; }
    void setFreqRange(double fmin, double fmax) { fmin_ = fmin; fmax_ = fmax; }
    // 置信度门限：cmndf 谷底深度(0=极周期，1=纯噪声)。谷底太浅视为无声，
    // 避免把底噪/空音误判成超低音。默认 0.25。
    void setConfidenceThresh(double c) { confThresh_ = c; }
    double timeNow() const { return t_; }

    // 追加一段单声道音频（实时回调里调用；内部无分配：pending_ 已预留）
    void process(const float* data, int n) {
        for (int i = 0; i < n; ++i) pending_.push_back(data[i]);
        while ((int)pending_.size() >= bufN_) {
            const float* w = pending_.data();
            double rms = 0.0;
            for (int i = 0; i < bufN_; ++i) rms += (double)w[i] * (double)w[i];
            rms = std::sqrt(rms / bufN_);
            float f = 0.0f;
            if (rms > gate_) {
                double conf = 1.0;
                f = (float)yinFreq(w, bufN_, sr_, fmin_, fmax_, 0.15, &conf);
                if (conf > confThresh_) f = 0.0f;   // 非周期噪声 → 判无声
            }
            out_.push_back({t_, f});
            pending_.erase(pending_.begin(), pending_.begin() + hopN_);
            t_ += (double)hopN_ / sr_;
        }
    }

    std::vector<Detection> take() {
        std::vector<Detection> v;
        v.swap(out_);
        return v;
    }

private:
    double sr_ = 44100.0;
    int hopN_ = 512, bufN_ = 2048;
    double gate_ = 0.0015, fmin_ = 60.0, fmax_ = 1500.0;
    double confThresh_ = 0.25;
    double t_ = 0.0;
    std::vector<float> pending_;
    std::vector<Detection> out_;
};

} // namespace pitchlab
