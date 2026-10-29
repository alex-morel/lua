#pragma once
// =============================================================================
// Simulador de fita -- porta do CHOW Tape Model (Jatin Chowdhury,
// github.com/jatinchowdhury18/AnalogTapeModel, GPLv3), a partir da versao sem
// JUCE que o Surge XT mantem (src/common/dsp/effects/chowdsp/tape, GPLv3) e,
// pro wow/flutter (que o Surge nao tem), dos Timing_Effects do original
// (WowProcess, FlutterProcess, OHProcess -- este derivado dos
// ZetaCarinaeModules de Michael Hampton, GPLv3). Modelo descrito em
// J. Chowdhury, "Real-time Physical Modelling for Analog Tape Machines",
// DAFx 2019.
//
// Cadeia (a do plugin original): tom de entrada -> histerese magnetica
// (Jiles-Atherton, solver RK4, 2x oversampling) -> tom de saida (o inverso)
// -> chew (fita amassada) -> degrade (ruido + passa-baixa + ganho) ->
// wow/flutter (atraso modulado) -> perdas da cabeca (FIR + head bump).
//
// O DSP dos modelos e o do original; o que mudou na porta:
//  - so o caminho escalar em double da histerese (sem SSE);
//  - infraestrutura trocada por equivalentes pequenos: SmoothedValue
//    (linear/multiplicativo, como o do JUCE), oversampling 2x com os
//    half-band do dx7fm, biquads RBJ, atraso com interpolacao de Lagrange 3a
//    ordem, ruido normal/uniforme de std:: com semente de newRandomSeed();
//  - blocos de tamanho variavel (o Surge assume BLOCK_SIZE fixo);
//  - robustez: a checagem de instabilidade da histerese vale pros 2 lados
//    (|M| > limite; o original so olha M > limite e uma explosao negativa
//    passava) e um 2*tanh(x/2) depois da histerese segura picos extremos;
//  - 3 correcoes, conferidas com o original: (1) os coeficientes do FIR de
//    perdas sao zerados antes da DFT inversa (a soma acumulava a cada
//    recalculo); (2) head bump com ganho /1000 e Q 2 como no original (o
//    Surge usa /100 e daria +23 dB em 150 Hz); (3) o ganho do degrade e
//    -24 dB x depth + variacao, como no original (o Surge perdia o termo).
// =============================================================================
#include "../dx7_dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace dx7fm::chowtape {

constexpr double kPiD = 3.14159265358979323846;

// --- infraestrutura -----------------------------------------------------------
enum class Smoothing { Linear, Multiplicative };

template <Smoothing kind> class Smoothed {
public:
    void reset(double sampleRate, double seconds) { reset(static_cast<int>(std::floor(seconds * sampleRate))); }
    void reset(int steps) { steps_ = std::max(steps, 0); setCurrentAndTarget(target_); }
    void setCurrentAndTarget(float v) { current_ = target_ = v; countdown_ = 0; }
    void setTarget(float v) {
        if (v == target_) return;
        if (steps_ <= 0) { setCurrentAndTarget(v); return; }
        target_ = v;
        countdown_ = steps_;
        if (kind == Smoothing::Linear) step_ = (target_ - current_) / static_cast<float>(countdown_);
        else step_ = std::exp((std::log(std::abs(target_)) - std::log(std::abs(current_))) / static_cast<float>(countdown_));
    }
    float target() const { return target_; }
    bool smoothing() const { return countdown_ > 0; }
    float next() {
        if (!smoothing()) return target_;
        --countdown_;
        if (countdown_ == 0) current_ = target_;
        else if (kind == Smoothing::Linear) current_ += step_;
        else current_ *= step_;
        return current_;
    }

private:
    float current_ = (kind == Smoothing::Multiplicative) ? 1.0f : 0.0f, target_ = current_, step_ = 0.0f;
    int steps_ = 0, countdown_ = 0;
};

