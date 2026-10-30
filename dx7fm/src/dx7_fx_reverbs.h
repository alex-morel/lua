#pragma once
// =============================================================================
// [ADICIONADO em 2026-10-29] Reverbs do rack no estilo das 12 "maquinas" do
// Strymon BigSky, sobre reverbs de codigo aberto:
//   Hall, Room, Plate, Reflections -- Dragonfly Reverb (Michael Willis,
//       GPLv3) sobre o freeverb3 (Teru Kamogashira, GPLv2+), vendorizado em
//       third_party/freeverb3 (ver README.dx7fm la);
//   Cloud (e a base do Swell e do Bloom) -- Cloud Seed Core (Ghost Note
//       Audio, MIT), vendorizado em third_party/cloudseed;
//   Spring -- SpringReverbProc do chowdsp no Surge XT (Jatin Chowdhury /
//       Surge Synth Team, GPLv3), portado;
//   Swell, Bloom, Chorale, Shimmer, Magneto, Nonlinear -- combinacoes
//       proprias dessas bases com o pitch shifter do Clouds (MIT) e o delay
//       de fita do Mare Tranquillitatis.
//
// ATENCAO: o freeverb3 e o Cloud Seed tem .cpp (funcoes nao-inline) e sao
// incluidos aqui como "unity build". Isso so funciona porque cada executavel
// do projeto tem o motor (dx7_alloc.h) em UMA unidade de traducao; incluir o
// motor em dois .cpp do mesmo executavel da "multiple definition" no link.
//
// Alocacao: varios setters do freeverb3 realocam buffers (taxa, tamanho,
// pre-delay, wander). Esses parametros sao marcados "rebuild" na tabela do
// rack: mudar um deles cria um DSP novo na thread da UI (configure()) em vez
// de mexer no que esta tocando. process() so chama setters que nao alocam.
// =============================================================================
#include "dx7_fx_extra.h"
#include "chowtape/ChowTape.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

// --- freeverb3 (os 17 .cpp que o Dragonfly compila, so float) ---------------
#ifndef LIBFV3_FLOAT
#define LIBFV3_FLOAT
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../third_party/freeverb3/allpass.cpp"
#include "../third_party/freeverb3/biquad.cpp"
#include "../third_party/freeverb3/comb.cpp"
#include "../third_party/freeverb3/delay.cpp"
#include "../third_party/freeverb3/delayline.cpp"
#include "../third_party/freeverb3/earlyref.cpp"
#include "../third_party/freeverb3/efilter.cpp"
#include "../third_party/freeverb3/nrev.cpp"
#include "../third_party/freeverb3/nrevb.cpp"
#include "../third_party/freeverb3/progenitor.cpp"
#include "../third_party/freeverb3/progenitor2.cpp"
#include "../third_party/freeverb3/revbase.cpp"
#include "../third_party/freeverb3/slot.cpp"
#include "../third_party/freeverb3/strev.cpp"
#include "../third_party/freeverb3/utils.cpp"
#include "../third_party/freeverb3/zrev.cpp"
#include "../third_party/freeverb3/zrev2.cpp"
// o fv3_defs.h define isfinite/isnormal/fpclassify como MACROS (pra
// std::isfinite...); fora do freeverb3 isso quebraria todo std::isfinite(x)
#undef isfinite
#undef isnormal
#undef fpclassify

