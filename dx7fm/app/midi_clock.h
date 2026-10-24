#pragma once
#include <algorithm>
#include <array>

// [ADICIONADO em 2026-10-24] Estimativa do BPM a partir do clock MIDI (24
// pulsos por seminima), sem ALSA -- testada no demo.cpp. Media dos ultimos 24
// intervalos (uma batida): segue mudancas de tempo em ~1 batida e e imune ao
// jitter de um pulso isolado. Usada so pela thread MIDI.
namespace dx7fm_app {

class ClockEstimator {
public:
    static constexpr int kPpqn = 24;

    void reset() { count_ = 0; last_ = -1.0; }

    // instante do pulso, em segundos (relogio monotonico)
    void tick(double t) {
        if (last_ >= 0.0) {
            double dt = t - last_;
            if (dt > 0.0 && dt < 1.0) { // > 1s entre pulsos (< 2.5 BPM): parou; recomeca a media
                intervals_[static_cast<size_t>(head_)] = dt;
                head_ = (head_ + 1) % kPpqn;
                count_ = std::min(count_ + 1, kPpqn);
            } else {
                count_ = 0;
            }
        }
        last_ = t;
    }

    // 0 enquanto ha poucos pulsos (menos de 6) pra estimar
    double bpm() const {
        if (count_ < 6) return 0.0;
        double sum = 0.0;
        for (int i = 0; i < count_; ++i) sum += intervals_[static_cast<size_t>(i)];
        return 60.0 / (sum / count_ * kPpqn);
    }

private:
    std::array<double, kPpqn> intervals_{};
    int head_ = 0, count_ = 0;
    double last_ = -1.0;
};

} // namespace dx7fm_app
