#pragma once
#include "dx7_fft.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

// [ADICIONADO em 2026-10-18] Analise espectral pro espectrograma da UI (sem
// ImGui, testada no demo.cpp): FFT radix-2, janela de Hann, bandas em
// escala logaritmica de frequencia, mapa de cores e nome de nota.
namespace dx7fm_app {

using dx7fm::fftInPlace; // src/dx7_fft.h

// Espectro de magnitude em dB de um bloco real, com janela de Hann.
// Normalizado pra um seno de amplitude A (no centro de um bin) dar
// 20*log10(A) dB -- ou seja, 0 dB = seno em escala cheia.
class SpectrumAnalyzer {
public:
    explicit SpectrumAnalyzer(int n = 4096) : n_(n), window_(static_cast<size_t>(n)), buf_(static_cast<size_t>(n)) {
        for (int i = 0; i < n; ++i)
            window_[static_cast<size_t>(i)] = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * 3.14159265358979323846 * i / n));
    }
    int size() const { return n_; }

    // outDb: n/2 + 1 valores (bin k = k * sampleRate / n Hz)
    void magnitudeDb(const float* samples, std::vector<float>& outDb) {
        for (int i = 0; i < n_; ++i) buf_[static_cast<size_t>(i)] = std::complex<float>(samples[i] * window_[static_cast<size_t>(i)], 0.0f);
        fftInPlace(buf_);
        outDb.resize(static_cast<size_t>(n_ / 2 + 1));
        const float norm = 4.0f / static_cast<float>(n_); // 2/N (lado unico) / 0.5 (ganho coerente da Hann)
        for (int k = 0; k <= n_ / 2; ++k) {
            float mag = std::abs(buf_[static_cast<size_t>(k)]) * norm;
            outDb[static_cast<size_t>(k)] = 20.0f * std::log10(std::max(mag, 1e-9f));
        }
    }

private:
    int n_;
    std::vector<float> window_;
    std::vector<std::complex<float>> buf_;
};

// Reduz o espectro a `bands` faixas de frequencia em escala LOG entre fMin
// e fMax (a faixa 0 e a mais grave). Cada faixa pega o maior valor dos bins
// que cobre; faixas mais estreitas que um bin (graves) interpolam entre os
// 2 bins vizinhos, pra nao ficar "em escada".
inline void logBands(const std::vector<float>& binDb, double sampleRate, int fftSize, int bands, float fMin, float fMax,
                     std::vector<float>& out) {
    out.assign(static_cast<size_t>(bands), -120.0f);
    const double binHz = sampleRate / fftSize;
    const int lastBin = static_cast<int>(binDb.size()) - 1;
    const double ratio = std::log(static_cast<double>(fMax) / fMin);
    for (int b = 0; b < bands; ++b) {
        double f0 = fMin * std::exp(ratio * b / bands), f1 = fMin * std::exp(ratio * (b + 1) / bands);
        double k0 = f0 / binHz, k1 = f1 / binHz;
        int i0 = static_cast<int>(std::ceil(k0)), i1 = static_cast<int>(std::floor(k1));
        float v = -120.0f;
        if (i1 >= i0) {
            for (int k = std::max(i0, 0); k <= std::min(i1, lastBin); ++k) v = std::max(v, binDb[static_cast<size_t>(k)]);
        } else { // faixa dentro de um bin so: interpola no centro da faixa
            double kc = 0.5 * (k0 + k1);
            int a = std::clamp(static_cast<int>(kc), 0, lastBin), c = std::min(a + 1, lastBin);
            float t = static_cast<float>(kc - a);
            v = binDb[static_cast<size_t>(a)] * (1.0f - t) + binDb[static_cast<size_t>(c)] * t;
        }
        out[static_cast<size_t>(b)] = v;
    }
}

// Mapa de cores do espectrograma (estilo "inferno"): dB em [dbMin, 0] ->
// RGB. Fundo quase preto, depois roxo, magenta, laranja e amarelo claro.
inline void spectrogramColor(float db, float dbMin, uint8_t& r, uint8_t& g, uint8_t& b) {
    static const float stops[][4] = {
        {0.00f, 12, 14, 20},   {0.25f, 50, 18, 92},  {0.50f, 140, 38, 116},
        {0.70f, 228, 92, 66},  {0.88f, 250, 190, 70}, {1.00f, 252, 250, 210}};
    float t = std::clamp((db - dbMin) / -dbMin, 0.0f, 1.0f);
    int i = 0;
    while (i < 4 && t > stops[i + 1][0]) ++i;
    float u = (t - stops[i][0]) / (stops[i + 1][0] - stops[i][0]);
    r = static_cast<uint8_t>(stops[i][1] + (stops[i + 1][1] - stops[i][1]) * u);
    g = static_cast<uint8_t>(stops[i][2] + (stops[i + 1][2] - stops[i][2]) * u);
    b = static_cast<uint8_t>(stops[i][3] + (stops[i + 1][3] - stops[i][3]) * u);
}

// Nome da nota mais proxima de uma frequencia (A4 = 440 Hz), ex. "A4", "C#3".
inline void noteName(float hz, char* out, size_t outSize) {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (hz <= 0.0f) { std::snprintf(out, outSize, "-"); return; }
    int midi = std::clamp(static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hz / 440.0))), 0, 200);
    int octave = midi / 12 - 1;
    std::snprintf(out, outSize, "%s%d", names[((midi % 12) + 12) % 12], octave);
}

} // namespace dx7fm_app