// biquad RBJ (forma direta II transposta) em double
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    float process(float xf) {
        const double x = xf, y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
    void highpass(double fc, double q, double fs) {
        const double w = 2 * kPiD * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = (1 + c) / 2 / a0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    void peak(double fc, double q, double gainLinear, double fs) {
        const double A = std::sqrt(gainLinear), w = 2 * kPiD * std::min(fc, 0.45 * fs) / fs, c = std::cos(w), al = std::sin(w) / (2 * q);
        const double a0 = 1 + al / A;
        b0 = (1 + al * A) / a0; b1 = -2 * c / a0; b2 = (1 - al * A) / a0; a1 = -2 * c / a0; a2 = (1 - al / A) / a0;
    }
};

// 2x: zera-e-filtra pra subir (o mesmo half-band do decimador, ganho 2), decimador do dx7fm pra descer
class Oversampler2x {
public:
    void reset() { for (auto& d : down_) d = HalfbandDecimator2x{}; for (auto& u : upHist_) for (float& x : u) x = 0.0f; upPos_[0] = upPos_[1] = 0; }
    // 1 amostra -> 2 (filtro polifasico: os taps pares do half-band sao 0 menos o central)
    void up(int ch, float x, float& o0, float& o1) {
        float* h = upHist_[ch];
        int& pos = upPos_[ch];
        h[pos] = x;
        pos = (pos + 1) % kHist;
        // sinal com zeros intercalados: na fase PAR so os taps laterais (indices
        // pares) pegam amostras, na IMPAR so o central (kCenter e impar)
        const int center = kCenter / 2;
        double acc = 0.0;
        for (int m = 0; m < kSide; ++m) acc += kDec.sideCoef(m) * (static_cast<double>(at(h, pos, center - m)) + at(h, pos, center + m + 1));
        o0 = static_cast<float>(2.0 * acc);
        o1 = static_cast<float>(2.0 * kDec.centerCoef() * at(h, pos, center));
    }
    bool down(int ch, float x, float& out) { return down_[ch].process(x, out); }

private:
    static constexpr int kCenter = HalfbandDecimator2x::kCenter, kSide = HalfbandDecimator2x::kSide, kHist = 64;
    static float at(const float* h, int pos, int back) { return h[((pos - 1 - back) % kHist + kHist) % kHist]; }
    static inline const HalfbandDecimator2x kDec{};
    float upHist_[2][kHist] = {};
    int upPos_[2] = {0, 0};
    HalfbandDecimator2x down_[2];
};

// --- histerese (HysteresisOps.h + HysteresisProcessing, caminho escalar) ------
struct HysteresisState {
    double M_s = 1.0, a = M_s / 4.0;
    static constexpr double alpha = 1.6e-3;
    double k = 0.47875, c = 1.7e-1;
    double nc = 1 - c, M_s_oa = M_s / a, M_s_oa_talpha = alpha * M_s / a, M_s_oa_tc = c * M_s / a;
    double M_s_oa_tc_talpha = alpha * c * M_s / a, M_s_oaSq_tc_talpha = alpha * c * M_s / (a * a);
    double M_s_oaSq_tc_talphaSq = alpha * alpha * c * M_s / (a * a);
    double Q = 0, M_diff = 0, L_prime = 0, kap1 = 0, f1Denom = 0, f1 = 0, f2 = 0, f3 = 0, coth = 0.0;
    bool nearZero = false;
};

inline int signOf(double x) { return int(x > 0.0) - int(x < 0.0); }
inline double langevin(double x, double coth, bool nearZero) { return !nearZero ? coth - 1.0 / x : x / 3.0; }
inline double langevinD(double x, double coth, bool nearZero) { return !nearZero ? 1.0 / (x * x) - coth * coth + 1.0 : 1.0 / 3.0; }
inline double deriv(double x_n, double x_n1, double x_d_n1, double T) {
    constexpr double dAlpha = 0.75;
    return ((1.0 + dAlpha) / T) * (x_n - x_n1) - dAlpha * x_d_n1;
}
inline double hysteresisFunc(double M, double H, double H_d, HysteresisState& hp) {
    hp.Q = (H + M * hp.alpha) * (1.0 / hp.a);
    hp.coth = 1.0 / std::tanh(hp.Q);
    hp.nearZero = hp.Q < 0.001 && hp.Q > -0.001;
    hp.M_diff = langevin(hp.Q, hp.coth, hp.nearZero) * hp.M_s - M;
    const double delta = static_cast<double>((H_d >= 0.0) - (H_d < 0.0));
    const double delta_M = static_cast<double>(signOf(delta) == signOf(hp.M_diff));
    hp.kap1 = hp.nc * delta_M;
    hp.L_prime = langevinD(hp.Q, hp.coth, hp.nearZero);
    hp.f1Denom = (hp.nc * delta) * hp.k - hp.alpha * hp.M_diff;
    hp.f1 = hp.kap1 * hp.M_diff / hp.f1Denom;
    hp.f2 = hp.L_prime * hp.M_s_oa_tc;
    hp.f3 = 1.0 - hp.L_prime * hp.M_s_oa_tc_talpha;
    return H_d * (hp.f1 + hp.f2) / hp.f3;
}

class Hysteresis {
public:
    void setSampleRate(double fs) { T_ = 1.0 / fs; }
    void reset() { M_n1_ = H_n1_ = H_d_n1_ = 0.0; hp_.coth = 0.0; hp_.nearZero = false; }
    void cook(float drive, float width, float sat) {
        hp_.M_s = 0.5 + 1.5 * (1.0 - static_cast<double>(sat));
        hp_.a = hp_.M_s / (0.01 + 6.0 * static_cast<double>(drive));
        hp_.c = std::sqrt(1.0 - static_cast<double>(width)) - 0.01;
        hp_.k = 0.47875;
        upperLim_ = 20.0;
        hp_.nc = 1.0 - hp_.c;
        hp_.M_s_oa = hp_.M_s / hp_.a;
        hp_.M_s_oa_talpha = hp_.alpha * hp_.M_s_oa;
        hp_.M_s_oa_tc = hp_.c * hp_.M_s_oa;
        hp_.M_s_oa_tc_talpha = hp_.alpha * hp_.M_s_oa_tc;
        hp_.M_s_oaSq_tc_talpha = hp_.M_s_oa_tc_talpha / hp_.a;
        hp_.M_s_oaSq_tc_talphaSq = hp_.alpha * hp_.M_s_oaSq_tc_talpha;
    }
    // RK4 (o solver padrao do Surge)
    double process(double H) {
        double H_d = deriv(H, H_n1_, H_d_n1_, T_);
        const double H_1_2 = (H + H_n1_) * 0.5, H_d_1_2 = (H_d + H_d_n1_) * 0.5;
        const double k1 = hysteresisFunc(M_n1_, H_n1_, H_d_n1_, hp_) * T_;
        const double k2 = hysteresisFunc(M_n1_ + k1 * 0.5, H_1_2, H_d_1_2, hp_) * T_;
        const double k3 = hysteresisFunc(M_n1_ + k2 * 0.5, H_1_2, H_d_1_2, hp_) * T_;
        const double k4 = hysteresisFunc(M_n1_ + k3, H, H_d, hp_) * T_;
        double M = M_n1_ + k1 / 6.0 + k2 / 3.0 + k3 / 3.0 + k4 / 6.0;
        const bool ill = std::isnan(M) || std::fabs(M) > upperLim_; // instabilidade (o original so olha M > limite; aqui os 2 lados)
        if (ill) { M = 0.0; H_d = 0.0; }
        M_n1_ = M;
        H_n1_ = H;
        H_d_n1_ = H_d;
        return M;
    }

private:
    double T_ = 1.0 / 96000.0, upperLim_ = 20.0;
    double M_n1_ = 0.0, H_n1_ = 0.0, H_d_n1_ = 0.0;
    HysteresisState hp_;
};

// HysteresisProcessor: suavizacao em 500 passos, 2x, makeup, DC blocker 35 Hz
class HysteresisProcessor {
public:
    void reset(double fs) {
        drive_.reset(kSteps); width_.reset(kSteps); sat_.reset(kSteps); makeup_.reset(kSteps);
        os_.reset();
        dc_[0].highpass(35.0, 0.707, fs); dc_[1] = dc_[0];
        dc_[0].reset(); dc_[1].reset();
        for (auto& h : h_) { h.setSampleRate(2.0 * fs); h.reset(); }
        cooked_ = false;
    }
    void setParams(float drive, float sat, float bias) {
        const float width = 1.0f - bias, mk = (1.0f + 0.6f * width) / (0.5f + 1.5f * (1.0f - sat));
        if (!cooked_) { // 1a vez depois do reset: direto nos valores, sem rampa de 500 amostras vindo do zero
            drive_.setCurrentAndTarget(drive); sat_.setCurrentAndTarget(sat); width_.setCurrentAndTarget(width); makeup_.setCurrentAndTarget(mk);
            return;
        }
        drive_.setTarget(drive);
        sat_.setTarget(sat);
        width_.setTarget(width);
        makeup_.setTarget(mk);
    }
    void process(float* L, float* R, int n) {
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            float up[2][2];
            os_.up(0, L[i], up[0][0], up[0][1]);
            os_.up(1, R[i], up[1][0], up[1][1]);
            for (int k = 0; k < 2; ++k) {
                const bool smooth = drive_.smoothing() || width_.smoothing() || sat_.smoothing() || !cooked_;
                const float mk = makeup_.next();
                if (smooth) {
                    const float d = drive_.next(), w = width_.next(), s = sat_.next();
                    h_[0].cook(d, w, s);
                    h_[1].cook(d, w, s);
                    cooked_ = true;
                }
                for (int ch = 0; ch < 2; ++ch) up[ch][k] = static_cast<float>(h_[ch].process(up[ch][k]) * mk);
            }
            for (int ch = 0; ch < 2; ++ch) {
                float out = 0.0f;
                os_.down(ch, up[ch][0], out);
                os_.down(ch, up[ch][1], out);
                io[ch][i] = dc_[ch].process(out);
            }
        }
    }

private:
    static constexpr int kSteps = 500;
    Smoothed<Smoothing::Linear> drive_, width_, sat_;
    Smoothed<Smoothing::Multiplicative> makeup_;
    Hysteresis h_[2];
    Oversampler2x os_;
    Biquad dc_[2];
    bool cooked_ = false;
};

