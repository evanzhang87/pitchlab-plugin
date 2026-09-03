#include <cmath>
#include <cstdio>
#include <vector>
#include "pitch/PitchDetector.h"

// 复刻 yinFreq 内部，但打印 cmn 诊断
static void diag(double hz, double sr) {
    const int n = 2048;
    std::vector<float> x(n);
    for (int i = 0; i < n; ++i)
        x[i] = 0.4f * (float)std::sin(2.0 * M_PI * hz * i / sr);
    const int tauMin = std::max(2, (int)std::floor(sr / 1500.0)); // 29
    const int tauMax = std::min(n - 3, (int)std::ceil(sr / 60.0)); // 735
    std::vector<double> pref(n + 1, 0.0);
    for (int i = 0; i < n; ++i) pref[i + 1] = pref[i] + (double)x[i] * x[i];
    std::vector<double> d(tauMax + 1, 0.0), dot(tauMax + 1, 0.0);
    for (int i = 0; i < n; ++i) {
        const double xi = x[i];
        const int tmax = std::min(tauMax, n - 1 - i);
        for (int tau = 1; tau <= tmax; ++tau) dot[tau] += xi * (double)x[i + tau];
    }
    for (int tau = 1; tau <= tauMax; ++tau) {
        double v = (pref[n - tau] - pref[0]) + (pref[n] - pref[tau]) - 2.0 * dot[tau];
        d[tau] = v > 0 ? v : 0.0;
    }
    std::vector<double> cmn(tauMax + 1);
    double csum = 0;
    for (int tau = 1; tau <= tauMax; ++tau) { csum += d[tau]; cmn[tau] = d[tau] * tau / (csum + 1e-9); }
    printf("\n== %.1f Hz (period tau=%.1f) ==\n", hz, sr / hz);
    // 找前几个低于 0.15 的局部极小
    int shown = 0;
    for (int tau = tauMin; tau <= tauMax; ++tau) {
        if (cmn[tau] < 0.15) {
            bool loOk = (tau == tauMin) || (cmn[tau] <= cmn[tau - 1]);
            bool hiOk = (tau == tauMax) || (cmn[tau] < cmn[tau + 1]);
            if (loOk && hiOk && shown < 6) {
                printf("  valley tau=%d cmn=%.4f freq=%.1f\n", tau, cmn[tau], sr / tau);
                ++shown;
            }
        }
    }
    double mn = 1e9; int mnT = -1;
    for (int tau = tauMin; tau <= tauMax; ++tau) if (cmn[tau] < mn) { mn = cmn[tau]; mnT = tau; }
    printf("  global min tau=%d cmn=%.5f freq=%.2f\n", mnT, mn, sr / mnT);
    for (int tau = 29; tau <= 40; ++tau)
        printf("   cmn[%d]=%.4f\n", tau, cmn[tau]);
}

int main() {
    diag(82.4, 44100.0);
    diag(110.0, 44100.0);
    diag(196.0, 44100.0);
    return 0;
}
