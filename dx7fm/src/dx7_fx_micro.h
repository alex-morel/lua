#pragma once
// =============================================================================
// [ADICIONADO em 2026-10-29] "Micro": efeito granular/de micro-loops no estilo
// do Hologram Microcosm.
//
// Motor: porta do granular do Mutable Instruments Clouds (Emilie Gillet,
// licenca MIT -- clouds/dsp/grain.h, granular_sample_player.h, fx/diffuser.h e
// as tabelas de clouds/resources/lookup_tables.py): grao com janela
// triangular/trapezoidal que vira cosseno levantado conforme a "forma",
// semeadura deterministica, posicao contada pra tras a partir da cabeca de
// gravacao descontando o que a reproducao vai comer no pitch escolhido,
// normalizacao por 1/sqrt(graos ativos) e o difusor de 4 all-pass por lado.
// Mudancas: buffer float (o Clouds usa 16/8 bits), qualquer taxa (o Clouds
// roda fixo a 32 kHz: tamanhos escalados), interpolacao linear, sem as
// qualidades low/medium.
//
// Por cima, implementacao propria: um buffer de captura continuo (6 s) que
// congela no Hold, "Repeticoes" realimentando a saida no buffer, filtro de
// inclinacao, o reverb Hall do Clouds (CloudsHall, dx7_globalfx.h) como
// "Espaco", e 11 VARIACOES em 4 familias -- cada uma e uma politica de
// agendamento de graos sobre o mesmo motor:
//   Micro-loop: Mosaic (loops curtos em camadas, oitavas acima/abaixo),
//               Seq (fatias tocadas em sequencia na grade do BPM),
//               Glide (loops sobrepostos com a altura deslizando);
//   Graos:      Haze (nuvem densa de graos pequenos), Tunnel (drone de graos
//               longos colado na entrada, uma oitava abaixo), Strum (rajada de
//               graos subindo um acorde a cada ataque);
//   Glitch:     Blocks (blocos repetidos na grade), Interrupt (passa a entrada
//               e de vez em quando gagueja o ultimo pedaco), Arp (graos em
//               arpejo na grade);
//   Multi-delay: Pattern (4 cabecas num padrao ritmico, pan alternado),
//               Warp (cabecas com a altura entortando).
// Nomes e algoritmos proprios -- inspirados no comportamento descrito do
// pedal, nao no codigo dele (que e fechado).
// =============================================================================
#include "dx7_dsp.h"
#include "dx7_globalfx.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

