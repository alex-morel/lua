#pragma once
#include "dx7_dsp.h"
#include "dx7_params.h"
#include "dx7_resynth.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

// [ADICIONADO em 2026-10-22] Geradores das ondas de um ciclo da
// Waveform::Wavetable (kWaveSize amostras): formas prontas, harmonicos,
// importacao de WAV (frame de wavetable do Serum/Vital ou um periodo de um
// som qualquer) e utilitarios de edicao. Sem ImGui; usados pela UI
// (app/wavetable_editor.h) e testados no demo.cpp. O motor nao depende deste
// arquivo (so de dx7_wavetable.h).
namespace dx7fm::wavegen {

using Wave = std::vector<float>;

inline std::shared_ptr<const Wave> share(Wave w) { return std::make_shared<const Wave>(std::move(w)); }

// --- utilitarios ----------------------------------------------------------------
inline void removeDc(Wave& w) {
    double m = 0.0;
    for (float x : w) m += x;
    m /= static_cast<double>(w.size());
    for (auto& x : w) x -= static_cast<float>(m);
}

inline void normalize(Wave& w) {
    float peak = 0.0f;
    for (float x : w) peak = std::max(peak, std::fabs(x));
    if (peak > 1e-9f) for (auto& x : w) x /= peak;
}

inline void invert(Wave& w) { for (auto& x : w) x = -x; }

// media movel circular de 5 pontos, `passes` vezes (tira os cantos do desenho)
inline void smooth(Wave& w, int passes = 1) {
    const size_t n = w.size();
    Wave t(n);
    for (int p = 0; p < passes; ++p) {
        for (size_t i = 0; i < n; ++i) {
            float s = 0.0f;
            for (int d = -2; d <= 2; ++d) s += w[(i + n + static_cast<size_t>(d + static_cast<int>(n))) % n];
            t[i] = s / 5.0f;
        }
        w.swap(t);
    }
}

// --- formas prontas ----------------------------------------------------------------
// Qualquer forma do oscilador comum (Sine..LogSaw; Noise = um ciclo de ruido fixo).
inline Wave fromShape(Waveform shape, unsigned noiseSeed = 1) {
    Wave w(static_cast<size_t>(kWaveSize));
    std::mt19937 rng(noiseSeed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (int i = 0; i < kWaveSize; ++i)
        w[static_cast<size_t>(i)] = oscWave(static_cast<float>(i) / kWaveSize, shape == Waveform::Wavetable || shape == Waveform::Additive ? Waveform::Sine : shape, rng, dist);
    return w;
}

// pulso com largura 0..1 (0.5 = quadrada); sem DC
inline Wave pulse(float width) {
    width = std::clamp(width, 0.01f, 0.99f);
    Wave w(static_cast<size_t>(kWaveSize));
    for (int i = 0; i < kWaveSize; ++i) w[static_cast<size_t>(i)] = static_cast<float>(i) / kWaveSize < width ? 1.0f : -1.0f;
    removeDc(w);
    normalize(w);
    return w;
}

// soma de senos nos harmonicos 1..n com as amplitudes dadas (fase 0); pico 1
inline Wave fromHarmonics(const float* amp, int n) {
    Wave w(static_cast<size_t>(kWaveSize), 0.0f);
    for (int h = 1; h <= n && h < kWaveSize / 2; ++h) {
        const float a = amp[h - 1];
        if (a == 0.0f) continue;
        for (int i = 0; i < kWaveSize; ++i)
            w[static_cast<size_t>(i)] += a * fastSin01(static_cast<float>((static_cast<long>(h) * i) % kWaveSize) / kWaveSize);
    }
    normalize(w);
    return w;
}

// --- importar de WAV -----------------------------------------------------------------
// Arquivo de wavetable (Serum/Vital): frames de kWaveSize amostras seguidos.
inline int wavetableFrames(const std::vector<float>& mono) {
    return mono.size() >= static_cast<size_t>(2 * kWaveSize) && mono.size() % static_cast<size_t>(kWaveSize) == 0
               ? static_cast<int>(mono.size() / static_cast<size_t>(kWaveSize)) : 0;
}

inline Wave fromWavFrame(const std::vector<float>& mono, int frame) {
    Wave w(static_cast<size_t>(kWaveSize), 0.0f);
    const size_t at = static_cast<size_t>(std::max(frame, 0)) * static_cast<size_t>(kWaveSize);
    for (size_t i = 0; i < w.size() && at + i < mono.size(); ++i) w[i] = mono[at + i];
    return w;
}

// Um periodo de um som qualquer, perto de `atSec`: f0 pela analise da
// ressintese (picos + detectReference), corte numa subida do zero e
// reamostragem do periodo (fracionario) pra kWaveSize. f0Hz devolve a
// frequencia usada. Vazio se nao achou altura.
inline Wave fromWavPeriod(const std::vector<float>& mono, double sr, double atSec, float* f0Hz = nullptr) {
    resynth::Options o;
    auto peaks = resynth::findPeaks(mono.data(), mono.size(), sr, atSec, o);
    const float f0 = resynth::detectReference(peaks);
    if (f0Hz) *f0Hz = f0;
    if (f0 <= 0.0f) return {};
    const double period = sr / f0;
    if (period < 4.0 || period * 2 > static_cast<double>(mono.size())) return {};
    // subida do zero mais perto do instante pedido -- zero em volta da MEDIA
    // do trecho (com DC, o zero cru cairia numa fase qualquer da onda)
    long start = std::clamp(static_cast<long>(atSec * sr), 1L, static_cast<long>(mono.size() - static_cast<size_t>(2 * period) - 2));
    const long limit = start + static_cast<long>(period) + 1;
    double mean = 0.0;
    for (long i = start; i < start + static_cast<long>(period); ++i) mean += mono[static_cast<size_t>(i)];
    mean /= std::floor(period);
    double zc = -1.0;
    for (long i = start; i < limit && i + 1 < static_cast<long>(mono.size()); ++i)
        if (mono[static_cast<size_t>(i)] - mean <= 0.0 && mono[static_cast<size_t>(i + 1)] - mean > 0.0) {
            float a = mono[static_cast<size_t>(i)] - static_cast<float>(mean), b = mono[static_cast<size_t>(i + 1)] - static_cast<float>(mean);
            zc = static_cast<double>(i) + a / (a - b); // cruzamento interpolado
            break;
        }
    if (zc < 0.0) zc = static_cast<double>(start);
    if (zc + period + 1.0 >= static_cast<double>(mono.size())) return {};
    Wave w(static_cast<size_t>(kWaveSize));
    for (int i = 0; i < kWaveSize; ++i) {
        double pos = zc + period * i / kWaveSize;
        size_t i0 = static_cast<size_t>(pos);
        float t = static_cast<float>(pos - static_cast<double>(i0));
        w[static_cast<size_t>(i)] = mono[i0] * (1.0f - t) + mono[i0 + 1] * t;
    }
    removeDc(w);
    normalize(w);
    return w;
}

} // namespace dx7fm::wavegen
