#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace dx7fm {

constexpr int kNumOps = 8;
constexpr int kNumModRoutes = 8;
constexpr int kNumPartials = 64; // osciladores senoidais da forma de onda Additive (multiplo de 8: kernel SIMD)
constexpr float kMaxPartialRatio = 128.0f; // razao maxima de um parcial (64 harmonicos impares chegam a 127)

struct EGParams {
    uint8_t rate[4]  = {99, 99, 99, 50};
    uint8_t level[4] = {99, 99, 99, 0};
};

struct PEGParams {
    uint8_t rate[4]  = {99, 99, 99, 99};
    uint8_t level[4] = {50, 50, 50, 50};
    float rangeOctaves = 4.0f;
};

// Sine..Noise: formas de onda "classicas" ja existentes. HalfSine..LogSaw:
// [ADICIONADO em 2026-09-21] reconstrucao propria das 8 formas de onda do
// motor AFM da Yamaha SY77/TX81Z (Sine e a primeira das 8; as outras 7 sao
// as extras aqui) -- ver ARQUITETURA.txt secao 5 pro disclaimer completo:
// NAO sao extraidas da ROM real, sao uma aproximacao matematica baseada
// nas descricoes publicamente conhecidas dessas formas (seno cortado,
// retificado, etc.), no mesmo espirito de egRateToSpeed()/
// egLevelToAmplitude() (aproximacao propria, nao a tabela exata do hardware).
enum class Waveform : uint8_t {
    Sine, Triangle, Saw, Square, Noise,
    HalfSine, AbsSine, QuarterSine, AlternatingSine, CamelSine, SquaredSine, LogSaw,
    // [ADICIONADO em 2026-10-18] soma de kNumPartials osciladores senoidais
    // com amplitude e razao de frequencia LIVRES (inarmonicos: sinos,
    // tambores...), morph A->B pelo fxEG -- ver OperatorParams::additiveA/B.
    // SEMPRE no fim, pra nao renumerar as anteriores.
    Additive
};

enum class FxType : uint8_t {
    Bypass, LowPass, BandPass, HighPass, RingMod, Saturate, Crush, FreqShift, Comb,
    Fold // [ADICIONADO em 2026-09-23] wavefolder; SEMPRE no fim, pra nao renumerar os anteriores
};

enum class LfoWave : uint8_t { Triangle, SawUp, SawDown, Square, Sine, SampleHold };

struct LFOParams {
    float rate = 5.0f;          // Hz
    LfoWave wave = LfoWave::Triangle;
    float delaySec = 0.0f;
    bool keySync = false;
    float pitchDepth = 0.0f;    // 0..1, profundidade maxima
    float ampDepth = 0.0f;      // 0..1, profundidade maxima
};

struct KeyLevelScaling {
    uint8_t breakpoint = 60;    // nota MIDI
    int8_t leftDepth = 0;       // dB-ish por oitava
    int8_t rightDepth = 0;
    bool leftExp = false;
    bool rightExp = false;
};

struct OperatorParams {
    bool fixedFreq = false;
    float coarseRatio = 1.0f;
    float fineCents = 0.0f;
    float fixedHz = 440.0f;
    float detuneCents = 0.0f;
    uint8_t outputLevel = 99;   // 0..99
    EGParams ampEG;
    EGParams fxEG;
    KeyLevelScaling levelScaling;
    uint8_t rateScaling = 0;    // 0..7
    uint8_t velocitySens = 0;   // 0..7
    uint8_t lfoAmpModSens = 0;  // 0..3
    uint8_t lfoPitchModSens = 0;// 0..7
    float feedback = 0.0f;      // 0..1, auto-modulacao; SOMADO com matrix.mod[i][i] (os dois controles se acumulam)
    Waveform waveform = Waveform::Sine;
    float phaseDistortion = 0.0f; // 0..1
    FxType fxType = FxType::Bypass;
    float fxParam1 = 1000.0f;
    float fxParam2 = 0.5f;
    int ringModSource = -1;
    bool fxEGReleaseWithAmp = true; // se true, fxEG recebe noteOff() junto com ampEG
                                     // (filtro/efeito "fecha" no release); se false, o
                                     // fxEG completa seu proprio ciclo de 4 estagios
                                     // independentemente da tecla ser solta.
    float pegDepth = 0.0f;      // 0..1
    bool pegAffectsFixed = false;

