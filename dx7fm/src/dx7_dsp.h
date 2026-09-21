#pragma once
#include "dx7_params.h"
#include <cmath>
#include <vector>
#include <random>
#include <algorithm>

namespace dx7fm {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float dbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// ~0.75 dB por passo (8 passos = 6dB, igual ao comportamento do DX7 real).
// Level 0 retorna silencio exato, nao apenas muito baixo.
inline float egLevelToAmplitude(float level0to99) {
    if (level0to99 <= 0.0f) return 0.0f;
    float db = (level0to99 - 99.0f) * 0.75f;
    return dbToLinear(db);
}

// speed = 5.0 * 1.0897^rate : rate=99 percorre 99 unidades em ~4ms,
// rate=0 percorre em ~20s. Aproximacao propria, nao a tabela real do DX7.
inline float egRateToSpeed(float rate0to99) {
    return 5.0f * std::pow(1.0897f, rate0to99);
}

// Envelope classico de 4 estagios (rate/level 0-99). Serve tanto para
// amplitude quanto para pitch (PEG) -- quem chama decide o dominio do "level".
class SegmentEG {
public:
    void configure(const uint8_t rate[4], const uint8_t level[4]) {
        for (int i = 0; i < 4; ++i) { rate_[i] = rate[i]; level_[i] = level[i]; }
    }

    void noteOn(float rateScaleAdd) {
        rateScaleAdd_ = rateScaleAdd;
        stage_ = 0;
        current_ = 0.0f;
        idle_ = false;
    }

    void noteOff() {
        stage_ = 3;
        idle_ = false;
    }

    float process(double controlRateHz) {
        if (idle_) return current_;
        float target = static_cast<float>(level_[stage_]);
        float rate = std::clamp(static_cast<float>(rate_[stage_]) + rateScaleAdd_, 0.0f, 99.0f);
        float speed = egRateToSpeed(rate);
        float step = speed / static_cast<float>(controlRateHz);
        if (current_ < target) current_ = std::min(target, current_ + step);
        else if (current_ > target) current_ = std::max(target, current_ - step);

        if (current_ == target) {
            if (stage_ < 2) {
                ++stage_;
            } else if (stage_ == 3) {
                idle_ = true;
            }
            // stage_ == 2: segura (sustain) ate noteOff() mover pro estagio 3
        }
        return current_;
    }

    bool isIdle() const { return idle_; }

private:
    uint8_t rate_[4]  = {99, 99, 99, 50};
    uint8_t level_[4] = {99, 99, 99, 0};
    float rateScaleAdd_ = 0.0f;
    int stage_ = 2;
    float current_ = 0.0f;
    bool idle_ = true;
};

class LFO {
public:
    void configure(const LFOParams& p) { params_ = p; }
    void setSampleRate(double sr) { sampleRate_ = sr; }

    void noteOn() {
        if (params_.keySync) phase_ = 0.0;
        delayTimer_ = 0.0f;
    }

    float process() {
        float fade = 1.0f;
        if (params_.delaySec > 0.0f) {
            delayTimer_ += static_cast<float>(1.0 / sampleRate_);
            fade = std::clamp(delayTimer_ / params_.delaySec, 0.0f, 1.0f);
        }
        phase_ += params_.rate / sampleRate_;
        if (phase_ >= 1.0) {
            phase_ -= std::floor(phase_);
            if (params_.wave == LfoWave::SampleHold) shValue_ = dist_(rng_);
        }
        return waveValue(phase_) * fade;
    }

private:
    float waveValue(double phase01) const {
        float t = static_cast<float>(phase01);
        switch (params_.wave) {
            case LfoWave::Triangle: return t < 0.5f ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
            case LfoWave::SawUp:    return 2.0f * t - 1.0f;
            case LfoWave::SawDown:  return 1.0f - 2.0f * t;
            case LfoWave::Square:   return t < 0.5f ? 1.0f : -1.0f;
            case LfoWave::Sine:     return std::sin(t * kTwoPi);
            case LfoWave::SampleHold: return shValue_;
        }
        return 0.0f;
    }

