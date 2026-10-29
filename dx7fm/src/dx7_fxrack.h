#pragma once
#include "dx7_globalfx.h"
#include "dx7_params.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// =============================================================================
// [ADICIONADO em 2026-10-29] Rack de efeitos estilo pedaleira multiefeitos:
// 2 cadeias de 4 slots; qualquer efeito em qualquer slot (inclusive repetido).
// Cada parte (A-D) manda pra saida um nivel SECO, um ENVIO pra cadeia 1 e um
// pra cadeia 2. As cadeias rodam em paralelo (saida = seco + cadeia 1 +
// cadeia 2) ou em serie (a saida da cadeia 1 entra na cadeia 2).
//
// Parametros genericos: cada slot tem o tipo, liga/desliga, mix (seco/molhado
// do proprio slot) e ate 8 parametros em unidades naturais (Hz, ms, dB...),
// descritos pela tabela fxInfo(kind) -- que gera os knobs da UI, o JSON (por
// NOME do parametro) e o sanitize.
//
// Threads: a UI chama FxRack::setParams(); os parametros vao por triple buffer
// (o audio sempre le um conjunto inteiro). O DSP de cada slot e um objeto do
// tipo escolhido, criado NA THREAD DA UI quando o tipo muda (reverbs tem MB de
// buffers -- nada e alocado na thread de audio) e trocado por um ponteiro
// atomico; o antigo so e apagado depois que o audio terminou o bloco em que
// podia estar usando ele (contador de blocos, estilo RCU).
// =============================================================================
namespace dx7fm {

constexpr int kFxChains = 2, kFxSlots = 4, kFxParams = 8;
constexpr int kFxParts = 4; // = kNumParts (dx7_alloc.h confere)

// Novos tipos SEMPRE no fim (o JSON usa o nome, mas o enum fica estavel).
enum class FxKind : uint8_t {
    Off,
    Chorus, Phaser, Flanger, TapeDelay, ReverbClassic, Eq3, Compressor, // 2026-10-29: os 7 efeitos globais de antes
    Count
};

struct FxParamInfo {
    const char* id = nullptr;    // nome estavel (JSON)
    const char* label = nullptr; // rotulo na UI
    float min = 0.0f, max = 1.0f, def = 0.0f;
    const char* unit = "";
    bool log = false;            // knob em escala logaritmica
    const char* choices = nullptr; // "A|B|C": parametro de escolha (valor = indice)
};

struct FxInfo {
    FxKind kind = FxKind::Off;
    const char* id = "Off";      // nome estavel (JSON)
    const char* name = "Vazio";  // nome na UI
    const char* category = "";
    const char* origin = "";     // base open source + licenca
    float defMix = 1.0f;
    int nParams = 0;
    FxParamInfo params[kFxParams];
};

inline const FxInfo& fxInfo(FxKind k) {
    static const FxInfo table[] = {
        {FxKind::Off, "Off", "Vazio", "", "", 1.0f, 0, {}},
        {FxKind::Chorus, "Chorus", "Chorus", "Modulacao", "Ensemble do Mutable Instruments Rings (Emilie Gillet, MIT)", 0.5f, 2,
         {{"rate", "Rate", 0.01f, 10.0f, 0.75f, "Hz", true}, {"depth", "Depth", 0.0f, 1.0f, 0.5f}}},
        {FxKind::Phaser, "Phaser", "Phaser", "Modulacao", "phaser2_stereo da phaflangers.lib do Faust (Julius O. Smith III, STK-4.3)", 1.0f, 8,
         {{"notches", "Notches", 1.0f, 8.0f, 4.0f},
          {"rate", "Rate", 0.01f, 20.0f, 0.5f, "Hz", true},
          {"freqMin", "Min", 20.0f, 4000.0f, 100.0f, "Hz", true},
          {"freqMax", "Max", 100.0f, 12000.0f, 800.0f, "Hz", true},
          {"ratio", "Razao", 1.0f, 4.0f, 1.5f},
          {"width", "Largura", 10.0f, 5000.0f, 1000.0f, "Hz", true},
          {"depth", "Depth", 0.0f, 2.0f, 1.0f},
          {"feedback", "Feedback", -0.99f, 0.99f, 0.3f}}},
        {FxKind::Flanger, "Flanger", "Flanger", "Modulacao", "flanger_stereo da phaflangers.lib do Faust (Julius O. Smith III, STK-4.3)", 1.0f, 6,
         {{"rate", "Rate", 0.01f, 20.0f, 0.25f, "Hz", true},
          {"delay", "Delay", 0.05f, 10.0f, 1.0f, "ms"},
          {"sweep", "Sweep", 0.0f, 10.0f, 3.0f, "ms"},
          {"depth", "Depth", 0.0f, 1.0f, 1.0f},
          {"feedback", "Feedback", -0.95f, 0.95f, 0.5f},
          {"invert", "Fase", 0.0f, 1.0f, 0.0f, "", false, "Normal|Invertida"}}},
        {FxKind::TapeDelay, "TapeDelay", "Delay de fita", "Delay", "Implementacao propria: filtros e saturacao no loop, wow & flutter", 0.3f, 8,
         {{"timeL", "Tempo E", 0.001f, 2.0f, 0.3f, "s", true},
          {"timeR", "Tempo D", 0.001f, 2.0f, 0.45f, "s", true},
          {"feedback", "Feedback", 0.0f, 0.95f, 0.35f},
          {"cross", "Ping-pong", 0.0f, 1.0f, 0.0f},
          {"lowCut", "Low cut", 10.0f, 2000.0f, 60.0f, "Hz", true},
          {"highCut", "High cut", 200.0f, 20000.0f, 8000.0f, "Hz", true},
          {"drive", "Drive", 0.0f, 1.0f, 0.0f},
          {"wow", "Wow", 0.0f, 1.0f, 0.0f}}},
        {FxKind::ReverbClassic, "ReverbClassic", "Reverb classico", "Reverb",
         "Room: Freeverb (Jezar, dominio publico); Hall: reverb do Mutable Instruments Clouds (MIT); Plate: MVerb (Martin Eastwood, GPLv3)",
         0.25f, 6,
         {{"type", "Tipo", 0.0f, 2.0f, 0.0f, "", false, "Room|Hall|Plate"},
          {"size", "Tamanho", 0.0f, 1.0f, 0.5f},
          {"damping", "Damping", 0.0f, 1.0f, 0.3f},
          {"width", "Largura", 0.0f, 1.0f, 1.0f},
          {"predelay", "Pre-delay", 0.0f, 250.0f, 0.0f, "ms"},
          {"freeze", "Freeze", 0.0f, 1.0f, 0.0f, "", false, "Nao|Sim"}}},
        {FxKind::Eq3, "Eq3", "EQ 3 bandas", "Filtro", "Biquads do Audio EQ Cookbook (Robert Bristow-Johnson)", 1.0f, 7,
         {{"lowFreq", "Grave", 20.0f, 1000.0f, 200.0f, "Hz", true},
          {"lowGain", "Ganho", -18.0f, 18.0f, 0.0f, "dB"},
          {"midFreq", "Medio", 100.0f, 10000.0f, 1000.0f, "Hz", true},
          {"midGain", "Ganho", -18.0f, 18.0f, 0.0f, "dB"},
          {"midQ", "Q", 0.1f, 10.0f, 0.7f, "", true},
          {"highFreq", "Agudo", 1000.0f, 18000.0f, 5000.0f, "Hz", true},
          {"highGain", "Ganho", -18.0f, 18.0f, 0.0f, "dB"}}},
        {FxKind::Compressor, "Compressor", "Compressor", "Dinamica", "ZamComp (Damien Zammit, GPLv2+), com knee suave corrigido", 1.0f, 6,
         {{"threshold", "Threshold", -60.0f, 0.0f, -18.0f, "dB"},
          {"ratio", "Ratio", 1.0f, 20.0f, 4.0f, "", true},
          {"attack", "Attack", 0.1f, 100.0f, 10.0f, "ms", true},
          {"release", "Release", 1.0f, 500.0f, 80.0f, "ms", true},
          {"knee", "Knee", 0.0f, 8.0f, 2.0f, "dB"},
          {"makeup", "Makeup", 0.0f, 30.0f, 0.0f, "dB"}}},
    };
    static_assert(sizeof(table) / sizeof(table[0]) == static_cast<size_t>(FxKind::Count), "fxInfo: um item por FxKind, na ordem");
    const int i = static_cast<int>(k);
    return table[i >= 0 && i < static_cast<int>(FxKind::Count) ? i : 0];
}

// tipo pelo nome estavel (JSON); desconhecido = Off
inline FxKind fxKindFromId(const std::string& id) {
    for (int i = 0; i < static_cast<int>(FxKind::Count); ++i)
        if (id == fxInfo(static_cast<FxKind>(i)).id) return static_cast<FxKind>(i);
    return FxKind::Off;
}

struct FxSlotParams {
    FxKind kind = FxKind::Off;
    bool on = true;
    float mix = 1.0f;          // 0 = so seco, 1 = so o efeito
    float p[kFxParams] = {};

