#pragma once
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <vector>

// [ADICIONADO em 2026-10-20] Presets e matematica do editor da forma de onda
// Additive (64 parciais), sem ImGui -- testados no demo.cpp. O desenho fica
// em additive_editor.h.
namespace dx7fm_app {

using PartialSet = dx7fm::OperatorParams::PartialSet;
using dx7fm::kNumPartials;
using dx7fm::kMaxPartialRatio;

// --- eixo de razao do editor (escala log) ------------------------------------
// Razoes abaixo de kRatioAxisMin cabem no parametro (ate 0.01) mas ficam no
// pe do grafico; editar elas so pelo campo numerico.
constexpr float kRatioAxisMin = 0.25f;

inline float ratioToAxis(float r) {
    float t = std::log(std::max(r, kRatioAxisMin) / kRatioAxisMin) / std::log(kMaxPartialRatio / kRatioAxisMin);
    return std::clamp(t, 0.0f, 1.0f);
}

// snap: inteiros (harmonicos) acima de 1, subharmonicos 1/n abaixo.
inline float axisToRatio(float t, bool snap) {
    float r = kRatioAxisMin * std::pow(kMaxPartialRatio / kRatioAxisMin, std::clamp(t, 0.0f, 1.0f));
    if (snap) r = r >= 1.0f ? std::round(r) : 1.0f / std::round(1.0f / r);
    return std::clamp(r, kRatioAxisMin, kMaxPartialRatio);
}

// eixo de amplitude: linear, ou em dB (-60..0 dB; o pe do grafico = desligado)
constexpr float kAmpAxisDbRange = 60.0f;

inline float ampToAxis(float a, bool db) {
    if (!db) return std::clamp(a, 0.0f, 1.0f);
    if (a <= 0.0f) return 0.0f;
    return std::clamp(1.0f + 20.0f * std::log10(a) / kAmpAxisDbRange, 0.0f, 1.0f);
}

inline float axisToAmp(float t, bool db) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (!db) return t;
    return t <= 0.0f ? 0.0f : std::pow(10.0f, (t - 1.0f) * kAmpAxisDbRange / 20.0f);
}

// Traco do mouse entre dois quadros: preenche TODAS as barras entre k0 e k1
// (arrastar rapido pula barras), interpolando o valor do eixo t.
template <typename SetFn>
inline void paintStroke(int k0, float t0, int k1, float t1, SetFn set) {
    k0 = std::clamp(k0, 0, kNumPartials - 1);
    k1 = std::clamp(k1, 0, kNumPartials - 1);
    if (k0 == k1) { set(k1, t1); return; }
    const int step = k1 > k0 ? 1 : -1;
    for (int k = k0; k != k1 + step; k += step) set(k, t0 + (t1 - t0) * static_cast<float>(k - k0) / static_cast<float>(k1 - k0));
}

inline int activePartials(const PartialSet& s) {
    int n = 0;
    for (float a : s.amp) n += a > 0.0f;
    return n;
}

