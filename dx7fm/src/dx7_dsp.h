#pragma once
#include "dx7_params.h"
#include <cmath>
#include <cstdlib>
#include <vector>
#include <random>
#include <algorithm>

namespace dx7fm {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kLog2Of10Over20 = 0.166096404744368f; // 10^(dB/20) == 2^(dB * log2(10)/20)

// [ADICIONADO em 2026-10-18] Seno rapido por tabela (4096 pontos +
// interpolacao linear; erro maximo ~3e-7) pros osciladores da forma de onda
// Additive: ~2x mais barato que std::sin, e e o que torna viavel ter 8
// osciladores reais por operador. Tabela montada na inicializacao estatica
// do programa (inline variable), nunca na audio thread.
struct SineTable {
    static constexpr int kSize = 4096;
    float v[kSize + 1];
    SineTable() {
        for (int i = 0; i <= kSize; ++i) v[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * i / kSize));
    }
};
inline const SineTable kSineTable{};

// Parte fracionaria em [0, 1) sem chamar std::floor (que nao vira uma
// instrucao so em todo alvo): truncar pra int e corrigir negativos. Vale pra
// |x| < 2^31, folgado pra fases.
inline float wrap01(float x) {
    x -= static_cast<float>(static_cast<int>(x));
    return x < 0.0f ? x + 1.0f : x;
}

// phase01 em [0, 1)
inline float fastSin01(float phase01) {
    float x = phase01 * static_cast<float>(SineTable::kSize);
    int i = static_cast<int>(x);
    i = i < 0 ? 0 : (i >= SineTable::kSize ? SineTable::kSize - 1 : i);
    float f = x - static_cast<float>(i);
    return kSineTable.v[i] + (kSineTable.v[i + 1] - kSineTable.v[i]) * f;
}


inline float dbToLinear(float db) {
    return std::exp2(db * kLog2Of10Over20);
}

// ~0.75 dB por passo (8 passos = 6dB, igual ao comportamento do DX7 real).
// Level 0 retorna silencio exato, nao apenas muito baixo.
inline float egLevelToAmplitude(float level0to99) {
    if (level0to99 <= 0.0f) return 0.0f;
    return std::exp2((level0to99 - 99.0f) * (0.75f * kLog2Of10Over20));
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
        stepStage_ = -1;
    }

    // startLevel: nivel de onde o estagio 0 parte. 0 pro EG de amplitude;
    // o PEG passa o proprio level[3] (como no DX7), senao toda nota comecaria
    // em 0 = -rangeOctaves e daria um "blip" de pitch nos primeiros ms.
    void noteOn(float rateScaleAdd, float startLevel = 0.0f) {
        rateScaleAdd_ = rateScaleAdd;
        stage_ = 0;
        current_ = startLevel;
        idle_ = false;
        stepStage_ = -1;
    }

    void noteOff() {
        stage_ = 3;
        idle_ = false;
    }

    // [2026-10-24] loop do sustain de volta ao estagio 0 (envelopes de modulacao)
    void setLoop(bool loop) { loop_ = loop; }

    float process(double controlRateHz) {
        if (idle_) return current_;
        float target = static_cast<float>(level_[stage_]);
        if (stage_ != stepStage_ || controlRateHz != stepRateHz_) {
            // egRateToSpeed usa pow: so recalcula quando o estagio (ou a taxa) muda
            float rate = std::clamp(static_cast<float>(rate_[stage_]) + rateScaleAdd_, 0.0f, 99.0f);
            step_ = egRateToSpeed(rate) / static_cast<float>(controlRateHz);
            stepStage_ = stage_;
            stepRateHz_ = controlRateHz;
        }
        float step = step_;
        if (current_ < target) current_ = std::min(target, current_ + step);
        else if (current_ > target) current_ = std::max(target, current_ - step);

        if (current_ == target) {
            if (stage_ < 2) {
                ++stage_;
            } else if (stage_ == 2 && loop_) {
                stage_ = 0; // de novo L1 -> L2 -> L3, partindo daqui
            } else if (stage_ == 3) {
                idle_ = true;
            }
            // stage_ == 2: segura (sustain) ate noteOff() mover pro estagio 3
        }
        return current_;
    }

    bool isIdle() const { return idle_; }
    // Idle E em nivel 0. Um EG de amplitude que termina o release num
    // level[3] > 0 fica idle mas continua soando (comportamento do DX7).
    bool isSilent() const { return idle_ && current_ <= 0.0f; }
    float current() const { return current_; }

