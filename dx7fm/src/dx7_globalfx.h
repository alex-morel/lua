#pragma once
#include "dx7_params.h"
#include "dx7_dsp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// MVerb (GPLv3, vendorizado sem modificacoes em third_party/mverb): usa
// memset/sin sem incluir nada (os includes acima resolvem) e gera warnings
// com -Wall -Wextra, silenciados so pra ele.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-private-field"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include "../third_party/mverb/MVerb.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// =============================================================================
// Efeitos globais POS-MIX ([REESCRITO em 2026-10-18]): aplicados uma vez na
// soma estereo das 4 partes, numa cadeia de ordem configuravel
// (GlobalFxParams::order). Cada bloco e baseado num efeito open source de
// referencia; origem e licenca no cabecalho de cada um e em
// THIRD_PARTY_NOTICES.md. ATENCAO: o modo Plate do reverb (MVerb) e o
// compressor (porta do ZamComp) sao GPL -- distribuir o dx7fm implica GPLv3
// (ver LICENSE).
//
// Convencoes: process() e chamado 1x por amostra estereo; todo bloco
// desligado e identidade EXATA (sai igual entra); buffers sao alocados em
// setSampleRate() (nunca na audio thread); ao ser religado, um bloco com
// memoria (delays, reverbs) zera o estado, pra nao "tocar de novo" uma cauda
// velha congelada.
// =============================================================================
namespace dx7fm {

namespace gfx {

inline float onePoleCoef(float hz, double sr) {
    return 1.0f - std::exp(-2.0f * kPi * std::max(hz, 1.0f) / static_cast<float>(sr));
}

// Linha de atraso com leitura fracionaria (interpolacao linear), tamanho
// potencia de 2. read(d) conta a partir da PROXIMA escrita: read(1) = a
// ultima amostra escrita. Lendo ANTES de escrever a amostra atual (delay,
// flanger), read(D) = x[n-D]; escrevendo antes (chorus, predelay), o
// atraso equivalente e read(D+1).
class DelayLine {
public:
    void allocate(size_t minSize) {
        size_t n = 1;
        while (n < minSize + 4) n <<= 1;
        buf_.assign(n, 0.0f);
        mask_ = n - 1;
        pos_ = 0;
    }
    void clear() { std::fill(buf_.begin(), buf_.end(), 0.0f); pos_ = 0; }
    void write(float x) { buf_[pos_] = x; pos_ = (pos_ + 1) & mask_; }
    float read(float delaySamples) const {
        float d = std::clamp(delaySamples, 1.0f, static_cast<float>(mask_ - 2));
        int di = static_cast<int>(d);
        float frac = d - static_cast<float>(di);
        size_t i0 = (pos_ - static_cast<size_t>(di)) & mask_;      // escrita ha di amostras
        size_t i1 = (pos_ - static_cast<size_t>(di) - 1) & mask_;  // ha di+1
        return buf_[i0] * (1.0f - frac) + buf_[i1] * frac;
    }
    size_t size() const { return buf_.size(); }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    size_t pos_ = 0;
};

} // namespace gfx

// -----------------------------------------------------------------------------
// Chorus "ensemble" -- porta do Ensemble do Mutable Instruments Rings
// (rings/dsp/fx/ensemble.h). Copyright 2014 Emilie Gillet. Licenca MIT:
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including
//   without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit
//   persons to whom the Software is furnished to do so, subject to the
//   following conditions: The above copyright notice and this permission
//   notice shall be included in all copies or substantial portions of the
//   Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
// Mudancas na porta: float em vez de buffer de 16 bits; atrasos/LFOs
// escalados pela taxa de amostragem (o original roda fixo a 48kHz); taxa do
// LFO lento exposta (o rapido mantem a proporcao original 1.37e-4/1.57e-5);
// e o canal R le com as modulacoes INVERTIDAS (como o chorus do Juno). No
// original L e R sao simetricos -- com entrada mono (L == R, o caso do
// synth com as partes no centro) a saida seria mono, sem largura nenhuma;
// no Rings isso nao aparece porque a entrada ja e estereo.
// -----------------------------------------------------------------------------
class EnsembleChorus {
public:
    void setSampleRate(double sr) {
        sr_ = sr;
        scale_ = static_cast<float>(sr / 48000.0);
        lineL_.allocate(static_cast<size_t>(2048.0f * scale_) + 8);
        lineR_.allocate(static_cast<size_t>(2048.0f * scale_) + 8);
        phase1_ = phase2_ = 0.0f;
    }
    void clear() { lineL_.clear(); lineR_.clear(); }