// --- presets -------------------------------------------------------------------
namespace presets {

inline void clear(PartialSet& s) {
    for (int k = 0; k < kNumPartials; ++k) { s.amp[k] = 0.0f; s.ratio[k] = static_cast<float>(k + 1); }
}

inline void sine(PartialSet& s) { clear(s); s.amp[0] = 1.0f; }

inline void saw(PartialSet& s) {
    for (int k = 0; k < kNumPartials; ++k) { s.amp[k] = 1.0f / (k + 1); s.ratio[k] = static_cast<float>(k + 1); }
}

// impares 1, 3, 5 .. 127; amplitude 1/k (quadrada) ou 1/k^2 (triangulo)
inline void odd(PartialSet& s, float power) {
    for (int k = 0; k < kNumPartials; ++k) {
        float h = static_cast<float>(2 * k + 1);
        s.ratio[k] = h;
        s.amp[k] = std::pow(h, -power);
    }
}

// Orgao de drawbars (16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'), registro "888000000" + um pouco de brilho
inline void organ(PartialSet& s) {
    clear(s);
    const float r[9] = {0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
    const float a[9] = {0.9f, 0.9f, 1.0f, 0.6f, 0.45f, 0.4f, 0.25f, 0.25f, 0.2f};
    for (int k = 0; k < 9; ++k) { s.ratio[k] = r[k]; s.amp[k] = a[k]; }
}

// Formantes de vogal "a" (~700 Hz e ~1200 Hz numa voz em ~140 Hz): envelope
// gaussiano sobre a serie harmonica.
inline void vowel(PartialSet& s) {
    auto g = [](float k, float c, float w) { return std::exp(-0.5f * (k - c) * (k - c) / (w * w)); };
    for (int k = 0; k < kNumPartials; ++k) {
        float h = static_cast<float>(k + 1);
        s.ratio[k] = h;
        float a = 0.35f * g(h, 1.0f, 1.2f) + g(h, 5.0f, 1.3f) + 0.7f * g(h, 8.5f, 1.8f) + 0.25f * g(h, 18.0f, 3.0f);
        s.amp[k] = a < 0.01f ? 0.0f : std::min(a, 1.0f);
    }
}

// Barra livre (sino/xilofone/glockenspiel): modos em (beta_n / beta_1)^2,
// beta_n raizes de cos(b)cosh(b) = 1 (beta_1 = 4.7300). So cabem os modos
// ate kMaxPartialRatio (16 deles); o resto fica desligado.
inline std::vector<float> freeBarRatios() {
    std::vector<float> out;
    auto f = [](double b) { return std::cos(b) - 1.0 / std::cosh(b); };
    double b1 = 0.0;
    for (int n = 1; static_cast<int>(out.size()) < kNumPartials; ++n) {
        double lo = (n + 0.5) * M_PI - 0.5, hi = (n + 0.5) * M_PI + 0.5;
        for (int it = 0; it < 60; ++it) {
            double mid = 0.5 * (lo + hi);
            ((f(lo) < 0) == (f(mid) < 0) ? lo : hi) = mid;
        }
        double b = 0.5 * (lo + hi);
        if (n == 1) b1 = b;
        double r = (b / b1) * (b / b1);
        if (r > kMaxPartialRatio) break;
        out.push_back(static_cast<float>(r));
    }
    return out;
}

inline void bell(PartialSet& s) {
    clear(s);
    static const auto r = freeBarRatios();
    for (size_t k = 0; k < r.size(); ++k) {
        s.ratio[k] = r[k];
        s.amp[k] = 1.0f / (1.0f + 0.5f * static_cast<float>(k));
    }
}

// Membrana circular (tambor/timpano): modos (m, n) em j_{m,n} / j_{0,1},
// j_{m,n} = n-esimo zero de J_m. Os 64 modos mais graves, em ordem.
inline std::vector<float> membraneRatios() {
    std::vector<double> zeros;
    const double xMax = 40.0, dx = 0.02;
    for (int m = 0; m < 40; ++m) {
        double prev = std::cyl_bessel_j(static_cast<double>(m), dx);
        for (double x = 2 * dx; x < xMax; x += dx) {
            double cur = std::cyl_bessel_j(static_cast<double>(m), x);
            if ((prev < 0) != (cur < 0)) {
                double lo = x - dx, hi = x;
                for (int it = 0; it < 40; ++it) {
                    double mid = 0.5 * (lo + hi);
                    ((std::cyl_bessel_j(static_cast<double>(m), lo) < 0) == (std::cyl_bessel_j(static_cast<double>(m), mid) < 0) ? lo : hi) = mid;
                }
                zeros.push_back(0.5 * (lo + hi));
            }
            prev = cur;
        }
    }
    std::sort(zeros.begin(), zeros.end());
    std::vector<float> out;
    for (size_t i = 0; i < zeros.size() && static_cast<int>(out.size()) < kNumPartials; ++i)
        out.push_back(static_cast<float>(zeros[i] / zeros[0]));
    return out;
}

inline void drum(PartialSet& s) {
    clear(s);
    static const auto r = membraneRatios(); // ~25ms: calcula so no primeiro uso
    for (size_t k = 0; k < r.size(); ++k) {
        s.ratio[k] = r[k];
        s.amp[k] = std::pow(r[k], -1.6f);
    }
}

} // namespace presets

struct AdditivePreset {
    const char* name;
    const char* tip;
    void (*make)(PartialSet&);
};

inline const AdditivePreset kAdditivePresets[] = {
    {"Seno", "so o parcial 1", presets::sine},
    {"Serra", "harmonicos 1..64 com amplitude 1/k", presets::saw},
    {"Quadrada", "harmonicos impares 1, 3 .. 127 com amplitude 1/k", [](PartialSet& s) { presets::odd(s, 1.0f); }},
    {"Triangulo", "harmonicos impares 1, 3 .. 127 com amplitude 1/k^2", [](PartialSet& s) { presets::odd(s, 2.0f); }},
    {"Orgao", "drawbars 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'", presets::organ},
    {"Vogal", "formantes da vogal \"a\" sobre os harmonicos (melhor em notas graves)", presets::vowel},
    {"Sino", "modos de uma barra livre (inarmonicos): 1, 2.76, 5.40, 8.93 .. ate 128x", presets::bell},
    {"Tambor", "64 modos de uma membrana circular (inarmonicos): 1, 1.59, 2.14, 2.30 ..", presets::drum},
};

} // namespace dx7fm_app
