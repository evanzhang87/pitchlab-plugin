// PitchLab —— 稳定音段分析（选区离线报告用），与 Python 原型对齐。
// 输入：按时间升序的检测序列 (time, freq)，freq<=0 表示无声。
// 输出：稳定保持音段列表(含音名/音分偏差/揉弦速率与深度/跨度)。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "PitchDetector.h"

namespace pitchlab {

static const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B"};

inline std::string noteName(double midi) {
    int m = (int)std::lround(midi);
    if (m < 0) return "--";
    return std::string(kNoteNames[m % 12]) + std::to_string(m / 12 - 1);
}

inline std::string centsLabel(double c) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+d\xc2\xa2", (int)std::lround(c)); // ¢ (UTF-8)
    return std::string(buf);
}

// ---------------- 工具 ----------------
inline double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double idx = q * (double)(v.size() - 1);
    size_t lo = (size_t)std::floor(idx);
    size_t hi = (size_t)std::ceil(idx);
    if (lo == hi) return v[lo];
    return v[lo] + (v[hi] - v[lo]) * (idx - (double)lo);
}

inline double median(std::vector<double> v) {
    return percentile(std::move(v), 0.5);
}

// 对一段"连续发声"的中位音高序列做窗口 7 的滚动中值(边缘截断)。
inline std::vector<double> smoothMidi(const std::vector<double>& midi) {
    const int w = 7, hw = 3;
    const size_t n = midi.size();
    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i) {
        size_t a = (size_t)std::max(0, (int)i - hw);
        size_t b = std::min(n, i + hw + 1);
        std::vector<double> win(midi.begin() + (long)a, midi.begin() + (long)b);
        out[i] = median(std::move(win));
    }
    return out;
}

// 揉弦统计：频率序列相对其中值的音分曲线 → (速率Hz, 深度¢)；测不到返回 (-1,-1)。
inline std::pair<double, double> vibratoStats(const std::vector<double>& freqIn,
                                              double dt) {
    std::vector<double> f;
    f.reserve(freqIn.size());
    for (double x : freqIn) if (x > 0.0) f.push_back(x);
    if (f.size() < 8) return {-1.0, -1.0};
    double med = median(f);
    if (med <= 0.0) return {-1.0, -1.0};
    double dur = (double)f.size() * dt;
    if (dur < 0.15) return {-1.0, -1.0};

    std::vector<double> c(f.size());
    for (size_t i = 0; i < f.size(); ++i)
        c[i] = 1200.0 * std::log2(f[i] / med);
    if (c.size() >= 3) { // 3 点平均去毛刺
        std::vector<double> c2(c.size() - 2);
        for (size_t i = 0; i + 2 < c.size(); ++i)
            c2[i] = (c[i] + c[i + 1] + c[i + 2]) / 3.0;
        c.swap(c2);
    }
    // 交替峰谷
    struct E { int i; bool peak; double v; };
    std::vector<E> ext;
    for (size_t i = 1; i + 1 < c.size(); ++i) {
        if (c[i] >= c[i - 1] && c[i] > c[i + 1]) ext.push_back({(int)i, true, c[i]});
        else if (c[i] <= c[i - 1] && c[i] < c[i + 1]) ext.push_back({(int)i, false, c[i]});
    }
    const int minGap = std::max(2, (int)(0.06 / dt));
    std::vector<E> filt;
    for (auto& e : ext) {
        if (!filt.empty() && e.i - filt.back().i < minGap) continue;
        filt.push_back(e);
    }
    ext = filt;
    int pairCount = 0;
    for (size_t k = 0; k + 1 < ext.size(); ++k)
        if (ext[k].peak != ext[k + 1].peak) ++pairCount;
    if (pairCount >= 2) {
        std::vector<double> sorted = c;
        std::sort(sorted.begin(), sorted.end());
        double depth = 0.5 * (percentile(sorted, 0.95) - percentile(sorted, 0.05));
        std::vector<double> gaps;
        std::vector<double> ppos, vpos;
        for (auto& e : ext) (e.peak ? ppos : vpos).push_back((double)e.i);
        for (size_t k = 1; k < ppos.size(); ++k)
            gaps.push_back((ppos[k] - ppos[k - 1]) * dt);
        for (size_t k = 1; k < vpos.size(); ++k)
            gaps.push_back((vpos[k] - vpos[k - 1]) * dt);
        double best = 0.0; int cnt = 0;
        for (double g : gaps) if (g > 0.03) { best += g; ++cnt; }
        if (cnt > 0) {
            double rate = 1.0 / (best / cnt);
            if (rate >= 1.5 && rate <= 10.0) return {rate, depth};
        }
    }
    return {-1.0, -1.0};
}

// ---------------- 段落切分 ----------------
struct Seg { bool move; int a, b; }; // 样本索引(相对所在发声句)

