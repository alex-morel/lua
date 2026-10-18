#pragma once
#include "dx7_dsp.h"
#include "dx7_params.h"
#include <algorithm>
#include <cmath>

// [ADICIONADO em 2026-10-18] Conta do grafico de envelopes da UI (sem
// ImGui, pra ser testada no demo.cpp contra o SegmentEG de verdade).
//
// O SegmentEG anda em linha reta (em unidades de level 0..99) com
// velocidade egRateToSpeed(rate) unidades/s, entao cada estagio dura
// |level_destino - level_origem| / velocidade. O grafico mostra o EG de uma
// nota sem rate scaling (nota <= 60), que e o que os parametros do patch
// descrevem.
namespace dx7fm_app {

// Duracao (s) de um estagio que vai de `from` a `to` com o rate dado.
inline float egStageSeconds(float from, float to, float rate) {
    return std::fabs(to - from) / dx7fm::egRateToSpeed(std::clamp(rate, 0.0f, 99.0f));
}

// Inverso: o rate que faz o estagio from->to durar `seconds`. Sem variacao
// de level (from == to) o estagio e instantaneo com QUALQUER rate -- devolve
// -1 pra sinalizar "o tempo nao depende do rate".
inline float egRateForSeconds(float from, float to, float seconds) {
    float delta = std::fabs(to - from);
    if (delta <= 0.0f) return -1.0f;
    float speed = delta / std::max(seconds, 1e-6f);
    // egRateToSpeed(r) = 5 * 1.0897^r  =>  r = log(speed/5) / log(1.0897)
    float rate = std::log(speed / 5.0f) / std::log(1.0897f);
    return std::clamp(rate, 0.0f, 99.0f);
}

// Duracoes dos 4 estagios de um EG de amplitude: ataque (0 -> L1), decai 1
// (L1 -> L2), decai 2 (L2 -> L3, o nivel de sustain) e release (L3 -> L4).
struct EgTimes {
    float stage[4];
    float untilSustain() const { return stage[0] + stage[1] + stage[2]; }
};

inline EgTimes egTimes(const dx7fm::EGParams& eg, float startLevel = 0.0f) {
    EgTimes t{};
    float from = startLevel;
    for (int s = 0; s < 3; ++s) {
        t.stage[s] = egStageSeconds(from, eg.level[s], eg.rate[s]);
        from = eg.level[s];
    }
    t.stage[3] = egStageSeconds(eg.level[2], eg.level[3], eg.rate[3]);
    return t;
}

// Eixo de tempo comprimido (logaritmico perto de 0): envelopes de 4ms e de
// 20s cabem no mesmo grafico. x em [0,1] pra t em [0, tMax].
constexpr float kEgGraphTau = 0.01f; // 10ms: abaixo disso o eixo e ~linear
inline float egTimeToX(float t, float tMax) {
    return std::log1p(std::max(t, 0.0f) / kEgGraphTau) / std::log1p(std::max(tMax, 1e-4f) / kEgGraphTau);
}
inline float egXToTime(float x, float tMax) {
    return kEgGraphTau * std::expm1(std::clamp(x, 0.0f, 1.0f) * std::log1p(std::max(tMax, 1e-4f) / kEgGraphTau));
}

} // namespace dx7fm_app