    void process(const ChorusParams& p, float& l, float& r) {
        constexpr float kFastRatio = 1.37e-04f / 1.57e-05f; // razao dos 2 LFOs no Rings
        phase1_ += std::clamp(p.rateHz, 0.01f, 10.0f) / static_cast<float>(sr_);
        if (phase1_ >= 1.0f) phase1_ -= 1.0f;
        phase2_ += std::clamp(p.rateHz, 0.01f, 10.0f) * kFastRatio / static_cast<float>(sr_);
        if (phase2_ >= 1.0f) phase2_ -= 1.0f;
        auto at = [](float ph) { ph -= std::floor(ph); return fastSin01(ph); };
        float slow0 = at(phase1_), slow120 = at(phase1_ + 1.0f / 3.0f), slow240 = at(phase1_ + 2.0f / 3.0f);
        float fast0 = at(phase2_), fast120 = at(phase2_ + 1.0f / 3.0f), fast240 = at(phase2_ + 2.0f / 3.0f);

        float a = std::clamp(p.depth, 0.0f, 1.0f) * 128.0f * scale_;
        float b = a * 0.1f;
        float mod1 = slow0 * a + fast0 * b;
        float mod2 = slow120 * a + fast120 * b;
        float mod3 = slow240 * a + fast240 * b;
        float base = 1024.0f * scale_;

        lineL_.write(l);
        lineR_.write(r);
        // +1: escreveu antes de ler (no FxEngine do Rings o offset conta a partir da escrita)
        float wetL = 0.33f * (lineL_.read(base + mod1 + 1) + lineL_.read(base + mod2 + 1) + lineR_.read(base + mod3 + 1));
        float wetR = 0.33f * (lineR_.read(base - mod1 + 1) + lineR_.read(base - mod2 + 1) + lineL_.read(base - mod3 + 1));
        float amount = std::clamp(p.mix, 0.0f, 1.0f);
        float dry = 1.0f - amount * 0.5f;
        l = wetL * amount + l * dry;
        r = wetR * amount + r * dry;
    }

private:
    double sr_ = 48000.0;
    float scale_ = 1.0f;
    gfx::DelayLine lineL_, lineR_;
    float phase1_ = 0.0f, phase2_ = 0.0f;
};

// -----------------------------------------------------------------------------
// Phaser -- porta de phaser2_mono/vibrato2_mono (phaflangers.lib do Faust),
// Copyright (C) 2003-2017 Julius O. Smith III, licenca STK-4.3 (estilo MIT,
// texto em THIRD_PARTY_NOTICES.md). Cada secao e um all-pass ressonante de
// 2a ordem realizado como SVF TPT (estavel com a frequencia variando), com
// polo em angulo th e raio R = exp(-pi*width/SR); out = x*(1-depth/2) +
// (depth/2)*cadeia(x + fb*y[n-1]). L usa seno e R cosseno do LFO
// (phase01 = 0 e 1 no phaser2_stereo).
// -----------------------------------------------------------------------------
class Phaser {
public:
    static constexpr int kMaxNotches = 8;

    void setSampleRate(double sr) { sr_ = sr; clear(); }
    void clear() {
        for (auto& c : ch_) c = Channel{};
        phase_ = 0.0f;
    }

    void process(const PhaserParams& p, float& l, float& r) {
        int notches = std::clamp(p.notches, 1, kMaxNotches);
        float sr = static_cast<float>(sr_);
        phase_ += std::clamp(p.rateHz, 0.0f, 20.0f) / sr;
        phase_ -= std::floor(phase_);
        float om = -std::expm1(-kPi * std::clamp(p.notchWidthHz, 1.0f, 10000.0f) / sr); // 1 - R
        float fq = om * (2.0f - om);                                                      // 1 - R^2
        float thMin = kTwoPi * std::clamp(p.freqMinHz, 10.0f, 0.45f * sr) / sr;
        float thMax = kTwoPi * std::clamp(p.freqMaxHz, 10.0f, 0.45f * sr) / sr;
        float ratio = std::clamp(p.freqRatio, 1.0f, 4.0f);
        float fb = std::clamp(p.feedback, -0.99f, 0.99f);
        float depth = std::clamp(p.depth, 0.0f, 2.0f);
        float g1 = 1.0f - depth * 0.5f;
        float g2 = (p.invert ? -1.0f : 1.0f) * depth * 0.5f;

        float s = fastSin01(phase_);
        float c = fastSin01(phase_ + 0.25f >= 1.0f ? phase_ - 0.75f : phase_ + 0.25f);
        l = l * g1 + g2 * ch_[0].chain(l, (1.0f - s) * 0.5f, notches, om, fq, thMin, thMax, ratio, fb);
        r = r * g1 + g2 * ch_[1].chain(r, (1.0f - c) * 0.5f, notches, om, fq, thMin, thMax, ratio, fb);
    }

private:
    struct Channel {
        float s1[kMaxNotches] = {}, s2[kMaxNotches] = {};
        float y = 0.0f; // saida anterior da cadeia (feedback)