    // [ADICIONADO em 2026-10-18; 8 -> 64 parciais em 2026-10-20] so usados
    // com waveform == Additive: kNumPartials osciladores senoidais reais. Parcial k soa em ratio[k] * f do operador
    // com amplitude amp[k]. A e B sao dois espectros; a posicao do morph
    // entre eles e morphDepth * fxEG/99 (+ ModDest::OperatorMorph) -- o MESMO
    // fxEG que modula o FX do operador -- e o morph interpola amplitudes E
    // razoes (um parcial pode deslizar de frequencia ao longo da nota).
    // Default: seno puro (so o parcial 1, razoes harmonicas 1..8).
    struct PartialSet {
        // alinhado a 32 bytes: o kernel SIMD (dx7_dsp.h) le 8 parciais por vez
        alignas(32) float amp[kNumPartials];
        alignas(32) float ratio[kNumPartials];
        PartialSet() {
            for (int k = 0; k < kNumPartials; ++k) { amp[k] = 0.0f; ratio[k] = static_cast<float>(k + 1); }
            amp[0] = 1.0f;
        }
    };
    PartialSet additiveA, additiveB;
    float morphDepth = 0.0f;    // 0..1
};

enum class ModSource : uint8_t { None, LFO1, LFO2, ModWheel, ChannelAT, PolyAT, PitchBend };
enum class ModDest : uint8_t { None, OperatorLevel, OperatorFx1, OperatorFeedback, VoicePitch,
                               OperatorMorph }; // OperatorMorph: [ADICIONADO em 2026-10-18], soma na posicao do morph A->B (Additive)

struct ModRoute {
    ModSource src = ModSource::None;
    ModDest dest = ModDest::None;
    int opIndex = 0;
    float depth = 0.0f; // -1..1
};

struct AlgorithmMatrix {
    float mod[kNumOps][kNumOps] = {};   // mod[i][j]: quanto j modula (PM) i; diagonal = feedback
    bool ring[kNumOps][kNumOps] = {};   // ring[i][j]: conexao j->i por multiplicacao (RM)
    float outputMix[kNumOps] = {};      // quanto de cada operador vai pro bus de saida da voz
};

struct VoiceParams {
    OperatorParams op[kNumOps];
    AlgorithmMatrix matrix;
    PEGParams peg;
    LFOParams lfo1, lfo2;
    ModRoute modMatrix[kNumModRoutes];

    VoiceParams() {
        // patch minimo tocavel: op0 e a unica portadora, op1 modula op0
        matrix.outputMix[0] = 1.0f;
        matrix.mod[0][1] = 1.0f;
        op[1].outputLevel = 60;
    }
};

enum class VoiceMode : uint8_t { Poly, Mono, Unison };

struct PartParams {
    std::string name = "Init";
    VoiceParams voice;
    int midiChannel = 0;        // 0 = omni
    uint8_t loKey = 0, hiKey = 127;
    uint8_t loVel = 0, hiVel = 127;
    int8_t transpose = 0;       // semitons, aplicado ANTES de tocar
    float level = 1.0f;
    float pan = 0.0f;           // -1..1
    VoiceMode voiceMode = VoiceMode::Poly;
    int unisonCount = 1;
    float unisonDetuneCents = 10.0f;
    float unisonSpread = 0.0f;
    bool glideEnabled = false;
    float glideTimeSec = 0.05f;
    bool glideLegatoOnly = false;
    bool monoLegatoEnvReuse = true;
    int reservedVoices = 0;
    bool enabled = true;
    float bendRangeSemis = 2.0f;
};

struct GlobalParams {
    double sampleRate = 48000.0;
    int maxVoices = 64;
    int oversample = 1; // placeholder, ainda nao usado
};

// [ADICIONADO em 2026-09-21] Efeitos globais POS-MIX: aplicados uma vez na
// soma das 4 partes (Synth::renderStereo), nao por operador/voz como os FX
// de dx7_params.h::OperatorParams. Ver dx7_globalfx.h pro DSP.

// [REESCRITO em 2026-10-18] Cada bloco abaixo e baseado num efeito open
// source de referencia -- origem e licenca de cada um em dx7_globalfx.h e
// em THIRD_PARTY_NOTICES.md.