// --- tom (ToneControl + ShelfFilter, Abel & Berners) ---------------------------
class Shelf {
public:
    void reset() { z_ = 0.0f; }
    void calc(float lowGain, float highGain, float fc, float fs) {
        if (lowGain == highGain) { b0_ = lowGain; b1_ = 0.0f; a1_ = 0.0f; return; }
        const float rho = std::sqrt(highGain / lowGain), K = 1.0f / std::tan(static_cast<float>(kPiD) * fc / fs);
        const float bs0 = highGain / rho, bs1 = lowGain, as0 = 1.0f / rho, as1 = 1.0f;
        const float a0 = as0 * K + as1;
        b0_ = (bs0 * K + bs1) / a0; b1_ = (-bs0 * K + bs1) / a0; a1_ = (-as0 * K + as1) / a0;
    }
    float process(float x) { const float y = b0_ * x + z_; z_ = b1_ * x - a1_ * y; return y; }

private:
    float b0_ = 1.0f, b1_ = 0.0f, a1_ = 0.0f, z_ = 0.0f;
};

class ToneStage {
public:
    void prepare(double fs) {
        fs_ = static_cast<float>(fs);
        for (int ch = 0; ch < 2; ++ch) {
            low_[ch].reset(fs, 0.05); high_[ch].reset(fs, 0.05); freq_[ch].reset(fs, 0.05);
            freq_[ch].setCurrentAndTarget(800.0f);
            tone_[ch].reset();
            tone_[ch].calc(low_[ch].target(), high_[ch].target(), freq_[ch].target(), fs_);
        }
    }
    void set(float lowDb, float highDb) {
        for (int ch = 0; ch < 2; ++ch) {
            low_[ch].setTarget(std::pow(10.0f, lowDb * 0.05f));
            high_[ch].setTarget(std::pow(10.0f, highDb * 0.05f));
        }
    }
    void process(float* L, float* R, int n) {
        float* io[2] = {L, R};
        for (int ch = 0; ch < 2; ++ch) {
            const bool sm = low_[ch].smoothing() || high_[ch].smoothing();
            for (int i = 0; i < n; ++i) {
                if (sm) tone_[ch].calc(low_[ch].next(), high_[ch].next(), freq_[ch].next(), fs_);
                io[ch][i] = tone_[ch].process(io[ch][i]);
            }
            if (sm && !low_[ch].smoothing()) tone_[ch].calc(low_[ch].target(), high_[ch].target(), freq_[ch].target(), fs_);
        }
    }

private:
    float fs_ = 48000.0f;
    Shelf tone_[2];
    Smoothed<Smoothing::Multiplicative> low_[2], high_[2], freq_[2];
};

