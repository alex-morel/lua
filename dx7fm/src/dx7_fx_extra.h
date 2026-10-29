#pragma once
// =============================================================================
// [ADICIONADO em 2026-10-29] DSP dos efeitos do rack que nao sao os 7 antigos
// (dx7_globalfx.h) nem portas geradas (src/airwindows, src/chowtape). Cada
// classe diz de onde veio; todas processam um bloco estereo no lugar.
// =============================================================================
#include "dx7_dsp.h"
#include "dx7_globalfx.h"
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace dx7fm::fxextra {

constexpr double kPiD = 3.14159265358979323846;

// -----------------------------------------------------------------------------
// Wah -- porta do crybaby e do autowah da vaeffects.lib do Faust (secao de
// Julius O. Smith III, licenca STK-4.3, texto em THIRD_PARTY_NOTICES.md).
// crybaby(wah): passa-banda ressonante com Q = 2^(2(1-w)+1), frequencia
// 450*2^(2.3w) e ganho 0.1*4^w, biquad (1-z^-1)/(1+a1 z^-1+a2 z^-2) escrito
// sobre f2 = 1+a1+a2 e fq = 1-a2 (sem cancelamento em precisao simples) e
// suavizado com polo 0.999 -- como no Faust. autowah: a posicao vem de um
// seguidor de amplitude (an.amp_follower(0.1)). Modo LFO: implementacao propria.
// -----------------------------------------------------------------------------
class Wah {
public:
    enum Mode { Manual, Auto, Lfo };
    void prepare(double sr) { sr_ = sr; reset(); }
    void reset() {
        for (auto& c : ch_) c = Ch{};
        f2s_ = fqs_ = 1.0;  // smooth(1) do Faust parte de 1 (o termo "e")
        gs_ = 0.0;
        env_ = 0.0f;
        phase_ = 0.0;
    }
    // pos: 0..1 (manual); sens: ganho do seguidor (auto); rateHz: LFO; depth: faixa varrida
    void process(Mode mode, float pos, float sens, float rateHz, float depth, float* L, float* R, int n) {
        const float rel = std::exp(-1.0f / (0.1f * static_cast<float>(sr_)));
        for (int i = 0; i < n; ++i) {
            float w = pos;
            if (mode == Auto) {
                const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
                env_ = a > env_ ? a : env_ * rel; // amp_follower: sobe na hora, cai com tau 0.1 s
                w = pos + depth * std::min(1.0f, env_ * sens);
            } else if (mode == Lfo) {
                phase_ += rateHz / sr_;
                phase_ -= std::floor(phase_);
                w = pos + depth * 0.5f * (1.0f - static_cast<float>(std::cos(2.0 * kPiD * phase_)));
            }
            const double wc = std::clamp(static_cast<double>(w), 0.0, 1.0);
            const double Q = std::pow(2.0, 2.0 * (1.0 - wc) + 1.0);
            const double fr = 450.0 * std::pow(2.0, 2.3 * wc);
            const double g = 0.1 * std::pow(4.0, wc);
            const double frn = fr / sr_;
            const double d = kPiD * frn / Q;
            const double sn = std::sin(kPiD * frn);
            const double f2 = d * d + 4.0 * (1.0 - d) * sn * sn;
            const double fq = d * (2.0 - d);
            constexpr double s = 0.999;
            f2s_ = s * f2s_ + (1.0 - s) * f2;
            fqs_ = s * fqs_ + (1.0 - s) * fq;
            gs_ = s * gs_ + (1.0 - s) * g;
            L[i] = ch_[0].tick(L[i], gs_, f2s_, fqs_);
            R[i] = ch_[1].tick(R[i], gs_, f2s_, fqs_);
        }
    }

private:
    struct Ch {
        double w = 0.0, v = 0.0;
        float tick(float x, double gs, double f2s, double fqs) {
            const double vn = v + (gs * x - f2s * w - fqs * v);
            const double wn = w + vn;
            w = wn;
            v = vn;
            if (!std::isfinite(v)) { w = v = 0.0; return 0.0f; }
            return static_cast<float>(vn);
        }
    };
    double sr_ = 48000.0, f2s_ = 1.0, fqs_ = 1.0, gs_ = 0.0, phase_ = 0.0;
    float env_ = 0.0f;
    Ch ch_[2];
};

// -----------------------------------------------------------------------------
// Filtro com LFO -- implementacao propria sobre o SVF TPT do motor
// (TPTStateVariableFilter, dx7_dsp.h). LFO livre em Hz ou sincronizado no BPM.
// -----------------------------------------------------------------------------
class LfoFilter {
public:
    void prepare(double sr) { sr_ = sr; for (auto& f : f_) f.setSampleRate(sr); reset(); }
    void reset() { for (auto& f : f_) f.reset(); phase_ = 0.0; sh_ = 0.0f; }
    // type 0 LP, 1 BP, 2 HP; shape 0 seno, 1 triangulo, 2 quadrada, 3 S&H; octaves = profundidade
    void process(int type, float cutoff, float q, int shape, float rateHz, float octaves, float* L, float* R, int n, double beatPos,
                 double cycleBeats, float bpm) {
        for (int i = 0; i < n; ++i) {
            double ph;
            if (cycleBeats > 0.0) { // sincronizado: fase pela posicao de batida
                const double b = (beatPos + i * bpm / (60.0 * sr_)) / cycleBeats;
                ph = b - std::floor(b);
            } else {
                phase_ += rateHz / sr_;
                phase_ -= std::floor(phase_);
                ph = phase_;
            }
            if (ph < lastPh_) sh_ = std::uniform_real_distribution<float>(-1.0f, 1.0f)(rng_);
            lastPh_ = ph;
            float lfo;
            switch (shape) {
                case 1: lfo = static_cast<float>(ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph); break;
                case 2: lfo = ph < 0.5 ? 1.0f : -1.0f; break;
                case 3: lfo = sh_; break;
                default: lfo = static_cast<float>(std::sin(2.0 * kPiD * ph)); break;
            }
            const float fc = std::clamp(cutoff * std::exp2(octaves * 0.5f * (lfo + 1.0f)), 20.0f, 0.45f * static_cast<float>(sr_));
            for (int c = 0; c < 2; ++c) f_[c].setParams(fc, q);
            auto ol = f_[0].process(L[i]), orr = f_[1].process(R[i]);
            L[i] = type == 0 ? ol.lp : type == 1 ? ol.bp : ol.hp;
            R[i] = type == 0 ? orr.lp : type == 1 ? orr.bp : orr.hp;
        }
    }

private:
    double sr_ = 48000.0, phase_ = 0.0, lastPh_ = 0.0;
    float sh_ = 0.0f;
    std::minstd_rand rng_{newRandomSeed()};
    TPTStateVariableFilter f_[2];
};

// -----------------------------------------------------------------------------
// Pitch shifter -- porta do PitchShifter do Mutable Instruments Clouds
// (clouds/dsp/fx/pitch_shifter.h), Copyright 2014 Emilie Gillet, licenca MIT.
// Duas leituras de uma linha de atraso que andam na velocidade (1 - razao),
// defasadas meia janela e cruzadas com uma janela triangular. Mudancas: float
// em vez de 16 bits; tamanho da janela escalado pela taxa (o Clouds roda a
// 32 kHz) e em ms; a janela suaviza com o mesmo one-pole 0.05 do original.
// -----------------------------------------------------------------------------
class PitchShifter {
public:
    void prepare(double sr) {
        sr_ = sr;
        scale_ = static_cast<float>(sr / 32000.0);
        maxSize_ = 2047.0f * scale_;
        for (auto& l : line_) l.allocate(static_cast<size_t>(maxSize_) + 8);
        reset();
    }
    void reset() { for (auto& l : line_) l.clear(); phase_ = 0.0f; size_ = maxSize_; }
    void setRatio(float r) { ratio_ = r; }
    void setSize(float size01) { // set_size do Clouds: 128..2047 amostras (a 32 kHz), curva cubica
        const float target = (128.0f + (2047.0f - 128.0f) * size01 * size01 * size01) * scale_;
        size_ += 0.05f * (target - size_); // ONE_POLE(size_, target, 0.05)
    }
    void processSample(float& l, float& r) {
        phase_ += (1.0f - ratio_) / size_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        if (phase_ <= 0.0f) phase_ += 1.0f;
        const float tri = 2.0f * (phase_ >= 0.5f ? 1.0f - phase_ : phase_);
        const float phase = phase_ * size_;
        float half = phase + size_ * 0.5f;
        if (half >= size_) half -= size_;
        float* io[2] = {&l, &r};
        for (int c = 0; c < 2; ++c) {
            line_[c].write(*io[c]);
            // read(1) = a amostra que acabou de entrar (offset 0 no FxEngine)
            *io[c] = line_[c].read(phase + 1.0f) * tri + line_[c].read(half + 1.0f) * (1.0f - tri);
        }
    }

private:
    double sr_ = 48000.0;
    float scale_ = 1.5f, maxSize_ = 3070.0f, phase_ = 0.0f, size_ = 3070.0f, ratio_ = 1.0f;
    gfx::DelayLine line_[2];
};

// -----------------------------------------------------------------------------
// Rotary (caixa Leslie) -- baseado no RotarySpeaker do sst-effects/Surge XT
// (github.com/surge-synthesizer/sst-effects, GPLv3): crossover de 800 Hz;
// corneta (agudos) por uma linha de atraso modulada (doppler, distancia de
// cada ouvido ate a corneta girando) e amplitude pelo angulo (tremolo); rotor
// (graves) com amplitude modulada abaixo de 200 Hz intactos, na mesma
// formula. Mudancas: interpolacao Hermite em vez da tabela de sinc; drive
// com tanh em vez dos waveshapers SIMD; chave Lento/Rapido com INERCIA (a
// corneta acelera em ~0.7 s, o rotor em ~3 s, como uma Leslie de verdade --
// implementacao propria, o Surge so tem a taxa direta).
// -----------------------------------------------------------------------------
class Rotary {
public:
    void prepare(double sr) {
        sr_ = sr;
        buf_.assign(1 << 13, 0.0f);
        lpX_.lowpass(800.0, sr); // crossover: graves = LP 800 Hz, agudos = entrada - graves
        lpBass_.lowpass(200.0, sr);
        reset();
    }
    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        pos_ = 0;
        lpX_.reset(); lpBass_.reset();
        hornPh_ = rotorPh_ = 0.0;
        hornHz_ = rotorHz_ = -1.0;
    }
    void process(bool fast, float slowHz, float fastHz, float doppler, float tremolo, float drive, float width, float* L, float* R, int n) {
        const double hornTarget = fast ? fastHz : slowHz, rotorTarget = hornTarget * 0.85;
        if (hornHz_ < 0.0) { hornHz_ = hornTarget; rotorHz_ = rotorTarget; }
        const double aH = 1.0 - std::exp(-1.0 / (0.7 * sr_)), aR = 1.0 - std::exp(-1.0 / (3.0 * sr_));
        const float dly = static_cast<float>(sr_) * 0.0018f * doppler;
        const float a = tremolo * 0.6f;
        const float dk = 1.0f + drive * drive * 15.0f;
        for (int i = 0; i < n; ++i) {
            hornHz_ += (hornTarget - hornHz_) * aH;
            rotorHz_ += (rotorTarget - rotorHz_) * aR;
            hornPh_ += hornHz_ / sr_; hornPh_ -= std::floor(hornPh_);
            rotorPh_ += rotorHz_ / sr_; rotorPh_ -= std::floor(rotorPh_);
            const float lr = static_cast<float>(std::cos(2.0 * kPiD * hornPh_)), li = static_cast<float>(std::sin(2.0 * kPiD * hornPh_));
            // geometria do sst: ouvidos em (-1,-2) e (+1,-2), corneta no circulo unitario
            const float p0 = -2.0f - li, p1 = -1.0f - lr, p2 = 1.0f - lr;
            const float lenL = std::sqrt(p0 * p0 + p1 * p1), lenR = std::sqrt(p0 * p0 + p2 * p2);
            const float dotL = (p1 * lr + p0 * li) / lenL, dotR = (p2 * lr + p0 * li) / lenR;
            float in = 0.5f * (L[i] + R[i]);
            if (drive > 0.0f) in = std::tanh(in * dk) / std::sqrt(dk); // compensacao aproximada
            const float low = lpX_.process(in), upper = in - low;
            buf_[static_cast<size_t>(pos_)] = upper;
            const float tL = hermite(std::max(2.0f, dly * lenL)), tR = hermite(std::max(2.0f, dly * lenR));
            const float sub = lpBass_.process(low);
            const float rotor = static_cast<float>(std::cos(2.0 * kPiD * rotorPh_));
            const float bass = sub + (low - sub) * (rotor * 0.6f + 0.3f);
            float wl = ((1.0f - a) + a * dotL) * tL + bass, wr = ((1.0f - a) + a * dotR) * tR + bass;
            // largura (mid/side)
            const float m = 0.5f * (wl + wr), s = 0.5f * (wl - wr) * width;
            L[i] = m + s;
            R[i] = m - s;
            pos_ = (pos_ + 1) & static_cast<int>(buf_.size() - 1);
        }
    }

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void reset() { z1 = z2 = 0; }
        void lowpass(double fc, double fs) {
            const double w = 2 * kPiD * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * 0.707), a0 = 1 + al;
            b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
        }
        void highpass(double fc, double fs) {
            const double w = 2 * kPiD * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * 0.707), a0 = 1 + al;
            b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
        }
        float process(float xf) {
            const double x = xf, y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return static_cast<float>(y);
        }
    };
    float hermite(float d) const {
        const int mask = static_cast<int>(buf_.size() - 1), di = static_cast<int>(d);
        const float f = d - static_cast<float>(di);
        auto s = [&](int k) { return buf_[static_cast<size_t>((pos_ - k + mask + 1) & mask)]; };
        const float xm1 = s(di - 1), x0 = s(di), x1 = s(di + 1), x2 = s(di + 2);
        const float c = 0.5f * (x1 - xm1), v = x0 - x1, w = c + v, a = w + v + 0.5f * (x2 - x0), b = w + a;
        return ((a * f - b) * f + c) * f + x0;
    }
    double sr_ = 48000.0, hornPh_ = 0.0, rotorPh_ = 0.0, hornHz_ = -1.0, rotorHz_ = -1.0;
    std::vector<float> buf_;
    int pos_ = 0;
    Biquad lpX_, lpBass_;
};

