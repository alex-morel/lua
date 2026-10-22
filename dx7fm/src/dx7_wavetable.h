#pragma once
#include "dx7_fft.h"
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

// [ADICIONADO em 2026-10-22] Tabelas da Waveform::Wavetable: duas ondas de um
// ciclo (A e B, kWaveSize amostras) viram um WavetableBank, montado na thread
// da UI (Synth::recompile) e so LIDO pela audio thread.
//
//  - Frames: crossfade = 2 frames (A, B); espectral = kSpectralFrames frames
//    intermediarios, cada harmonico interpolado de A pra B (magnitude em
//    linha reta, fase pelo menor arco). O morph le os 2 frames vizinhos da
//    posicao e mistura -- no crossfade isso e (1-m)A + mB; no espectral, a
//    mistura e entre frames proximos (quase iguais), entao uma serra virando
//    serra invertida NAO passa pelo silencio no meio.
//  - Anti-aliasing: cada frame tem kLevels niveis (mipmaps); o nivel k so tem
//    harmonicos ate 1024 >> k, numa tabela de max(2048 >> k, 32) amostras.
//    A leitura escolhe o nivel mais rico que ainda cabe abaixo de Nyquist
//    pra frequencia do operador (taxa interna, com oversampling).
//  - DC removido. A e B normalizados pra pico 1 (no nivel 0); os frames
//    intermediarios do espectral usam o ganho INTERPOLADO entre os de A e B,
//    nao o proprio pico: no meio de serra -> serra invertida cada harmonico
//    gira 90 graus e a onda vira a transformada de Hilbert da serra, com um
//    pico estreito e alto -- normalizar esse pico pra 1 derrubava o volume
//    (RMS) pela metade. Assim o volume fica constante e o pico pode passar
//    um pouco de 1 no meio do morph.
namespace dx7fm {

class WavetableBank {
public:
    static constexpr int kLevels = 11;          // 1024, 512, ..., 1 harmonicos
    static constexpr int kSpectralFrames = 17;  // posicoes pre-calculadas do morph espectral

    // a/b nullptr = seno; tamanho kWaveSize (garantido pelo sanitize)
    WavetableBank(const std::vector<float>* a, const std::vector<float>* b, bool spectral) {
        frames_ = spectral ? kSpectralFrames : 2;
        stride_ = 0;
        for (int k = 0; k < kLevels; ++k) {
            size_[k] = std::max(kWaveSize >> k, 32);
            offset_[k] = stride_;
            stride_ += size_[k] + 1; // +1 amostra de guarda (= primeira): interpolacao sem modulo
        }
        data_.assign(static_cast<size_t>(stride_) * static_cast<size_t>(frames_), 0.0f);
        const auto sa = spectrum(a), sb = spectrum(b);
        const float gainA = peakGain(sa), gainB = peakGain(sb);
        std::vector<std::complex<float>> x(kWaveSize / 2 + 1);
        for (int f = 0; f < frames_; ++f) {
            const float t = static_cast<float>(f) / static_cast<float>(frames_ - 1);
            for (int h = 0; h <= kWaveSize / 2; ++h) {
                const std::complex<float> ca = sa[static_cast<size_t>(h)], cb = sb[static_cast<size_t>(h)];
                if (!spectral) { x[static_cast<size_t>(h)] = f == 0 ? ca : cb; continue; }
                const float ma = std::abs(ca), mb = std::abs(cb);
                float pa = std::arg(ca), d = std::arg(cb) - pa;
                // harmonico ausente num dos lados: usa a fase do outro (so a magnitude anda)
                if (ma < 1e-9f) { pa = std::arg(cb); d = 0.0f; }
                else if (mb < 1e-9f) d = 0.0f;
                d -= 2.0f * static_cast<float>(M_PI) * std::round(d / (2.0f * static_cast<float>(M_PI))); // menor arco
                // fase oposta (+-180 graus) e ambigua: o arredondamento escolhia +90 pra
                // uns harmonicos e -90 pra outros, espalhando as fases (onda com cara de
                // ruido no meio do morph). Sempre o mesmo sentido mantem a onda coerente.
                if (d < -static_cast<float>(M_PI) + 1e-3f) d += 2.0f * static_cast<float>(M_PI);
                x[static_cast<size_t>(h)] = std::polar(ma + (mb - ma) * t, pa + d * t);
            }
            buildFrame(f, x, gainA + (gainB - gainA) * t);
        }
    }

    int frames() const { return frames_; }