namespace dx7fm::micro {

enum Variation { Mosaic, Seq, Glide, Haze, Tunnel, Strum, Blocks, Interrupt, Arp, Pattern, Warp, kNumVariations };

// --- buffer de captura (estereo, circular, congelavel) --------------------------
class CaptureBuffer {
public:
    void allocate(int n) { size_ = n; for (auto& d : data_) d.assign(static_cast<size_t>(n), 0.0f); head_ = 0; }
    void clear() { for (auto& d : data_) std::fill(d.begin(), d.end(), 0.0f); head_ = 0; }
    void write(float l, float r) { data_[0][static_cast<size_t>(head_)] = l; data_[1][static_cast<size_t>(head_)] = r; if (++head_ >= size_) head_ = 0; }
    float read(int ch, double pos) const { // pos absoluto (qualquer inteiro; o modulo e feito aqui)
        const double fl = std::floor(pos);
        const float f = static_cast<float>(pos - fl);
        int i0 = static_cast<int>(static_cast<long long>(fl) % size_);
        if (i0 < 0) i0 += size_;
        const int i1 = i0 + 1 >= size_ ? 0 : i0 + 1;
        const auto& d = data_[ch];
        return d[static_cast<size_t>(i0)] + f * (d[static_cast<size_t>(i1)] - d[static_cast<size_t>(i0)]);
    }
    int size() const { return size_; }
    int head() const { return head_; }

private:
    std::vector<float> data_[2];
    int size_ = 1, head_ = 0;
};

// --- grao (clouds::Grain) ---------------------------------------------------------
class Grain {
public:
    bool active() const { return active_; }
    void stop() { active_ = false; }
    // start: posicao absoluta no buffer; width em amostras; rate = razao de altura
    void start(double startPos, int width, double rate, float windowShape, float gainL, float gainR) {
        first_ = startPos;
        width_ = std::max(width, 4);
        phase_ = 0.0;
        rate_ = rate;
        envPhase_ = 0.0f;
        envInc_ = 2.0f / static_cast<float>(width_);
        // como no Clouds, mas o ramo e decidido pela forma (>= 0.5 = janela
        // suave), nao por smoothness > 0: no original, com a forma EXATAMENTE
        // em 0.5 smoothness e slope dao 0 e o grao sai mudo
        smoothWindow_ = windowShape >= 0.5f;
        if (smoothWindow_) { smooth_ = (windowShape - 0.5f) * 2.0f; slope_ = 0.0f; }
        else { smooth_ = 0.0f; slope_ = 0.5f / (windowShape + 0.01f); }
        gainL_ = gainL;
        gainR_ = gainR;
        active_ = true;
    }
    // soma uma amostra em l/r (o "OverlapAdd" do Clouds, uma amostra por vez)
    void tick(const CaptureBuffer& buf, float& l, float& r) {
        if (!active_) return;
        float gain = envPhase_;
        gain = gain >= 1.0f ? 2.0f - gain : gain;
        if (smoothWindow_) {
            const float window = 0.5f - 0.5f * std::cos(static_cast<float>(kPi) * gain); // lut_window: 1 - (cos(pi t) + 1) / 2
            gain += smooth_ * (window - gain);
        } else {
            gain = std::min(1.0f, gain * slope_);
        }
        envPhase_ += envInc_;
        if (envPhase_ >= 2.0f) { active_ = false; return; }
        const double pos = first_ + phase_;
        const float sl = buf.read(0, pos) * gain, sr = buf.read(1, pos) * gain;
        l += sl * gainL_ + sr * (1.0f - gainR_);
        r += sr * gainR_ + sl * (1.0f - gainL_);
        phase_ += rate_;
    }

private:
    double first_ = 0.0, phase_ = 0.0, rate_ = 1.0;
    int width_ = 4;
    float envPhase_ = 2.0f, envInc_ = 0.0f, smooth_ = 0.0f, slope_ = 1.0f, gainL_ = 1.0f, gainR_ = 1.0f;
    bool active_ = false, smoothWindow_ = false;
};

// --- difusor (clouds::Diffuser): 4 all-pass por lado, g = 0.625 --------------------
class Diffuser {
public:
    void prepare(double sr) {
        static const int kLen[8] = {126, 180, 269, 444, 151, 205, 245, 405}; // amostras a 32 kHz
        for (int i = 0; i < 8; ++i) ap_[i].assign(static_cast<size_t>(std::max(2, static_cast<int>(kLen[i] * sr / 32000.0))), 0.0f);
        clear();
    }
    void clear() { for (auto& a : ap_) std::fill(a.begin(), a.end(), 0.0f); for (int& p : pos_) p = 0; }
    void process(float amount, float& l, float& r) {
        auto chain = [&](int base, float x) {
            for (int k = 0; k < 4; ++k) {
                auto& b = ap_[base + k];
                int& p = pos_[base + k];
                const float d = b[static_cast<size_t>(p)];
                const float v = x + kAp * d;
                b[static_cast<size_t>(p)] = v;
                if (++p >= static_cast<int>(b.size())) p = 0;
                x = d - kAp * v;
            }
            return x;
        };
        const float wl = chain(0, l), wr = chain(4, r);
        l += amount * (wl - l);
        r += amount * (wr - r);
    }

private:
    static constexpr float kAp = 0.625f;
    std::vector<float> ap_[8];
    int pos_[8] = {};
};

// --- o efeito -------------------------------------------------------------------------
struct MicroParams {
    int variation = Mosaic;
    float activity = 0.5f, shape = 0.5f, repeats = 0.3f;
    float seconds = 0.25f;      // tempo base (loop, passo, espacamento das cabecas)
    float semitones = 0.0f;     // altura geral
    float spread = 0.6f, filter = 0.0f, space = 0.25f, variationAmt = 0.5f;
    bool hold = false;
};

class Micro {
public:
    static constexpr int kMaxGrains = 48;

    void prepare(double sr) {
        sr_ = sr;
        buf_.allocate(static_cast<int>(sr * 6.0));
        diffuser_.prepare(sr);
        space_.setSampleRate(sr);
        for (auto& f : filt_) f.setSampleRate(sr);
        reset();
    }
    void reset() {
        buf_.clear();
        diffuser_.clear();
        space_.clear();
        for (auto& g : grains_) g.stop();
        for (auto& f : filt_) f.reset();
        step_ = 0;
        seedPhasor_ = 0.0f;
        numGrains_ = 0.0f;
        norm_ = 1.0f;
        fbL_ = fbR_ = 0.0f;
        envFast_ = envSlow_ = 0.0f;
        onsetHold_ = 0;
        strumLeft_ = 0;
        stutterLeft_ = 0;
        for (auto& s : seqSlices_) s = 0.0;
    }

