#pragma once
#include "dx7_dsp.h"
#include "dx7_filters.h"
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

// [ADICIONADO em 2026-10-20] Conta dos graficos do painel de efeito por
// operador (op_fx_ui.h), sem ImGui -- testada no demo.cpp contra os blocos de
// DSP de verdade (TPTStateVariableFilter, CombFilter), pra o desenho nao mentir.
//
// O que fxParam1 / fxParam2 significam em cada efeito (ver Voice::process):
//   LowPass/BandPass/HighPass  corte (Hz)            Q (0.1..16)
//   Saturate                   --                    drive (0..16)
//   Crush                      taxa de amostragem Hz bits / 16
//   FreqShift                  deslocamento (Hz)     --
//   Comb                       afinacao (Hz)         feedback (0..0.99)
//   Fold                       drive (1..16)         simetria (0..1)
//   Waveshape                  drive (1..16)         bias (0..1); curva em fxShape
//   Notch                      corte (Hz)            Q (0.1..16); sempre SVF
// O fxEG so mexe nos filtros (corte de 20% a 100% de FX1) e no Fold (drive);
// a mod matrix (OperatorFx1) multiplica FX1 por (1 + quantidade).
namespace dx7fm_app::opfx {

using dx7fm::FxType;

inline bool isFilter(FxType t) { return t == FxType::LowPass || t == FxType::BandPass || t == FxType::HighPass || t == FxType::Notch; }
// filtros que aceitam OperatorParams::fxModel
inline bool hasModel(FxType t) { return t == FxType::LowPass || t == FxType::BandPass || t == FxType::HighPass; }
inline bool egSweeps(FxType t) { return isFilter(t) || t == FxType::Fold || t == FxType::Waveshape; }
// efeitos cujo FX1 e uma frequencia: os que o key tracking (fxKeyTrack) afeta
inline bool keyTracks(FxType t) { return isFilter(t) || t == FxType::Crush || t == FxType::FreqShift || t == FxType::Comb; }
inline bool usesParam1(FxType t) { return isFilter(t) || t == FxType::Crush || t == FxType::FreqShift || t == FxType::Comb || t == FxType::Fold || t == FxType::Waveshape; }

inline const char* filterModelLabel(dx7fm::FilterModel m) {
    switch (m) {
        case dx7fm::FilterModel::Svf: return "SVF 12 dB";
        case dx7fm::FilterModel::Svf24: return "SVF 24 dB";
        case dx7fm::FilterModel::Ladder: return "Ladder 24 dB";
        case dx7fm::FilterModel::Diode: return "Diodo (acid)";
        case dx7fm::FilterModel::Ms20: return "Sallen-Key (MS)";
    }
    return "?";
}
inline const char* waveshapeLabel(dx7fm::WaveshapeCurve c) {
    switch (c) {
        case dx7fm::WaveshapeCurve::Soft: return "Suave";
        case dx7fm::WaveshapeCurve::Hard: return "Clip";
        case dx7fm::WaveshapeCurve::Sine: return "Seno";
        case dx7fm::WaveshapeCurve::Tube: return "Valvula";
        case dx7fm::WaveshapeCurve::Cheby3: return "Chebyshev 3";
        case dx7fm::WaveshapeCurve::Rectify: return "Retificador";
    }
    return "?";
}

struct Info {
    const char* name;  // rotulo do botao
    const char* about; // uma linha
};

inline Info info(FxType t) {
    switch (t) {
        case FxType::Bypass:    return {"Nenhum", "Sem efeito: o oscilador sai direto."};
        case FxType::LowPass:   return {"Passa-baixa", "Corta os agudos acima do corte; o carater vem do modelo."};
        case FxType::BandPass:  return {"Passa-banda", "Deixa so uma faixa em volta do corte (Q estreita a faixa)."};
        case FxType::HighPass:  return {"Passa-alta", "Corta os graves abaixo do corte; o carater vem do modelo."};
        case FxType::RingMod:   return {"Ring mod", "Multiplica pela saida de outro operador: soma e diferenca das frequencias."};
        case FxType::Saturate:  return {"Saturacao", "tanh: arredonda os picos, harmonicos impares, mais forte com o drive."};
        case FxType::Crush:     return {"Bitcrusher", "Reduz a taxa de amostragem (aliasing) e os bits (degraus)."};
        case FxType::FreqShift: return {"Freq shift", "Soma Hz fixos a todas as frequencias: harmonicos viram inarmonicos."};
        case FxType::Comb:      return {"Comb", "Delay curto com feedback: ressoa na afinacao e nos multiplos (metalico)."};
        case FxType::Fold:      return {"Wavefolder", "Dobra o sinal que passa do limite: harmonicos brilhantes (estilo Buchla)."};
        case FxType::Waveshape: return {"Waveshaper", "Passa o sinal por uma curva: cada curva cria um conjunto de harmonicos."};
        case FxType::Notch:     return {"Rejeita-faixa", "Filtro SVF: remove uma faixa em volta do corte (Q estreita o corte)."};
    }
    return {"?", ""};
}

// Valores iniciais sensatos ao trocar de efeito (os parametros mudam de
// significado: 1000 de corte viraria drive 16 no Fold). Entre os 3 filtros
// mantem corte e Q.
inline void applyDefaults(FxType from, FxType to, float& p1, float& p2) {
    if (isFilter(from) && isFilter(to)) return;
    switch (to) {
        case FxType::LowPass: case FxType::BandPass: case FxType::HighPass: case FxType::Notch: p1 = 2000.0f; p2 = 0.707f; break;
        case FxType::Saturate:  p2 = 1.0f; break;
        case FxType::Crush:     p1 = 8000.0f; p2 = 0.5f; break; // 8 bits
        case FxType::FreqShift: p1 = 50.0f; break;
        case FxType::Comb:      p1 = 440.0f; p2 = 0.7f; break;
        case FxType::Fold:      p1 = 3.0f; p2 = 0.0f; break;
        case FxType::Waveshape: p1 = 2.0f; p2 = 0.0f; break;
        default: break;
    }
}

// Resposta do TPTStateVariableFilter (exata, nao a do analogico): o TPT e a transformada
// bilinear com pre-warp do prototipo H(s), entao em z = e^{jw} vale
// s = j tan(pi f / sr) / g, com g = tan(pi fc / sr).
inline float svfMagnitudeDb(FxType t, float f, float fc, float q, float sr) {
    fc = std::clamp(fc, 10.0f, sr * 0.49f);
    q = std::max(0.05f, q);
    const float g = std::tan(static_cast<float>(M_PI) * fc / sr);
    const std::complex<float> s(0.0f, std::tan(static_cast<float>(M_PI) * std::min(f, sr * 0.4999f) / sr) / g);
    const std::complex<float> den = s * s + s / q + 1.0f;
    std::complex<float> h = t == FxType::LowPass ? 1.0f / den : t == FxType::BandPass ? s / den
                         : t == FxType::Notch ? (s * s + 1.0f) / den : s * s / den;
    return 20.0f * std::log10(std::max(std::abs(h), 1e-6f));
}

// Resposta do filtro com o modelo escolhido (Notch e sempre SVF)
inline float filterMagnitudeDb(FxType t, dx7fm::FilterModel m, float f, float fc, float q, float sr) {
    if (!hasModel(t) || m == dx7fm::FilterModel::Svf) return svfMagnitudeDb(t, f, fc, q, sr);
    const int mode = t == FxType::LowPass ? dx7fm::kFilterLP : t == FxType::BandPass ? dx7fm::kFilterBP : dx7fm::kFilterHP;
    return dx7fm::modelFilterMagnitudeDb(m, mode, f, fc, q, sr);
}

// Comb: y = atraso(x + g*y) => |H| = 1 / |1 - g e^{-j 2 pi f / fd}|
inline float combMagnitudeDb(float f, float fd, float fb) {
    fb = std::clamp(fb, -0.99f, 0.99f);
    const std::complex<float> e = std::polar(1.0f, -2.0f * static_cast<float>(M_PI) * f / std::max(fd, 1.0f));
    return -20.0f * std::log10(std::max(std::abs(1.0f - fb * e), 1e-6f));
}

// Curva de transferencia dos efeitos sem memoria (entrada -> saida, sem o
// sample & hold do Crush). eg01 = nivel do fxEG (so o Fold usa).
inline float shape(FxType t, float x, float p1, float p2, float eg01 = 1.0f, dx7fm::WaveshapeCurve curve = dx7fm::WaveshapeCurve::Soft) {
    switch (t) {
        case FxType::Saturate: return dx7fm::softSaturate(x, p2);
        case FxType::Crush: {
            float half = 0.5f * std::exp2(std::clamp(p2 * 16.0f, 1.0f, 24.0f));
            return std::round(x * half) / half;
        }
        case FxType::Fold: {
            float maxDrive = std::clamp(p1, 1.0f, 16.0f);
            float drive = 1.0f + (maxDrive - 1.0f) * (0.2f + 0.8f * eg01);
            return dx7fm::triangleFold(x * drive + std::clamp(p2, 0.0f, 1.0f));
        }
        case FxType::Waveshape: {
            float maxDrive = std::clamp(p1, 1.0f, 16.0f);
            float drive = 1.0f + (maxDrive - 1.0f) * (0.2f + 0.8f * eg01);
            return dx7fm::waveshapeCurve(curve, x * drive + std::clamp(p2, 0.0f, 1.0f));
        }
        default: return x;
    }
}

// Resumo curto pra tabela de operadores (ex. "2.0 kHz  Q 0.7").
inline void summary(const dx7fm::OperatorParams& op, char* out, size_t n) {
    auto hz = [](float v, char* b, size_t m) {
        if (v >= 1000.0f) std::snprintf(b, m, "%.1f kHz", v / 1000.0f);
        else std::snprintf(b, m, "%.0f Hz", v);
    };
    char h[24];
    switch (op.fxType) {
        case FxType::Bypass: std::snprintf(out, n, "-"); break;
        case FxType::LowPass: case FxType::BandPass: case FxType::HighPass:
            hz(op.fxParam1, h, sizeof h);
            if (op.fxModel == dx7fm::FilterModel::Svf) std::snprintf(out, n, "%s  Q %.1f", h, op.fxParam2);
            else std::snprintf(out, n, "%s  %s  Q %.1f", filterModelLabel(op.fxModel), h, op.fxParam2);
            break;
        case FxType::Notch: hz(op.fxParam1, h, sizeof h); std::snprintf(out, n, "%s  Q %.1f", h, op.fxParam2); break;
        case FxType::Waveshape: std::snprintf(out, n, "%s  drive %.1f", waveshapeLabel(op.fxShape), op.fxParam1); break;
        case FxType::RingMod:
            if (op.ringModSource >= 0) std::snprintf(out, n, "x Op%d", op.ringModSource + 1);
            else std::snprintf(out, n, "sem fonte");
            break;
        case FxType::Saturate: std::snprintf(out, n, "drive %.1f", op.fxParam2); break;
        case FxType::Crush: hz(op.fxParam1, h, sizeof h); std::snprintf(out, n, "%s  %.0f bits", h, std::clamp(op.fxParam2 * 16.0f, 1.0f, 24.0f)); break;
        case FxType::FreqShift: std::snprintf(out, n, "+%.0f Hz", op.fxParam1); break;
        case FxType::Comb: hz(op.fxParam1, h, sizeof h); std::snprintf(out, n, "%s  fb %.2f", h, std::min(op.fxParam2, 0.99f)); break;
        case FxType::Fold: std::snprintf(out, n, "drive %.1f  sim %.2f", op.fxParam1, op.fxParam2); break;
    }
}

} // namespace dx7fm_app::opfx