        float chain(float x, float lfo01, int notches, float om, float fq, float thMin, float thMax, float ratio, float fb) {
            float v = x + fb * y;
            float th1 = thMin + (thMax - thMin) * lfo01;
            float th = th1;
            for (int i = 0; i < notches; ++i) {
                float t = std::min(th, 0.95f * kPi);
                // ap2p(th, x) do Faust: SVF TPT rodando em 2k*x; saida = x - bandpass
                float cth = std::cos(t);
                float P = om * om + 4.0f * (1.0f - om) * (1.0f - cth) * 0.5f; // A(1),  sin^2(th/2) = (1-cos)/2
                float S = om * om + 4.0f * (1.0f - om) * (1.0f + cth) * 0.5f; // A(-1), cos^2(th/2) = (1+cos)/2
                float iS = 1.0f / S;
                float g = std::sqrt(P * iS);
                float ig = 1.0f / g;
                float k2 = 4.0f * fq * ig * iS;
                float kpg = (2.0f * fq + P) * ig * iS;
                float d = 0.25f * S;
                float u = v * k2;
                float hp = (u - kpg * s1[i] - s2[i]) * d;
                float bp = g * hp + s1[i];
                float lp = g * bp + s2[i];
                s1[i] = bp + g * hp;
                s2[i] = lp + g * bp;
                v = v - bp;
                th *= ratio;
            }
            if (!std::isfinite(v)) { *this = Channel{}; v = 0.0f; }
            y = v;
            return v;
        }
    };

    double sr_ = 48000.0;
    float phase_ = 0.0f;
    Channel ch_[2];
};

// -----------------------------------------------------------------------------
// Flanger -- porta de flanger_mono/flanger_stereo (phaflangers.lib do Faust),
// Copyright (C) 2003-2017 Julius O. Smith III, licenca STK-4.3. Original:
// v[n] = x[n] + fb*v[n-D]; y = 0.5*(x + depth*v[n-D]) (sinal de depth
// trocado com invert). Mudanca: normaliza por 1/(1+depth) em vez de 0.5 --
// identico com depth=1, mas depth=0 vira identidade em vez de -6dB. O
// atraso D e varrido por LFO (L e R com 90 graus de diferenca).
// -----------------------------------------------------------------------------
class Flanger {
public:
    void setSampleRate(double sr) {
        sr_ = sr;
        size_t n = static_cast<size_t>(sr * 0.025) + 8; // ate 25ms
        lineL_.allocate(n);
        lineR_.allocate(n);
        phase_ = 0.0f;
    }
    void clear() { lineL_.clear(); lineR_.clear(); }

    void process(const FlangerParams& p, float& l, float& r) {
        float sr = static_cast<float>(sr_);
        phase_ += std::clamp(p.rateHz, 0.0f, 20.0f) / sr;
        phase_ -= std::floor(phase_);
        float minMs = std::clamp(p.delayMs, 0.05f, 10.0f);
        float sweepMs = std::clamp(p.sweepMs, 0.0f, 10.0f);
        auto delayAt = [&](float ph) {
            ph -= std::floor(ph);
            float lfo01 = 0.5f - 0.5f * fastSin01(ph + 0.25f >= 1.0f ? ph - 0.75f : ph + 0.25f); // (1-cos)/2
            return (minMs + sweepMs * lfo01) * 0.001f * sr;
        };
        float fb = std::clamp(p.feedback, -0.95f, 0.95f);
        float depth = std::clamp(p.depth, 0.0f, 1.0f) * (p.invert ? -1.0f : 1.0f);
        float norm = 1.0f / (1.0f + std::fabs(depth));
        l = flange(lineL_, l, delayAt(phase_), fb, depth, norm);
        r = flange(lineR_, r, delayAt(phase_ + 0.25f), fb, depth, norm);
    }

private:
    static float flange(gfx::DelayLine& line, float x, float delaySamples, float fb, float depth, float norm) {
        float delayed = line.read(delaySamples); // v[n-D]
        float v = x + fb * delayed;
        if (!std::isfinite(v)) v = 0.0f;
        line.write(v);
        return (x + depth * delayed) * norm;
    }

    double sr_ = 48000.0;
    float phase_ = 0.0f;
    gfx::DelayLine lineL_, lineR_;
};

// -----------------------------------------------------------------------------
// Delay estereo estilo fita -- implementacao propria (evolucao do delay
// anterior do dx7fm). Feedback com cross-feed (ping-pong) passando por
// passa-altas + passa-baixas de 1 polo e saturacao tanh normalizada (ganho 1
// pra sinal pequeno, entao nao instabiliza o loop); tempo suavizado (~60ms:
// mexer no tempo da um "glide" de fita em vez de clique) e modulado por wow
// (0.5Hz) + flutter (6Hz).
// -----------------------------------------------------------------------------
class TapeDelay {
public:
    void setSampleRate(double sr) {
        sr_ = sr;
        size_t n = static_cast<size_t>(sr * 2.1) + 8; // ate 2s + modulacao
        lineL_.allocate(n);
        lineR_.allocate(n);
        clear();
    }
    void clear() {
        lineL_.clear();
        lineR_.clear();
        lpL_ = lpR_ = hpL_ = hpR_ = 0.0f;
        smoothL_ = smoothR_ = -1.0f;
        wowPh_ = flutterPh_ = 0.0f;
    }

