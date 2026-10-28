#pragma once
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

// [ADICIONADO em 2026-10-28] "Mutar": variacao aleatoria do som atual, pra
// descobrir timbres vizinhos (Ctrl+Z desfaz). `amount` (0..1) escala tudo:
// 0.1 = um retoque, 0.5 = bem diferente. So mexe nos operadores EM USO (com
// nivel > 0 e que soam ou modulam em algum slot de algoritmo) e mantem o
// carater: portadoras mudam pouco de nivel (o volume nao pula), moduladores
// mudam mais (e o brilho); razoes so trocam pra vizinhas musicais e com
// probabilidade proporcional ao amount; o algoritmo, as formas de onda, o
// arpejador, as macros e a afinacao ficam como estao.
namespace dx7fm_app {

inline void mutatePatch(dx7fm::VoiceParams& v, float amount, uint32_t seed) {
    using namespace dx7fm;
    amount = std::clamp(amount, 0.0f, 1.0f);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f), prob(0.0f, 1.0f);
    auto jitterInt = [&](uint8_t x, float range, int lo, int hi) {
        return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(x + uni(rng) * range * amount)), lo, hi));
    };

    bool carrier[kNumOps] = {}, used[kNumOps] = {};
    for (int s = 0; s < v.activeAlgos(); ++s) {
        const auto& m = v.algo(s);
        for (int i = 0; i < kNumOps; ++i) {
            if (m.outputMix[i] != 0.0f) carrier[i] = used[i] = true;
            for (int j = 0; j < kNumOps; ++j) if (m.mod[i][j] != 0.0f) used[j] = true;
        }
    }
    static const float kRatios[] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 11.0f, 13.0f, 14.0f};
    for (int i = 0; i < kNumOps; ++i) {
        auto& o = v.op[i];
        if (!used[i] || o.outputLevel == 0) continue;
        // nivel: portadora +-3 * amount, modulador +-14 * amount (0..99)
        o.outputLevel = jitterInt(o.outputLevel, carrier[i] ? 3.0f : 14.0f, 1, 99);
        // razao vizinha (so moduladores e com chance = amount/2): troca o timbre, nao a nota
        if (!carrier[i] && !o.fixedFreq && prob(rng) < 0.5f * amount) {
            const float* it = std::lower_bound(std::begin(kRatios), std::end(kRatios), o.coarseRatio);
            int idx = static_cast<int>(it - std::begin(kRatios));
            idx = std::clamp(idx + (uni(rng) < 0.0f ? -1 : 1), 0, static_cast<int>(std::size(kRatios)) - 1);
            o.coarseRatio = kRatios[idx];
        }
        o.detuneCents = std::clamp(o.detuneCents + uni(rng) * 6.0f * amount, -50.0f, 50.0f);
        // envelopes: rates +-20 (o de ataque +-8: a escala e exponencial e um
        // ataque 2x mais lento ja muda o carater), niveis intermediarios +-15
        // (L1 e L4 ficam: pico do ataque e repouso)
        o.ampEG.rate[0] = jitterInt(o.ampEG.rate[0], 8.0f, 0, 99);
        for (int k = 1; k < 4; ++k) o.ampEG.rate[k] = jitterInt(o.ampEG.rate[k], 20.0f, 0, 99);
        for (int k = 1; k <= 2; ++k) o.ampEG.level[k] = jitterInt(o.ampEG.level[k], carrier[i] ? 8.0f : 15.0f, 0, 99);
        if (o.fxType != FxType::Bypass || o.morphDepth != 0.0f) {
            for (int k = 0; k < 4; ++k) o.fxEG.rate[k] = jitterInt(o.fxEG.rate[k], 20.0f, 0, 99);
            for (int k = 1; k <= 2; ++k) o.fxEG.level[k] = jitterInt(o.fxEG.level[k], 15.0f, 0, 99);
        }
        if (o.feedback > 0.0f || prob(rng) < 0.15f * amount) o.feedback = std::clamp(o.feedback + uni(rng) * 0.25f * amount, 0.0f, 1.0f);
        o.velocitySens = jitterInt(o.velocitySens, 2.0f, 0, 7);
        // efeito: FX1 (corte, drive, taxa) x 2^(+-1.5 * amount) -- multiplicativo,
        // cabe em qualquer unidade. Comb e FreqShift ficam: o FX1 deles e uma
        // ALTURA (o comb afinado na nota sairia desafinado)
        if (o.fxType != FxType::Bypass && o.fxType != FxType::RingMod && o.fxType != FxType::Comb && o.fxType != FxType::FreqShift)
            o.fxParam1 *= std::exp2(uni(rng) * 1.5f * amount);
        if (o.waveform == Waveform::Wavetable) o.wavetable.position = std::clamp(o.wavetable.position + uni(rng) * 0.3f * amount, 0.0f, 1.0f);
    }
    // LFOs que estao em uso: velocidade x 2^(+-1 * amount), profundidades +-30%
    for (int k = 0; k < kNumLfos; ++k) {
        auto& l = v.lfo(k);
        if (l.pitchDepth == 0.0f && l.ampDepth == 0.0f) continue;
        if (!l.tempoSync) l.rate = std::clamp(l.rate * std::exp2(uni(rng) * amount), 0.01f, 50.0f);
        l.pitchDepth = std::clamp(l.pitchDepth * (1.0f + uni(rng) * 0.3f * amount), 0.0f, 1.0f);
        l.ampDepth = std::clamp(l.ampDepth * (1.0f + uni(rng) * 0.3f * amount), 0.0f, 1.0f);
    }
    // rotas da matriz: profundidade +-25% (sinal mantido)
    for (auto& r : v.modMatrix)
        if (r.src != ModSource::None && r.dest != ModDest::None) r.depth = std::clamp(r.depth * (1.0f + uni(rng) * 0.25f * amount), -1.0f, 1.0f);
}

} // namespace dx7fm_app