// --- perdas da cabeca (LossFilter) --------------------------------------------
class LossFilter {
public:
    void prepare(float fs) {
        fs_ = fs;
        order_ = static_cast<int>(64 * (fs / 44100.0f));
        coefs_.assign(static_cast<size_t>(order_), 0.0f);
        H_.assign(static_cast<size_t>(order_), 0.0f);
        for (auto& f : fir_) { f.coefs.assign(static_cast<size_t>(order_), 0.0f); f.hist[0].assign(static_cast<size_t>(order_), 0.0f); f.hist[1].assign(static_cast<size_t>(order_), 0.0f); f.pos = 0; }
        calc();
        fir_[0].coefs = coefs_;
        fir_[1].coefs = coefs_;
        bump_[0].reset(); bump_[1].reset();
        prev_ = cur_;
        fade_ = 0;
        active_ = 0;
    }
    void setParams(float speed, float spacing, float gap, float thick) { cur_ = {speed, spacing, gap, thick}; }
    void process(float* L, float* R, int n) {
        if (cur_ != prev_ && fade_ == 0) { // parametro mudou: FIR novo no filtro inativo e crossfade
            calc();
            fir_[1 - active_].coefs = coefs_;
            fade_ = kFade;
            prev_ = cur_;
        }
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                const float x = io[ch][i];
                float y = fir_[active_].tick(ch, x);
                const float other = fir_[1 - active_].tick(ch, x); // o inativo segue andando (historico pronto)
                if (fade_ > 0) {
                    const float g = static_cast<float>(fade_) / kFade;
                    y = y * g + other * (1.0f - g);
                }
                io[ch][i] = bump_[ch].process(y);
            }
            if (fade_ > 0 && --fade_ == 0) active_ = 1 - active_;
        }
    }