    void process(const DelayParams& p, float& l, float& r) {
        float sr = static_cast<float>(sr_);
        auto secs = [](float s) { return std::isfinite(s) ? std::clamp(s, 0.001f, 2.0f) : 0.3f; };
        float targetL = secs(p.timeLeftSec) * sr, targetR = secs(p.timeRightSec) * sr;
        if (smoothL_ < 0.0f) { smoothL_ = targetL; smoothR_ = targetR; }
        float sc = 1.0f - std::exp(-1.0f / (0.06f * sr));
        smoothL_ += (targetL - smoothL_) * sc;
        smoothR_ += (targetR - smoothR_) * sc;

        wowPh_ += 0.5f / sr;   wowPh_ -= std::floor(wowPh_);
        flutterPh_ += 6.0f / sr; flutterPh_ -= std::floor(flutterPh_);
        float wow = std::clamp(p.wow, 0.0f, 1.0f);
        float modSamples = wow * (0.0025f * sr * fastSin01(wowPh_) + 0.0002f * sr * fastSin01(flutterPh_));

        float tapL = lineL_.read(smoothL_ + modSamples);
        float tapR = lineR_.read(smoothR_ + modSamples);

        float fb = std::clamp(p.feedback, 0.0f, 0.95f);
        float cross = std::clamp(p.crossFeed, 0.0f, 1.0f);
        float fbL = tapL * (1.0f - cross) + tapR * cross;
        float fbR = tapR * (1.0f - cross) + tapL * cross;
        float aLp = gfx::onePoleCoef(std::clamp(p.highCutHz, 200.0f, 20000.0f), sr_);
        float aHp = gfx::onePoleCoef(std::clamp(p.lowCutHz, 10.0f, 2000.0f), sr_);
        lpL_ += (fbL - lpL_) * aLp; fbL = lpL_;
        lpR_ += (fbR - lpR_) * aLp; fbR = lpR_;
        hpL_ += (fbL - hpL_) * aHp; fbL -= hpL_;
        hpR_ += (fbR - hpR_) * aHp; fbR -= hpR_;
        float drive = std::clamp(p.drive, 0.0f, 1.0f);
        if (drive > 0.0f) {
            float k = 1.0f + 4.0f * drive;
            fbL = std::tanh(fbL * k) / k;
            fbR = std::tanh(fbR * k) / k;
        }
        float wL = l + fbL * fb, wR = r + fbR * fb;
        lineL_.write(std::isfinite(wL) ? wL : 0.0f);
        lineR_.write(std::isfinite(wR) ? wR : 0.0f);

        float mix = std::clamp(p.mix, 0.0f, 1.0f);
        l = l * (1.0f - mix) + tapL * mix;
        r = r * (1.0f - mix) + tapR * mix;
    }

private:
    double sr_ = 48000.0;
    gfx::DelayLine lineL_, lineR_;
    float lpL_ = 0, lpR_ = 0, hpL_ = 0, hpR_ = 0;
    float smoothL_ = -1.0f, smoothR_ = -1.0f;
    float wowPh_ = 0.0f, flutterPh_ = 0.0f;
};

// -----------------------------------------------------------------------------
// Reverb "Room" -- Freeverb, de Jezar at Dreampoint (2000), DOMINIO PUBLICO.
// Porta fiel de comb.hpp/allpass.hpp/revmodel.cpp/tuning.h: 8 combs com
// lowpass no loop + 4 all-pass em serie por canal, afinacoes do tuning.h
// (feitas pra 44.1kHz -- aqui escaladas pela taxa), stereospread 23,
// roomsize*0.28+0.7, damp*0.4, ganho de entrada 0.015, all-pass 0.5, modo
// freeze. Produz so o sinal molhado (o dry/wet e do bloco Reverb).
// -----------------------------------------------------------------------------
class FreeverbRoom {
public:
    void setSampleRate(double sr) {
        static const int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int kAllpass[4] = {556, 441, 341, 225};
        constexpr int kStereoSpread = 23;
        double k = sr / 44100.0;
        for (int i = 0; i < 8; ++i) {
            combL_[i].resize(static_cast<size_t>(kComb[i] * k));
            combR_[i].resize(static_cast<size_t>((kComb[i] + kStereoSpread) * k));
        }
        for (int i = 0; i < 4; ++i) {
            apL_[i].resize(static_cast<size_t>(kAllpass[i] * k));
            apR_[i].resize(static_cast<size_t>((kAllpass[i] + kStereoSpread) * k));
        }
    }
    void clear() {
        for (auto& c : combL_) c.clear();
        for (auto& c : combR_) c.clear();
        for (auto& a : apL_) a.clear();
        for (auto& a : apR_) a.clear();
    }

