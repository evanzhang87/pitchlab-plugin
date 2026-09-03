#include <cmath>
#include <cstdio>
#include <vector>
#include "pitch/PitchDetector.h"
#include "pitch/Analysis.h"

using namespace pitchlab;

static std::vector<float> synthBend(double sr) {
    int n = (int)(sr * 3.0);
    std::vector<float> x(n, 0.0f);
    double ph = 0.0;
    for (int i = 0; i < n; ++i) {
        double t = (double)i / sr;
        double f = 0.0, a = 0.0;
        if (t >= 0.15 && t < 1.05) {
            f = 196.0 * std::pow(2.0, (8.0 + 22.0 * std::sin(2.0 * M_PI * 5.0 * (t - 0.15))) / 1200.0);
            a = 0.42 * std::min(1.0, (t - 0.15) / 0.010) * std::exp(-(t - 0.15) / 1.1);
        } else if (t >= 1.30 && t < 1.62) {
            double u = (t - 1.30) / 0.32;
            f = 440.0 * std::pow(2.0, (55.0 + 2.0 * u - 69.0) / 12.0);
            a = 0.42 * std::exp(-(t - 1.30) / 1.1);
        } else if (t >= 1.62 && t < 2.75) {
            f = 220.0 * std::pow(2.0, (-12.0 + 18.0 * std::sin(2.0 * M_PI * 4.5 * (t - 1.62))) / 1200.0);
            a = 0.42 * std::exp(-(t - 1.62) / 1.1) * std::min(1.0, (2.75 - t) / 0.08);
        }
        if (a < 0.0) a = 0.0;
        if (f > 0.0 && a > 0.0) {
            ph += 2.0 * M_PI * f / sr;
            x[i] = (float)(a * (std::sin(ph) + 0.15 * std::sin(2.0 * ph + 0.5)));
        }
    }
    return x;
}

int main() {
    double sr = 44100.0;
    auto x = synthBend(sr);
    PitchDetector det; det.prepare(sr);
    det.process(x.data(), (int)x.size());
    auto dets = det.take();
    std::vector<double> t, f;
    for (auto& d : dets) { t.push_back(d.time); f.push_back(d.freq); }
    double dt = t[1] - t[0];
    printf("dt=%.6f voiced=%zu\n", dt, (size_t)std::count_if(f.begin(), f.end(), [](double v){return v>0;}));

    // 断句
    std::vector<int> vi;
    for (size_t i = 0; i < t.size(); ++i) if (f[i] > 0) vi.push_back((int)i);
    std::vector<std::pair<int,int>> runs;
    int rs=-1, rp=-1;
    for (int idx : vi) {
        if (rs<0) { rs=rp=idx; continue; }
        if (t[idx]-t[rp] > 0.16) { runs.push_back({rs,rp}); rs=rp=idx; }
        else rp=idx;
    }
    if (rs>=0 && rp>rs) runs.push_back({rs,rp});
    printf("runs: %zu\n", runs.size());
    for (auto& r : runs) printf("  run %d..%d  t %.2f-%.2f n=%d\n", r.first, r.second,
                                t[r.first], t[r.second], r.second-r.first+1);

    for (auto& r : runs) {
        int a=r.first,b=r.second, n=b-a+1;
        if (n<4) continue;
        std::vector<double> mv(n);
        for (int i=0;i<n;++i) mv[i]=midiFromFreq(f[a+i]);
        auto sm=smoothMidi(mv);
        auto segs=splitMovements(sm);
        printf("run n=%d segs=%zu\n", n, segs.size());
        for (auto& s: segs) printf("   %s %d..%d (n=%d) midi[%.1f..%.1f]\n",
            s.move?"MOVE":"note", s.a, s.b, s.b-s.a+1, sm[s.a], sm[s.b]);
        // 检查第一个 note 段 stats
        for (auto& s: segs) {
            if (s.move || s.b-s.a<4) continue;
            int s0=a+s.a, s1=a+s.b;
            std::vector<double> tt(t.begin()+s0, t.begin()+s1+1);
            std::vector<double> ff(f.begin()+s0, f.begin()+s1+1);
            NoteStat st=noteStats(tt, ff, dt);
            printf("   -> noteStats ok=%d midi=%.2f dev=%.1f vibRate=%.2f\n",
                   st.ok, st.midi, st.devCents, st.vibRate);
        }
    }
    return 0;
}