    LFOParams params_;
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    float delayTimer_ = 0.0f;
    float shValue_ = 0.0f;
    std::mt19937 rng_{std::random_device{}()};
    std::uniform_real_distribution<float> dist_{-1.0f, 1.0f};
};

// Osciladores NAO sao band-limited: Saw/Square geram aliasing em frequencias
// altas sem oversampling (decisao consciente, ver ARQUITETURA.txt secao 8).
// [ADICIONADO em 2026-09-21] Reconstrucao propria das 7 formas de onda
// extras do motor AFM da Yamaha SY77/TX81Z (a 8a, Sine, ja existia). Todas
// derivadas de sin(2*pi*phase) por retificacao/corte/dobra de fase -- a
// MESMA familia de tecnicas que o hardware real usa (manipular o indice de
// fase e/ou o sinal antes de ler a tabela de seno), mas os cortes exatos
// abaixo sao uma aproximacao PROPRIA, nao extraida da ROM real (nao tenho
// as tabelas bit-exatas da SY77/TX81Z) -- mesmo espirito de
// egRateToSpeed()/egLevelToAmplitude(). Ver ARQUITETURA.txt secao 5.
inline float afmHalfSine(float p) {
    return p < 0.5f ? std::sin(p * kTwoPi) : 0.0f;
}
inline float afmAbsSine(float p) {
    return std::fabs(std::sin(p * kTwoPi));
}
inline float afmQuarterSine(float p) {
    // 2 "bicos" rapidos (taxa dobrada) comprimidos na primeira metade, silencio na segunda
    return p < 0.5f ? std::fabs(std::sin(p * 2.0f * kTwoPi)) : 0.0f;
}
inline float afmAlternatingSine(float p) {
    // seno em taxa dobrada: alterna +,-,+,- dentro de UM ciclo do operador
    return std::sin(p * 2.0f * kTwoPi);
}
inline float afmCamelSine(float p) {
    // 2 corcovas positivas separadas por silencio (em vez de se tocarem
    // suavemente como no AbsSine) -- daí o nome "camelo"
    bool inHump = (p < 0.25f) || (p >= 0.5f && p < 0.75f);
    return inHump ? std::fabs(std::sin(p * 2.0f * kTwoPi)) : 0.0f;
}
inline float afmSquaredSine(float p) {
    // s*|s|: "espreme" a regiao central pro zero mantendo os picos, dando
    // um carater mais "ressonante/pontudo" que o seno puro
    float s = std::sin(p * kTwoPi);
    return s * std::fabs(s);
}
inline float afmLogSaw(float p) {
    // dente-de-serra com curvatura exponencial em vez de rampa linear
    constexpr float k = 4.0f;
    return 2.0f * (std::exp(k * p) - 1.0f) / (std::exp(k) - 1.0f) - 1.0f;
}

inline float oscWave(float phase01, Waveform wf, std::mt19937& rng,
                      std::uniform_real_distribution<float>& dist) {
    switch (wf) {
        case Waveform::Sine:            return std::sin(phase01 * kTwoPi);
        case Waveform::Triangle:        return phase01 < 0.5f ? (4.0f * phase01 - 1.0f) : (3.0f - 4.0f * phase01);
        case Waveform::Saw:             return 2.0f * phase01 - 1.0f;
        case Waveform::Square:          return phase01 < 0.5f ? 1.0f : -1.0f;
        case Waveform::Noise:           return dist(rng);
        case Waveform::HalfSine:        return afmHalfSine(phase01);
        case Waveform::AbsSine:         return afmAbsSine(phase01);
        case Waveform::QuarterSine:     return afmQuarterSine(phase01);
        case Waveform::AlternatingSine: return afmAlternatingSine(phase01);
        case Waveform::CamelSine:       return afmCamelSine(phase01);
        case Waveform::SquaredSine:     return afmSquaredSine(phase01);
        case Waveform::LogSaw:          return afmLogSaw(phase01);
    }
    return 0.0f;
}

// Warp de fase estilo Casio CZ. amount=0 e identidade.
inline float phaseDistort(float phase01, float amount) {
    if (amount <= 0.0f) return phase01;
    float warped = phase01 + amount * (std::sin(kTwoPi * phase01) / kTwoPi);
    warped -= std::floor(warped);
    return warped;
}

// Filtro State-Variable "Topology-Preserving Transform" (zero-delay feedback,
// forma de Andrew Simper). Estavel sob modulacao rapida de corte.
class TPTStateVariableFilter {
public:
    void setSampleRate(double sr) { sampleRate_ = sr; }