    void process(float inL, float inR, float roomSize, float damping, bool freeze, float& wetL, float& wetR) {
        constexpr float kFixedGain = 0.015f, kScaleRoom = 0.28f, kOffsetRoom = 0.7f, kScaleDamp = 0.4f;
        float feedback = freeze ? 1.0f : std::clamp(roomSize, 0.0f, 1.0f) * kScaleRoom + kOffsetRoom;
        float damp = freeze ? 0.0f : std::clamp(damping, 0.0f, 1.0f) * kScaleDamp;
        float input = freeze ? 0.0f : (inL + inR) * kFixedGain;
        float outL = 0.0f, outR = 0.0f;
        for (int i = 0; i < 8; ++i) {
            outL += combL_[i].process(input, feedback, damp);
            outR += combR_[i].process(input, feedback, damp);
        }
        for (int i = 0; i < 4; ++i) {
            outL = apL_[i].process(outL);
            outR = apR_[i].process(outR);
        }
        wetL = outL;
        wetR = outR;
    }

private:
    static float undenormalise(float x) { return std::fabs(x) < 1e-20f ? 0.0f : x; }
    struct Comb {
        std::vector<float> buf;
        size_t idx = 0;
        float filterstore = 0.0f;
        void resize(size_t n) { buf.assign(std::max<size_t>(n, 1), 0.0f); idx = 0; filterstore = 0.0f; }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0f); filterstore = 0.0f; }
        float process(float input, float feedback, float damp1) {
            float output = undenormalise(buf[idx]);
            filterstore = undenormalise(output * (1.0f - damp1) + filterstore * damp1);
            buf[idx] = input + filterstore * feedback;
            if (++idx >= buf.size()) idx = 0;
            return output;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        size_t idx = 0;
        void resize(size_t n) { buf.assign(std::max<size_t>(n, 1), 0.0f); idx = 0; }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
        float process(float input) {
            constexpr float kFeedback = 0.5f;
            float bufout = undenormalise(buf[idx]);
            float output = -input + bufout;
            buf[idx] = input + bufout * kFeedback;
            if (++idx >= buf.size()) idx = 0;
            return output;
        }
    };
    Comb combL_[8], combR_[8];
    Allpass apL_[4], apR_[4];
};

// -----------------------------------------------------------------------------
// Reverb "Hall" -- porta do Reverb do Mutable Instruments Clouds
// (clouds/dsp/fx/reverb.h + fx_engine.h). Copyright 2014 Emilie Gillet.
// Licenca MIT (mesmo texto do bloco do chorus acima). Topologia de
// Griesinger descrita no paper do Dattorro: 4 all-pass difusores na entrada
// e um loop de 2x (2 all-pass + 1 delay), com modulacao no 1o difusor
// (smear) e num delay longo (shimmer). Mudancas na porta: float em vez de
// 12 bits; FxEngine de tamanhos fixos em template virou tamanhos em tempo de
// execucao escalados pela taxa (o original roda a 32kHz); LFOs senoidais
// unipolares (0..1, como o CosineOscillator do stmlib) atualizados a cada
// amostra em vez de a cada 32. Parametros como o Clouds configura no
// granular_processor: difusao 0.7, ganho de entrada 0.2, tempo
// 0.35+0.63*size, lowpass 0.6+0.37*(1-damping).
// -----------------------------------------------------------------------------
class CloudsHall {
public:
    void setSampleRate(double sr) {
        sr_ = sr;
        scale_ = static_cast<float>(sr / 32000.0);
        static const int kLen32k[kLines] = {113, 162, 241, 399, 1653, 2038, 3411, 1913, 1663, 4782};
        int base = 0;
        for (int i = 0; i < kLines; ++i) {
            line_[i].length = std::max(2, static_cast<int>(static_cast<float>(kLen32k[i]) * scale_));
            line_[i].base = base;
            base += line_[i].length + 1;
        }
        int size = 1;
        while (size < base + 8) size <<= 1;
        buffer_.assign(static_cast<size_t>(size), 0.0f);
        mask_ = size - 1;
        writePtr_ = 0;
        lfo1Ph_ = lfo2Ph_ = 0.0f;
        lp1_ = lp2_ = 0.0f;
    }
    void clear() { std::fill(buffer_.begin(), buffer_.end(), 0.0f); lp1_ = lp2_ = 0.0f; }

