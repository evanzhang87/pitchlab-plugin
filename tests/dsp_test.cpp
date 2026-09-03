// PitchLab DSP 自测：验证 YIN 精度与选区分析(与 Python 原型同标准)。
// 编译: c++ -std=c++17 -O2 -I Source tests/dsp_test.cpp -o dsp_test
#define _USE_MATH_DEFINES   // MSVC 需要，使 <cmath> 提供 M_PI
#include <cmath>
#include <cstdio>
#include <vector>

#include "pitch/PitchDetector.h"
#include "pitch/Analysis.h"

using namespace pitchlab;

static std::vector<float> synthTone(double sr, double dur, double f0,
                                    double centsCenter, double centsDepth,
                                    double vibHz) {
    int n = (int)(sr * dur);
    std::vector<float> x(n);
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        double t = (double)i / sr;
        double c = centsCenter + centsDepth * std::sin(2.0 * M_PI * vibHz * t);
        double f = f0 * std::pow(2.0, c / 1200.0);
        phase += 2.0 * M_PI * f / sr;
        x[i] = 0.4f * (float)(std::sin(phase) + 0.16 * std::sin(2.0 * phase));
    }
    return x;
}

// 推弦 G3->A3 + 落点揉弦
static std::vector<float> synthBend(double sr) {
    int n = (int)(sr * 3.0);
    std::vector<float> x(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        double t = (double)i / sr;
        double f = 0.0, a = 0.0;
        if (t >= 0.15 && t < 1.05) { // G3 揉弦 +8¢, 5Hz ±22
            f = 196.0 * std::pow(2.0, (8.0 + 22.0 * std::sin(2.0 * M_PI * 5.0 * (t - 0.15))) / 1200.0);
            a = 0.42 * std::min(1.0, (t - 0.15) / 0.010) * std::exp(-(t - 0.15) / 1.1);
        } else if (t >= 1.30 && t < 1.62) { // 滑上 2 半音
            double u = (t - 1.30) / 0.32;
            double m = 55.0 + 2.0 * u;
            f = 440.0 * std::pow(2.0, (m - 69.0) / 12.0);
            a = 0.42 * std::exp(-(t - 1.30) / 1.1);
        } else if (t >= 1.62 && t < 2.75) { // A3 揉弦 -12¢, 4.5Hz ±18
            f = 220.0 * std::pow(2.0, (-12.0 + 18.0 * std::sin(2.0 * M_PI * 4.5 * (t - 1.62))) / 1200.0);
            a = 0.42 * std::exp(-(t - 1.62) / 1.1) * std::min(1.0, (2.75 - t) / 0.08);
        }
        if (a < 0.0) a = 0.0;
        if (f > 0.0 && a > 0.0) {
            static double ph = 0.0;
            ph += 2.0 * M_PI * f / sr;
            x[i] = (float)(a * (std::sin(ph) + 0.15 * std::sin(2.0 * ph + 0.5)));
        }
    }
    return x;
}

static int failures = 0;
static void check(const char* name, double got, double want, double tol) {
    bool ok = std::fabs(got - want) <= tol;
    if (!ok) ++failures;
    std::printf("  %-28s got %9.2f  want %9.2f  %s\n", name, got, want,
                ok ? "OK" : "FAIL");
}

int main() {
    const double sr = 44100.0;

    std::printf("[1] YIN 纯正弦精度(期望 ~0¢)\n");
    for (double hz : {82.4, 110.0, 196.0, 220.0, 330.0, 440.0, 880.0}) {
        auto x = synthTone(sr, 0.8, hz, 0.0, 0.0, 0.0);
        PitchDetector det;
        det.prepare(sr);
        det.process(x.data(), (int)x.size());
        auto dets = det.take();
        double sum = 0; int cnt = 0;
        for (auto& d : dets) if (d.freq > 0) { sum += d.freq; ++cnt; }
        double med = cnt ? sum / cnt : 0.0; // 均值足够(纯正弦)
        check("sine", med, hz, 0.6);
    }

    std::printf("[2] 推弦+揉弦 选区分析\n");
    auto x = synthBend(sr);
    PitchDetector det;
    det.prepare(sr);
    det.setGate(0.0015);
    det.process(x.data(), (int)x.size());
    auto dets = det.take();
    std::vector<double> t, f;
    t.reserve(dets.size()); f.reserve(dets.size());
    for (auto& d : dets) { t.push_back(d.time); f.push_back(d.freq); }
    double dt = t.size() > 1 ? (t[1] - t[0]) : 0.0116;
    std::printf("  detections %zu, t=%.2f..%.2f\n", t.size(), t.front(), t.back());

    auto notes = analyzeRegion(t, f, 0.0, 3.0, dt);
    std::printf("  notes found: %zu\n", notes.size());
    for (auto& st : notes) {
        std::printf("    %.2f-%.2fs  %-4s  %+6.1f¢  vib %+.1fHz %+.1f¢  span %.2f st\n",
                    st.t0, st.t1, noteName(st.midi).c_str(), st.devCents,
                    st.vibRate, st.vibDepth, st.spanSt);
    }
    // 期望: G3(+8±6¢, 5Hz±1, depth 22±8), 之后无稳定音(推弦为 move), A3(-12±6, 4.5±1, 18±8)
    if (notes.size() >= 1) {
        check("note1 dev", notes[0].devCents, 8.0, 6.0);
        check("note1 rate", notes[0].vibRate, 5.0, 1.2);
        check("note1 depth", notes[0].vibDepth, 22.0, 10.0);
    } else { ++failures; std::printf("  FAIL missing note1\n"); }
    if (notes.size() >= 2) {
        check("note2 dev", notes[1].devCents, -12.0, 6.0);
        check("note2 rate", notes[1].vibRate, 4.5, 1.2);
        check("note2 depth", notes[1].vibDepth, 18.0, 10.0);
    } else { ++failures; std::printf("  FAIL missing note2\n"); }

    std::printf(failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
