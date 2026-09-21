#pragma once
#include "dx7_params.h"
#include "dx7_dsp.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dx7fm {

// Efeitos globais POS-MIX (aplicados na soma das 4 partes, uma vez por
// amostra estereo, nao por operador/voz). Blocos genericos, assim como
// dx7_dsp.h -- nao sabem nada sobre Synth/PartRuntime, so recebem os
// params de dx7_params.h e um par de amostras L/R.

// Chorus/ensemble: N linhas de delay moduladas por LFOs de fase
// deslocada (largura estereo por 90 graus de diferenca entre L e R).
class StereoChorus {
public:
    void setSampleRate(double sr) {
        sampleRate_ = sr;
        size_t bufSize = static_cast<size_t>(sr * 0.05) + 8; // ate 50ms
        bufL_.assign(bufSize, 0.0f);
        bufR_.assign(bufSize, 0.0f);
        writePos_ = 0;
    }

    void process(const ChorusParams& p, float xL, float xR, float& outL, float& outR) {
        if (!p.enabled) { outL = xL; outR = xR; return; }

        bufL_[writePos_] = xL;
        bufR_[writePos_] = xR;

        int voices = std::clamp(p.voices, 1, 4);
        float wetL = 0.0f, wetR = 0.0f;
        for (int v = 0; v < voices; ++v) {
            float off = static_cast<float>(v) / static_cast<float>(voices);
            float lfoL = std::sin(kTwoPi * (phase_ + off));
            float lfoR = std::sin(kTwoPi * (phase_ + off + 0.25f)); // 90 graus: largura estereo
            wetL += readInterp(bufL_, p.baseDelayMs + lfoL * p.depthMs) / static_cast<float>(voices);
            wetR += readInterp(bufR_, p.baseDelayMs + lfoR * p.depthMs) / static_cast<float>(voices);
        }

        phase_ += p.rateHz / static_cast<float>(sampleRate_);
        if (phase_ >= 1.0f) phase_ -= std::floor(phase_);
        writePos_ = (writePos_ + 1) % bufL_.size();

        float mix = std::clamp(p.mix, 0.0f, 1.0f);
        outL = xL * (1.0f - mix) + wetL * mix;
        outR = xR * (1.0f - mix) + wetR * mix;
    }

private:
    float readInterp(const std::vector<float>& buf, float delayMs) const {
        float delaySamples = std::clamp(delayMs * 0.001f * static_cast<float>(sampleRate_),
                                         1.0f, static_cast<float>(buf.size() - 2));
        float readPos = static_cast<float>(writePos_) - delaySamples;
        while (readPos < 0.0f) readPos += static_cast<float>(buf.size());
        size_t i0 = static_cast<size_t>(readPos) % buf.size();
        size_t i1 = (i0 + 1) % buf.size();
        float frac = readPos - std::floor(readPos);
        return buf[i0] * (1.0f - frac) + buf[i1] * frac;
    }

    double sampleRate_ = 48000.0;
    std::vector<float> bufL_, bufR_;
    size_t writePos_ = 0;
    float phase_ = 0.0f;
};

// Delay estereo com tempos independentes por canal, feedback e cross-feed
// (0 = delays L/R independentes, 1 = ping-pong total).
class StereoDelay {
public:
    void setSampleRate(double sr) {
        sampleRate_ = sr;
        size_t bufSize = static_cast<size_t>(sr * 2.0) + 8; // ate 2s
        bufL_.assign(bufSize, 0.0f);
        bufR_.assign(bufSize, 0.0f);
        writePos_ = 0;
    }

