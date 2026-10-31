#pragma once
// [ADICIONADO em 2026-10-30] Modelos de filtro do efeito por operador, alem do
// SVF de 12 dB (TPTStateVariableFilter, dx7_dsp.h). Implementacao propria,
// zero-delay feedback (trapezoidal com pre-warp), a partir das topologias
// publicadas em Vadim Zavalishin, "The Art of VA Filter Design":
//   Svf24   dois SVF em cascata (24 dB/oit, limpo)
//   Ladder  4 polos iguais com realimentacao global (estilo transistor ladder);
//           BP/HP pelas somas de saidas do Oberheim Xpander
//   Diode   4 polos acoplados (estilo diode ladder de baixo acido): cada estagio
//           carrega o vizinho, por isso a ressonancia e a inclinacao sao outras
//   Ms20    Sallen-Key de 2 polos com realimentacao saturada (estilo Korg 35)
// Ladder e Diode saturam 2*tanh(u/2) na soma de entrada; o Ms20 reduz o ganho
// da realimentacao com o nivel -- a auto-oscilacao fica limitada. Q 0.707 =
// sem ressonancia e Q 16 = auto-oscila (no Ladder, Diode e Ms20).
// modelFilterMagnitudeDb() da a resposta linear pro grafico da UI (o demo
// confere contra o DSP medido).
#include "dx7_dsp.h"
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dx7fm {

enum FilterMode : int { kFilterLP = 0, kFilterBP = 1, kFilterHP = 2 };

namespace filterdetail {
// Diode: prototipo u -> y4 com fase de -180 graus em w0 e |H| = 1 / kMax
constexpr float kDiodeW0 = 1.1952286f, kDiodeKMax = 18.387755f;
constexpr float kLadderKMax = 4.0f;
// 0 ate Q 0.707, chega a kMax*1.02 em Q 16 (log: o knob de Q e logaritmico)
inline float resonance01(float q) {
    return std::clamp(std::log(std::max(q, 0.1f) / 0.707f) / std::log(16.0f / 0.707f), 0.0f, 1.0f);
}
inline float ladderK(float q) { return kLadderKMax * 1.02f * resonance01(q); }
inline float diodeK(float q) { return kDiodeKMax * 1.02f * resonance01(q); }
// 2 - 1/Q; Q alto passa um pouco de 2 pra auto-oscilar
inline float ms20K(float q) {
    const float x = std::clamp(std::max(q, 0.5f), 0.5f, 16.0f);
    const float r = x / 16.0f;
    return 2.0f - 1.0f / x + 0.1f * r * r * r * r;
}
inline float softInput(float u) { return 2.0f * std::tanh(0.5f * u); }
inline float ladderComp(float k) { return 1.0f + 0.5f * k; } // repoe parte do grave que a ressonancia tira
inline float diodeComp(float k) { return 1.0f + 0.55f * k; }  // o mesmo: k vai ate ~18, e nao 4; grave fica em ~-4 dB no maximo
inline float svf24Q2(float q) { return std::max(q, 0.1f) * 1.85f; } // 0.707 -> Butterworth (1.31)
constexpr float kSvf24Q1 = 0.541f;
} // namespace filterdetail

class ModelFilter {
public:
    void setSampleRate(double sr) {
        sr_ = sr;
        a_.setSampleRate(sr);
        b_.setSampleRate(sr);
        lastFc_ = -1.0f;
    }
    void reset() {
        a_.reset();
        b_.reset();
        for (float& v : s_) v = 0.0f;
        v_ = 0.0f;
    }
    float process(float x, float fc, float q, FilterModel model, int mode) {
        if (model != lastModel_) { reset(); lastModel_ = model; lastFc_ = -1.0f; }
        float y = 0.0f;
        switch (model) {
            case FilterModel::Svf24: y = svf24(x, fc, q, mode); break;
            case FilterModel::Ladder: y = ladder(x, fc, q, mode); break;
            case FilterModel::Diode: y = diode(x, fc, q, mode); break;
            case FilterModel::Ms20: y = ms20(x, fc, q, mode); break;
            case FilterModel::Svf: {
                a_.setParams(fc, std::max(0.1f, q));
                const auto o = a_.process(x);
                y = mode == kFilterLP ? o.lp : mode == kFilterBP ? o.bp : o.hp;
                break;
            }
        }
        if (!std::isfinite(y)) { reset(); return 0.0f; } // nao deixa um NaN preso no estado
        return y;
    }

private:
    float warped(float fc) {
        fc = std::clamp(fc, 10.0f, static_cast<float>(sr_) * 0.45f);
        if (fc != lastFc_) { lastFc_ = fc; g_ = std::tan(kPi * fc / static_cast<float>(sr_)); }
        return g_;
    }
    float svf24(float x, float fc, float q, int mode) {
        a_.setParams(fc, filterdetail::kSvf24Q1);
        b_.setParams(fc, filterdetail::svf24Q2(q));
        const auto o1 = a_.process(x);
        if (mode == kFilterLP) return b_.process(o1.lp).lp;
        if (mode == kFilterHP) return b_.process(o1.hp).hp;
        return b_.process(o1.bp / filterdetail::kSvf24Q1).bp; // BP de pico = Q, como o de 12 dB
    }
    float ladder(float x, float fc, float q, int mode) {
        const float g = warped(fc), G = g / (1.0f + g), inv = 1.0f / (1.0f + g);
        const float k = filterdetail::ladderK(q);
        const float s0 = s_[0] * inv, s1 = s_[1] * inv, s2 = s_[2] * inv, s3 = s_[3] * inv;
        const float S = G * (G * (G * s0 + s1) + s2) + s3;
        const float G4 = G * G * G * G;
        const float u = filterdetail::softInput((x - k * S) / (1.0f + k * G4));
        const float y1 = G * u + s0, y2 = G * y1 + s1, y3 = G * y2 + s2, y4 = G * y3 + s3;
        s_[0] = 2.0f * y1 - s_[0];
        s_[1] = 2.0f * y2 - s_[1];
        s_[2] = 2.0f * y3 - s_[2];
        s_[3] = 2.0f * y4 - s_[3];
        if (mode == kFilterLP) return y4 * filterdetail::ladderComp(k);
        if (mode == kFilterBP) return 4.0f * y2 - 8.0f * y3 + 4.0f * y4;
        return u - 4.0f * y1 + 6.0f * y2 - 4.0f * y3 + y4;
    }
    // (I - gA) y = s + g B u, A tridiagonal: u - 2y1 + y2, y1 - 2y2 + y3, y2 - 2y3 + y4, y3 - y4
    float diode(float x, float fc, float q, int mode) {
        const float g = warped(fc) / filterdetail::kDiodeW0;
        const float k = filterdetail::diodeK(q);
        if (g != thomasG_) { // eliminacao de Thomas: so refaz quando o corte muda
            thomasG_ = g;
            const float d = 1.0f + 2.0f * g;
            c_[0] = -g / d;
            piv_[0] = d;
            for (int i = 1; i < 4; ++i) {
                piv_[i] = (i == 3 ? 1.0f + g : d) + g * c_[i - 1];
                c_[i] = -g / piv_[i];
            }
        }
        auto solve = [&](const float* r, float* out) {
            float t[4];
            t[0] = r[0] / piv_[0];
            for (int i = 1; i < 4; ++i) t[i] = (r[i] + g * t[i - 1]) / piv_[i];
            out[3] = t[3];
            for (int i = 2; i >= 0; --i) out[i] = t[i] - c_[i] * out[i + 1];
        };
        const float e[4] = {g, 0.0f, 0.0f, 0.0f};
        float P[4], Q[4];
        solve(s_, P);
        solve(e, Q);
        const float u = filterdetail::softInput((x - k * P[3]) / (1.0f + k * Q[3]));
        float y[4];
        for (int i = 0; i < 4; ++i) { y[i] = P[i] + Q[i] * u; s_[i] = 2.0f * y[i] - s_[i]; }
        if (mode == kFilterLP) return y[3] * filterdetail::diodeComp(k);
        if (mode == kFilterBP) return (y[1] - y[3]) * filterdetail::diodeComp(k);
        return u - y[3];
    }
    // y1' = u - y1 + k(y1 - y2), y2' = y1 - y2 => 1 / (s^2 + (2 - k)s + 1)
    float ms20(float x, float fc, float q, int mode) {
        const float g = warped(fc);
        const float av = std::fabs(v_);
        const float r = av > 1e-4f ? std::tanh(av) / av : 1.0f; // o ganho cai com o nivel
        const float k = filterdetail::ms20K(q) * r;
        const float a11 = 1.0f + g - g * k, a12 = g * k, a22 = 1.0f + g; // a21 = -g
        const float b1 = s_[0] + g * x, b2 = s_[1];
        const float det = a11 * a22 + a12 * g;
        const float y1 = (b1 * a22 - a12 * b2) / det;
        const float y2 = (a11 * b2 + g * b1) / det;
        s_[0] = 2.0f * y1 - s_[0];
        s_[1] = 2.0f * y2 - s_[1];
        v_ = y1 - y2;
        if (mode == kFilterLP) return y2;
        if (mode == kFilterBP) return v_;
        return x - (2.0f - k) * v_ - y2;
    }

    double sr_ = 48000.0;
    TPTStateVariableFilter a_, b_;
    FilterModel lastModel_ = FilterModel::Svf;
    float s_[4] = {};
    float c_[4] = {}, piv_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float thomasG_ = -1.0f;
    float v_ = 0.0f;
    float lastFc_ = -1.0f, g_ = 0.0f;
};

// Resposta linear (sinal pequeno) de cada modelo, exata no dominio z: como nos
// TPT, s = j tan(pi f / sr) / tan(pi fc / sr).
inline float modelFilterMagnitudeDb(FilterModel model, int mode, float f, float fc, float q, float sr) {
    using C = std::complex<float>;
    using namespace filterdetail;
    fc = std::clamp(fc, 10.0f, sr * (model == FilterModel::Svf || model == FilterModel::Svf24 ? 0.49f : 0.45f));
    const C s(0.0f, std::tan(kPi * std::min(f, sr * 0.4999f) / sr) / std::tan(kPi * fc / sr));
    C h(1.0f, 0.0f);
    auto svf = [&](float qq) {
        const C den = s * s + s / std::max(0.05f, qq) + C(1.0f);
        return mode == kFilterLP ? C(1.0f) / den : mode == kFilterBP ? s / den : s * s / den;
    };
    switch (model) {
        case FilterModel::Svf: h = svf(q); break;
        case FilterModel::Svf24:
            h = svf(kSvf24Q1) * svf(svf24Q2(q));
            if (mode == kFilterBP) h /= kSvf24Q1;
            break;
        case FilterModel::Ladder: {
            const float k = ladderK(q);
            const C p = C(1.0f) / (s + C(1.0f)), p2 = p * p, p3 = p2 * p, p4 = p2 * p2;
            const C u = C(1.0f) / (C(1.0f) + k * p4);
            h = mode == kFilterLP ? u * p4 * ladderComp(k)
              : mode == kFilterBP ? u * (C(4.0f) * p2 - C(8.0f) * p3 + C(4.0f) * p4)
                                  : u * (C(1.0f) - C(4.0f) * p + C(6.0f) * p2 - C(4.0f) * p3 + p4);
            break;
        }
        case FilterModel::Diode: {
            const float k = diodeK(q);
            const C w = s * kDiodeW0, a = w + C(2.0f);
            const C y4(1.0f), y3 = (w + C(1.0f)) * y4, y2 = a * y3 - y4, y1 = a * y2 - y3, un = a * y1 - y2; // u (entrada) pra y4 = 1
            const C u = un / (un + k * y4); // entrada do laco com k, relativa a x
            const C out4 = u * y4 / un, out2 = u * y2 / un;
            const float comp = diodeComp(k);
            h = mode == kFilterLP ? out4 * comp : mode == kFilterBP ? (out2 - out4) * comp : u - out4;
            break;
        }
        case FilterModel::Ms20: {
            const float k = std::min(ms20K(q), 1.98f); // o grafico mostra ate o limite estavel
            const C den = s * s + (2.0f - k) * s + C(1.0f);
            h = mode == kFilterLP ? C(1.0f) / den : mode == kFilterBP ? s / den : s * s / den;
            break;
        }
    }
    return 20.0f * std::log10(std::max(std::abs(h), 1e-6f));
}

} // namespace dx7fm