    // troca o tipo com os valores padrao dele
    void setKind(FxKind k) {
        kind = k;
        const FxInfo& info = fxInfo(k);
        mix = info.defMix;
        for (int i = 0; i < kFxParams; ++i) p[i] = i < info.nParams ? info.params[i].def : 0.0f;
    }
    bool active() const { return kind != FxKind::Off && on; }
};

struct FxChainParams {
    FxSlotParams slot[kFxSlots];
    float level = 1.0f;        // 0..2
    float pan = 0.0f;          // -1..1 (balanco)
};

struct FxRackParams {
    FxChainParams chain[kFxChains];
    bool serial = false;       // true: cadeia 1 -> cadeia 2 (so a 2 sai)
    // por parte (A-D): nivel seco direto pra saida e envios pras cadeias.
    // Padrao: tudo entra na cadeia 1 (o rack vazio e identidade, como os
    // efeitos globais de antes).
    float dry[kFxParts] = {0.0f, 0.0f, 0.0f, 0.0f};
    float send[kFxParts][kFxChains] = {{1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.0f}};
};

inline void sanitize(FxRackParams& r) {
    auto f = [](float v, float lo, float hi, float def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
    for (auto& c : r.chain) {
        c.level = f(c.level, 0.0f, 2.0f, 1.0f);
        c.pan = f(c.pan, -1.0f, 1.0f, 0.0f);
        for (auto& s : c.slot) {
            if (static_cast<int>(s.kind) >= static_cast<int>(FxKind::Count)) s.kind = FxKind::Off;
            const FxInfo& info = fxInfo(s.kind);
            s.mix = f(s.mix, 0.0f, 1.0f, info.defMix);
            for (int i = 0; i < kFxParams; ++i) {
                if (i >= info.nParams) { s.p[i] = 0.0f; continue; }
                const FxParamInfo& pi = info.params[i];
                s.p[i] = f(s.p[i], pi.min, pi.max, pi.def);
                if (pi.choices) s.p[i] = std::round(s.p[i]);
            }
        }
    }
    for (int p = 0; p < kFxParts; ++p) {
        r.dry[p] = f(r.dry[p], 0.0f, 2.0f, 0.0f);
        for (float& s : r.send[p]) s = f(s, 0.0f, 2.0f, 0.0f);
    }
}

// Contexto de um bloco de audio (o mesmo pra todos os slots)
struct FxContext {
    double sampleRate = 48000.0;
    float bpm = 120.0f;
    double beatPos = 0.0;     // posicao de batida no inicio do bloco
};

// DSP de um tipo de efeito. process() trabalha no lugar (L/R entram secos e
// saem com o efeito a 100%); o mix do slot e aplicado pelo rack.
class FxProcessor {
public:
    virtual ~FxProcessor() = default;
    virtual void prepare(double sampleRate) = 0; // aloca (thread da UI)
    virtual void reset() = 0;                    // zera o estado (sem alocar)
    virtual void process(const FxSlotParams& s, float* L, float* R, int n, const FxContext& ctx) = 0;
    virtual float meter() const { return 0.0f; } // ex.: reducao de ganho em dB
};

namespace fxrack_detail {

class ChorusFx : public FxProcessor {
public:
    void prepare(double sr) override { fx_.setSampleRate(sr); }
    void reset() override { fx_.clear(); }
    void process(const FxSlotParams& s, float* L, float* R, int n, const FxContext&) override {
        ChorusParams p;
        p.enabled = true; p.rateHz = s.p[0]; p.depth = s.p[1]; p.mix = 1.0f;
        for (int i = 0; i < n; ++i) {
            const float l0 = L[i], r0 = R[i];
            fx_.process(p, L[i], R[i]);
            L[i] -= 0.5f * l0; // com mix 1 o Ensemble devolve molhado + metade do seco
            R[i] -= 0.5f * r0;
        }
    }
private:
    EnsembleChorus fx_;
};

class PhaserFx : public FxProcessor {
public:
    void prepare(double sr) override { fx_.setSampleRate(sr); }
    void reset() override { fx_.clear(); }
    void process(const FxSlotParams& s, float* L, float* R, int n, const FxContext&) override {
        PhaserParams p;
        p.enabled = true; p.notches = static_cast<int>(std::lround(s.p[0])); p.rateHz = s.p[1];
        p.freqMinHz = s.p[2]; p.freqMaxHz = s.p[3]; p.freqRatio = s.p[4]; p.notchWidthHz = s.p[5];
        p.depth = s.p[6]; p.feedback = s.p[7];
        for (int i = 0; i < n; ++i) fx_.process(p, L[i], R[i]);
    }
private:
    Phaser fx_;
};

class FlangerFx : public FxProcessor {
public:
    void prepare(double sr) override { fx_.setSampleRate(sr); }
    void reset() override { fx_.clear(); }
    void process(const FxSlotParams& s, float* L, float* R, int n, const FxContext&) override {
        FlangerParams p;
        p.enabled = true; p.rateHz = s.p[0]; p.delayMs = s.p[1]; p.sweepMs = s.p[2]; p.depth = s.p[3];
        p.feedback = s.p[4]; p.invert = s.p[5] >= 0.5f;
        for (int i = 0; i < n; ++i) fx_.process(p, L[i], R[i]);
    }
private:
    Flanger fx_;
};

class TapeDelayFx : public FxProcessor {
public:
    void prepare(double sr) override { fx_.setSampleRate(sr); }
    void reset() override { fx_.clear(); }
    void process(const FxSlotParams& s, float* L, float* R, int n, const FxContext&) override {
        DelayParams p;
        p.enabled = true; p.timeLeftSec = s.p[0]; p.timeRightSec = s.p[1]; p.feedback = s.p[2]; p.crossFeed = s.p[3];
        p.lowCutHz = s.p[4]; p.highCutHz = s.p[5]; p.drive = s.p[6]; p.wow = s.p[7]; p.mix = 1.0f; // so as repeticoes
        for (int i = 0; i < n; ++i) fx_.process(p, L[i], R[i]);
    }
private:
    TapeDelay fx_;
};

class ReverbClassicFx : public FxProcessor {
public:
    void prepare(double sr) override { fx_.setSampleRate(sr); }
    void reset() override { fx_.clear(); }
    void process(const FxSlotParams& s, float* L, float* R, int n, const FxContext&) override {
        ReverbParams p;
        p.enabled = true; p.type = static_cast<ReverbType>(std::clamp(static_cast<int>(std::lround(s.p[0])), 0, 2));
        p.roomSize = s.p[1]; p.damping = s.p[2]; p.width = s.p[3]; p.predelayMs = s.p[4]; p.freeze = s.p[5] >= 0.5f;
        p.mix = 1.0f; // so a cauda
        for (int i = 0; i < n; ++i) fx_.process(p, L[i], R[i]);
    }
private:
    ReverbBlock fx_;
};

class Eq3Fx : public FxProcessor {
public:
    void prepare(double sr) override { fx_.setSampleRate(sr); }
    void reset() override { fx_.clear(); }
    void process(const FxSlotParams& s, float* L, float* R, int n, const FxContext&) override {
        EqParams p;
        p.enabled = true; p.lowFreqHz = s.p[0]; p.lowGainDb = s.p[1]; p.midFreqHz = s.p[2]; p.midGainDb = s.p[3];
        p.midQ = s.p[4]; p.highFreqHz = s.p[5]; p.highGainDb = s.p[6];
        for (int i = 0; i < n; ++i) fx_.process(p, L[i], R[i]);
    }
private:
    Eq3 fx_;
};

class CompressorFx : public FxProcessor {
public:
    void prepare(double sr) override { fx_.setSampleRate(sr); }
    void reset() override { fx_.clear(); }
    void process(const FxSlotParams& s, float* L, float* R, int n, const FxContext&) override {
        CompressorParams p;
        p.enabled = true; p.thresholdDb = s.p[0]; p.ratio = s.p[1]; p.attackMs = s.p[2]; p.releaseMs = s.p[3];
        p.kneeDb = s.p[4]; p.makeupDb = s.p[5];
        for (int i = 0; i < n; ++i) fx_.process(p, L[i], R[i]);
    }
    float meter() const override { return fx_.gainReductionDb(); }
private:
    ZamCompressor fx_;
};

} // namespace fxrack_detail

// Cria (e prepara) o DSP de um tipo. Chamar FORA da thread de audio.
inline std::unique_ptr<FxProcessor> makeFxProcessor(FxKind k, double sampleRate) {
    using namespace fxrack_detail;
    std::unique_ptr<FxProcessor> p;
    switch (k) {
        case FxKind::Chorus:        p = std::make_unique<ChorusFx>(); break;
        case FxKind::Phaser:        p = std::make_unique<PhaserFx>(); break;
        case FxKind::Flanger:       p = std::make_unique<FlangerFx>(); break;
        case FxKind::TapeDelay:     p = std::make_unique<TapeDelayFx>(); break;
        case FxKind::ReverbClassic: p = std::make_unique<ReverbClassicFx>(); break;
        case FxKind::Eq3:           p = std::make_unique<Eq3Fx>(); break;
        case FxKind::Compressor:    p = std::make_unique<CompressorFx>(); break;
        case FxKind::Off: case FxKind::Count: return nullptr;
    }
    if (p) p->prepare(sampleRate);
    return p;
}

// =============================================================================
// O rack
// =============================================================================
class FxRack {
public:
    static constexpr int kMaxBlock = 256;

    FxRack() = default;
    FxRack(const FxRack&) = delete;
    FxRack& operator=(const FxRack&) = delete;
    ~FxRack() {
        for (auto& c : proc_) for (auto& s : c) delete s.load();
        for (auto& g : graveyard_) delete g.first;
    }

    // (Re)cria o DSP de todos os slots na taxa nova. Com o audio PARADO.
    void prepare(double sampleRate) {
        sampleRate_ = sampleRate;
        collect(true);
        for (int c = 0; c < kFxChains; ++c)
            for (int s = 0; s < kFxSlots; ++s) {
                delete proc_[c][s].exchange(nullptr);
                uiKind_[c][s] = FxKind::Off;
                audioProc_[c][s] = nullptr;
            }
        setParams(ui_);
    }

    // Thread da UI: publica um conjunto novo de parametros. Slots com tipo
    // novo ganham um DSP criado aqui (o antigo vai pro "cemiterio").
    void setParams(const FxRackParams& in) {
        FxRackParams p = in;
        sanitize(p);
        for (int c = 0; c < kFxChains; ++c)
            for (int s = 0; s < kFxSlots; ++s) {
                const FxKind k = p.chain[c].slot[s].kind;
                if (k == uiKind_[c][s]) continue;
                FxProcessor* fresh = makeFxProcessor(k, sampleRate_).release();
                FxProcessor* old = proc_[c][s].exchange(fresh, std::memory_order_acq_rel);
                if (old) graveyard_.push_back({old, blocks_.load(std::memory_order_acquire)});
                uiKind_[c][s] = k;
            }
        ui_ = p;
        // triple buffer: escreve num indice que nao e o publicado nem o anterior a ele
        int idx = 0;
        while (idx == published_ || idx == previous_) ++idx;
        snap_[idx] = p;
        previous_ = published_;
        published_ = idx;
        live_.store(idx, std::memory_order_release);
        collect(false);
    }
    const FxRackParams& params() const { return ui_; } // ultimo enviado pela UI

    // Thread de audio: parametros do bloco atual (pros envios por parte)
    const FxRackParams& beginBlock() {
        cur_ = &snap_[live_.load(std::memory_order_acquire)];
        return *cur_;
    }

    // Thread de audio. Entrada: L/R = soma SECA, in1/in2 = envios pras cadeias
    // (n amostras, n <= kMaxBlock). Saida em L/R. Chamar depois de beginBlock().
    void process(float* L, float* R, float* in1L, float* in1R, float* in2L, float* in2R, int n, const FxContext& ctx) {
        const FxRackParams& p = *cur_;
        runChain(0, p.chain[0], in1L, in1R, n, ctx);
        if (p.serial) {
            for (int i = 0; i < n; ++i) { in2L[i] += in1L[i]; in2R[i] += in1R[i]; }
        } else {
            for (int i = 0; i < n; ++i) { L[i] += in1L[i]; R[i] += in1R[i]; }
        }
        runChain(1, p.chain[1], in2L, in2R, n, ctx);
        for (int i = 0; i < n; ++i) { L[i] += in2L[i]; R[i] += in2R[i]; }
        for (int c = 0; c < kFxChains; ++c) {
            float pk = 0.0f;
            const float* xl = c == 0 ? in1L : in2L;
            const float* xr = c == 0 ? in1R : in2R;
            for (int i = 0; i < n; ++i) pk = std::max({pk, std::fabs(xl[i]), std::fabs(xr[i])});
            chainPeak_[c].store(pk, std::memory_order_relaxed);
        }
        blocks_.fetch_add(1, std::memory_order_acq_rel);
    }

    // pra UI (outra thread)
    float chainPeak(int c) const { return chainPeak_[c].load(std::memory_order_relaxed); }
    float slotMeter(int c, int s) const { return slotMeter_[c][s].load(std::memory_order_relaxed); }

private:
    void runChain(int c, const FxChainParams& ch, float* L, float* R, int n, const FxContext& ctx) {
        for (int s = 0; s < kFxSlots; ++s) {
            const FxSlotParams& sp = ch.slot[s];
            FxProcessor* proc = proc_[c][s].load(std::memory_order_acquire);
            const bool on = sp.active() && proc;
            if (!on) { wasOn_[c][s] = false; continue; }
            if (!wasOn_[c][s] || proc != audioProc_[c][s]) proc->reset(); // religado ou trocado: sem cauda velha
            wasOn_[c][s] = true;
            audioProc_[c][s] = proc;
            if (sp.mix >= 1.0f) {
                proc->process(sp, L, R, n, ctx);
            } else {
                std::memcpy(dryL_, L, sizeof(float) * static_cast<size_t>(n));
                std::memcpy(dryR_, R, sizeof(float) * static_cast<size_t>(n));
                proc->process(sp, L, R, n, ctx);
                const float w = sp.mix, d = 1.0f - sp.mix;
                for (int i = 0; i < n; ++i) { L[i] = dryL_[i] * d + L[i] * w; R[i] = dryR_[i] * d + R[i] * w; }
            }
            slotMeter_[c][s].store(proc->meter(), std::memory_order_relaxed);
        }
        if (ch.level != 1.0f || ch.pan != 0.0f) { // balanco: o lado oposto ao pan cai, o outro fica
            const float gl = ch.level * std::min(1.0f, 1.0f - ch.pan), gr = ch.level * std::min(1.0f, 1.0f + ch.pan);
            for (int i = 0; i < n; ++i) { L[i] *= gl; R[i] *= gr; }
        }
        for (int i = 0; i < n; ++i) { // rede de seguranca: um NaN nao pode travar a saida
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) {
                std::fill(L, L + n, 0.0f);
                std::fill(R, R + n, 0.0f);
                for (int s = 0; s < kFxSlots; ++s) wasOn_[c][s] = false;
                break;
            }
        }
    }

    // apaga DSPs que o audio ja nao pode estar usando (2 blocos depois da troca)
    void collect(bool all) {
        const uint64_t now = blocks_.load(std::memory_order_acquire);
        for (size_t i = 0; i < graveyard_.size();) {
            if (all || now >= graveyard_[i].second + 2) {
                delete graveyard_[i].first;
                graveyard_[i] = graveyard_.back();
                graveyard_.pop_back();
            } else {
                ++i;
            }
        }
    }

    double sampleRate_ = 48000.0;
    // parametros: copia da UI + triple buffer
    FxRackParams ui_;
    FxRackParams snap_[3];
    std::atomic<int> live_{0};
    int published_ = 0, previous_ = 1;
    const FxRackParams* cur_ = &snap_[0];
    // DSP por slot
    std::atomic<FxProcessor*> proc_[kFxChains][kFxSlots] = {};
    FxKind uiKind_[kFxChains][kFxSlots] = {};
    std::vector<std::pair<FxProcessor*, uint64_t>> graveyard_;
    std::atomic<uint64_t> blocks_{0};
    // estado do audio
    FxProcessor* audioProc_[kFxChains][kFxSlots] = {};
    bool wasOn_[kFxChains][kFxSlots] = {};
    float dryL_[kMaxBlock], dryR_[kMaxBlock];
    std::atomic<float> chainPeak_[kFxChains] = {};
    std::atomic<float> slotMeter_[kFxChains][kFxSlots] = {};
};

// -----------------------------------------------------------------------------
// Ponte com o painel antigo de efeitos globais (7 blocos fixos, ordem
// configuravel): cada bloco ligado vira um slot, na ordem, enchendo a cadeia 1
// e depois a 2 em serie (8 slots >= 7 blocos). Usado pela UI ate o painel da
// pedaleira e pelos testes dos efeitos antigos.
// -----------------------------------------------------------------------------
inline FxRackParams fxRackFromLegacy(const GlobalFxParams& g) {
    FxRackParams r;
    r.serial = true;
    int at = 0;
    auto next = [&]() -> FxSlotParams& { FxSlotParams& s = r.chain[at / kFxSlots].slot[at % kFxSlots]; ++at; return s; };
    for (GlobalFxType t : g.order) {
        switch (t) {
            case GlobalFxType::Chorus: {
                if (!g.chorus.enabled) break;
                auto& s = next(); s.setKind(FxKind::Chorus);
                s.p[0] = g.chorus.rateHz; s.p[1] = g.chorus.depth; s.mix = g.chorus.mix;
                break;
            }
            case GlobalFxType::Phaser: {
                if (!g.phaser.enabled) break;
                auto& s = next(); s.setKind(FxKind::Phaser);
                const auto& q = g.phaser;
                s.p[0] = static_cast<float>(q.notches); s.p[1] = q.rateHz; s.p[2] = q.freqMinHz; s.p[3] = q.freqMaxHz;
                s.p[4] = q.freqRatio; s.p[5] = q.notchWidthHz; s.p[6] = q.depth; s.p[7] = q.feedback; // (o "invert" antigo ficou de fora)
                break;
            }
            case GlobalFxType::Flanger: {
                if (!g.flanger.enabled) break;
                auto& s = next(); s.setKind(FxKind::Flanger);
                const auto& q = g.flanger;
                s.p[0] = q.rateHz; s.p[1] = q.delayMs; s.p[2] = q.sweepMs; s.p[3] = q.depth; s.p[4] = q.feedback; s.p[5] = q.invert ? 1.0f : 0.0f;
                break;
            }
            case GlobalFxType::Delay: {
                if (!g.delay.enabled) break;
                auto& s = next(); s.setKind(FxKind::TapeDelay);
                const auto& q = g.delay;
                s.p[0] = q.timeLeftSec; s.p[1] = q.timeRightSec; s.p[2] = q.feedback; s.p[3] = q.crossFeed;
                s.p[4] = q.lowCutHz; s.p[5] = q.highCutHz; s.p[6] = q.drive; s.p[7] = q.wow; s.mix = q.mix;
                break;
            }
            case GlobalFxType::Reverb: {
                if (!g.reverb.enabled) break;
                auto& s = next(); s.setKind(FxKind::ReverbClassic);
                const auto& q = g.reverb;
                s.p[0] = static_cast<float>(q.type); s.p[1] = q.roomSize; s.p[2] = q.damping; s.p[3] = q.width;
                s.p[4] = q.predelayMs; s.p[5] = q.freeze ? 1.0f : 0.0f; s.mix = q.mix;
                break;
            }
            case GlobalFxType::Eq: {
                if (!g.eq.enabled) break;
                auto& s = next(); s.setKind(FxKind::Eq3);
                const auto& q = g.eq;
                s.p[0] = q.lowFreqHz; s.p[1] = q.lowGainDb; s.p[2] = q.midFreqHz; s.p[3] = q.midGainDb;
                s.p[4] = q.midQ; s.p[5] = q.highFreqHz; s.p[6] = q.highGainDb;
                break;
            }
            case GlobalFxType::Compressor: {
                if (!g.compressor.enabled) break;
                auto& s = next(); s.setKind(FxKind::Compressor);
                const auto& q = g.compressor;
                s.p[0] = q.thresholdDb; s.p[1] = q.ratio; s.p[2] = q.attackMs; s.p[3] = q.releaseMs; s.p[4] = q.kneeDb; s.p[5] = q.makeupDb;
                break;
            }
        }
    }
    return r;
}

} // namespace dx7fm