    // n amostras no lugar; beatPos/bpm pra grade (Seq, Blocks, Arp, Pattern)
    void process(const MicroParams& p, float* L, float* R, int n, double beatPos, float bpm) {
        const double spb = 60.0 / std::max(1.0f, bpm) * sr_; // amostras por batida
        const float baseRate = std::exp2(p.semitones / 12.0f);
        const float sizeSamp = std::clamp(p.seconds, 0.02f, 2.0f) * static_cast<float>(sr_);
        // filtro de inclinacao: < 0 passa-baixa, > 0 passa-altas
        const bool lp = p.filter < 0.0f;
        const float fc = lp ? 20000.0f * std::exp2(p.filter * 7.0f) : 20.0f * std::exp2(p.filter * 9.0f);
        for (auto& f : filt_) f.setParams(std::clamp(fc, 20.0f, 0.45f * static_cast<float>(sr_)), 0.7f);
        float spaceWetL = 0.0f, spaceWetR = 0.0f;

        for (int i = 0; i < n; ++i) {
            // grava a entrada (+ repeticoes) -- o Hold congela o buffer
            if (!p.hold) buf_.write(L[i] + p.repeats * fbL_, R[i] + p.repeats * fbR_);
            const double beat = beatPos + i / spb;
            schedule(p, beat, spb, sizeSamp, baseRate, L[i], R[i]);

            float l = 0.0f, r = 0.0f;
            int active = 0;
            for (auto& g : grains_) if (g.active()) { g.tick(buf_, l, r); ++active; }
            // normalizacao do Clouds: 1/sqrt(graos - 1), suavizada
            numGrains_ += (static_cast<float>(active) - numGrains_) * (static_cast<float>(active) > numGrains_ ? 0.9f : 0.2f) * 0.01f;
            const float target = (numGrains_ > 2.0f ? 1.0f / std::sqrt(numGrains_ - 1.0f) : 1.0f) * (1.0f + p.shape);
            norm_ += (target - norm_) * 0.01f;
            l *= norm_;
            r *= norm_;
            if (p.variation == Interrupt && stutterLeft_ == 0) { l += L[i]; r += R[i]; } // fora da gagueira: a entrada passa
            diffuser_.process(0.4f * p.activity * (p.variation == Haze || p.variation == Tunnel ? 1.0f : 0.5f), l, r);
            l = lp ? filt_[0].process(l).lp : filt_[0].process(l).hp;
            r = lp ? filt_[1].process(r).lp : filt_[1].process(r).hp;
            if (!std::isfinite(l) || !std::isfinite(r)) { l = r = 0.0f; for (auto& f : filt_) f.reset(); }
            fbL_ = std::tanh(l);
            fbR_ = std::tanh(r);
            // espaco: reverb Hall do Clouds
            if (p.space > 0.0f) {
                space_.process(l, r, 0.75f, 0.35f, spaceWetL, spaceWetR);
                l += p.space * spaceWetL;
                r += p.space * spaceWetR;
            }
            L[i] = l;
            R[i] = r;
        }
    }

private:
    Grain* freeGrain() {
        for (auto& g : grains_) if (!g.active()) return &g;
        return nullptr;
    }
    float rnd() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }
    // grao de "width" amostras comecando "back" amostras atras da cabeca, na altura rate
    void launch(double back, float width, float rate, float shape, float spread) {
        Grain* g = freeGrain();
        if (!g) return;
        const float maxW = static_cast<float>(buf_.size()) * 0.4f / std::max(1.0f, rate);
        width = std::clamp(width, 32.0f, maxW);
        const double eaten = rate > 1.0f ? width * rate : 0.0; // a reproducao nao pode passar a gravacao
        const double start = static_cast<double>(buf_.head()) - back - eaten - 1.0;
        const float pan = 0.5f + spread * (rnd() - 0.5f);
        const float gl = pan < 0.5f ? 1.0f : 2.0f * (1.0f - pan), gr = pan < 0.5f ? 2.0f * pan : 1.0f;
        g->start(start, static_cast<int>(width), rate, shape, gl, gr);
    }
    // um passo da grade de `div` batidas: true na amostra em que o passo muda
    bool gridStep(double beat, double div) {
        const long s = static_cast<long>(std::floor(beat / div));
        if (s != lastGridStep_) { lastGridStep_ = s; ++step_; return true; }
        return false;
    }