    // Uma amostra: fase 0..1, morph 0..1, frequencia do operador e taxa (interna).
    float read(float phase01, float morph, float freqHz, float sampleRate) const {
        const float maxH = 0.5f * sampleRate / std::max(freqHz, 1e-3f);
        if (maxH < 1.0f) return 0.0f; // nem a fundamental cabe abaixo de Nyquist
        const int k = maxH >= 1024.0f ? 0 : std::clamp(10 - std::ilogb(maxH), 0, kLevels - 1);
        const float pos = std::clamp(morph, 0.0f, 1.0f) * static_cast<float>(frames_ - 1);
        const int f0 = std::min(static_cast<int>(pos), frames_ - 2);
        const float u = pos - static_cast<float>(f0);
        const float idx = phase01 * static_cast<float>(size_[k]);
        const int i = std::min(static_cast<int>(idx), size_[k] - 1);
        const float fr = idx - static_cast<float>(i);
        const float* p0 = &data_[static_cast<size_t>(f0 * stride_ + offset_[k] + i)];
        const float* p1 = p0 + stride_;
        const float a = p0[0] + (p0[1] - p0[0]) * fr, b = p1[0] + (p1[1] - p1[0]) * fr;
        return a + (b - a) * u;
    }

    // Um ciclo (nivel 0, sem limite de banda extra) na posicao `morph`, pra UI.
    void preview(float morph, float* out, int n) const {
        for (int j = 0; j < n; ++j) out[j] = read(static_cast<float>(j) / static_cast<float>(n), morph, 1.0f, 48000.0f);
    }

private:
    // Espectro de um ciclo (bins 0..N/2, normalizado por N; DC zerado). nullptr = seno.
    static std::vector<std::complex<float>> spectrum(const std::vector<float>* w) {
        std::vector<std::complex<float>> out(kWaveSize / 2 + 1);
        if (!w || static_cast<int>(w->size()) != kWaveSize) { out[1] = std::complex<float>(0.0f, -0.5f); return out; } // sin = (e^jx - e^-jx)/2j
        std::vector<std::complex<float>> buf(kWaveSize);
        for (int i = 0; i < kWaveSize; ++i) buf[static_cast<size_t>(i)] = (*w)[static_cast<size_t>(i)];
        fftInPlace(buf);
        for (int h = 1; h <= kWaveSize / 2; ++h) out[static_cast<size_t>(h)] = buf[static_cast<size_t>(h)] / static_cast<float>(kWaveSize);
        return out;
    }

    // Ganho que leva o pico do ciclo completo (nivel 0) a 1.
    static float peakGain(const std::vector<std::complex<float>>& x) {
        std::vector<float> lvl(static_cast<size_t>(kWaveSize));
        synth(x, kWaveSize, 1023, lvl.data());
        float peak = 0.0f;
        for (float v : lvl) peak = std::max(peak, std::fabs(v));
        return peak > 1e-9f ? 1.0f / peak : 1.0f;
    }

    // Um ciclo de n amostras com os harmonicos 1..maxH do espectro x.
    static void synth(const std::vector<std::complex<float>>& x, int n, int maxH, float* out) {
        std::vector<std::complex<float>> y(static_cast<size_t>(n));
        for (int h = 1; h <= maxH; ++h) {
            // x[n] = sum_h 2 Re(c_h e^{j 2pi h n/N}): conjugados espelhados
            y[static_cast<size_t>(h)] = std::conj(x[static_cast<size_t>(h)]);
            y[static_cast<size_t>(n - h)] = x[static_cast<size_t>(h)];
        }
        fftInPlace(y); // FFT de conj(Y) = conj(IFFT(Y)) * n; parte real = sinal
        for (int i = 0; i < n; ++i) out[i] = y[static_cast<size_t>(i)].real();
    }

    // Monta os kLevels niveis de um frame a partir do espectro x (bins 0..N/2).
    void buildFrame(int f, const std::vector<std::complex<float>>& x, float gain) {
        float* base = &data_[static_cast<size_t>(f * stride_)];
        for (int k = 0; k < kLevels; ++k) {
            const int n = size_[k], maxH = std::min(1024 >> k, n / 2 - 1);
            float* lvl = base + offset_[k];
            synth(x, n, maxH, lvl);
            for (int i = 0; i < n; ++i) lvl[i] *= gain;
            lvl[n] = lvl[0];
        }
    }

    int frames_ = 2, stride_ = 0;
    int size_[kLevels] = {}, offset_[kLevels] = {};
    std::vector<float> data_;
};

} // namespace dx7fm