private:
    struct Params {
        float speed = 30.0f, spacing = 0.1f, gap = 1.0f, thick = 0.1f;
        bool operator!=(const Params& o) const { return speed != o.speed || spacing != o.spacing || gap != o.gap || thick != o.thick; }
    };
    struct Fir {
        std::vector<float> coefs, hist[2];
        int pos = 0;
        float tick(int ch, float x) {
            auto& h = hist[ch];
            const int N = static_cast<int>(coefs.size());
            if (ch == 0) pos = (pos + 1) % N;
            h[static_cast<size_t>(pos)] = x;
            float acc = 0.0f;
            int j = pos;
            for (int k = 0; k < N; ++k) { acc += coefs[static_cast<size_t>(k)] * h[static_cast<size_t>(j)]; j = j == 0 ? N - 1 : j - 1; }
            return acc;
        }
    };
    void calc() {
        const float binWidth = fs_ / static_cast<float>(order_);
        for (int k = 0; k < order_ / 2; k++) {
            const float freq = static_cast<float>(k) * binWidth;
            const float waveNumber = 2.0f * static_cast<float>(kPiD) * std::max(freq, 20.0f) / (cur_.speed * 0.0254f);
            const float thickTimesK = waveNumber * (cur_.thick * 1.0e-6f);
            const float kGapOverTwo = waveNumber * (cur_.gap * 1.0e-6f) / 2.0f;
            float h = std::exp(-waveNumber * (cur_.spacing * 1.0e-6f)); // perda por espacamento
            h *= (1.0f - std::exp(-thickTimesK)) / thickTimesK;             // espessura
            h *= std::sin(kGapOverTwo) / kGapOverTwo;                       // gap
            H_[static_cast<size_t>(k)] = h;
            H_[static_cast<size_t>(order_ - k - 1)] = h;
        }
        std::fill(coefs_.begin(), coefs_.end(), 0.0f); // correcao (1): ver o cabecalho
        for (int n = 0; n < order_ / 2; n++) {
            const size_t idx = static_cast<size_t>(order_ / 2 + n);
            for (int k = 0; k < order_; k++)
                coefs_[idx] += H_[static_cast<size_t>(k)] * std::cos(2.0f * static_cast<float>(kPiD) * static_cast<float>(k) * static_cast<float>(n) / static_cast<float>(order_));
            coefs_[idx] /= static_cast<float>(order_);
            coefs_[static_cast<size_t>(order_ / 2 - n)] = coefs_[idx];
        }
        // head bump (correcao (2): ganho e Q do original)
        const float bumpFreq = cur_.speed * 0.0254f / (cur_.gap * 1.0e-6f * 500.0f);
        const float gain = std::max(1.5f * (1000.0f - std::abs(bumpFreq - 100.0f)) / 1000.0f, 1.0f);
        bump_[0].peak(bumpFreq, 2.0, gain, fs_);
        const double z1 = bump_[1].z1, z2 = bump_[1].z2;
        bump_[1] = bump_[0];
        bump_[1].z1 = z1; bump_[1].z2 = z2;
    }
    static constexpr int kFade = 1024;
    float fs_ = 48000.0f;
    int order_ = 64;
    std::vector<float> coefs_, H_;
    Fir fir_[2];
    Biquad bump_[2];
    Params cur_, prev_;
    int fade_ = 0, active_ = 0;
};

// --- degrade (DegradeNoise + DegradeFilter + ganho) ---------------------------
class DegradeFilter {
public:
    void reset(float fs, int steps) {
        fs_ = fs;
        z_ = 0.0f;
        freq_.reset(steps);
        freq_.setCurrentAndTarget(freq_.target() == 1.0f ? 20000.0f : freq_.target());
        calc(freq_.target());
    }
    void setFreq(float f) { freq_.setTarget(f); }
    void process(float* x, int n) {
        for (int i = 0; i < n; ++i) {
            if (freq_.smoothing()) calc(freq_.next());
            const float y = z_ + x[i] * b0_;
            z_ = x[i] * b1_ - y * a1_;
            x[i] = y;
        }
    }

private:
    void calc(float fc) {
        const float wc = 2.0f * static_cast<float>(kPiD) * fc / fs_, c = 1.0f / std::tan(wc / 2.0f), a0 = c + 1.0f;
        b0_ = 1.0f / a0; b1_ = b0_; a1_ = (1.0f - c) / a0;
    }
    Smoothed<Smoothing::Multiplicative> freq_;
    float fs_ = 48000.0f, b0_ = 1.0f, b1_ = 0.0f, a1_ = 0.0f, z_ = 0.0f;
};

class Degrade {
public:
    void prepare(float fs) {
        fs_ = fs;
        rng_.seed(newRandomSeed());
        for (auto& f : filt_) f.reset(fs, 20);
        gain_.reset(fs, 0.05);
        prevNoise_ = noise_;
    }
    void setParams(float depth, float amt, float var) {
        const float freqHz = 200.0f * std::pow(20000.0f / 200.0f, 1.0f - amt);
        noise_ = 0.5f * depth * amt;
        for (auto& f : filt_) f.setFreq(std::min(freqHz + var * (freqHz / 0.6f) * uni(), 0.49f * fs_));
        gain_.setTarget(std::pow(10.0f, std::min(-24.0f * depth + var * 36.0f * uni(), 3.0f) * 0.05f)); // correcao (3)
    }
    void process(float* L, float* R, int n) {
        float* io[2] = {L, R};
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i) {
                const float g = noise_ * static_cast<float>(i) / static_cast<float>(n) + prevNoise_ * (1.0f - static_cast<float>(i) / static_cast<float>(n));
                io[ch][i] += uni() * g;
            }
        prevNoise_ = noise_;
        filt_[0].process(L, n);
        filt_[1].process(R, n);
        for (int i = 0; i < n; ++i) { const float g = gain_.next(); L[i] *= g; R[i] *= g; }
    }

private:
    float uni() { return std::uniform_real_distribution<float>(-0.5f, 0.5f)(rng_); }
    float fs_ = 48000.0f, noise_ = 0.0f, prevNoise_ = 0.0f;
    DegradeFilter filt_[2];
    Smoothed<Smoothing::Multiplicative> gain_;
    std::minstd_rand rng_;
};