// -----------------------------------------------------------------------------
// Delay digital / ping-pong -- implementacao propria: repeticoes limpas com
// tom (passa-baixa e passa-altas no loop), ping-pong opcional e tempo livre
// ou sincronizado no BPM (o tempo suaviza ~60 ms ao mudar).
// -----------------------------------------------------------------------------
class DigitalDelay {
public:
    void prepare(double sr) { sr_ = sr; for (auto& l : line_) l.allocate(static_cast<size_t>(sr * 4.1) + 8); reset(); }
    void reset() { for (auto& l : line_) l.clear(); lp_[0] = lp_[1] = hp_[0] = hp_[1] = 0.0f; smooth_ = -1.0f; }
    void process(float seconds, float feedback, bool pingpong, float highCut, float lowCut, float* L, float* R, int n) {
        const float target = std::clamp(seconds, 0.001f, 4.0f) * static_cast<float>(sr_);
        if (smooth_ < 0.0f) smooth_ = target;
        const float sc = 1.0f - std::exp(-1.0f / (0.06f * static_cast<float>(sr_)));
        const float aLp = gfx::onePoleCoef(highCut, sr_), aHp = gfx::onePoleCoef(lowCut, sr_);
        const float fb = std::clamp(feedback, 0.0f, 0.98f);
        for (int i = 0; i < n; ++i) {
            smooth_ += (target - smooth_) * sc;
            float tl = line_[0].read(smooth_), tr = line_[1].read(smooth_);
            float y[2] = {tl, tr};
            for (int c = 0; c < 2; ++c) {
                lp_[c] += (y[c] - lp_[c]) * aLp;
                hp_[c] += (lp_[c] - hp_[c]) * aHp;
                y[c] = lp_[c] - hp_[c];
            }
            // ping-pong: a entrada (mono) vai so pra esquerda e cada repeticao cruza
            const float inL = pingpong ? 0.5f * (L[i] + R[i]) : L[i], inR = pingpong ? 0.0f : R[i];
            const float wl = inL + fb * (pingpong ? y[1] : y[0]), wr = inR + fb * (pingpong ? y[0] : y[1]);
            line_[0].write(std::isfinite(wl) ? wl : 0.0f);
            line_[1].write(std::isfinite(wr) ? wr : 0.0f);
            L[i] = tl;
            R[i] = tr;
        }
    }

private:
    double sr_ = 48000.0;
    gfx::DelayLine line_[2];
    float lp_[2] = {}, hp_[2] = {}, smooth_ = -1.0f;
};