// --- Cloud Seed Core -----------------------------------------------------------
namespace Cloudseed { // strcpy_s so existe no MSVC (usado so na formatacao de texto deles)
inline void strcpy_s(char* d, int n, const char* s) { std::strncpy(d, s, static_cast<size_t>(n - 1)); d[n - 1] = 0; }
}
#define BUFFER_SIZE 256
#define MAX_STR_SIZE 32
#include "../third_party/cloudseed/DSP/ReverbController.h"
#include "../third_party/cloudseed/DSP/Biquad.cpp"
#include "../third_party/cloudseed/DSP/RandomBuffer.cpp"
#include "../third_party/cloudseed/Parameters.cpp"
#undef BUFFER_SIZE
#undef MAX_STR_SIZE
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace dx7fm::reverbs {

constexpr int kBlock = 256;
using fxextra::kPiD;

// =============================================================================
// Dragonfly Hall (plugins/dragonfly-hall-reverb/DSP.cpp): early reflections
// (earlyref) + cauda (zrev2), parte das reflexoes realimentando a cauda. Os
// valores fixos sao os do preset "Small Clear Hall" do Dragonfly.
// =============================================================================
class DragonflyHall {
public:
    void configure(double sr, float sizeMeters, float predelayMs) {
        early_.loadPresetReflection(FV3_EARLYREF_PRESET_1);
        early_.setMuteOnChange(false);
        early_.setdryr(0);
        early_.setwet(0);
        early_.setwidth(1.0);
        early_.setLRDelay(0.3);
        early_.setLRCrossApFreq(750, 4);
        early_.setDiffusionApFreq(150, 4);
        early_.setSampleRate(static_cast<float>(sr));
        late_.setMuteOnChange(false);
        late_.setwet(0);
        late_.setdryr(0);
        late_.setwidth(1.0);
        late_.setSampleRate(static_cast<float>(sr));
        early_.setRSFactor(sizeMeters / 10.0f);
        late_.setRSFactor(sizeMeters / 80.0f);
        late_.setPreDelay(std::max(0.1f, predelayMs)); // o Dragonfly evita 0 (o freeverb3 trata mal)
        late_.setwander(15.0f);                          // aloca: so aqui
        late_.setspin(3.3f);
        early_.setoutputhpf(4.0f);
        late_.setoutputhpf(4.0f);
        late_.setxover_low(500.0f);
        late_.setrt60_factor_low(1.3f);
        late_.setxover_high(5500.0f);
        late_.setrt60_factor_high(0.5f);
        mute();
    }
    void mute() { early_.mute(); late_.mute(); last_ = Live{}; }
    // decay em s, highCut em Hz, diffuse e mod 0..1, earlyLevel relativo a cauda
    void process(float decay, float highCut, float diffuse, float mod, float earlyLevel, float* L, float* R, int n) {
        if (decay != last_.decay) { late_.setrt60(decay); last_.decay = decay; }
        if (highCut != last_.highCut) { early_.setoutputlpf(highCut); late_.setoutputlpf(highCut); last_.highCut = highCut; }
        if (diffuse != last_.diffuse) { late_.setidiffusion1(diffuse * 100.0f / 140.0f); late_.setapfeedback(diffuse * 100.0f / 140.0f); last_.diffuse = diffuse; }
        if (mod != last_.mod) { const float m = mod <= 0.0f ? 0.001f : mod; late_.setspinfactor(m); late_.setlfofactor(m); last_.mod = mod; }
        for (int off = 0; off < n; off += kBlock) {
            const int len = std::min(kBlock, n - off);
            float* l = L + off;
            float* r = R + off;
            early_.processreplace(l, r, eo_[0], eo_[1], len);
            for (int i = 0; i < len; ++i) { li_[0][i] = kEarlySend * eo_[0][i] + l[i]; li_[1][i] = kEarlySend * eo_[1][i] + r[i]; }
            late_.processreplace(li_[0], li_[1], lo_[0], lo_[1], len);
            for (int i = 0; i < len; ++i) { l[i] = earlyLevel * eo_[0][i] + lo_[0][i]; r[i] = earlyLevel * eo_[1][i] + lo_[1][i]; }
        }
    }

private:
    static constexpr float kEarlySend = 0.20f;
    struct Live { float decay = -1, highCut = -1, diffuse = -1, mod = -1; } last_;
    fv3::earlyref_f early_;
    fv3::zrev2_f late_;
    float eo_[2][kBlock], li_[2][kBlock], lo_[2][kBlock];
};

// =============================================================================
// Dragonfly Room (plugins/dragonfly-room-reverb/DSP.cpp): filtros de entrada,
// early reflections + cauda progenitor2 com "low boost". Fixos do preset
// "Medium Clear Room".
// =============================================================================
class DragonflyRoom {
public:
    void configure(double sr, float sizeMeters, float predelayMs) {
        sr_ = sr;
        for (auto* f : {&inLpf_[0], &inLpf_[1], &inHpf_[0], &inHpf_[1]}) f->mute();
        early_.loadPresetReflection(FV3_EARLYREF_PRESET_1);
        early_.setMuteOnChange(false);
        early_.setdryr(0);
        early_.setwet(0);
        early_.setwidth(100.0f / 120.0f);
        early_.setLRDelay(0.3);
        early_.setLRCrossApFreq(750, 4);
        early_.setDiffusionApFreq(150, 4);
        early_.setSampleRate(static_cast<float>(sr));
        late_.setMuteOnChange(false);
        late_.setwet(0);
        late_.setdryr(0);
        late_.setwidth(1.0);
        late_.setSampleRate(static_cast<float>(sr));
        size_ = sizeMeters;
        early_.setRSFactor(sizeMeters / 10.0f);
        late_.setRSFactor(sizeMeters / 10.0f);
        late_.setPreDelay(std::max(0.1f, predelayMs));
        const float spin = 0.8f, wander = 40.0f;
        late_.setspin(spin);                                                   // aloca: so aqui
        late_.setspin2(std::sqrt(100.0f - (10.0f - spin) * (10.0f - spin)) / 2.0f);
        late_.setwander(wander / 200.0f + 0.1f);
        late_.setwander2(wander / 200.0f + 0.1f);
        early_.setoutputlpf(10000.0f);
        late_.setdamp2(600.0f);
        for (int c = 0; c < 2; ++c) { inLpf_[c].setLPF_BW(16000.0f, static_cast<float>(sr)); inHpf_[c].setHPF_BW(4.0f, static_cast<float>(sr)); }
        mute();
    }
    void mute() { early_.mute(); late_.mute(); for (auto* f : {&inLpf_[0], &inLpf_[1], &inHpf_[0], &inHpf_[1]}) f->mute(); last_ = Live{}; }
    void process(float decay, float damp, float diffuse, float boost, float earlyLevel, float* L, float* R, int n) {
        const bool decayChanged = decay != last_.decay, boostChanged = boost != last_.boost;
        if (decayChanged) { late_.setrt60(decay); last_.decay = decay; }
        if (decayChanged || boostChanged) { // a formula do Dragonfly depende de decay, tamanho e boost
            late_.setbassboost(boost * 100.0f / 20.0f / std::pow(decay, 1.5f) * (size_ / 10.0f));
            last_.boost = boost;
        }
        if (damp != last_.damp) { late_.setdamp(damp); late_.setoutputdamp(damp); last_.damp = damp; }
        if (diffuse != last_.diffuse) { late_.setidiffusion1(diffuse * 100.0f / 120.0f); late_.setodiffusion1(diffuse * 100.0f / 120.0f); last_.diffuse = diffuse; }
        for (int off = 0; off < n; off += kBlock) {
            const int len = std::min(kBlock, n - off);
            float* l = L + off;
            float* r = R + off;
            for (int i = 0; i < len; ++i) { fi_[0][i] = inLpf_[0].process(inHpf_[0].process(l[i])); fi_[1][i] = inLpf_[1].process(inHpf_[1].process(r[i])); }
            early_.processreplace(fi_[0], fi_[1], eo_[0], eo_[1], len);
            for (int i = 0; i < len; ++i) { li_[0][i] = kEarlySend * eo_[0][i] + fi_[0][i]; li_[1][i] = kEarlySend * eo_[1][i] + fi_[1][i]; }
            late_.processreplace(li_[0], li_[1], lo_[0], lo_[1], len);
            for (int i = 0; i < len; ++i) { l[i] = earlyLevel * eo_[0][i] + lo_[0][i]; r[i] = earlyLevel * eo_[1][i] + lo_[1][i]; }
        }
    }

private:
    static constexpr float kEarlySend = 0.20f;
    struct Live { float decay = -1, damp = -1, diffuse = -1, boost = -1; } last_;
    double sr_ = 48000.0;
    float size_ = 12.0f;
    fv3::iir_1st_f inLpf_[2], inHpf_[2];
    fv3::earlyref_f early_;
    fv3::progenitor2_f late_;
    float fi_[2][kBlock], eo_[2][kBlock], li_[2][kBlock], lo_[2][kBlock];
};

// =============================================================================
// Dragonfly Plate (plugins/dragonfly-plate-reverb/DSP.cpp): 3 algoritmos do
// freeverb3 -- nrev, nrevb (as duas com o passa-baixa de amortecimento que o
// Dragonfly acrescenta no laco, classes NRev/NRevB abaixo, copiadas do
// Dragonfly) e strev. Fixos do preset "Clear Plate".
// DIFERENCA do Dragonfly como distribuido: no freeverb3 que ele traz,
// nrev::processreplace NAO chama processloop2 e o nrevb herda esse
// processreplace -- entao no plugin o "NRevB" soa igual ao "NRev" e o
// passa-baixa de amortecimento do Dragonfly nunca entra. Aqui os laços do
// Dragonfly (processloop2) sao chamados direto: NRevB tem o algoritmo dele e
// o "Damp" vale, como o codigo do Dragonfly pretende.
// =============================================================================
class DfNRev : public fv3::nrev_f {
public:
    void setDampLpf(float value) {
        dampLpf = limFs2(value);
        dampLpfL.setLPF_BW(dampLpf, getTotalSampleRate());
        dampLpfR.setLPF_BW(dampLpf, getTotalSampleRate());
    }
    void mute() override { fv3::nrev_f::mute(); dampLpfL.mute(); dampLpfR.mute(); }
    void setFsFactors() override { fv3::nrev_f::setFsFactors(); setDampLpf(dampLpf); }

    // (nao e virtual no nrev deste freeverb3: chamado direto pelo DragonflyPlate)
    void processloop2(long count, float* inputL, float* inputR, float* outputL, float* outputR) {
        float outL, outR;
        while (count-- > 0) {
            outL = outR = 0;
            hpf = damp3_1 * inDCC(*inputL + *inputR) - damp3 * hpf;
            UNDENORMAL(hpf);
            hpf *= FV3_NREV_SCALE_WET;
            for (long i = 0; i < FV3_NREV_NUM_COMB; i++) outL += combL[i]._process(hpf);
            for (long i = 0; i < 3; i++) outL = allpassL[i]._process_ov(outL);
            lpfL = dampLpfL(damp2 * lpfL + damp2_1 * outL);
            UNDENORMAL(lpfL);
            outL = allpassL[3]._process_ov(lpfL);
            outL = allpassL[5]._process_ov(outL);
            outL = delayWL(lLDCC(outL));
            for (long i = 0; i < FV3_NREV_NUM_COMB; i++) outR += combR[i]._process(hpf);
            for (long i = 0; i < 3; i++) outR = allpassR[i]._process_ov(outR);
            lpfR = dampLpfR(damp2 * lpfR + damp2_1 * outR);
            UNDENORMAL(lpfR);
            outR = allpassR[3]._process_ov(lpfR);
            outR = allpassR[6]._process_ov(outR);
            outR = delayWR(lRDCC(outR));
            *outputL = outL * wet1 + outR * wet2 + delayL(*inputL) * dry;
            *outputR = outR * wet1 + outL * wet2 + delayR(*inputR) * dry;
            inputL++; inputR++; outputL++; outputR++;
        }
    }

private:
    float dampLpf = 20000;
    fv3::iir_1st_f dampLpfL, dampLpfR;
};

class DfNRevB : public fv3::nrevb_f {
public:
    void setDampLpf(float value) {
        dampLpf = limFs2(value);
        dampLpfL.setLPF_BW(dampLpf, getTotalSampleRate());
        dampLpfR.setLPF_BW(dampLpf, getTotalSampleRate());
    }
    void mute() override { fv3::nrevb_f::mute(); dampLpfL.mute(); dampLpfR.mute(); }
    void setFsFactors() override { fv3::nrevb_f::setFsFactors(); setDampLpf(dampLpf); }

    void processloop2(long count, float* inputL, float* inputR, float* outputL, float* outputR) override {
        float outL, outR, tmpL, tmpR;
        while (count-- > 0) {
            hpf = damp3_1 * inDCC.process(*inputL + *inputR) - damp3 * hpf;
            UNDENORMAL(hpf);
            outL = outR = tmpL = tmpR = hpf;
            outL += apfeedback * lastL;
            lastL += -1 * apfeedback * outL;
            for (long i = 0; i < FV3_NREV_NUM_COMB; i++) outL += combL[i]._process(tmpL);
            for (long i = 0; i < FV3_NREVB_NUM_COMB_2; i++) outL += comb2L[i]._process(tmpL);
            for (long i = 0; i < 3; i++) outL = allpassL[i]._process(outL);
            for (long i = 0; i < FV3_NREVB_NUM_ALLPASS_2; i++) outL = allpass2L[i]._process(outL);
            lpfL = dampLpfL(damp2 * lpfL + damp2_1 * outL);
            UNDENORMAL(lpfL);
            outL = allpassL[3]._process(lpfL);
            outL = allpassL[5]._process(outL);
            outL = lLDCC(outL);
            outR += apfeedback * lastR;
            lastR += -1 * apfeedback * outR;
            for (long i = 0; i < FV3_NREV_NUM_COMB; i++) outR += combR[i]._process(tmpR);
            for (long i = 0; i < FV3_NREVB_NUM_COMB_2; i++) outR += comb2R[i]._process(tmpR);
            for (long i = 0; i < 3; i++) outR = allpassR[i]._process(outR);
            for (long i = 0; i < FV3_NREVB_NUM_ALLPASS_2; i++) outR = allpass2R[i]._process(outR);
            lpfR = dampLpfR(damp2 * lpfR + damp2_1 * outR);
            UNDENORMAL(lpfR);
            outR = allpassR[3]._process(lpfR);
            outR = allpassL[6]._process(outR); // (sic) como no Dragonfly
            outR = lRDCC(outR);
            lastL = FV3_NREVB_SCALE_WET * delayWL(lastL);
            lastR = FV3_NREVB_SCALE_WET * delayWR(lastR);
            *outputL = lastL * wet1 + lastR * wet2 + delayL(*inputL) * dry;
            *outputR = lastR * wet1 + lastL * wet2 + delayR(*inputR) * dry;
            lastL = outL;
            lastR = outR;
            inputL++; inputR++; outputL++; outputR++;
        }
    }

private:
    float dampLpf = 20000;
    fv3::iir_1st_f dampLpfL, dampLpfR;
};

class DragonflyPlate {
public:
    void configure(double sr, float predelayMs) {
        for (auto* f : {&inLpf_[0], &inLpf_[1], &inHpf_[0], &inHpf_[1]}) f->mute();
        for (fv3::revbase_f* m : {static_cast<fv3::revbase_f*>(&nrev_), static_cast<fv3::revbase_f*>(&nrevb_), static_cast<fv3::revbase_f*>(&strev_)}) {
            m->setdryr(0);
            m->setwetr(1);
            m->setMuteOnChange(false);
            m->setSampleRate(static_cast<float>(sr));
            m->setwidth(100.0f / 120.0f);
            m->setPreDelay(std::max(0.1f, predelayMs));
        }
        strev_.setdccutfreq(6);
        strev_.setspinlimit(12);
        strev_.setspindiff(0.15);
        for (int c = 0; c < 2; ++c) { inLpf_[c].setLPF_BW(13000.0f, static_cast<float>(sr)); inHpf_[c].setHPF_BW(100.0f, static_cast<float>(sr)); }
        mute();
    }
    void mute() { nrev_.mute(); nrevb_.mute(); strev_.mute(); for (auto* f : {&inLpf_[0], &inLpf_[1], &inHpf_[0], &inHpf_[1]}) f->mute(); last_ = Live{}; }
    // algo 0 nrev, 1 nrevb, 2 strev
    void process(int algo, float decay, float damp, float* L, float* R, int n) {
        fv3::revbase_f* model = algo == 0 ? static_cast<fv3::revbase_f*>(&nrev_) : algo == 2 ? static_cast<fv3::revbase_f*>(&strev_) : static_cast<fv3::revbase_f*>(&nrevb_);
        if (algo != last_.algo) { if (prev_) prev_->mute(); last_.algo = algo; }
        prev_ = model;
        if (decay != last_.decay) { nrev_.setrt60(decay); nrevb_.setrt60(decay); strev_.setrt60(decay); last_.decay = decay; }
        if (damp != last_.damp) { nrev_.setDampLpf(damp); nrevb_.setDampLpf(damp); strev_.setdamp(damp); strev_.setoutputdamp(std::fmax(damp * 2.0f, 16000.0f)); last_.damp = damp; }
        for (int off = 0; off < n; off += kBlock) {
            const int len = std::min(kBlock, n - off);
            float* l = L + off;
            float* r = R + off;
            for (int i = 0; i < len; ++i) { fi_[0][i] = inLpf_[0].process(inHpf_[0].process(l[i])); fi_[1][i] = inLpf_[1].process(inHpf_[1].process(r[i])); }
            if (algo == 0) nrev_.processloop2(len, fi_[0], fi_[1], l, r);
            else if (algo == 1) nrevb_.processloop2(len, fi_[0], fi_[1], l, r);
            else strev_.processreplace(fi_[0], fi_[1], l, r, len);
        }
    }

private:
    struct Live { int algo = -1; float decay = -1, damp = -1; } last_;
    fv3::iir_1st_f inLpf_[2], inHpf_[2];
    DfNRev nrev_;
    DfNRevB nrevb_;
    fv3::strev_f strev_;
    fv3::revbase_f* prev_ = nullptr;
    float fi_[2][kBlock];
};

// =============================================================================
// Dragonfly Early Reflections (plugins/dragonfly-early-reflections/DSP.cpp):
// so o earlyref, com os 8 programas de sala do Dragonfly.
// =============================================================================
class DragonflyEarly {
public:
    static constexpr int kPrograms[8] = {2, 18, 0, 19, 1, 13, 14, 21}; // Abrupt Echo, Backstage Pass, Concert Venue, ...
    void configure(double sr, int program, float sizeMeters) {
        model_.setMuteOnChange(false);
        model_.setdryr(0);
        model_.setwet(0);
        model_.setwidth(0.8);
        model_.setLRDelay(0.3);
        model_.setLRCrossApFreq(750, 4);
        model_.setDiffusionApFreq(150, 4);
        model_.setSampleRate(static_cast<float>(sr));
        model_.loadPresetReflection(kPrograms[std::clamp(program, 0, 7)]);
        model_.setRSFactor(sizeMeters / 10.0f);
        model_.setoutputhpf(4.0f);
        model_.mute();
        last_ = Live{};
    }
    void mute() { model_.mute(); last_ = Live{}; }
    void process(float width, float highCut, float* L, float* R, int n) {
        if (width != last_.width) { model_.setwidth(width); last_.width = width; }
        if (highCut != last_.highCut) { model_.setoutputlpf(highCut); last_.highCut = highCut; }
        for (int off = 0; off < n; off += kBlock) {
            const int len = std::min(kBlock, n - off);
            std::memcpy(in_[0], L + off, sizeof(float) * static_cast<size_t>(len));
            std::memcpy(in_[1], R + off, sizeof(float) * static_cast<size_t>(len));
            model_.processreplace(in_[0], in_[1], L + off, R + off, len);
        }
    }

private:
    struct Live { float width = -1, highCut = -1; } last_;
    fv3::earlyref_f model_;
    float in_[2][kBlock];
};

// =============================================================================
// Cloud Seed (Ghost Note Audio, MIT): rede de linhas de atraso moduladas com
// difusores, early taps e EQ. Parametros normalizados 0..1 do proprio Cloud
// Seed; as sementes (que alocam) so mudam no configure().
// =============================================================================
class CloudSeed {
public:
    void configure(double sr) {
        rc_ = std::make_unique<Cloudseed::ReverbController>(static_cast<int>(sr));
        setBase(); // as sementes (que alocam) so aqui, na thread da UI
    }
    // valores-base de um "cloud" longo e modulado; o resto vem dos knobs
    void setBase() {
        namespace P = Cloudseed::Parameter;
        auto s = [&](int id, double v) { rc_->SetParameter(id, v); };
        s(P::Interpolation, 1.0); s(P::LowCutEnabled, 1.0); s(P::LowCut, 0.25); s(P::HighCutEnabled, 0.0);
        s(P::InputMix, 0.25); s(P::DryOut, 0.0); s(P::EarlyOut, 0.0); s(P::LateOut, 0.9);
        s(P::TapEnabled, 1.0); s(P::TapCount, 0.3); s(P::TapDecay, 1.0); s(P::TapLength, 0.9);
        s(P::EarlyDiffuseEnabled, 1.0); s(P::EarlyDiffuseCount, 0.4); s(P::EarlyDiffuseDelay, 0.4); s(P::EarlyDiffuseModAmount, 0.3);
        s(P::EarlyDiffuseFeedback, 0.75); s(P::EarlyDiffuseModRate, 0.3);
        s(P::LateMode, 1.0); s(P::LateLineCount, 1.0); s(P::LateDiffuseEnabled, 1.0); s(P::LateDiffuseCount, 0.5);
        s(P::LateDiffuseDelay, 0.3); s(P::LateDiffuseModAmount, 0.2); s(P::LateLineModRate, 0.25); s(P::LateDiffuseFeedback, 0.8);
        s(P::LateDiffuseModRate, 0.2); s(P::EqLowShelfEnabled, 0.0); s(P::EqHighShelfEnabled, 1.0); s(P::EqLowpassEnabled, 0.0);
        s(P::EqLowFreq, 0.4); s(P::EqHighFreq, 0.5); s(P::EqCutoff, 0.95); s(P::EqLowGain, 0.55);
        s(P::EqCrossSeed, 0.0); s(P::SeedTap, 0.33); s(P::SeedDiffusion, 0.18); s(P::SeedDelay, 0.22); s(P::SeedPostDiffusion, 0.36);
        rc_->ClearBuffers();
        last_ = Live{};
    }
    void clear() { rc_->ClearBuffers(); }
    // todos 0..1 (escala do Cloud Seed)
    void process(float decay, float size, float mod, float diffusion, float tone, float predelay, float* L, float* R, int n) {
        namespace P = Cloudseed::Parameter;
        auto upd = [&](float& last, float v, std::initializer_list<int> ids) {
            if (v == last) return;
            for (int id : ids) rc_->SetParameter(id, v);
            last = v;
        };
        upd(last_.decay, decay, {P::LateLineDecay});
        upd(last_.size, size, {P::LateLineSize});
        upd(last_.mod, mod, {P::LateLineModAmount});
        upd(last_.diffusion, diffusion, {P::LateDiffuseFeedback});
        upd(last_.tone, tone, {P::EqHighGain});
        upd(last_.predelay, predelay, {P::TapPredelay});
        for (int off = 0; off < n; off += kBlock) {
            const int len = std::min(kBlock, n - off);
            rc_->Process(L + off, R + off, out_[0], out_[1], len);
            std::memcpy(L + off, out_[0], sizeof(float) * static_cast<size_t>(len));
            std::memcpy(R + off, out_[1], sizeof(float) * static_cast<size_t>(len));
        }
    }

private:
    struct Live { float decay = -1, size = -1, mod = -1, diffusion = -1, tone = -1, predelay = -1; } last_;
    std::unique_ptr<Cloudseed::ReverbController> rc_;
    float out_[2][kBlock];
};

struct CloudSeedFx : CloudSeed { void reset() { clear(); } };

// =============================================================================
// Spring -- porta do SpringReverbProc do chowdsp no Surge XT
// (src/common/dsp/effects/chowdsp/spring_reverb, Jatin Chowdhury, GPLv3):
// laco de realimentacao com 16 all-pass Schroeder de 2a ordem (interpolacao
// Thiran) que dao a dispersao de mola, rede de reflexoes (4 atrasos com
// matriz de Householder e shelf de amortecimento), passa-baixa e DC blocker.
// Mudancas: os 4 canais SIMD viraram 4 escalares (mesma conta); cada linha de
// atraso dimensionada pro maximo que ela usa (o Surge aloca 1<<18 amostras
// em TODAS, ~134 MB so nas all-pass); tanh no lugar do fasttanh do sst; sem
// o "knock" (shake).
// =============================================================================
class Spring {
public:
    void configure(double sr) {
        fs_ = static_cast<float>(sr);
        delay_.allocate(static_cast<int>(1000.0f + 0.099f * fs_ * 1.1f) + 64);
        for (auto& lane : apf_) for (auto& a : lane) a.allocate(static_cast<int>(3.4f * 0.001f * fs_) + 16);
        for (auto& d : refl_) d.allocate(static_cast<int>(0.30f * fs_) + 16);
        for (int c = 0; c < 2; ++c) {
            dc_[c].setSampleRate(sr); dc_[c].setParams(40.0f, 0.70710678f);
            lpf_[c].setSampleRate(sr);
        }
        reset();
    }
    void reset() {
        delay_.clear();
        for (auto& lane : apf_) for (auto& a : lane) a.clear();
        for (auto& d : refl_) d.clear();
        for (int c = 0; c < 2; ++c) { dc_[c].reset(); lpf_[c].reset(); }
        shelf_.reset();
        std::fill(std::begin(state_), std::end(state_), 0.0f);
        chaos_ = 0.0f;
    }
    void process(float size, float decay, float reflections, float spin, float damping, float chaos, float* L, float* R, int n) {
        // setParams (por bloco, como no Surge)
        const float lowT60 = 0.5f, highT60 = 4.5f;
        const float decayCorr = 0.7f * (1.0f - size * size);
        const float t60 = lowT60 * std::pow(highT60 / lowT60, 0.95f * decay - decayCorr);
        float delaySamples = 1000.0f + size * 0.099f * fs_;
        chaosTarget_ = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_) * delaySamples * 0.07f;
        const float chaosStep = 1.0f - std::exp(-static_cast<float>(n) / (0.05f * fs_)); // SmoothedValue 50 ms, pulando n amostras
        chaos_ += (chaosTarget_ - chaos_) * chaosStep;
        delaySamples += std::pow(chaos, 3.0f) * chaos_;
        delay_.setDelay(delaySamples);
        const float fb = std::pow(0.001f, delaySamples / (t60 * fs_));
        const float g = 0.5f - 0.4f * spin;
        const float apDelay = (0.35f + 3.0f * size) * 0.001f * fs_;
        for (int k = 0; k < 4; ++k) for (auto& a : apf_[k]) a.setDelay(apDelay);
        const float laneG[4] = {g, -g, g, -g};
        const float dampFreq = 4000.0f * std::pow(18000.0f / 4000.0f, 1.0f - damping);
        for (auto& f : lpf_) f.setParams(dampFreq, 0.70710678f);
        // rede de reflexoes
        const float base[4] = {0.07f, 0.17f, 0.23f, 0.29f};
        const float mix = 0.95f * reflections * reflections;
        for (int i = 0; i < 4; ++i) {
            const float d = base[i] * size * fs_;
            refl_[i].setDelay(d);
            reflFb_[i] = std::pow(0.001f, d / (t60 * fs_)) * 0.23f * mix * (0.735f + 0.235f * size);
        }
        shelf_.calc(1.0f, std::pow(10.0f, (-1.0f - 9.0f * damping) / 20.0f), 800.0f, fs_);

        for (int i = 0; i < n; ++i) {
            for (int ch = 0; ch < 2; ++ch) { // doSpringInput
                const float x = (ch == 0 ? L : R)[i];
                const float y = std::tanh(x - fb * delay_.pop(ch));
                state_[2 * ch] = dc_[ch].process(y).hp;
            }
            for (int k = 0; k < 4; ++k) // 16 all-pass de 2a ordem em cada uma das 4 "lanes"
                for (auto& a : apf_[k]) state_[k] = a.process(state_[k], laneG[k]);
            for (int ch = 0; ch < 2; ++ch) { // doSpringOutput
                const int idx = 2 * ch;
                delay_.push(ch, state_[1 + idx]);
                state_[idx] -= reflPop(ch);
                for (auto& d : refl_) d.push(ch, state_[idx]);
                state_[idx] = lpf_[ch].process(state_[idx]).lp;
            }
            L[i] = state_[0];
            state_[1] = state_[0];
            R[i] = state_[2];
            state_[3] = state_[2];
        }
    }

private:
    // linha de atraso no estilo chowdsp::DelayLine (ponteiros que decrescem,
    // buffer duplicado) com interpolacao Lagrange de 3a ordem, 2 canais
    struct LagrangeDelay {
        std::vector<float> buf[2];
        int total = 0, w[2] = {0, 0}, rd[2] = {0, 0}, di = 0;
        float df = 0.0f;
        void allocate(int n) { total = n; for (auto& b : buf) b.assign(static_cast<size_t>(2 * n + 8), 0.0f); }
        void clear() { for (auto& b : buf) std::fill(b.begin(), b.end(), 0.0f); w[0] = w[1] = rd[0] = rd[1] = 0; }
        void setDelay(float d) {
            d = std::clamp(d, 0.0f, static_cast<float>(total - 1));
            di = static_cast<int>(d);
            df = d - static_cast<float>(di);
            if (di >= 1) { df += 1.0f; --di; } // Lagrange3rd::updateInternalVariables
        }
        void push(int c, float x) { buf[c][static_cast<size_t>(w[c])] = x; buf[c][static_cast<size_t>(w[c] + total)] = x; w[c] = (w[c] + total - 1) % total; }
        float pop(int c) {
            const float* b = buf[c].data() + rd[c] + di;
            const float d1 = df - 1.0f, d2 = df - 2.0f, d3 = df - 3.0f;
            const float y = b[0] * (-d1 * d2 * d3 / 6.0f) + df * (b[1] * (d2 * d3 * 0.5f) + b[2] * (-d1 * d3 * 0.5f) + b[3] * (d1 * d2 / 6.0f));
            rd[c] = (rd[c] + total - 1) % total;
            return y;
        }
    };
    // all-pass Schroeder de 2a ordem (um all-pass dentro do outro, mesmo atraso
    // e ganho), atraso com interpolacao Thiran -- SchroederAllpass<T, 2>
    struct ThiranDelay {
        std::vector<float> buf;
        int total = 0, w = 0, rd = 0, di = 0;
        double alpha = 0.0;
        float state = 0.0f;
        void allocate(int n) { total = n; buf.assign(static_cast<size_t>(2 * n + 8), 0.0f); }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0f); w = rd = 0; state = 0.0f; }
        void setDelay(float d) {
            d = std::clamp(d, 0.0f, static_cast<float>(total - 1));
            di = static_cast<int>(d);
            float f = d - static_cast<float>(di);
            if (f < 0.618f && di >= 1) { f += 1.0f; --di; } // Thiran::updateInternalVariables
            alpha = (1.0 - f) / (1.0 + f);
        }
        void push(float x) { buf[static_cast<size_t>(w)] = x; buf[static_cast<size_t>(w + total)] = x; w = (w + total - 1) % total; }
        float pop() {
            const float* b = buf.data() + rd + di;
            const float out = b[1] + static_cast<float>(alpha) * (b[0] - state);
            state = out;
            rd = (rd + total - 1) % total;
            return out;
        }
    };
    struct Allpass2 {
        ThiranDelay outer, inner;
        void allocate(int n) { outer.allocate(n); inner.allocate(n); }
        void clear() { outer.clear(); inner.clear(); }
        void setDelay(float d) { outer.setDelay(d); inner.setDelay(d); }
        static float stage(ThiranDelay& dl, float x, float g, float delayed) {
            x += g * delayed;
            dl.push(x);
            return delayed - g * x;
        }
        float process(float x, float g) {
            const float innerOut = stage(inner, outer.pop(), g, inner.pop()); // nestedAllpass.processSample(delay.popSample())
            return stage(outer, x, g, innerOut);
        }
    };
    float reflPop(int ch) {
        float out[4], sum = 0.0f;
        for (int i = 0; i < 4; ++i) { out[i] = refl_[i].pop(ch); sum += out[i]; }
        const float hh = sum * (-2.0f / 4.0f); // Householder
        float acc = 0.0f;
        for (int i = 0; i < 4; ++i) acc += (out[i] + hh) * reflFb_[i];
        return shelf_.process(acc / 4.0f);
    }
    float fs_ = 48000.0f, chaos_ = 0.0f, chaosTarget_ = 0.0f;
    LagrangeDelay delay_;
    Allpass2 apf_[4][16];
    LagrangeDelay refl_[4];
    float reflFb_[4] = {};
    chowtape::Shelf shelf_;
    TPTStateVariableFilter dc_[2], lpf_[2];
    float state_[4] = {};
    std::minstd_rand rng_{newRandomSeed()};
};