    void setParams(float cutoffHz, float q) {
        cutoffHz = std::clamp(cutoffHz, 10.0f, static_cast<float>(sampleRate_) * 0.49f);
        q = std::max(0.05f, q);
        float g = std::tan(kPi * cutoffHz / static_cast<float>(sampleRate_));
        float k = 1.0f / q;
        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
        k_ = k;
    }

    struct Outputs { float lp, bp, hp; };

    Outputs process(float x) {
        float v3 = x - ic2eq_;
        float v1 = a1_ * ic1eq_ + a2_ * v3;
        float v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;
        ic1eq_ = 2.0f * v1 - ic1eq_;
        ic2eq_ = 2.0f * v2 - ic2eq_;
        float bp = v1;
        float lp = v2;
        float hp = x - k_ * v1 - v2;
        return {lp, bp, hp};
    }

private:
    double sampleRate_ = 48000.0;
    float k_ = 1.0f, a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1eq_ = 0.0f, ic2eq_ = 0.0f;
};

inline float softSaturate(float x, float drive) {
    return std::tanh(x * (1.0f + drive * 8.0f));
}

// [ADICIONADO em 2026-09-21] Limitador de saida (master bus), NAO um
// efeito de distorcao por operador como softSaturate() acima. Diferenca
// crucial: e TRANSPARENTE (identidade exata) abaixo de `threshold` -- so
// comprime suavemente (joelho suave, via tanh) o excesso ACIMA disso,
// aproximando-se assintoticamente de +-1.0 sem jamais ultrapassar. Existe
// porque Synth::renderStereo() soma varias vozes (um acorde, por exemplo)
// SEM nenhuma normalizacao por numero de vozes -- uma unica nota ja pode
// chegar perto de +-1.0 com o patch default, entao um acorde de 3 notas
// facilmente passa de +-3.0 sem isso (ver ARQUITETURA.txt secao 7 sobre o
// bug que motivou essa correcao). Aplicado 1x no final de renderStereo(),
// depois de somar as 4 partes E dos efeitos globais.
inline float softLimit(float x, float threshold = 0.9f) {
    float ax = std::fabs(x);
    if (ax <= threshold) return x;
    float sign = x < 0.0f ? -1.0f : 1.0f;
    float excess = ax - threshold;
    float headroom = 1.0f - threshold;
    float compressed = threshold + headroom * std::tanh(excess / headroom);
    return sign * compressed;
}

// Reducao de sample rate (sample & hold) + reducao de bits.
class Crusher {
public:
    void setSampleRate(double sr) { sampleRate_ = sr; }

    float process(float x, float crushSampleHz, float bits) {
        crushSampleHz = std::max(1.0f, crushSampleHz);
        float holdTime = 1.0f / crushSampleHz;
        timer_ += static_cast<float>(1.0 / sampleRate_);
        if (timer_ >= holdTime) {
            timer_ -= holdTime;
            held_ = x;
        }
        float levels = std::pow(2.0f, std::max(1.0f, bits));
        return std::round(held_ * levels * 0.5f) / (levels * 0.5f);
    }

private:
    double sampleRate_ = 48000.0;
    float timer_ = 0.0f;
    float held_ = 0.0f;
};

// Delay curto com feedback e interpolacao linear (buffer de 50ms).
class CombFilter {
public:
    void setSampleRate(double sr) {
        sampleRate_ = sr;
        buffer_.assign(static_cast<size_t>(sr * 0.05) + 4, 0.0f);
        writePos_ = 0;
    }