private:
    uint8_t rate_[4]  = {99, 99, 99, 50};
    uint8_t level_[4] = {99, 99, 99, 0};
    float rateScaleAdd_ = 0.0f;
    int stage_ = 2;
    float current_ = 0.0f;
    bool idle_ = true;
    bool loop_ = false;
    // cache do passo por amostra do estagio atual
    float step_ = 0.0f;
    int stepStage_ = -1;
    double stepRateHz_ = 0.0;
};

class LFO {
public:
    void configure(const LFOParams& p) { params_ = p; }
    void setSampleRate(double sr) { sampleRate_ = sr; }

    void noteOn() {
        if (params_.keySync) phase_ = 0.0;
        delayTimer_ = 0.0f;
    }

    // [2026-10-22] BPM global pros LFOs com tempoSync (o Synth atualiza)
    void setTempo(float bpm) { tempoBpm_ = bpm; }

    // rateMul: multiplicador da velocidade (ModDest::LfoRate); 1 = sem modulacao
    float process(float rateMul = 1.0f) {
        float fade = 1.0f;
        if (params_.delaySec > 0.0f) {
            delayTimer_ += static_cast<float>(1.0 / sampleRate_);
            fade = std::clamp(delayTimer_ / params_.delaySec, 0.0f, 1.0f);
        }
        double rate = params_.tempoSync ? tempoBpm_ / (60.0 * lfoDivisionBeats(params_.division)) : params_.rate;
        if (rateMul != 1.0f) rate *= rateMul;
        phase_ += rate / sampleRate_;
        if (phase_ >= 1.0) {
            phase_ -= std::floor(phase_);
            newCycle();
        }
        return output(fade);
    }

    // [2026-10-24] LFO global com sync: a fase vem de fora (posicao de batida);
    // quando ela volta pra tras, comeca um ciclo novo (S&H/SmoothRandom sorteiam)
    float processAtPhase(double phase01) {
        float fade = 1.0f;
        if (params_.delaySec > 0.0f) {
            delayTimer_ += static_cast<float>(1.0 / sampleRate_);
            fade = std::clamp(delayTimer_ / params_.delaySec, 0.0f, 1.0f);
        }
        if (phase01 < phase_) newCycle();
        phase_ = phase01;
        return output(fade);
    }

private:
    void newCycle() {
        if (params_.wave == LfoWave::SampleHold) shValue_ = dist_(rng_);
        if (params_.wave == LfoWave::SmoothRandom) { smoothFrom_ = smoothTo_; smoothTo_ = dist_(rng_); }
    }

    float output(float fade) const {
        float v = waveValue(phase_);
        if (params_.steps >= 2) { // escada de `steps` niveis em -1..1
            const float n = static_cast<float>(params_.steps - 1);
            v = std::round((v + 1.0f) * 0.5f * n) / n * 2.0f - 1.0f;
        }
        return v * fade;
    }

    float waveValue(double phase01) const {
        float t = static_cast<float>(phase01);
        switch (params_.wave) {
            case LfoWave::Triangle: return t < 0.5f ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
            case LfoWave::SawUp:    return 2.0f * t - 1.0f;
            case LfoWave::SawDown:  return 1.0f - 2.0f * t;
            case LfoWave::Square:   return t < 0.5f ? 1.0f : -1.0f;
            case LfoWave::Sine:     return fastSin01(t);
            case LfoWave::SampleHold: return shValue_;
            case LfoWave::SmoothRandom: // transicao em cosseno do valor anterior ao sorteado
                return smoothFrom_ + (smoothTo_ - smoothFrom_) * (0.5f - 0.5f * fastSin01(0.25f + 0.5f * t));
        }
        return 0.0f;
    }