// --- chew (ChewProcessor + ChewDropout) ----------------------------------------
class Chew {
public:
    void prepare(float fs) {
        fs_ = fs;
        rng_.seed(newRandomSeed());
        mix_.reset(fs, 0.01);
        for (auto& p : power_) p.reset(fs, 0.005);
        for (auto& f : filt_) f.reset(fs, static_cast<int>(fs * 0.02f));
        crinkled_ = false;
        counter_ = 0;
        untilChange_ = dryTime();
    }
    void setParams(float freq, float depth, float var) { freq_ = freq; depth_ = depth; var_ = var; }
    void process(float* L, float* R, int n) {
        const float highFreq = std::min(22000.0f, 0.49f * fs_), freqChange = highFreq - 5000.0f;
        if (freq_ == 0.0f) {
            mixTarget_ = 0.0f;
            for (auto& f : filt_) f.setFreq(highFreq);
        } else if (freq_ == 1.0f) {
            mixTarget_ = 1.0f;
            powerTarget_ = 3.0f * depth_;
            for (auto& f : filt_) f.setFreq(highFreq - freqChange * depth_);
        } else if (counter_ >= untilChange_) {
            counter_ = 0;
            crinkled_ = !crinkled_;
            if (crinkled_) {
                mixTarget_ = 1.0f;
                powerTarget_ = (1.0f + u02()) * depth_;
                for (auto& f : filt_) f.setFreq(highFreq - freqChange * depth_);
                untilChange_ = wetTime();
            } else {
                mixTarget_ = 0.0f;
                for (auto& f : filt_) f.setFreq(highFreq);
                untilChange_ = dryTime();
            }
        } else {
            powerTarget_ = (1.0f + u02()) * depth_;
            if (crinkled_) for (auto& f : filt_) f.setFreq(highFreq - freqChange * depth_);
        }
        mix_.setTarget(mixTarget_);
        for (auto& p : power_) p.setTarget(1.0f + powerTarget_);
        if (!(mix_.target() == 0.0f && !mix_.smoothing())) {
            for (int i = 0; i < n; ++i) {
                const float m = mix_.next();
                L[i] = m * dropout(L[i], 0) + (1.0f - m) * L[i];
                R[i] = m * dropout(R[i], 1) + (1.0f - m) * R[i];
            }
        }
        filt_[0].process(L, n);
        filt_[1].process(R, n);
        counter_ += n;
    }

private:
    float dropout(float x, int ch) {
        const float s = x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
        return std::pow(std::abs(x), power_[ch].next()) * s;
    }
    float u01() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }
    float u02() { return std::uniform_real_distribution<float>(0.0f, 2.0f)(rng_); }
    int dryTime() {
        const float tScale = std::pow(freq_, 0.1f), varScale = std::pow(u02(), var_);
        const int lo = static_cast<int>((1.0f - tScale) * fs_ * varScale), hi = static_cast<int>((2.0f - 1.99f * tScale) * fs_ * varScale);
        return lo + static_cast<int>(u01() * static_cast<float>(hi - lo));
    }
    int wetTime() {
        const float tScale = std::pow(freq_, 0.1f), start = 0.2f + 0.8f * depth_, end = start - (0.001f + 0.01f * depth_);
        const float varScale = std::pow(u02(), var_);
        const int lo = static_cast<int>((1.0f - tScale) * fs_ * varScale);
        const int hi = static_cast<int>(((1.0f - tScale) + start - end * tScale) * fs_ * varScale);
        return lo + static_cast<int>(u01() * static_cast<float>(hi - lo));
    }
    float fs_ = 48000.0f, freq_ = 0.0f, depth_ = 0.0f, var_ = 0.0f, mixTarget_ = 0.0f, powerTarget_ = 0.0f;
    Smoothed<Smoothing::Linear> mix_, power_[2];
    DegradeFilter filt_[2];
    std::minstd_rand rng_;
    int untilChange_ = 1000, counter_ = 0;
    bool crinkled_ = false;
};