// Chorus de "ensemble" (string machine): base = Ensemble do Mutable
// Instruments Rings (MIT). 3 leituras moduladas por canal, 2 LFOs (lento e
// rapido, este ~8.7x o lento) em fases de 120 graus.
struct ChorusParams {
    bool enabled = false;
    float rateHz = 0.75f;     // LFO lento (o rapido acompanha na proporcao do Rings)
    float depth = 0.5f;       // 0..1 (1 = +-128 amostras a 48kHz, como no Rings)
    float mix = 0.5f;         // 0..1 (amount do Rings: wet*mix + dry*(1 - mix/2))
};

// Phaser: base = phaser2_stereo da phaflangers.lib do Faust (Julius O.
// Smith III, licenca STK-4.3/MIT). Cadeia de all-pass de 2a ordem (TPT)
// com notches varridos por LFO, L e R em quadratura.
struct PhaserParams {
    bool enabled = false;
    int notches = 4;          // 1..8
    float rateHz = 0.5f;
    float freqMinHz = 100.0f; // primeiro notch varre entre min e max
    float freqMaxHz = 800.0f;
    float freqRatio = 1.5f;   // razao entre notches vizinhos
    float notchWidthHz = 1000.0f;
    float depth = 1.0f;       // 0 = seco, 1 = phaser, 2 = vibrato (so all-pass)
    float feedback = 0.3f;    // -0.99..0.99
    bool invert = false;
};

// Flanger: base = flanger_stereo da phaflangers.lib do Faust (STK-4.3).
struct FlangerParams {
    bool enabled = false;
    float rateHz = 0.25f;
    float delayMs = 1.0f;     // atraso minimo
    float sweepMs = 3.0f;     // quanto o LFO soma ao atraso (varredura)
    float depth = 1.0f;       // 0..1
    float feedback = 0.5f;    // -0.95..0.95
    bool invert = false;
};

// Delay estereo estilo fita (implementacao propria): feedback com
// cross-feed (ping-pong), filtros passa-altas/passa-baixas e saturacao no
// loop (cada repeticao fica mais escura/suja), wow & flutter no tempo.
struct DelayParams {
    bool enabled = false;
    float timeLeftSec = 0.3f;
    float timeRightSec = 0.45f; // diferente do esquerdo por padrao (delay "duplo" com largura estereo)
    float feedback = 0.35f;     // 0..0.95
    float crossFeed = 0.0f;     // 0..1 (1 = ping-pong total entre os canais)
    float mix = 0.25f;
    float lowCutHz = 60.0f;     // passa-altas no loop de feedback
    float highCutHz = 8000.0f;  // passa-baixas no loop de feedback
    float drive = 0.0f;         // 0..1 saturacao (tanh) no loop
    float wow = 0.0f;           // 0..1 modulacao lenta+rapida do tempo (fita)
};

// Reverb com 3 algoritmos de referencia:
//   Room  = Freeverb (Jezar, dominio publico) -- 8 combs + 4 all-pass
//   Hall  = reverb do Mutable Instruments Clouds (MIT) -- Griesinger/Dattorro
//   Plate = MVerb (Martin Eastwood, GPLv3) -- plate de Dattorro
enum class ReverbType : uint8_t { Room, Hall, Plate };

struct ReverbParams {
    bool enabled = false;
    ReverbType type = ReverbType::Room;
    float roomSize = 0.5f;  // 0..1 (tamanho/tempo de decaimento)
    float damping = 0.3f;   // 0..1, mais alto = cauda mais curta/escura
    float mix = 0.25f;
    float width = 1.0f;     // 0..1, separacao estereo da cauda
    float predelayMs = 0.0f;// 0..250
    bool freeze = false;    // so no Room (modo "freeze" do Freeverb: cauda infinita)
};

// EQ de 3 bandas: shelving grave/agudo + peaking no meio, biquads do
// "Audio EQ Cookbook" (Robert Bristow-Johnson).
struct EqParams {
    bool enabled = false;
    float lowFreqHz = 200.0f;
    float lowGainDb = 0.0f;   // -18..18
    float midFreqHz = 1000.0f;
    float midGainDb = 0.0f;
    float midQ = 0.7f;
    float highFreqHz = 5000.0f;
    float highGainDb = 0.0f;
};