// =============================================================================
// Maquinas compostas (implementacao propria sobre as bases acima)
// =============================================================================

// all-pass Schroeder de atraso fixo (difusor)
struct Diffuser {
    std::vector<float> buf;
    int pos = 0;
    void allocate(int n) { buf.assign(static_cast<size_t>(std::max(n, 1)), 0.0f); pos = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    float process(float x, float g) {
        const float d = buf[static_cast<size_t>(pos)];
        const float v = x + g * d;
        buf[static_cast<size_t>(pos)] = v;
        if (++pos >= static_cast<int>(buf.size())) pos = 0;
        return d - g * v;
    }
};

// Swell: cada ataque reinicia uma rampa de volume (o ataque some e a nota
// "cresce"), e o som crescendo entra num Cloud Seed.
class Swell {
public:
    void configure(double sr) { sr_ = sr; cloud_.configure(sr); reset(); }
    void reset() { cloud_.clear(); fast_ = slow_ = 0.0f; gain_ = 1.0f; hold_ = 0; }
    void process(float riseMs, float sens, float decay, float tone, float reverb, float* L, float* R, int n) {
        const float aF = 1.0f - std::exp(-1.0f / (0.001f * static_cast<float>(sr_))), rF = 1.0f - std::exp(-1.0f / (0.02f * static_cast<float>(sr_)));
        const float aS = 1.0f - std::exp(-1.0f / (0.05f * static_cast<float>(sr_))), rS = 1.0f - std::exp(-1.0f / (0.4f * static_cast<float>(sr_)));
        const float step = 1.0f / std::max(1.0f, riseMs * 0.001f * static_cast<float>(sr_));
        const float thr = 0.002f + 0.05f * (1.0f - sens);
        for (int i = 0; i < n; ++i) {
            const float a = std::max(std::fabs(L[i]), std::fabs(R[i]));
            fast_ += (a - fast_) * (a > fast_ ? aF : rF);
            slow_ += (a - slow_) * (a > slow_ ? aS : rS);
            if (hold_ > 0) --hold_;
            if (hold_ == 0 && fast_ > thr && fast_ > 1.8f * slow_ + 0.001f) { gain_ = 0.0f; hold_ = static_cast<int>(0.05 * sr_); } // ataque novo
            gain_ = std::min(1.0f, gain_ + step);
            const float g = gain_ * gain_; // curva de volume
            L[i] *= g; R[i] *= g;
            dryL_[i] = L[i]; dryR_[i] = R[i];
        }
        cloud_.process(decay, 0.6f, 0.35f, 0.8f, tone, 0.0f, L, R, n);
        for (int i = 0; i < n; ++i) { L[i] = dryL_[i] + reverb * L[i]; R[i] = dryR_[i] + reverb * R[i]; }
    }

private:
    double sr_ = 48000.0;
    CloudSeed cloud_;
    float fast_ = 0.0f, slow_ = 0.0f, gain_ = 1.0f;
    int hold_ = 0;
    float dryL_[kBlock], dryR_[kBlock];
};

// Bloom: a saida do Cloud Seed alimenta um laco de difusores com
// realimentacao longa -- a cauda "floresce" e cresce depois do ataque.
class Bloom {
public:
    void configure(double sr) {
        sr_ = sr;
        cloud_.configure(sr);
        static const float kMs[4] = {13.7f, 19.3f, 29.9f, 37.1f};
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 4; ++k) diff_[c][k].allocate(static_cast<int>((kMs[k] + 3.1f * c) * 0.001 * sr));
            loop_[c].allocate(static_cast<size_t>(sr * 1.05) + 8);
        }
        reset();
    }
    void reset() { cloud_.clear(); for (auto& c : diff_) for (auto& d : c) d.clear(); for (auto& l : loop_) l.clear(); lp_[0] = lp_[1] = 0.0f; }
    void process(float decay, float lengthMs, float feedback, float tone, float* L, float* R, int n) {
        cloud_.process(decay, 0.7f, 0.4f, 0.85f, tone, 0.0f, L, R, n);
        const float len = std::clamp(lengthMs, 50.0f, 1000.0f) * 0.001f * static_cast<float>(sr_);
        const float aLp = gfx::onePoleCoef(1500.0f + 12000.0f * tone, sr_);
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                const float back = loop_[c].read(len);
                float x = io[c][i] + feedback * back;
                for (auto& d : diff_[c]) x = d.process(x, 0.62f);
                lp_[c] += (x - lp_[c]) * aLp;
                loop_[c].write(std::isfinite(lp_[c]) ? lp_[c] : 0.0f);
                io[c][i] = 0.6f * io[c][i] + 0.6f * lp_[c];
            }
    }