    LFOParams params_;
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    float delayTimer_ = 0.0f;
    float shValue_ = 0.0f;
    float smoothFrom_ = 0.0f, smoothTo_ = 0.0f;
    double tempoBpm_ = 120.0;
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
    return p < 0.5f ? fastSin01(p) : 0.0f;
}
inline float afmAbsSine(float p) {
    return std::fabs(fastSin01(p));
}
inline float afmQuarterSine(float p) {
    // 2 "bicos" rapidos (taxa dobrada) comprimidos na primeira metade, silencio na segunda
    return p < 0.5f ? std::fabs(fastSin01(p * 2.0f)) : 0.0f; // p*2 < 1 aqui
}
inline float afmAlternatingSine(float p) {
    // seno em taxa dobrada: alterna +,-,+,- dentro de UM ciclo do operador
    float q = p * 2.0f;
    return fastSin01(q >= 1.0f ? q - 1.0f : q);
}
inline float afmCamelSine(float p) {
    // 2 corcovas positivas separadas por silencio (em vez de se tocarem
    // suavemente como no AbsSine) -- daí o nome "camelo"
    bool inHump = (p < 0.25f) || (p >= 0.5f && p < 0.75f);
    float q = p * 2.0f;
    return inHump ? std::fabs(fastSin01(q >= 1.0f ? q - 1.0f : q)) : 0.0f;
}
inline float afmSquaredSine(float p) {
    // s*|s|: "espreme" a regiao central pro zero mantendo os picos, dando
    // um carater mais "ressonante/pontudo" que o seno puro
    float s = fastSin01(p);
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
        case Waveform::Sine:            return fastSin01(phase01);
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
        // Additive tem estado (fase de kNumPartials osciladores) e e gerada pela Voice
        // via PartialOscillators; se chegar aqui, cai no seno do parcial 1
        case Waveform::Additive:        return fastSin01(phase01);
        // Wavetable le as tabelas do patch (dx7_wavetable.h) na Voice; sem
        // tabela (onda nao carregada) soa como seno
        case Waveform::Wavetable:       return fastSin01(phase01);
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

// [ADICIONADO em 2026-10-18; SIMD e 8 -> 64 parciais em 2026-10-20]
// Kernel dos osciladores da forma de onda Additive. Os kNumPartials
// parciais sao independentes e fazem a MESMA conta, entao sao calculados 8
// por vez com vetores de 8 floats (extensoes de vetor do GCC): numa CPU com
// AVX2+FMA cada vetor e uma instrucao; sem AVX2 (qualquer x64 tem SSE2) o
// compilador divide em 2 de 4. A versao AVX2 e escolhida em tempo de
// execucao (cpuSupportsAvx2Fma), entao o binario roda em qualquer x64.
//
// Seno: polinomio (Taylor ate t^11 apos reduzir a fase a +-1/4 de ciclo,
// erro ~6e-8) em vez da tabela de fastSin01 -- leitura de tabela com indice
// diferente por lane nao vetoriza bem.
// -Wpsabi ("vetor AVX sem AVX muda a ABI") desligado de proposito daqui em
// diante (sem push/pop): os helpers que recebem/devolvem vetores de 32 bytes
// sao always_inline, nunca viram chamada de verdade, entao a ABI nao entra em
// jogo -- mas o GCC emite o aviso no PONTO DE USO (no .cpp que inclui este
// header), depois de um eventual pop.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wpsabi"
#endif
namespace simd {
// Largura do vetor e parametro de template: 8 floats (AVX2, 256 bits) ou 4
// (SSE2, 128 bits -- a largura nativa sem AVX; com 8 o GCC divide em 2
// metades e faz as selecoes condicionais lane a lane, ficando MAIS LENTO
// que o codigo escalar).
typedef float f8 __attribute__((vector_size(32)));
typedef int i8 __attribute__((vector_size(32)));
typedef float f4 __attribute__((vector_size(16)));
typedef int i4 __attribute__((vector_size(16)));

#define DX7_SIMD_INLINE inline __attribute__((always_inline))
template <typename F> DX7_SIMD_INLINE F loadv(const float* p) { F v; __builtin_memcpy(&v, p, sizeof v); return v; }
template <typename F> DX7_SIMD_INLINE void storev(float* p, const F& v) { __builtin_memcpy(p, &v, sizeof v); }
template <typename F, typename I> DX7_SIMD_INLINE F truncv(const F& x) { return __builtin_convertvector(__builtin_convertvector(x, I), F); } // |x| < 2^31

// sin(2*pi*p) pra p em [0,1)
template <typename F, typename I> DX7_SIMD_INLINE F sin2piv(const F& p) {
    F x = p - 0.5f;                        // [-0.5, 0.5): sin(2pi p) = -sin(2pi x)
    I hi = x > 0.25f, lo = x < -0.25f;     // reflete pra |x| <= 1/4 (sin(pi - a) = sin(a))
    x = hi ? 0.5f - x : (lo ? -0.5f - x : x);
    F t = x * 6.28318530717958647692f;
    F t2 = t * t;
    F poly = 1.0f + t2 * (-1.0f / 6.0f + t2 * (1.0f / 120.0f + t2 * (-1.0f / 5040.0f + t2 * (1.0f / 362880.0f + t2 * (-1.0f / 39916800.0f)))));
    return -(t * poly);
}

template <typename F> DX7_SIMD_INLINE float hsum(const F& v) {
    float s = 0.0f;
    for (unsigned i = 0; i < sizeof(F) / sizeof(float); ++i) s += v[i];
    return s;
}

// Uma amostra de `groups` grupos de 8 parciais. Devolve sum(amp*sen) e
// escreve sum(amp) em ampSumOut. Parcial com amp 0 ou acima de Nyquist entra
// so na soma das amplitudes (a fase anda igual, pra voltar sem descontinuidade).
template <typename F, typename I, bool Morph>
DX7_SIMD_INLINE float partialsKernel(float* phase, const float* aAmp, const float* aRatio, const float* bAmp, const float* bRatio,
                                     float morph, float baseInc, float baseHz, float modIn, float nyquist, int groups, float& ampSumOut) {
    constexpr int W = sizeof(F) / sizeof(float);
    F sum = {}, ampSum = {};
    const int n = groups * 8;
    for (int o = 0; o < n; o += W) {
        F amp = loadv<F>(aAmp + o), ratio = loadv<F>(aRatio + o);
        if (Morph) {
            amp += (loadv<F>(bAmp + o) - amp) * morph;
            ratio += (loadv<F>(bRatio + o) - ratio) * morph;
        }
        F ph = loadv<F>(phase + o) + ratio * baseInc;  // >= 0
        ph -= truncv<F, I>(ph);
        storev<F>(phase + o, ph);
        ampSum += amp;
        F p = ph + ratio * modIn;                      // PM pode empurrar pra qualquer lado
        p -= truncv<F, I>(p);
        p = p < 0.0f ? p + 1.0f : p;
        I audible = (amp > 0.0f) & (ratio * baseHz < nyquist);
        F s = sin2piv<F, I>(p);
        sum += audible ? amp * s : F{};
    }
    ampSumOut = hsum(ampSum);
    return hsum(sum);
}

using KernelFn = float (*)(float*, const float*, const float*, const float*, const float*, float, float, float, float, float, int, float&);

inline float kernelGeneric(float* ph, const float* aa, const float* ar, const float* ba, const float* br, float m, float inc, float hz,
                           float mod, float nyq, int groups, float& ampSum) {
    return m > 0.0f ? partialsKernel<f4, i4, true>(ph, aa, ar, ba, br, m, inc, hz, mod, nyq, groups, ampSum)
                    : partialsKernel<f4, i4, false>(ph, aa, ar, ba, br, m, inc, hz, mod, nyq, groups, ampSum);
}
__attribute__((target("avx2,fma"))) inline float kernelAvx2(float* ph, const float* aa, const float* ar, const float* ba, const float* br,
                                                            float m, float inc, float hz, float mod, float nyq, int groups, float& ampSum) {
    return m > 0.0f ? partialsKernel<f8, i8, true>(ph, aa, ar, ba, br, m, inc, hz, mod, nyq, groups, ampSum)
                    : partialsKernel<f8, i8, false>(ph, aa, ar, ba, br, m, inc, hz, mod, nyq, groups, ampSum);
}

inline bool cpuSupportsAvx2Fma() {
    static const bool ok = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    }();
    return ok;
}
// DX7FM_NO_AVX2 no ambiente forca o caminho generico (usado pelos testes)
inline KernelFn selectedKernel() {
    static const KernelFn fn = (cpuSupportsAvx2Fma() && !std::getenv("DX7FM_NO_AVX2")) ? &kernelAvx2 : &kernelGeneric;
    return fn;
}
#undef DX7_SIMD_INLINE
} // namespace simd

