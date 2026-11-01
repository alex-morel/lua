#pragma once
// [ADICIONADO em 2026-11-01] Sequencer de 4 tracks, uma por parte. Cada passo
// tem nota, velocity, gate e um valor de modulacao (fonte ModSource::SeqMod da
// parte e fontes "Sequencer A..D" dos efeitos). O Synth toca (dx7_alloc.h); os
// passos caem no instante exato, como o arpejador. Fica fora do patch: e do
// conjunto das partes, como a pedaleira.
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dx7fm {

constexpr int kSeqTracks = 4;
constexpr int kSeqSteps = 32;

struct SeqStep {
    bool on = false;
    uint8_t note = 60;
    uint8_t velocity = 100;
    float gate = 0.5f;  // fracao do passo; >= 1 = ligada na proxima (legato)
    float mod = 0.0f;   // -1..1; vale mesmo com o passo sem nota
};
struct SeqTrack {
    bool mute = false;  // cala as notas; a modulacao continua
    uint8_t length = 16;
    uint8_t division = 12; // indice de lfoDivisionName (1/16)
    float swing = 0.0f;    // 0..0.75: atraso dos passos pares (2o, 4o...), em fracao do passo
    SeqStep step[kSeqSteps];
};
struct SeqParams {
    SeqTrack track[kSeqTracks];
};

inline void sanitize(SeqParams& p) {
    auto f = [](float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
    for (auto& t : p.track) {
        t.length = static_cast<uint8_t>(std::clamp<int>(t.length, 1, kSeqSteps));
        t.division = static_cast<uint8_t>(std::clamp<int>(t.division, 0, kNumLfoDivisions - 1));
        t.swing = f(t.swing, 0.0f, 0.75f, 0.0f);
        for (auto& s : t.step) {
            s.note = static_cast<uint8_t>(std::clamp<int>(s.note, 0, 127));
            s.velocity = static_cast<uint8_t>(std::clamp<int>(s.velocity, 1, 127));
            s.gate = f(s.gate, 0.01f, 1.0f, 0.5f);
            s.mod = f(s.mod, -1.0f, 1.0f, 0.0f);
        }
    }
}

} // namespace dx7fm
