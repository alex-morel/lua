#pragma once
#include <cstdint>
#include <string>

namespace dx7fm {

constexpr int kNumOps = 8;
constexpr int kNumModRoutes = 8;

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
    HalfSine, AbsSine, QuarterSine, AlternatingSine, CamelSine, SquaredSine, LogSaw
};

enum class FxType : uint8_t {
    Bypass, LowPass, BandPass, HighPass, RingMod, Saturate, Crush, FreqShift, Comb
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
    float feedback = 0.0f;      // 0..1, usado se matrix.mod[i][i] != 0
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
};

enum class ModSource : uint8_t { None, LFO1, LFO2, ModWheel, ChannelAT, PolyAT, PitchBend };
enum class ModDest : uint8_t { None, OperatorLevel, OperatorFx1, OperatorFeedback, VoicePitch };

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

struct ChorusParams {
    bool enabled = false;
    float rateHz = 0.5f;      // velocidade do LFO de modulacao
    float depthMs = 3.0f;     // profundidade da modulacao do delay
    float baseDelayMs = 15.0f;
    float mix = 0.35f;        // 0..1, seco/molhado
    int voices = 3;           // 1..4 linhas de delay moduladas
};

struct DelayParams {
    bool enabled = false;
    float timeLeftSec = 0.3f;
    float timeRightSec = 0.45f; // diferente do esquerdo por padrao (delay "duplo" com largura estereo)
    float feedback = 0.35f;     // 0..0.95
    float crossFeed = 0.0f;     // 0..1 (1 = ping-pong total entre os canais)
    float mix = 0.25f;
};

struct ReverbParams {
    bool enabled = false;
    float roomSize = 0.5f;  // 0..1
    float damping = 0.3f;   // 0..1, mais alto = cauda mais curta/escura
    float mix = 0.25f;
    float width = 1.0f;     // 0..1, separacao estereo da cauda
};

struct GlobalFxParams {
    ChorusParams chorus;
    DelayParams delay;
    ReverbParams reverb;
};

} // namespace dx7fm