// Os osciladores de um operador Additive (estado = fase de cada parcial).
// Cada parcial tem frequencia propria (razao livre, nao precisa ser
// harmonica), entao nao da pra usar uma wavetable: sao osciladores de verdade.
//  - morph (0..1) interpola amplitude E razao entre os espectros A e B.
//  - PM: o parcial k recebe modIn * ratio_k ciclos de desvio de fase -- o
//    mesmo que acontece com o harmonico k de uma onda periodica modulada em
//    fase, entao com razoes harmonicas soa igual a modular a onda inteira.
//  - Anti-aliasing: parcial com frequencia >= Nyquist e desligado (a fase
//    continua andando, pra voltar sem descontinuidade).
//  - Normalizacao: divide pela SOMA das amplitudes (pico <= 1). A soma inclui
//    os parciais desligados por Nyquist, entao o volume nao sobe quando uma
//    nota aguda perde parciais. Todas as amplitudes em 0 = silencio.
//  - groups: quantos grupos de 8 parciais processar (ate o ultimo parcial com
//    amplitude != 0 em A ou B, calculado pela Voice no cache do patch) --
//    um seno puro processa 1 grupo, nao 8.
struct PartialOscillators {
    alignas(32) float phase[kNumPartials] = {};

    void reset() { for (auto& p : phase) p = 0.0f; }