// Compressor: base = ZamComp (Damien Zammit, GPLv2+), detector com as duas
// pontas ligadas (stereo link pelo maior |L|,|R|).
struct CompressorParams {
    bool enabled = false;
    float thresholdDb = -18.0f; // -60..0
    float ratio = 4.0f;         // 1..20
    float attackMs = 10.0f;     // 0.1..100
    float releaseMs = 80.0f;    // 1..500
    float kneeDb = 2.0f;        // 0..8 (como no ZamComp: largura = 6*knee dB)
    float makeupDb = 0.0f;      // 0..30
};

enum class GlobalFxType : uint8_t { Chorus, Phaser, Flanger, Delay, Reverb, Eq, Compressor };
constexpr int kNumGlobalFx = 7;

struct GlobalFxParams {
    ChorusParams chorus;
    PhaserParams phaser;
    FlangerParams flanger;
    DelayParams delay;
    ReverbParams reverb;
    EqParams eq;
    CompressorParams compressor;
    // Ordem da cadeia, editavel na UI. Precisa ser uma permutacao dos 7
    // tipos; se nao for (ex.: lida no meio de uma troca feita pela UI em
    // outra thread), o processamento usa kDefaultGlobalFxOrder.
    GlobalFxType order[kNumGlobalFx] = {GlobalFxType::Phaser, GlobalFxType::Flanger, GlobalFxType::Chorus,
                                        GlobalFxType::Delay, GlobalFxType::Reverb, GlobalFxType::Eq,
                                        GlobalFxType::Compressor};
};

constexpr GlobalFxType kDefaultGlobalFxOrder[kNumGlobalFx] = {
    GlobalFxType::Phaser, GlobalFxType::Flanger, GlobalFxType::Chorus, GlobalFxType::Delay,
    GlobalFxType::Reverb, GlobalFxType::Eq, GlobalFxType::Compressor};

// [ADICIONADO em 2026-09-23] Forca todos os campos pra faixa valida. A UI
// (ctrl+clique num slider aceita qualquer numero digitado) e o load de JSON
// podiam entregar valores fora da faixa -- ex.: fxParam2 enorme no Crush
// gerava NaN, LFO rate negativo nunca dava a volta na fase. Synth::recompile()
// e os *FromJson de dx7_patch_io.h chamam isso antes de publicar/devolver.
namespace detail {
inline float sanF(float v, float lo, float hi, float def) {
    if (!std::isfinite(v)) return def;
    return std::clamp(v, lo, hi);
}
template <typename E> inline E sanEnum(E v, E maxValue) {
    return static_cast<int>(v) > static_cast<int>(maxValue) ? E{} : v;
}
inline void sanEG(uint8_t (&rate)[4], uint8_t (&level)[4]) {
    for (int i = 0; i < 4; ++i) {
        rate[i] = std::min<uint8_t>(rate[i], 99);
        level[i] = std::min<uint8_t>(level[i], 99);
    }
}
} // namespace detail

inline void sanitize(OperatorParams& o) {
    using namespace detail;
    o.coarseRatio = sanF(o.coarseRatio, 0.01f, 32.0f, 1.0f);
    o.fineCents = sanF(o.fineCents, -1200.0f, 1200.0f, 0.0f);
    o.fixedHz = sanF(o.fixedHz, 0.1f, 20000.0f, 440.0f);
    o.detuneCents = sanF(o.detuneCents, -1200.0f, 1200.0f, 0.0f);
    o.outputLevel = std::min<uint8_t>(o.outputLevel, 99);
    sanEG(o.ampEG.rate, o.ampEG.level);
    sanEG(o.fxEG.rate, o.fxEG.level);
    o.levelScaling.breakpoint = std::min<uint8_t>(o.levelScaling.breakpoint, 127);
    o.levelScaling.leftDepth = static_cast<int8_t>(std::clamp<int>(o.levelScaling.leftDepth, -99, 99));
    o.levelScaling.rightDepth = static_cast<int8_t>(std::clamp<int>(o.levelScaling.rightDepth, -99, 99));
    o.rateScaling = std::min<uint8_t>(o.rateScaling, 7);
    o.velocitySens = std::min<uint8_t>(o.velocitySens, 7);
    o.lfoAmpModSens = std::min<uint8_t>(o.lfoAmpModSens, 3);
    o.lfoPitchModSens = std::min<uint8_t>(o.lfoPitchModSens, 7);
    o.feedback = sanF(o.feedback, 0.0f, 1.0f, 0.0f);
    o.waveform = sanEnum(o.waveform, Waveform::Additive);
    o.phaseDistortion = sanF(o.phaseDistortion, 0.0f, 1.0f, 0.0f);
    o.fxType = sanEnum(o.fxType, FxType::Fold);
    o.fxParam1 = sanF(o.fxParam1, 0.0f, 20000.0f, 1000.0f);
    o.fxParam2 = sanF(o.fxParam2, 0.0f, 16.0f, 0.5f);
    o.ringModSource = std::clamp(o.ringModSource, -1, kNumOps - 1);
    o.pegDepth = sanF(o.pegDepth, 0.0f, 1.0f, 0.0f);
    for (auto* set : {&o.additiveA, &o.additiveB}) {
        for (int k = 0; k < kNumPartials; ++k) {
            set->amp[k] = sanF(set->amp[k], 0.0f, 1.0f, 0.0f);
            set->ratio[k] = sanF(set->ratio[k], 0.01f, kMaxPartialRatio, static_cast<float>(k + 1));
        }
    }
    o.morphDepth = sanF(o.morphDepth, 0.0f, 1.0f, 0.0f);
}