// -----------------------------------------------------------------------------
// Delay reverso -- implementacao propria: grava sem parar e toca de tras pra
// frente pedacos do tamanho do tempo, com duas leituras defasadas meio pedaco
// e janelas de Hann que somam 1 (sem clique na emenda).
// -----------------------------------------------------------------------------
class ReverseDelay {
public:
    void prepare(double sr) { sr_ = sr; for (auto& l : line_) l.allocate(static_cast<size_t>(sr * 4.1) + 8); reset(); }
    void reset() { for (auto& l : line_) l.clear(); pos_ = 0.0f; }
    void process(float seconds, float feedback, float* L, float* R, int n) {
        const float len = std::clamp(seconds, 0.05f, 2.0f) * static_cast<float>(sr_);
        const float fb = std::clamp(feedback, 0.0f, 0.9f);
        for (int i = 0; i < n; ++i) {
            // dentro do pedaco: a leitura anda pra tras 2 amostras por amostra
            // (atraso cresce de 0 a 2*len), entao toca o trecho de tras pra frente
            pos_ += 1.0f;
            if (pos_ >= len) pos_ -= len;
            float out[2];
            for (int c = 0; c < 2; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < 2; ++k) {
                    float p = pos_ + k * 0.5f * len;
                    if (p >= len) p -= len;
                    const float w = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPiD * p / len)); // Hann
                    acc += w * line_[c].read(2.0f * p + 1.0f);
                }
                out[c] = acc;
            }
            const float wl = L[i] + fb * out[0], wr = R[i] + fb * out[1];
            line_[0].write(std::isfinite(wl) ? wl : 0.0f);
            line_[1].write(std::isfinite(wr) ? wr : 0.0f);
            L[i] = out[0];
            R[i] = out[1];
        }
    }

private:
    double sr_ = 48000.0;
    gfx::DelayLine line_[2];
    float pos_ = 0.0f;
};

} // namespace dx7fm::fxextra