private:
    double sr_ = 48000.0;
    CloudSeed cloud_;
    Diffuser diff_[2][4];
    gfx::DelayLine loop_[2];
    float lp_[2] = {};
};

// Chorale: Hall seguido de um banco de 3 passa-bandas nos formantes de uma
// vogal -- a cauda "canta".
class Chorale {
public:
    void configure(double sr, float sizeMeters) {
        sr_ = sr;
        hall_.configure(sr, sizeMeters, 10.0f);
        for (auto& c : f_) for (auto& f : c) f.setSampleRate(sr);
        reset();
    }
    void reset() { hall_.mute(); for (auto& c : f_) for (auto& f : c) f.reset(); }
    void process(float decay, int vowel, float resonance, float amount, float tone, float* L, float* R, int n) {
        static const float kF[5][3] = {{800, 1150, 2900}, {400, 1700, 2600}, {300, 2200, 3000}, {450, 800, 2830}, {325, 700, 2530}};
        static const float kG[3] = {1.0f, 0.6f, 0.35f};
        hall_.process(decay, tone, 0.85f, 0.25f, 0.3f, L, R, n);
        const int v = std::clamp(vowel, 0, 4);
        float* io[2] = {L, R};
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < 3; ++k) f_[c][k].setParams(kF[v][k] * (c ? 1.02f : 1.0f), resonance);
        // o passa-banda do SVF tem pico = Q: normaliza pra pico 1 (senao um tom
        // em cima do formante sai Q vezes mais alto) e compensa so em parte a
        // perda de banda larga (~10*log10(Q) dB): 1.5*Q^0.25
        const float r = std::max(1.0f, resonance);
        const float g = 1.5f * std::sqrt(std::sqrt(r)) / r;
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                float y = 0.0f;
                for (int k = 0; k < 3; ++k) y += kG[k] * f_[c][k].process(io[c][i]).bp;
                io[c][i] = (1.0f - amount) * io[c][i] + amount * g * y;
            }
    }