    void process(float inL, float inR, float roomSize, float damping, float& wetL, float& wetR) {
        enum { AP1, AP2, AP3, AP4, DAP1A, DAP1B, DEL1, DAP2A, DAP2B, DEL2 };
        const float kap = 0.7f;
        const float klp = 0.6f + 0.37f * (1.0f - std::clamp(damping, 0.0f, 1.0f));
        const float krt = 0.35f + 0.63f * std::clamp(roomSize, 0.0f, 1.0f);
        const float gain = 0.2f;

        // engine_.Start(): write_ptr anda pra tras; LFOs (0.5Hz e 0.3Hz)
        if (--writePtr_ < 0) writePtr_ += mask_ + 1;
        lfo1Ph_ += 0.5f / static_cast<float>(sr_); lfo1Ph_ -= std::floor(lfo1Ph_);
        lfo2Ph_ += 0.3f / static_cast<float>(sr_); lfo2Ph_ -= std::floor(lfo2Ph_);
        float lfo1 = 0.5f + 0.5f * fastSin01(lfo1Ph_), lfo2 = 0.5f + 0.5f * fastSin01(lfo2Ph_);

        float acc = 0.0f, prev = 0.0f, apout, wet;
        auto tail = [&](int d) { return at(line_[d].base + line_[d].length - 1); };
        auto read = [&](int d, float scale) { prev = tail(d); acc += prev * scale; };
        auto write = [&](int d, int offset, float scale) { at(line_[d].base + offset) = acc; acc *= scale; };
        auto writeAllPass = [&](int d, float scale) { write(d, 0, scale); acc += prev; };
        auto interpolate = [&](int d, float offset, float lfo, float amplitude, float scale) {
            offset += amplitude * lfo;
            int oi = static_cast<int>(offset);
            float of = offset - static_cast<float>(oi);
            float a = at(oi + line_[d].base), b = at(oi + line_[d].base + 1);
            prev = a + (b - a) * of;
            acc += prev * scale;
        };

        // smear do AP1 dentro do loop
        interpolate(AP1, 10.0f * scale_, lfo1, 60.0f * scale_, 1.0f);
        write(AP1, static_cast<int>(100.0f * scale_), 0.0f);

        acc += (inL + inR) * gain;
        // difusao por 4 all-pass
        read(AP1, kap); writeAllPass(AP1, -kap);
        read(AP2, kap); writeAllPass(AP2, -kap);
        read(AP3, kap); writeAllPass(AP3, -kap);
        read(AP4, kap); writeAllPass(AP4, -kap);
        apout = acc;

        // loop principal, metade esquerda
        acc = apout;
        interpolate(DEL2, 4680.0f * scale_, lfo2, 100.0f * scale_, krt);
        lp1_ += klp * (acc - lp1_); acc = lp1_;
        read(DAP1A, -kap); writeAllPass(DAP1A, kap);
        read(DAP1B, kap); writeAllPass(DAP1B, -kap);
        write(DEL1, 0, 2.0f);
        wet = acc; acc = 0.0f;
        wetL = wet;

        // metade direita
        acc = apout;
        read(DEL1, krt);
        lp2_ += klp * (acc - lp2_); acc = lp2_;
        read(DAP2A, kap); writeAllPass(DAP2A, -kap);
        read(DAP2B, -kap); writeAllPass(DAP2B, kap);
        write(DEL2, 0, 2.0f);
        wet = acc;
        wetR = wet;
        if (!std::isfinite(wetL) || !std::isfinite(wetR)) { clear(); wetL = wetR = 0.0f; }
    }

private:
    static constexpr int kLines = 10;
    struct Line { int base = 0, length = 0; };
    float& at(int offset) { return buffer_[static_cast<size_t>((writePtr_ + offset) & mask_)]; }

    double sr_ = 48000.0;
    float scale_ = 1.5f;
    Line line_[kLines];
    std::vector<float> buffer_;
    int mask_ = 0;
    int writePtr_ = 0;
    float lfo1Ph_ = 0.0f, lfo2Ph_ = 0.0f;
    float lp1_ = 0.0f, lp2_ = 0.0f;
};

// -----------------------------------------------------------------------------
// Reverb "Plate" -- MVerb (Martin Eastwood, 2010), GPLv3, usado SEM
// modificacoes (third_party/mverb/MVerb.h). Plate de Dattorro com early
// reflections. Fica NO HEAP: o objeto tem ~5.8MB de buffers inline (15
// linhas de 96000 amostras), que estourariam a pilha onde o Synth costuma
// morar. Mix=1 (so molhado; o dry/wet e do bloco Reverb), Size fixo (mudar
// o Size do MVerb limpa todos os buffers, o que daria um buraco no som a
// cada movimento do slider) -- o tamanho percebido vem do Decay.
// -----------------------------------------------------------------------------
class MVerbPlate {
public:
    MVerbPlate() : verb_(std::make_unique<MVerb<float>>()) {}

    void setSampleRate(double sr) {
        verb_->setSampleRate(static_cast<float>(sr));
        verb_->setParameter(MVerb<float>::SIZE, 0.8f);
        verb_->setParameter(MVerb<float>::MIX, 1.0f);
        verb_->setParameter(MVerb<float>::GAIN, 1.0f);
        verb_->setParameter(MVerb<float>::DENSITY, 0.5f);
        verb_->setParameter(MVerb<float>::BANDWIDTHFREQ, 0.9f);
        verb_->setParameter(MVerb<float>::PREDELAY, 0.0f); // o predelay e do bloco Reverb
        verb_->setParameter(MVerb<float>::EARLYMIX, 0.75f);
        verb_->reset();
    }
    void clear() { verb_->reset(); }

    void process(float inL, float inR, float roomSize, float damping, float& wetL, float& wetR) {
        verb_->setParameter(MVerb<float>::DECAY, 0.2f + 0.78f * std::clamp(roomSize, 0.0f, 1.0f));
        verb_->setParameter(MVerb<float>::DAMPINGFREQ, 0.1f + 0.8f * std::clamp(damping, 0.0f, 1.0f));
        float in[2] = {inL, inR}, out[2] = {0.0f, 0.0f};
        float* ins[2] = {&in[0], &in[1]};
        float* outs[2] = {&out[0], &out[1]};
        verb_->process(ins, outs, 1);
        wetL = out[0];
        wetR = out[1];
        if (!std::isfinite(wetL) || !std::isfinite(wetR)) { verb_->reset(); wetL = wetR = 0.0f; }
    }

private:
    std::unique_ptr<MVerb<float>> verb_;
};