// --- wow & flutter (WowFlutterProcessor + WowProcess + FlutterProcess + OHProcess)
class WowFlutter {
public:
    void prepare(float fs) {
        fs_ = fs;
        rng_.seed(newRandomSeed());
        for (int ch = 0; ch < 2; ++ch) {
            wowSlew_[ch].reset(fs, 0.05); wowSlew_[ch].setCurrentAndTarget(kMin);
            flutSlew_[ch].reset(fs, 0.05); flutSlew_[ch].setCurrentAndTarget(kMin);
            wowPhase_[ch] = 0.0f;
            for (float& p : flutPhase_[ch]) p = 0.0f;
            ohY_[ch] = 1.0f;
            ohLp_[ch] = 1.0f;
            line_[ch].assign(kLine, 0.0f);
            dc_[ch] = 0.0f;
        }
        pos_ = 0;
        wowAmp_ = 1000.0f * 1000.0f / fs;
        flutAmp_[0] = -230.0f * 1000.0f / fs; flutAmp_[1] = -80.0f * 1000.0f / fs; flutAmp_[2] = -99.0f * 1000.0f / fs;
        flutDc_ = 350.0f * 1000.0f / fs;
        sqrtDelta_ = 1.0f / std::sqrt(fs);
        T_ = 1.0f / fs;
        ohLpCoef_ = 1.0f - std::exp(-2.0f * static_cast<float>(kPiD) * 10.0f / fs); // passa-baixa de 10 Hz do OH
        dcCoef_ = 1.0f - std::exp(-2.0f * static_cast<float>(kPiD) * 15.0f / fs);  // DC blocker 15 Hz
    }
    // parametros 0..1 como no plugin
    void setParams(float wowDepth, float wowRate, float wowVar, float flutterDepth, float flutterRate) {
        const float wd = std::pow(wowDepth, 3.0f), wf = std::pow(4.5f, wowRate) - 1.0f;
        for (auto& s : wowSlew_) s.setTarget(std::max(kMin, wd));
        wowDelta_ = 2.0f * static_cast<float>(kPiD) * wf / fs_;
        ohAmt_ = std::pow(wowVar, 1.25f);
        ohDamping_ = ohAmt_ * 20.0f + 1.0f;
        const float fd = std::pow(std::pow(flutterDepth, 3.0f) * 81.0f / 625.0f, 0.5f), ff = 0.1f * std::pow(1000.0f, flutterRate);
        for (auto& s : flutSlew_) s.setTarget(std::max(kMin, fd));
        flutDelta_ = 2.0f * static_cast<float>(kPiD) * ff / fs_;
    }
    bool off() const { return wowSlew_[0].target() == kMin && flutSlew_[0].target() == kMin && !wowSlew_[0].smoothing() && !flutSlew_[0].smoothing(); }
    void process(float* L, float* R, int n) {
        float* io[2] = {L, R};
        static constexpr float kOff[3] = {0.0f, 13.0f * static_cast<float>(kPiD) / 4.0f, -static_cast<float>(kPiD) / 10.0f};
        for (int i = 0; i < n; ++i) {
            const float noise = std::normal_distribution<float>(0.0f, 1.0f / 2.33f)(rng_); // um ruido pros 2 canais, como o original
            for (int ch = 0; ch < 2; ++ch) {
                // wow: cosseno + processo de Ornstein-Uhlenbeck filtrado
                wowPhase_[ch] += wowDelta_;
                const float wd = wowSlew_[ch].next() * wowAmp_;
                ohY_[ch] += sqrtDelta_ * noise * ohAmt_;
                ohY_[ch] += ohDamping_ * (ohAmt_ - ohY_[ch]) * T_;
                ohLp_[ch] += ohLpCoef_ * (ohY_[ch] - ohLp_[ch]);
                const float wow = wd * (std::cos(wowPhase_[ch]) + ohLp_[ch]);
                // flutter: 3 harmonicos
                float* fp = flutPhase_[ch];
                fp[0] += flutDelta_; fp[1] += 2.0f * flutDelta_; fp[2] += 3.0f * flutDelta_;
                const float fd = flutSlew_[ch].next();
                const float flut = fd * (flutAmp_[0] * std::cos(fp[0] + kOff[0]) + flutAmp_[1] * std::cos(fp[1] + kOff[1]) + flutAmp_[2] * std::cos(fp[2] + kOff[2]));
                const float len = std::clamp((wow + flut + flutDc_ + wd) * fs_ / 1000.0f, 0.0f, static_cast<float>(kLine - 8));
                line_[ch][static_cast<size_t>(pos_)] = io[ch][i];
                float y = lagrange(line_[ch], len);
                // DC blocker de 15 Hz
                dc_[ch] += dcCoef_ * (y - dc_[ch]);
                io[ch][i] = y - dc_[ch];
            }
            pos_ = (pos_ + 1) & (kLine - 1);
        }
        for (int ch = 0; ch < 2; ++ch) {
            constexpr float kTwoPiF = 2.0f * static_cast<float>(kPiD);
            while (wowPhase_[ch] >= kTwoPiF) wowPhase_[ch] -= kTwoPiF;
            for (float& p : flutPhase_[ch]) while (p >= kTwoPiF) p -= kTwoPiF;
        }
    }

private:
    // atraso fracionario com Lagrange de 3a ordem (o chowdsp::DelayLine do original)
    float lagrange(const std::vector<float>& b, float d) const {
        const int di = static_cast<int>(d);
        const float f = d - static_cast<float>(di);
        auto s = [&](int k) { return b[static_cast<size_t>((pos_ - di - k + kLine) & (kLine - 1))]; };
        const float x0 = s(0), x1 = s(1), x2 = s(2), x3 = s(3);
        const float d1 = f - 1.0f, d2 = f - 2.0f, d3 = f - 3.0f;
        return -x0 * d1 * d2 * d3 / 6.0f + x1 * f * d2 * d3 * 0.5f - x2 * f * d1 * d3 * 0.5f + x3 * f * d1 * d2 / 6.0f;
    }
    static constexpr int kLine = 1 << 14;
    static constexpr float kMin = 0.001f;
    float fs_ = 48000.0f;
    std::minstd_rand rng_;
    Smoothed<Smoothing::Multiplicative> wowSlew_[2], flutSlew_[2];
    float wowPhase_[2] = {}, flutPhase_[2][3] = {}, wowDelta_ = 0.0f, flutDelta_ = 0.0f;
    float wowAmp_ = 0.0f, flutAmp_[3] = {}, flutDc_ = 0.0f;
    float ohY_[2] = {1.0f, 1.0f}, ohLp_[2] = {1.0f, 1.0f}, ohAmt_ = 0.0f, ohDamping_ = 1.0f, sqrtDelta_ = 0.0f, T_ = 0.0f, ohLpCoef_ = 0.0f;
    std::vector<float> line_[2];
    int pos_ = 0;
    float dc_[2] = {}, dcCoef_ = 0.0f;
};