private:
    double sr_ = 48000.0;
    DragonflyHall hall_;
    TPTStateVariableFilter f_[2][3];
};

// Shimmer: dois pitch shifters (Clouds) num laco proprio de realimentacao
// (atraso de 150 ms + difusores + amortecimento) que alimenta o Hall -- cada
// volta pelo laco sobe (ou desce) os intervalos escolhidos e o Hall espalha.
// O laco NAO passa pelo Hall: com a cauda inteira dentro do laco o ganho por
// volta era amount x energia do Hall (> 1 com decay normal) e a cauda nunca
// morria. Aqui o ganho por volta e <= 1 (deslocador: janelas que somam 1;
// difusores passa-tudo; passa-baixa de 1 polo) vezes o ganho do laco,
// que e < 1 e acompanha o decay do Hall -- a cauda sempre decai.
class Shimmer {
public:
    void configure(double sr, float sizeMeters) {
        sr_ = sr;
        hall_.configure(sr, sizeMeters, 10.0f);
        for (auto& p : ps_) p.prepare(sr);
        static const float kMs[3] = {11.3f, 17.9f, 26.1f};
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 3; ++k) diff_[c][k].allocate(static_cast<int>((kMs[k] + 2.3f * c) * 0.001 * sr));
            loop_[c].allocate(static_cast<size_t>(sr * kLoopSec) + 8);
        }
        reset();
    }
    void reset() {
        hall_.mute();
        for (auto& p : ps_) p.reset();
        for (auto& c : diff_) for (auto& d : c) d.clear();
        for (auto& l : loop_) l.clear();
        lp_[0] = lp_[1] = 0.0f;
    }
    void process(float decay, float semis1, float semis2, float amount, float tone, float* L, float* R, int n) {
        ps_[0].setRatio(std::exp2(std::round(semis1) / 12.0f));
        ps_[1].setRatio(std::exp2(std::round(semis2) / 12.0f));
        const float len = kLoopSec * static_cast<float>(sr_);
        const float aLp = gfx::onePoleCoef(std::clamp(tone, 1000.0f, 16000.0f), sr_);
        // o laco decai no mesmo RT60 do Hall (-60 dB em `decay` segundos), entao
        // as vozes deslocadas duram tanto quanto a cauda; amount = quanto delas
        // entra no Hall
        const float gLoop = std::min(0.97f, std::pow(10.0f, -3.0f * kLoopSec / std::max(0.2f, decay)));
        const float send = 2.0f * std::clamp(amount, 0.0f, 0.95f);
        for (int i = 0; i < n; ++i) {
            float a = loop_[0].read(len), b = loop_[1].read(len), c = a, d = b;
            ps_[0].setSize(0.7f); ps_[0].processSample(a, b);
            ps_[1].setSize(0.7f); ps_[1].processSample(c, d);
            const float sh[2] = {0.5f * (a + c), 0.5f * (b + d)};
            float* io[2] = {L + i, R + i};
            for (int ch = 0; ch < 2; ++ch) {
                lp_[ch] += (sh[ch] - lp_[ch]) * aLp;
                float x = *io[ch] + gLoop * lp_[ch];
                *io[ch] += send * lp_[ch]; // entrada do Hall: seco + vozes deslocadas
                for (auto& df : diff_[ch]) x = df.process(x, 0.6f);
                loop_[ch].write(std::isfinite(x) ? x : 0.0f);
            }
        }
        hall_.process(decay, tone, 0.9f, 0.3f, 0.3f, L, R, n);
    }