    void schedule(const MicroParams& p, double beat, double spb, float size, float baseRate, float inL, float inR) {
        const float act = std::clamp(p.activity, 0.0f, 1.0f), var = std::clamp(p.variationAmt, 0.0f, 1.0f);
        // detector de ataque (Strum)
        const float a = std::max(std::fabs(inL), std::fabs(inR));
        envFast_ += (a - envFast_) * (a > envFast_ ? 0.3f : 0.002f);
        envSlow_ += (a - envSlow_) * 0.0005f;
        if (onsetHold_ > 0) --onsetHold_;
        const bool onset = onsetHold_ == 0 && envFast_ > 0.02f && envFast_ > 2.0f * envSlow_;
        if (onset) onsetHold_ = static_cast<int>(0.08 * sr_);
        // "densidade": graos por segundo (deterministico, como o Clouds com use_deterministic_seed)
        auto every = [&](float perSecond) {
            seedPhasor_ += perSecond / static_cast<float>(sr_);
            if (seedPhasor_ >= 1.0f) { seedPhasor_ -= 1.0f; return true; }
            return false;
        };
        const int v = std::clamp(p.variation, 0, kNumVariations - 1);
        switch (v) {
            case Mosaic: { // camadas de micro-loops (1..8 sobrepostos), +-oitava com chance "variacao"
                if (every((1.0f + 7.0f * act) * static_cast<float>(sr_) / size)) {
                    float rate = baseRate;
                    const float u = rnd();
                    if (u < 0.35f * var) rate *= 2.0f;
                    else if (u < 0.5f * var) rate *= 0.5f;
                    launch(rnd() * size * 2.0, size * (0.5f + 0.5f * rnd()), rate, p.shape, p.spread);
                }
                break;
            }
            case Seq: { // fatias do ultimo compasso tocadas em sequencia na grade (1/16 escalado pela atividade)
                const double div = act > 0.66f ? 0.25 : act > 0.33f ? 0.5 : 1.0;
                if (gridStep(beat, div)) {
                    const int k = step_ % 8;
                    if (k == 0 || rnd() < 0.2f * var) for (auto& s : seqSlices_) s = rnd() * 4.0; // embaralha as fatias
                    const double sliceLen = div * spb;
                    launch(seqSlices_[k] * sliceLen, static_cast<float>(sliceLen), baseRate, std::min(p.shape, 0.4f), p.spread);
                }
                break;
            }
            case Glide: { // loops sobrepostos com a altura deslizando num LFO lento
                glidePh_ += 0.15 / sr_;
                glidePh_ -= std::floor(glidePh_);
                if (every(4.0f * (1.0f + 3.0f * act) * static_cast<float>(sr_) / size)) {
                    const float semis = var * 12.0f * static_cast<float>(std::sin(2.0 * kPi * glidePh_));
                    launch(rnd() * size, size, baseRate * std::exp2(semis / 12.0f), std::max(p.shape, 0.6f), p.spread);
                }
                break;
            }
            case Haze: { // nuvem: graos pequenos, densos, de qualquer lugar do ultimo segundo, levemente desafinados
                if (every(10.0f + 140.0f * act)) {
                    const float det = (rnd() - 0.5f) * 0.6f * var;
                    launch(rnd() * sr_ * 1.0, std::min(size, 0.15f * static_cast<float>(sr_)) * (0.4f + 0.6f * rnd()), baseRate * std::exp2(det / 12.0f),
                           std::max(p.shape, 0.5f), p.spread);
                }
                break;
            }
            case Tunnel: { // drone colado na entrada: graos longos do que acabou de entrar, uma oitava abaixo
                if (every((3.0f + 9.0f * act) * static_cast<float>(sr_) / std::max(size, 0.2f * static_cast<float>(sr_)))) {
                    const float rate = baseRate * (rnd() < 0.5f + 0.5f * var ? 0.5f : 1.0f);
                    launch(rnd() * 0.05 * sr_, std::max(size, 0.2f * static_cast<float>(sr_)), rate, 0.9f, p.spread);
                }
                break;
            }
            case Strum: { // a cada ataque, uma rajada de graos subindo um acorde
                static const float kChord[8] = {0, 4, 7, 12, 16, 19, 24, 28};
                if (onset) { strumLeft_ = 2 + static_cast<int>(6.0f * act); strumNext_ = 0; strumIdx_ = 0; }
                if (strumLeft_ > 0 && --strumNext_ <= 0) {
                    const float semis = kChord[strumIdx_ % 8] * (0.5f + 0.5f * var);
                    launch(0.03 * sr_, std::min(size, 0.4f * static_cast<float>(sr_)), baseRate * std::exp2(semis / 12.0f), p.shape, p.spread);
                    ++strumIdx_;
                    --strumLeft_;
                    strumNext_ = static_cast<int>(size / 6.0f);
                }
                break;
            }
            case Blocks: { // a cada passo, um bloco do passado repetido 1..4 vezes
                const double div = act > 0.5f ? 0.25 : 0.5;
                if (gridStep(beat, div)) {
                    const double len = div * spb;
                    if (blockRepeats_ <= 0) { blockBack_ = (1 + static_cast<int>(rnd() * 8.0f)) * len; blockRepeats_ = 1 + static_cast<int>(rnd() * (1.0f + 3.0f * var)); }
                    launch(blockBack_, static_cast<float>(len), baseRate, std::min(p.shape, 0.2f), p.spread);
                    blockBack_ += len; // o mesmo bloco de novo (a cabeca andou um passo)
                    --blockRepeats_;
                }
                break;
            }
            case Interrupt: { // a entrada passa; de vez em quando gagueja o ultimo pedaco
                if (stutterLeft_ > 0) {
                    --stutterLeft_;
                    if (--stutterNext_ <= 0) { launch(stutterBack_, stutterLen_, baseRate, 0.1f, 0.0f); stutterBack_ += stutterLen_; stutterNext_ = static_cast<int>(stutterLen_); }
                } else if (every(0.5f + 3.0f * act)) {
                    stutterLen_ = std::max(0.02f, size * (0.15f + 0.35f * rnd())) ;
                    stutterBack_ = 0.0;
                    stutterNext_ = 0;
                    stutterLeft_ = static_cast<int>(stutterLen_ * (2 + static_cast<int>(rnd() * (2.0f + 4.0f * var))));
                }
                break;
            }
            case Arp: { // graos em arpejo na grade
                static const float kMaj[6] = {0, 4, 7, 12, 7, 4}, kMin[6] = {0, 3, 7, 12, 7, 3};
                const double div = act > 0.5f ? 0.25 : 0.5;
                if (gridStep(beat, div)) {
                    const float semis = (var > 0.5f ? kMin : kMaj)[step_ % 6];
                    launch(0.02 * sr_, static_cast<float>(div * spb) * 0.9f, baseRate * std::exp2(semis / 12.0f), p.shape, p.spread);
                }
                break;
            }
            case Pattern:
            case Warp: { // 4 cabecas num padrao ritmico (fracoes do tempo); Warp entorta a altura de cada uma
                static const float kPat[4][4] = {{0.25f, 0.5f, 0.75f, 1.0f}, {0.375f, 0.5f, 0.875f, 1.0f}, {0.25f, 0.625f, 0.75f, 1.0f}, {0.125f, 0.375f, 0.625f, 1.0f}};
                const int pat = std::clamp(static_cast<int>(var * 4.0f), 0, 3);
                if (every(static_cast<float>(sr_) / std::max(64.0f, size * 0.125f))) { // cabecas "lidas" em graos que se sobrepoem
                    const int h = static_cast<int>(rnd() * 4.0f) % 4;
                    if (rnd() < 0.4f + 0.6f * act) {
                        float rate = baseRate;
                        if (v == Warp) rate *= std::exp2(((rnd() - 0.5f) * 2.0f * var) / 12.0f);
                        launch(kPat[pat][h] * size * 2.0, size * 0.25f, rate, 0.8f, std::min(1.0f, p.spread + 0.3f));
                    }
                }
                break;
            }
        }
    }

    double sr_ = 48000.0, glidePh_ = 0.0, blockBack_ = 0.0, stutterBack_ = 0.0;
    CaptureBuffer buf_;
    Grain grains_[kMaxGrains];
    Diffuser diffuser_;
    CloudsHall space_;
    TPTStateVariableFilter filt_[2];
    std::minstd_rand rng_{newRandomSeed()};
    float seedPhasor_ = 0.0f, numGrains_ = 0.0f, norm_ = 1.0f, fbL_ = 0.0f, fbR_ = 0.0f, envFast_ = 0.0f, envSlow_ = 0.0f, stutterLen_ = 0.0f;
    int step_ = 0, onsetHold_ = 0, strumLeft_ = 0, strumNext_ = 0, strumIdx_ = 0, blockRepeats_ = 0, stutterLeft_ = 0, stutterNext_ = 0;
    long lastGridStep_ = -1;
    double seqSlices_[8] = {};
};

} // namespace dx7fm::micro