// --- a maquina inteira ---------------------------------------------------------
struct TapeParams {
    float drive = 0.85f, saturation = 0.5f, bias = 0.5f, tone = 0.0f; // tone -1..1
    float speed = 30.0f, spacing = 0.1f, gap = 1.0f, thickness = 0.1f; // ips, microns
    float wow = 0.0f, flutter = 0.0f;                                 // 0..1
    float degrade = 0.0f, degradeAmount = 0.0f;                       // 0..1
};

class TapeMachine {
public:
    void prepare(double fs) {
        fs_ = fs;
        hyst_.reset(fs);
        toneIn_.prepare(fs);
        toneOut_.prepare(fs);
        loss_.prepare(static_cast<float>(fs));
        degrade_.prepare(static_cast<float>(fs));
        chew_.prepare(static_cast<float>(fs));
        wf_.prepare(static_cast<float>(fs));
    }
    void reset() { prepare(fs_); }
    void process(const TapeParams& p, float* L, float* R, int n) {
        // tom: realce antes da saturacao e o inverso depois (ToneControl in/out)
        const float tone = std::clamp(p.tone, -1.0f, 1.0f);
        float bass = 0.0f, treble = 0.0f;
        if (tone > 0.0f) { treble = tone; bass = -0.5f * treble; }
        else if (tone < 0.0f) { bass = -tone; treble = -0.5f * bass; }
        toneIn_.set(18.0f * bass, 18.0f * treble);
        toneOut_.set(-18.0f * bass, -18.0f * treble);
        toneIn_.process(L, R, n);
        hyst_.setParams(std::clamp(p.drive, 0.0f, 1.0f), std::clamp(p.saturation, 0.0f, 1.0f), std::clamp(p.bias, 0.0f, 1.0f));
        hyst_.process(L, R, n);
        // (sem o makeup de +9 dB do TapeEffect do Surge: no rack nao ha knob de
        // saida e a histerese sozinha ja sai perto do nivel de entrada)
        // Rede de seguranca (nao existe no original): com ruido cheio e
        // pre-enfase a magnetizacao passa de M_s ate o limite de estabilidade
        // (20) e o makeup multiplica isso; 2*tanh(x/2) e quase linear abaixo
        // de 1 e segura esses picos.
        for (int i = 0; i < n; ++i) { L[i] = 2.0f * std::tanh(0.5f * L[i]); R[i] = 2.0f * std::tanh(0.5f * R[i]); }
        toneOut_.process(L, R, n);
        const float dd = std::clamp(p.degrade, 0.0f, 1.0f), da = std::clamp(p.degradeAmount, 0.0f, 1.0f), dv = 0.3f * dd;
        if (dd > 0.0f) {
            chew_.setParams(0.9f - da * 0.8f, dd * 0.15f, dv); // o TapeEffect liga o chew ao degrade
            chew_.process(L, R, n);
            degrade_.setParams(dd, da, dv);
            degrade_.process(L, R, n);
        }
        wf_.setParams(std::clamp(p.wow, 0.0f, 1.0f), 0.25f, 0.3f * p.wow, std::clamp(p.flutter, 0.0f, 1.0f), 0.3f);
        wf_.process(L, R, n); // sempre (ligar/desligar mudaria o atraso fixo de ~7 ms do flutter num salto)
        loss_.setParams(std::clamp(p.speed, 1.0f, 30.0f), std::clamp(p.spacing, 0.1f, 20.0f), std::clamp(p.gap, 1.0f, 50.0f),
                        std::clamp(p.thickness, 0.1f, 50.0f));
        loss_.process(L, R, n);
    }

private:
    double fs_ = 48000.0;
    HysteresisProcessor hyst_;
    ToneStage toneIn_, toneOut_;
    LossFilter loss_;
    Degrade degrade_;
    Chew chew_;
    WowFlutter wf_;
};

} // namespace dx7fm::chowtape