// Bloco Reverb: predelay comum + o algoritmo escolhido + largura + dry/wet.
class ReverbBlock {
public:
    void setSampleRate(double sr) {
        sr_ = sr;
        predelayL_.allocate(static_cast<size_t>(sr * 0.26) + 8);
        predelayR_.allocate(static_cast<size_t>(sr * 0.26) + 8);
        room_.setSampleRate(sr);
        hall_.setSampleRate(sr);
        plate_.setSampleRate(sr);
    }
    void clear() {
        predelayL_.clear(); predelayR_.clear();
        room_.clear(); hall_.clear(); plate_.clear();
    }

    void process(const ReverbParams& p, float& l, float& r) {
        ReverbType type = static_cast<int>(p.type) <= static_cast<int>(ReverbType::Plate) ? p.type : ReverbType::Room;
        if (type != lastType_) { // algoritmo trocado: nao tocar uma cauda velha congelada do outro
            clear();
            lastType_ = type;
        }
        float pd = std::clamp(p.predelayMs, 0.0f, 250.0f) * 0.001f * static_cast<float>(sr_);
        predelayL_.write(l);
        predelayR_.write(r);
        float inL = pd >= 1.0f ? predelayL_.read(pd + 1.0f) : l; // +1: escreveu antes de ler
        float inR = pd >= 1.0f ? predelayR_.read(pd + 1.0f) : r;

        float wetL = 0.0f, wetR = 0.0f;
        switch (type) {
            case ReverbType::Room:  room_.process(inL, inR, p.roomSize, p.damping, p.freeze, wetL, wetR); break;
            case ReverbType::Hall:  hall_.process(inL, inR, p.roomSize, p.damping, wetL, wetR); break;
            case ReverbType::Plate: plate_.process(inL, inR, p.roomSize, p.damping, wetL, wetR); break;
        }
        // largura (o wet1/wet2 do Freeverb, aplicado a todos os modos)
        float width = std::clamp(p.width, 0.0f, 1.0f);
        float w1 = width * 0.5f + 0.5f, w2 = (1.0f - width) * 0.5f;
        float wideL = wetL * w1 + wetR * w2;
        float wideR = wetR * w1 + wetL * w2;
        float mix = std::clamp(p.mix, 0.0f, 1.0f);
        l = l * (1.0f - mix) + wideL * mix;
        r = r * (1.0f - mix) + wideR * mix;
    }

private:
    double sr_ = 48000.0;
    ReverbType lastType_ = ReverbType::Room;
    gfx::DelayLine predelayL_, predelayR_;
    FreeverbRoom room_;
    CloudsHall hall_;
    MVerbPlate plate_;
};

// -----------------------------------------------------------------------------
// EQ de 3 bandas -- biquads com as formulas do "Audio EQ Cookbook" de Robert
// Bristow-Johnson (low shelf e high shelf com S=1, peaking com Q). Forma
// direta I. Coeficientes so recalculados quando um parametro muda.
// -----------------------------------------------------------------------------
class Eq3 {
public:
    void setSampleRate(double sr) { sr_ = sr; dirty_ = true; clear(); }
    void clear() { for (auto& b : bands_) for (auto& s : b.state) s = {}; }