    float process(float x, float delayHz, float feedback) {
        delayHz = std::max(1.0f, delayHz);
        float delaySamples = std::min(static_cast<float>(sampleRate_) / delayHz,
                                       static_cast<float>(buffer_.size() - 2));
        float readPos = static_cast<float>(writePos_) - delaySamples;
        while (readPos < 0.0f) readPos += static_cast<float>(buffer_.size());
        size_t i0 = static_cast<size_t>(readPos) % buffer_.size();
        size_t i1 = (i0 + 1) % buffer_.size();
        float frac = readPos - std::floor(readPos);
        float y = buffer_[i0] * (1.0f - frac) + buffer_[i1] * frac;
        buffer_[writePos_] = x + y * std::clamp(feedback, -0.99f, 0.99f);
        writePos_ = (writePos_ + 1) % buffer_.size();
        return y;
    }

private:
    double sampleRate_ = 48000.0;
    std::vector<float> buffer_;
    size_t writePos_ = 0;
};

// APROXIMACAO grosseira de single-sideband shifter (allpass de 1 polo,
// NAO e um Hilbert de qualidade). Ver limitacao conhecida em ARQUITETURA.txt.
class FreqShifter {
public:
    void setSampleRate(double sr) { sampleRate_ = sr; }

    float process(float x, float shiftHz) {
        const float a = 0.6f;
        float apOut = a * (x - apState_) + apDelay_;
        apDelay_ = x;
        apState_ = apOut;

        phase_ += kTwoPi * shiftHz / static_cast<float>(sampleRate_);
        if (phase_ > kPi) phase_ -= kTwoPi;
        if (phase_ < -kPi) phase_ += kTwoPi;
        return x * std::cos(phase_) - apOut * std::sin(phase_);
    }

private:
    double sampleRate_ = 48000.0;
    float phase_ = 0.0f;
    float apState_ = 0.0f, apDelay_ = 0.0f;
};

// Decimador FIR de 2:1 com filtro half-band (janela de Hamming, corte
// normalizado em 0.25 -- exatamente o Nyquist do sinal DECIMADO). Usado
// pra oversampling anti-aliasing: cascatear 2 instancias decima 4:1.
// Corte em fc=0.25 faz os coeficientes de indice PAR (exceto o central)
// carem em zero automaticamente (propriedade classica do half-band),
// mas aqui a convolucao e feita direto (sem explorar isso), priorizando
// clareza sobre eficiencia (ver ARQUITETURA.txt secao 1).
class HalfbandDecimator2x {
public:
    static constexpr int kTaps = 31;

    HalfbandDecimator2x() {
        const int M = kTaps - 1;
        const double fc = 0.25;
        double sum = 0.0;
        for (int n = 0; n < kTaps; ++n) {
            double k = n - M / 2.0;
            double sinc = (k == 0.0) ? 2.0 * fc : std::sin(2.0 * kPiD * fc * k) / (kPiD * k);
            double w = 0.54 - 0.46 * std::cos(2.0 * kPiD * n / M); // Hamming
            coeffs_[n] = sinc * w;
            sum += coeffs_[n];
        }
        for (auto& c : coeffs_) c /= sum; // normaliza ganho DC pra 1.0
    }

    // Chame 1x por amostra na taxa DE ENTRADA (superamostrada). Retorna
    // true e escreve out a cada 2 chamadas (na taxa decimada).
    bool process(float x, float& out) {
        buf_[pos_] = x;
        pos_ = (pos_ + 1) % kTaps;
        phase_ ^= 1;
        if (phase_ != 0) return false;

        double acc = 0.0;
        int idx = pos_;
        for (int i = 0; i < kTaps; ++i) {
            idx = (idx == 0) ? kTaps - 1 : idx - 1;
            acc += coeffs_[i] * buf_[idx];
        }
        out = static_cast<float>(acc);
        return true;
    }

private:
    static constexpr double kPiD = 3.14159265358979323846;
    double coeffs_[kTaps];
    float buf_[kTaps] = {};
    int pos_ = 0;
    int phase_ = 0;
};

} // namespace dx7fm