inline void sanitize(LFOParams& l) {
    using namespace detail;
    l.rate = sanF(l.rate, 0.01f, 50.0f, 5.0f);
    l.wave = sanEnum(l.wave, LfoWave::SampleHold);
    l.delaySec = sanF(l.delaySec, 0.0f, 10.0f, 0.0f);
    l.pitchDepth = sanF(l.pitchDepth, 0.0f, 1.0f, 0.0f);
    l.ampDepth = sanF(l.ampDepth, 0.0f, 1.0f, 0.0f);
}

inline void sanitize(VoiceParams& v) {
    using namespace detail;
    for (auto& o : v.op) sanitize(o);
    for (int i = 0; i < kNumOps; ++i) {
        for (int j = 0; j < kNumOps; ++j) v.matrix.mod[i][j] = sanF(v.matrix.mod[i][j], -1.0f, 1.0f, 0.0f);
        v.matrix.outputMix[i] = sanF(v.matrix.outputMix[i], 0.0f, 1.0f, 0.0f);
    }
    sanEG(v.peg.rate, v.peg.level);
    v.peg.rangeOctaves = sanF(v.peg.rangeOctaves, 0.1f, 8.0f, 4.0f);
    sanitize(v.lfo1);
    sanitize(v.lfo2);
    for (auto& r : v.modMatrix) {
        r.src = sanEnum(r.src, ModSource::PitchBend);
        r.dest = sanEnum(r.dest, ModDest::OperatorMorph);
        r.opIndex = std::clamp(r.opIndex, 0, kNumOps - 1);
        r.depth = sanF(r.depth, -1.0f, 1.0f, 0.0f);
    }
}

inline void sanitize(PartParams& p) {
    using namespace detail;
    sanitize(p.voice);
    p.midiChannel = std::clamp(p.midiChannel, 0, 16);
    p.loKey = std::min<uint8_t>(p.loKey, 127);
    p.hiKey = std::min<uint8_t>(p.hiKey, 127);
    p.loVel = std::min<uint8_t>(p.loVel, 127);
    p.hiVel = std::min<uint8_t>(p.hiVel, 127);
    p.transpose = static_cast<int8_t>(std::clamp<int>(p.transpose, -48, 48));
    p.level = sanF(p.level, 0.0f, 2.0f, 1.0f);
    p.pan = sanF(p.pan, -1.0f, 1.0f, 0.0f);
    p.voiceMode = sanEnum(p.voiceMode, VoiceMode::Unison);
    p.unisonCount = std::clamp(p.unisonCount, 1, 8);
    p.unisonDetuneCents = sanF(p.unisonDetuneCents, 0.0f, 100.0f, 10.0f);
    p.unisonSpread = sanF(p.unisonSpread, 0.0f, 1.0f, 0.0f);
    p.glideTimeSec = sanF(p.glideTimeSec, 0.0f, 10.0f, 0.05f);
    p.reservedVoices = std::clamp(p.reservedVoices, 0, 1024);
    p.bendRangeSemis = sanF(p.bendRangeSemis, 0.0f, 48.0f, 2.0f);
}

} // namespace dx7fm