    void process(const EqParams& p, float& l, float& r) {
        if (dirty_ || changed(p, last_)) {
            design(p);
            last_ = p;
            dirty_ = false;
        }
        for (auto& b : bands_) {
            l = b.tick(0, l);
            r = b.tick(1, r);
        }
    }

private:
    static bool changed(const EqParams& a, const EqParams& b) { // campo a campo (memcmp pegaria o padding)
        return a.lowFreqHz != b.lowFreqHz || a.lowGainDb != b.lowGainDb || a.midFreqHz != b.midFreqHz ||
               a.midGainDb != b.midGainDb || a.midQ != b.midQ || a.highFreqHz != b.highFreqHz ||
               a.highGainDb != b.highGainDb;
    }
    enum class Shape { LowShelf, Peak, HighShelf };
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        struct St { float x1 = 0, x2 = 0, y1 = 0, y2 = 0; } state[2];
        float tick(int ch, float x) {
            St& s = state[ch];
            float y = b0 * x + b1 * s.x1 + b2 * s.x2 - a1 * s.y1 - a2 * s.y2;
            if (std::fabs(y) < 1e-20f) y = 0.0f;
            s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y;
            return y;
        }
    };

    void design(const EqParams& p) {
        set(bands_[0], Shape::LowShelf, p.lowFreqHz, p.lowGainDb, 0.707f);
        set(bands_[1], Shape::Peak, p.midFreqHz, p.midGainDb, p.midQ);
        set(bands_[2], Shape::HighShelf, p.highFreqHz, p.highGainDb, 0.707f);
    }

    void set(Biquad& bq, Shape shape, float freq, float gainDb, float q) {
        double f0 = std::clamp(static_cast<double>(freq), 20.0, sr_ * 0.45);
        double A = std::pow(10.0, std::clamp(static_cast<double>(gainDb), -24.0, 24.0) / 40.0);
        double w0 = 2.0 * 3.14159265358979323846 * f0 / sr_;
        double cw = std::cos(w0), sw = std::sin(w0);
        double b0, b1, b2, a0, a1, a2;
        if (shape == Shape::Peak) {
            double alpha = sw / (2.0 * std::clamp(static_cast<double>(q), 0.1, 10.0));
            b0 = 1.0 + alpha * A; b1 = -2.0 * cw; b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A; a1 = -2.0 * cw; a2 = 1.0 - alpha / A;
        } else {
            double alpha = sw / 2.0 * std::sqrt(2.0); // S = 1
            double sA = 2.0 * std::sqrt(A) * alpha;
            if (shape == Shape::LowShelf) {
                b0 = A * ((A + 1) - (A - 1) * cw + sA);
                b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - sA);
                a0 = (A + 1) + (A - 1) * cw + sA;
                a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - sA;
            } else {
                b0 = A * ((A + 1) + (A - 1) * cw + sA);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - sA);
                a0 = (A + 1) - (A - 1) * cw + sA;
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - sA;
            }
        }
        bq.b0 = static_cast<float>(b0 / a0); bq.b1 = static_cast<float>(b1 / a0); bq.b2 = static_cast<float>(b2 / a0);
        bq.a1 = static_cast<float>(a1 / a0); bq.a2 = static_cast<float>(a2 / a0);
    }

    double sr_ = 48000.0;
    Biquad bands_[3];
    EqParams last_{};
    bool dirty_ = true;
};

// -----------------------------------------------------------------------------
// Compressor -- porta do ZamComp (zam-plugins, plugins/ZamComp), Copyright
// (C) 2014 Damien Zammit, GNU GPL v2 ou posterior. Feed-forward no dominio
// de dB: curva estatica com knee, detector de pico com attack/release
// exponenciais (coef = exp(-1000/(ms*SR))), makeup. Mudancas na porta:
//  - estereo com detector ligado (maior |L|,|R|) aplicando o MESMO ganho
//    nos dois canais (o ZamComp e mono);
//  - knee suave de verdade: no original a formula quadratica do knee e
//    calculada e logo sobrescrita pela reta de razao fixa (o "Knee" nao
//    suavizava nada); aqui, dentro da regiao do knee vale a quadratica;
//  - sem o parametro "slew" de ataque e sem sidechain externo.
// -----------------------------------------------------------------------------
class ZamCompressor {
public:
    void setSampleRate(double sr) { sr_ = sr; clear(); }
    void clear() { oldYl_ = 0.0f; }

    void process(const CompressorParams& p, float& l, float& r) {
        float sr = static_cast<float>(sr_);
        float thres = std::clamp(p.thresholdDb, -60.0f, 0.0f);
        float ratio = std::clamp(p.ratio, 1.0f, 20.0f);
        float width = 6.0f * std::clamp(p.kneeDb, 0.0f, 8.0f) + 0.01f;
        float attackCoeff = std::exp(-1000.0f / (std::clamp(p.attackMs, 0.1f, 100.0f) * sr));
        float releaseCoeff = std::exp(-1000.0f / (std::clamp(p.releaseMs, 1.0f, 500.0f) * sr));
        float makeup = std::clamp(p.makeupDb, 0.0f, 30.0f);

        float in = std::max(std::fabs(l), std::fabs(r));
        float xg = in == 0.0f ? -160.0f : 20.0f * std::log10(in);
        float over = xg - thres;
        float yg;
        if (2.0f * over < -width) yg = xg;
        else if (2.0f * std::fabs(over) <= width) yg = xg + (1.0f / ratio - 1.0f) * (over + width / 2.0f) * (over + width / 2.0f) / (2.0f * width);
        else yg = thres + over / ratio;

        float xl = xg - yg;
        float yl;
        if (xl < oldYl_) yl = releaseCoeff * oldYl_ + (1.0f - releaseCoeff) * xl;
        else if (xl > oldYl_) yl = attackCoeff * oldYl_ + (1.0f - attackCoeff) * xl;
        else yl = xl;
        if (!std::isfinite(yl)) yl = 0.0f;
        oldYl_ = yl;
        gainReductionDb_.store(yl, std::memory_order_relaxed);

        float gain = std::pow(10.0f, (makeup - yl) / 20.0f);
        l *= gain;
        r *= gain;
    }

    // reducao de ganho atual em dB (>= 0), pra medidor na UI (outra thread)
    float gainReductionDb() const { return gainReductionDb_.load(std::memory_order_relaxed); }

private:
    double sr_ = 48000.0;
    float oldYl_ = 0.0f;
    std::atomic<float> gainReductionDb_{0.0f};
};

} // namespace dx7fm