private:
    static constexpr float kLoopSec = 0.15f;
    double sr_ = 48000.0;
    DragonflyHall hall_;
    fxextra::PitchShifter ps_[2];
    Diffuser diff_[2][3];
    gfx::DelayLine loop_[2];
    float lp_[2] = {};
};

// Magneto: eco de 4 cabecas (1/4, 2/4, 3/4 e 4/4 do tempo) com wow e laco
// escurecendo, difusao e um Hall curto por cima.
class Magneto {
public:
    void configure(double sr) {
        sr_ = sr;
        for (auto& l : line_) l.allocate(static_cast<size_t>(sr * 2.1) + 8);
        static const float kMs[4] = {7.3f, 11.9f, 17.1f, 23.3f};
        for (int c = 0; c < 2; ++c) for (int k = 0; k < 4; ++k) diff_[c][k].allocate(static_cast<int>((kMs[k] + 1.7f * c) * 0.001 * sr));
        hall_.configure(sr, 16.0f, 4.0f);
        reset();
    }
    void reset() { for (auto& l : line_) l.clear(); for (auto& c : diff_) for (auto& d : c) d.clear(); hall_.mute(); lp_[0] = lp_[1] = 0.0f; ph_ = 0.0; }
    // heads: mascara de 4 bits (cabeca 1 = bit 0)
    void process(float seconds, int heads, float feedback, float diffusion, float decay, float tone, float wow, float* L, float* R, int n) {
        const float T = std::clamp(seconds, 0.02f, 2.0f) * static_cast<float>(sr_);
        const float aLp = gfx::onePoleCoef(1500.0f + 9000.0f * tone, sr_);
        int last = 3;
        while (last > 0 && !(heads & (1 << last))) --last;
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            ph_ += 0.7 / sr_; ph_ -= std::floor(ph_);
            const float mod = 1.0f + wow * 0.004f * static_cast<float>(std::sin(2.0 * kPiD * ph_));
            for (int c = 0; c < 2; ++c) {
                float echo = 0.0f;
                int count = 0;
                for (int h = 0; h < 4; ++h)
                    if (heads & (1 << h)) { echo += line_[c].read(T * mod * (h + 1) * 0.25f + c * 3.0f); ++count; }
                if (count) echo /= std::sqrt(static_cast<float>(count));
                const float back = line_[c].read(T * mod * (last + 1) * 0.25f);
                lp_[c] += (back - lp_[c]) * aLp;
                const float w = io[c][i] + feedback * std::tanh(lp_[c]);
                line_[c].write(std::isfinite(w) ? w : 0.0f);
                float x = echo;
                for (auto& d : diff_[c]) x = d.process(x, 0.7f * diffusion);
                io[c][i] = x;
            }
        }
        // um Hall curto por cima (copia: o eco segue seco + a cauda)
        for (int i = 0; i < n; ++i) { tmpL_[i] = L[i]; tmpR_[i] = R[i]; }
        hall_.process(decay, 3000.0f + 9000.0f * tone, 0.8f, 0.2f, 0.3f, tmpL_, tmpR_, n);
        for (int i = 0; i < n; ++i) { L[i] += 0.5f * tmpL_[i]; R[i] += 0.5f * tmpR_[i]; }
    }