    static int groupsFor(const OperatorParams::PartialSet& a, const OperatorParams::PartialSet& b) {
        int last = -1;
        for (int k = 0; k < kNumPartials; ++k) if (a.amp[k] != 0.0f || b.amp[k] != 0.0f) last = k;
        return (last + 8) / 8; // last = -1 -> 0 grupos
    }

    float process(const OperatorParams::PartialSet& a, const OperatorParams::PartialSet& b, float morph,
                  float baseHz, float modIn, float phaseDistortion, float sampleRate, int groups = kNumPartials / 8) {
        const float nyquist = 0.5f * sampleRate;
        const float baseInc = baseHz / sampleRate;
        if (phaseDistortion <= 0.0f) {
            float ampSum = 0.0f;
            float sum = simd::selectedKernel()(phase, a.amp, a.ratio, b.amp, b.ratio, morph, baseInc, baseHz, modIn, nyquist, groups, ampSum);
            return ampSum > 0.0f ? sum / ampSum : 0.0f;
        }
        // phase distortion (rara): caminho escalar, um seno por parcial
        const bool morphing = morph > 0.0f;
        float sum = 0.0f, ampSum = 0.0f;
        for (int k = 0; k < groups * 8; ++k) {
            float amp = morphing ? a.amp[k] + (b.amp[k] - a.amp[k]) * morph : a.amp[k];
            float ratio = morphing ? a.ratio[k] + (b.ratio[k] - a.ratio[k]) * morph : a.ratio[k];
            float ph = phase[k] + ratio * baseInc;
            if (ph >= 1.0f) ph -= static_cast<float>(static_cast<int>(ph));
            phase[k] = ph;
            ampSum += amp;
            if (amp <= 0.0f || ratio * baseHz >= nyquist) continue;
            float p = ph + ratio * modIn;
            if (p >= 1.0f || p < 0.0f) p = wrap01(p);
            p = phaseDistort(p, phaseDistortion);
            sum += amp * fastSin01(p);
        }
        return ampSum > 0.0f ? sum / ampSum : 0.0f;
    }
};

// Filtro State-Variable "Topology-Preserving Transform" (zero-delay feedback,
// forma de Andrew Simper). Estavel sob modulacao rapida de corte.
class TPTStateVariableFilter {
public:
    void setSampleRate(double sr) { sampleRate_ = sr; lastCutoff_ = -1.0f; }

