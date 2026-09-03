#include <cmath>
#include <cstdio>
#include <vector>
#include "pitch/PitchDetector.h"

using namespace pitchlab;

int main() {
    const double sr = 44100.0;
    for (double hz : {82.4, 110.0}) {
        int n = (int)(sr * 0.8);
        std::vector<float> x(n);
        for (int i = 0; i < n; ++i)
            x[i] = 0.4f * (float)std::sin(2.0 * M_PI * hz * i / sr);
        // 直接测单帧
        float f = (float)yinFreq(x.data(), 2048, sr);
        printf("yinFreq %.1fHz frame -> %.2fHz\n", hz, f);
        PitchDetector det; det.prepare(sr);
        det.process(x.data(), n);
        auto dets = det.take();
        int cnt = 0; double sum = 0;
        for (auto& d : dets) if (d.freq > 0) { sum += d.freq; ++cnt; }
        printf("  stream: %d voiced / %zu, mean=%.2f\n", cnt, dets.size(),
               cnt ? sum / cnt : 0.0);
    }
    return 0;
}