    void process(const DelayParams& p, float xL, float xR, float& outL, float& outR) {
        if (!p.enabled) { outL = xL; outR = xR; return; }

        size_t n = bufL_.size();
        size_t delayL = std::clamp(static_cast<size_t>(p.timeLeftSec * sampleRate_), size_t(1), n - 2);
        size_t delayR = std::clamp(static_cast<size_t>(p.timeRightSec * sampleRate_), size_t(1), n - 2);
        size_t readL = (writePos_ + n - delayL) % n;
        size_t readR = (writePos_ + n - delayR) % n;

        float tapL = bufL_[readL];
        float tapR = bufR_[readR];

        float fb = std::clamp(p.feedback, 0.0f, 0.95f);
        float cross = std::clamp(p.crossFeed, 0.0f, 1.0f);
        bufL_[writePos_] = xL + (tapL * (1.0f - cross) + tapR * cross) * fb;
        bufR_[writePos_] = xR + (tapR * (1.0f - cross) + tapL * cross) * fb;
        writePos_ = (writePos_ + 1) % n;

        float mix = std::clamp(p.mix, 0.0f, 1.0f);
        outL = xL * (1.0f - mix) + tapL * mix;
        outR = xR * (1.0f - mix) + tapR * mix;
    }

private:
    double sampleRate_ = 48000.0;
    std::vector<float> bufL_, bufR_;
    size_t writePos_ = 0;
};

// Reverb algoritmico estilo Schroeder/Freeverb simplificado: 4 combs
// paralelos com feedback amortecido (lowpass no loop) por canal, seguidos
// de 2 allpass em serie. Tunings dos combs/allpass em L e R deslocados
// (+23 amostras) pra decorrelacionar os canais e dar largura estereo.
class SimpleReverb {
public:
    void setSampleRate(double sr) {
        sampleRate_ = sr;
        static const float combMsL[4] = {25.31f, 26.94f, 28.96f, 30.75f};
        static const float apMsL[2] = {12.61f, 9.68f};
        for (int i = 0; i < 4; ++i) {
            combL_[i].resize(static_cast<size_t>(combMsL[i] * 0.001 * sr) + 1);
            combR_[i].resize(combL_[i].buf.size() + 23);
        }
        for (int i = 0; i < 2; ++i) {
            apL_[i].resize(static_cast<size_t>(apMsL[i] * 0.001 * sr) + 1);
            apR_[i].resize(apL_[i].buf.size() + 23);
        }
    }

    void process(const ReverbParams& p, float xL, float xR, float& outL, float& outR) {
        if (!p.enabled) { outL = xL; outR = xR; return; }

        float input = (xL + xR) * 0.5f * 0.25f; // atenua antes dos combs (evita saturar o loop)
        float fb = 0.7f + std::clamp(p.roomSize, 0.0f, 1.0f) * 0.28f; // 0.70..0.98
        float damp = std::clamp(p.damping, 0.0f, 1.0f);

        float wetL = 0.0f, wetR = 0.0f;
        for (int i = 0; i < 4; ++i) {
            wetL += combL_[i].process(input, fb, damp);
            wetR += combR_[i].process(input, fb, damp);
        }
        for (int i = 0; i < 2; ++i) {
            wetL = apL_[i].process(wetL, 0.5f);
            wetR = apR_[i].process(wetR, 0.5f);
        }

        float width = std::clamp(p.width, 0.0f, 1.0f);
        float mid = width * 0.5f;
        float wideL = wetL * (0.5f + mid) + wetR * (0.5f - mid);
        float wideR = wetR * (0.5f + mid) + wetL * (0.5f - mid);

        float mix = std::clamp(p.mix, 0.0f, 1.0f);
        outL = xL * (1.0f - mix) + wideL * mix;
        outR = xR * (1.0f - mix) + wideR * mix;
    }

private:
    struct DampedComb {
        std::vector<float> buf;
        size_t pos = 0;
        float filterStore = 0.0f;
        void resize(size_t n) { buf.assign(n, 0.0f); pos = 0; filterStore = 0.0f; }
        float process(float x, float feedback, float damp) {
            float out = buf[pos];
            filterStore = out * (1.0f - damp) + filterStore * damp;
            buf[pos] = x + filterStore * feedback;
            pos = (pos + 1) % buf.size();
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        size_t pos = 0;
        void resize(size_t n) { buf.assign(n, 0.0f); pos = 0; }
        float process(float x, float feedback) {
            float bufOut = buf[pos];
            float y = -x + bufOut;
            buf[pos] = x + bufOut * feedback;
            pos = (pos + 1) % buf.size();
            return y;
        }
    };

    double sampleRate_ = 48000.0;
    DampedComb combL_[4], combR_[4];
    Allpass apL_[2], apR_[2];
};

} // namespace dx7fm