    void setParams(float cutoffHz, float q) {
        if (cutoffHz == lastCutoff_ && q == lastQ_) return; // sustain do fxEG: nada mudou, poupa o tan()
        lastCutoff_ = cutoffHz;
        lastQ_ = q;
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

    void reset() { ic1eq_ = 0.0f; ic2eq_ = 0.0f; }

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
    float lastCutoff_ = -1.0f, lastQ_ = -1.0f;
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

// [ADICIONADO em 2026-09-23] Wavefolder triangular: identidade exata em
// [-1, 1]; fora disso o sinal "rebate" nas paredes +-1 em vez de ser
// cortado (clip) ou arredondado (softSaturate/tanh). Cada dobra adiciona
// harmonicos novos -- quanto mais drive, mais dobras e mais brilho
// metalico (estilo Buchla/"West Coast"). Continua e limitada a [-1, 1]
// pra QUALQUER entrada finita, entao nunca estoura por si so.
inline float triangleFold(float v) {
    if (std::fabs(v) <= 1.0f) return v; // identidade EXATA (a formula abaixo erra 1 ulp aqui)
    // periodo 4: v=-1 -> -1, 0 -> 0, 1 -> 1, 2 -> 0, 3 -> -1, ...
    float t = (v + 1.0f) * 0.25f;
    t -= std::floor(t);
    return 1.0f - 4.0f * std::fabs(t - 0.5f);
}

// Passa-altas de 1 polo (~10Hz) pra tirar o DC que o wavefolder com
// simetria (bias != 0) introduz -- sem isso uma portadora dobrada com bias
// empurraria a saida inteira pra um lado.
class DcBlocker {
public:
    void setSampleRate(double sr) { r_ = 1.0f - static_cast<float>(kTwoPi * 10.0 / sr); }
    void reset() { x1_ = 0.0f; y1_ = 0.0f; }
    float process(float x) {
        float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float r_ = 0.9987f;
    float x1_ = 0.0f, y1_ = 0.0f;
};

// Reducao de sample rate (sample & hold) + reducao de bits.
class Crusher {
public:
    void setSampleRate(double sr) { sampleRate_ = sr; }
    void reset() { timer_ = 0.0f; held_ = 0.0f; }

    float process(float x, float crushSampleHz, float bits) {
        crushSampleHz = std::max(1.0f, crushSampleHz);
        float holdTime = 1.0f / crushSampleHz;
        timer_ += static_cast<float>(1.0 / sampleRate_);
        if (timer_ >= holdTime) {
            timer_ -= holdTime;
            held_ = x;
        }
        // teto de 24 bits: acima de ~127, 2^bits estoura float (inf) e
        // round(held*inf)/inf vira NaN -- que contaminava o master bus inteiro.
        if (bits != lastBits_) { lastBits_ = bits; halfLevels_ = 0.5f * std::exp2(std::clamp(bits, 1.0f, 24.0f)); }
        return std::round(held_ * halfLevels_) / halfLevels_;
    }

private:
    double sampleRate_ = 48000.0;
    float timer_ = 0.0f;
    float held_ = 0.0f;
    float lastBits_ = -1.0f, halfLevels_ = 1.0f;
};

// Delay curto com feedback e interpolacao linear (buffer de 50ms).
class CombFilter {
public:
    void setSampleRate(double sr) {
        sampleRate_ = sr;
        buffer_.assign(static_cast<size_t>(sr * 0.05) + 4, 0.0f);
        writePos_ = 0;
    }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
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
    void reset() { phase_ = 0.0f; apState_ = 0.0f; apDelay_ = 0.0f; }

    float process(float x, float shiftHz) {
        const float a = 0.6f;
        float apOut = a * (x - apState_) + apDelay_;
        apDelay_ = x;
        apState_ = apOut;

        phase_ += kTwoPi * shiftHz / static_cast<float>(sampleRate_);
        phase_ -= kTwoPi * std::floor((phase_ + kPi) / kTwoPi); // volta pra [-pi, pi) mesmo com shift > sampleRate

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