// 在一句连续发声内，把"明显滑动(推弦/滑音)"与"稳定保持"分段。
// 用跨度 span 样本的中心差分估计斜率。span/阈值要点：
//   平滑后揉弦残余波动跨 10 样本差分 <= ~0.4 半音(即使 ±40¢ 深揉弦)；
//   常规推弦(2 半音/0.3s)跨 10 样本差分 ~0.73 半音。取 0.5 分界。
inline std::vector<Seg> splitMovements(const std::vector<double>& v,
                                       int span = 5,
                                       double stepThr = 0.5,
                                       int cons = 4) {
    const int n = (int)v.size();
    std::vector<bool> moving(n, false);
    if (n > span * 2) {
        int run = 0;
        for (int i = span; i < n - span; ++i) {
            double diff = std::fabs(v[i + span] - v[i - span]); // /(2span) 已并入阈值
            if (diff > stepThr) {
                if (++run >= cons) {
                    for (int k = i - cons + 1 + span; k <= i + span; ++k)
                        if (k >= 0 && k < n) moving[k] = true;
                }
            } else run = 0;
        }
    }
    // 向两侧各扩 1(用副本避免级联)
    std::vector<bool> m2 = moving;
    for (int i = 0; i < n; ++i) {
        if (i > 0 && moving[i - 1]) m2[i] = true;
        if (i + 1 < n && moving[i + 1]) m2[i] = true;
    }
    std::vector<Seg> segs;
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && m2[j] == m2[i]) ++j;
        segs.push_back({m2[i], i, j - 1});
        i = j;
    }
    return segs;
}

// ---------------- 稳定音段统计 ----------------
struct NoteStat {
    double t0 = 0, t1 = 0, dur = 0;
    double midi = 0, devCents = 0, vibRate = -1, vibDepth = -1;
    double spanSt = 0; // 段内 p95-p5 跨度(半音)
    bool ok = false;
};

inline NoteStat noteStats(const std::vector<double>& tSeg,
                          const std::vector<double>& fSeg,
                          double dt, double trimS = 0.045) {
    NoteStat st;
    // 掐掉起音/收尾各 trimS 秒，避免攻击瞬态干扰
    size_t a = 0, b = fSeg.size() - 1;
    if (tSeg.size() >= 2) {
        double tEnd = tSeg.back();
        while (a < b && tSeg[a] < tSeg.front() + trimS) ++a;
        while (b > a && tSeg[b] > tEnd - trimS) --b;
    }
    std::vector<double> fr;
    for (size_t i = a; i <= b; ++i) if (fSeg[i] > 0.0) fr.push_back(fSeg[i]);
    if ((int)fr.size() < 4) return st;
    double fmed = median(fr);
    double mmed = midiFromFreq(fmed);
    int nominal = (int)std::lround(mmed);
    double fNom = freqFromMidi((double)nominal);
    st.midi = mmed;
    st.devCents = 1200.0 * std::log2(fmed / fNom);
    std::vector<double> mrun;
    mrun.reserve(fr.size());
    for (double x : fr) mrun.push_back(midiFromFreq(x));
    st.spanSt = percentile(mrun, 0.95) - percentile(mrun, 0.05);
    if (st.spanSt < 1.0) {
        auto vb = vibratoStats(fr, dt);
        st.vibRate = vb.first; st.vibDepth = vb.second;
    }
    st.ok = true;
    return st;
}

// ---------------- 选区总分析 ----------------
// 对 [t0Sel, t1Sel] 内检测序列做：断句(静音间隔>gapS)→ 句中切分移动 → 稳定音统计。
inline std::vector<NoteStat> analyzeRegion(const std::vector<double>& t,
                                           const std::vector<double>& f,
                                           double t0Sel, double t1Sel,
                                           double dt,
                                           double minDur = 0.30,
                                           double gapS = 0.16) {
    std::vector<NoteStat> notes;

    // 找出选区内的发声索引(连续 voiced，且时刻递增)
    std::vector<int> vi;
    for (size_t i = 0; i < t.size(); ++i)
        if (f[i] > 0.0 && t[i] >= t0Sel && t[i] <= t1Sel) vi.push_back((int)i);

    // 断句：连续两个发声样本间隔 > gapS 视为静音断开
    std::vector<std::pair<int, int>> runs;
    int rs = -1, rp = -1;
    for (int idx : vi) {
        if (rs < 0) { rs = rp = idx; continue; }
        if (t[idx] - t[rp] > gapS) { runs.push_back({rs, rp}); rs = rp = idx; }
        else rp = idx;
    }
    if (rs >= 0 && rp > rs) runs.push_back({rs, rp});

    for (auto& run : runs) {
        int a = run.first, b = run.second;
        const int n = b - a + 1;
        if (n < 4) continue;
        // 平滑(仅本句 voiced)
        std::vector<double> mv(n);
        for (int i = 0; i < n; ++i) mv[i] = midiFromFreq(f[a + i]);
        std::vector<double> sm = smoothMidi(mv);
        // 句中切分
        auto segs = splitMovements(sm);
        for (auto& sg : segs) {
            if (sg.move) continue;
            int s0 = a + sg.a, s1 = a + sg.b;
            double dur = t[s1] - t[s0] + dt;
            if (sg.b - sg.a < 4 || dur < minDur) continue;
            std::vector<double> tt(t.begin() + s0, t.begin() + s1 + 1);
            std::vector<double> ff(f.begin() + s0, f.begin() + s1 + 1);
            NoteStat st = noteStats(tt, ff, dt);
            if (!st.ok) continue;
            st.t0 = t[s0]; st.t1 = t[s1]; st.dur = dur;
            notes.push_back(st);
        }
    }
    return notes;
}

} // namespace pitchlab