private:
    double sr_ = 48000.0, ph_ = 0.0;
    gfx::DelayLine line_[2];
    Diffuser diff_[2][4];
    DragonflyHall hall_;
    float lp_[2] = {};
    float tmpL_[kBlock], tmpR_[kBlock];
};

// Nonlinear: 48 taps espalhados no tempo escolhido com envelopes "nao
// naturais" -- Reverse (cresce e corta), Swoosh (cresce e cai), Ramp (cresce
// reto e corta) e Gate (plano e corta) -- difundidos por all-pass.
class Nonlinear {
public:
    static constexpr int kTaps = 48;
    void configure(double sr) {
        sr_ = sr;
        for (auto& l : line_) l.allocate(static_cast<size_t>(sr * 1.05) + 8);
        static const float kMs[3] = {4.7f, 8.3f, 13.1f};
        for (int c = 0; c < 2; ++c) for (int k = 0; k < 3; ++k) diff_[c][k].allocate(static_cast<int>((kMs[k] + 1.3f * c) * 0.001 * sr));
        reset();
    }
    void reset() { for (auto& l : line_) l.clear(); for (auto& c : diff_) for (auto& d : c) d.clear(); lp_[0] = lp_[1] = 0.0f; shape_ = -1; }
    void process(int shape, float timeMs, float diffusion, float tone, float* L, float* R, int n) {
        if (shape != shape_) { // ganhos dos taps pela forma, normalizados em energia
            shape_ = shape;
            float e = 0.0f;
            for (int k = 0; k < kTaps; ++k) {
                const float t = (k + 0.5f) / kTaps;
                float g = 1.0f;
                switch (shape) {
                    case 0: g = std::pow(t, 1.6f); break;                                       // Reverse
                    case 1: g = std::sin(static_cast<float>(kPiD) * std::pow(t, 0.7f)); break;   // Swoosh
                    case 2: g = t; break;                                                        // Ramp
                    default: g = 1.0f; break;                                                    // Gate
                }
                gain_[k] = g;
                e += g * g;
            }
            for (float& g : gain_) g /= std::sqrt(e) / 1.2f;
        }
        const float len = std::clamp(timeMs, 50.0f, 1000.0f) * 0.001f * static_cast<float>(sr_);
        const float aLp = gfx::onePoleCoef(1500.0f + 12000.0f * tone, sr_);
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) {
                line_[c].write(io[c][i]);
                float y = 0.0f;
                for (int k = 0; k < kTaps; ++k) y += gain_[k] * line_[c].read(1.0f + len * (k + 0.5f) / kTaps * (c ? 0.985f : 1.0f));
                for (auto& d : diff_[c]) y = d.process(y, 0.65f * diffusion);
                lp_[c] += (y - lp_[c]) * aLp;
                io[c][i] = lp_[c];
            }
    }

private:
    double sr_ = 48000.0;
    gfx::DelayLine line_[2];
    Diffuser diff_[2][3];
    float gain_[kTaps] = {}, lp_[2] = {};
    int shape_ = -1;
};

} // namespace dx7fm::reverbs
