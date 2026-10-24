#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dx7fm {

constexpr int kNumOps = 8;
constexpr int kNumModRoutes = 16; // [2026-10-22] era 8; patches antigos carregam as 8 primeiras
constexpr int kNumLfos = 4;       // [2026-10-22] lfo1..lfo4 (eram 2)
constexpr int kNumModEnvs = 3;    // [2026-10-22] envelopes de modulacao (so fontes da matriz)
constexpr int kMaxAlgoSlots = 8;  // [2026-10-24] algoritmos por patch pro morph (slot 1 = VoiceParams::matrix)
constexpr int kNumPartials = 64; // osciladores senoidais da forma de onda Additive (multiplo de 8: kernel SIMD)
constexpr float kMaxPartialRatio = 128.0f; // razao maxima de um parcial (64 harmonicos impares chegam a 127)
constexpr int kWaveSize = 2048; // amostras de UM ciclo de onda da Waveform::Wavetable (= frame do Serum/Vital)

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
    Additive,
    // [ADICIONADO em 2026-10-22] duas ondas de um ciclo (A e B) com morph
    // entre elas -- ver OperatorParams::wavetable e dx7_wavetable.h.
    Wavetable
};

enum class FxType : uint8_t {
    Bypass, LowPass, BandPass, HighPass, RingMod, Saturate, Crush, FreqShift, Comb,
    Fold // [ADICIONADO em 2026-09-23] wavefolder; SEMPRE no fim, pra nao renumerar os anteriores
};

// SmoothRandom [ADICIONADO em 2026-10-22, no fim]: S&H suave (um valor
// aleatorio novo por ciclo, com transicao em cosseno ate ele).
enum class LfoWave : uint8_t { Triangle, SawUp, SawDown, Square, Sine, SampleHold, SmoothRandom };

// [ADICIONADO em 2026-10-22] divisoes de tempo do LFO sincronizado: duracao
// de UM ciclo em batidas (seminimas) -- 1/4 = 1 batida, 1/4. = 1.5, 1/4T = 2/3.
constexpr int kNumLfoDivisions = 16;
inline const char* lfoDivisionName(int d) {
    static const char* n[kNumLfoDivisions] = {"4/1", "2/1", "1/1", "1/2", "1/2.", "1/2T", "1/4", "1/4.",
                                              "1/4T", "1/8", "1/8.", "1/8T", "1/16", "1/16.", "1/16T", "1/32"};
    return n[d < 0 || d >= kNumLfoDivisions ? 6 : d];
}
inline float lfoDivisionBeats(int d) {
    static const float b[kNumLfoDivisions] = {16.0f, 8.0f, 4.0f, 2.0f, 3.0f, 4.0f / 3.0f, 1.0f, 1.5f,
                                              2.0f / 3.0f, 0.5f, 0.75f, 1.0f / 3.0f, 0.25f, 0.375f, 1.0f / 6.0f, 0.125f};
    return b[d < 0 || d >= kNumLfoDivisions ? 6 : d];
}

struct LFOParams {
    float rate = 5.0f;          // Hz (quando tempoSync = false)
    LfoWave wave = LfoWave::Triangle;
    float delaySec = 0.0f;
    bool keySync = false;
    float pitchDepth = 0.0f;    // 0..1, profundidade maxima
    float ampDepth = 0.0f;      // 0..1, profundidade maxima
    // [ADICIONADO em 2026-10-22] sincronia com o BPM global (Synth::setTempoBpm):
    // um ciclo dura lfoDivisionBeats(division) batidas
    bool tempoSync = false;
    uint8_t division = 6;       // indice em lfoDivisionName (6 = 1/4)
    // [ADICIONADO em 2026-10-24] quantizacao em degraus: 0 = suave; 2..32 =
    // numero de niveis entre -1 e 1 (escada), aplicado depois da forma
    uint8_t steps = 0;
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
    // [ADICIONADO em 2026-10-20] key tracking do efeito (0..1): FX1 (corte dos
    // filtros, afinacao do Comb, Hz do FreqShift, taxa do Crush) multiplicado
    // por 2^(fxKeyTrack * oitavas da nota em relacao ao C4). 1 = acompanha a
    // nota (uma oitava acima dobra), 0 = Hz fixo (como antes).
    float fxKeyTrack = 0.0f;
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
    float morphDepth = 0.0f;    // 0..1; profundidade do fxEG no morph (Additive E Wavetable)

    // [ADICIONADO em 2026-10-22] so usado com waveform == Wavetable: duas
    // ondas de um ciclo (kWaveSize amostras) e o morph entre elas, em
    // clamp(position + morphDepth * fxEG/99 + ModDest::OperatorMorph, 0, 1).
    // As ondas ficam no heap e sao IMUTAVEIS (shared_ptr<const>): copiar o
    // patch (staging -> snapshot a cada recompile) so incrementa o contador, e
    // editar uma onda = trocar o ponteiro por um vetor novo. nullptr = seno.
    // As tabelas com anti-aliasing sao montadas no recompile (dx7_wavetable.h).
    struct WavetableParams {
        std::shared_ptr<const std::vector<float>> waveA, waveB;
        float position = 0.0f;     // 0..1 (0 = A, 1 = B)
        bool spectralMorph = true; // true: interpola os harmonicos; false: crossfade das amostras
    };
    WavetableParams wavetable;
};

// KeyNote/Velocity [ADICIONADOS em 2026-10-20, no fim pra nao renumerar patches]:
// KeyNote = (nota - 60) / 60, clampado -1..1 (C4 = 0, +1 cinco oitavas acima);
// Velocity = velocity / 127 (0..1). Os dois sao fixos durante a nota.
// LFO3/LFO4/Env1..Env3 [ADICIONADOS em 2026-10-22, no fim]: LFOs -1..1,
// envelopes de modulacao 0..1 (nivel/99).
enum class ModSource : uint8_t { None, LFO1, LFO2, ModWheel, ChannelAT, PolyAT, PitchBend, KeyNote, Velocity,
                                 LFO3, LFO4, Env1, Env2, Env3,
                                 Macro1, Macro2, Macro3, Macro4 }; // [2026-10-24] macros da parte, 0..1
// OperatorMorph: [ADICIONADO em 2026-10-18], soma na posicao do morph A->B (Additive/Wavetable).
// [ADICIONADOS em 2026-10-22, no fim]:
//   VoicePan      soma no pan da voz (-1..1; +1 = toda pra direita)
//   VoiceLevel    volume da voz x max(0, 1 + quantidade)
//   OperatorPitch afinacao do operador opIndex, quantidade x 12 semitons (como VoicePitch)
//   OperatorFx2   fxParam2 do operador x (1 + quantidade) (Q, drive, bits, feedback, simetria)
//   LfoRate       velocidade do LFO opIndex (0..3) x 2^(3 x quantidade): +-3 oitavas
//   LfoDepth      saida do LFO opIndex x max(0, 1 + quantidade) (em todos os usos dele)
// LfoRate/LfoDepth usam os valores da amostra ANTERIOR (um LFO pode modular o
// proprio rate sem ciclo dentro da mesma amostra).
enum class ModDest : uint8_t { None, OperatorLevel, OperatorFx1, OperatorFeedback, VoicePitch,
                               OperatorMorph, VoicePan, VoiceLevel, OperatorPitch, OperatorFx2, LfoRate, LfoDepth,
                               AlgoMorph }; // [2026-10-24] soma na posicao do morph entre algoritmos (0..1)
inline bool modDestPerOperator(ModDest d) {
    return d == ModDest::OperatorLevel || d == ModDest::OperatorFx1 || d == ModDest::OperatorFeedback || d == ModDest::OperatorMorph ||
           d == ModDest::OperatorPitch || d == ModDest::OperatorFx2;
}
inline bool modDestPerLfo(ModDest d) { return d == ModDest::LfoRate || d == ModDest::LfoDepth; }

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

// [ADICIONADO em 2026-10-24] envelope de modulacao = EG estilo DX + loop e sync.
// loop: ao chegar no sustain (L3) volta ao estagio 1 a partir do nivel atual
// (ciclo L3 -> L1 -> L2 -> L3) ate o note off, que faz o release normal.
// tempoSync: o envelope inteiro anda mais rapido/devagar pra esse ciclo durar
// lfoDivisionBeats(division) batidas do BPM global (mesmas divisoes dos LFOs).
struct ModEnvParams : EGParams {
    bool loop = false;
    bool tempoSync = false;
    uint8_t division = 6; // 1/4
    // padrao: sobe em ~0.3s, decai pra 50 e solta em ~1s
    ModEnvParams() {
        rate[0] = 60; rate[1] = 45; rate[2] = 45; rate[3] = 45;
        level[0] = 99; level[1] = 70; level[2] = 50; level[3] = 0;
    }
};

// Topologia que vale pra QUALQUER ponto do morph: conexao presente em algum
// slot ativo (pesos 1/0; so a existencia importa pra ordem de avaliacao e pros
// operadores usados). Com 1 algoritmo, e a propria matriz.
struct VoiceParams;
AlgorithmMatrix algoUnion(const VoiceParams& v);

struct VoiceParams {
    OperatorParams op[kNumOps];
    AlgorithmMatrix matrix;
    PEGParams peg;
    LFOParams lfo1, lfo2, lfo3, lfo4; // lfo3/lfo4 [2026-10-22]: mesmos campos (tambem tem pitch/amp depth)
    // [ADICIONADO em 2026-10-22] envelopes de modulacao (estilo DX: 4 rates/4
    // levels, comecam no level[3] como o PEG); fontes Env1..Env3 da matriz.
    ModEnvParams modEnv[kNumModEnvs];
    ModRoute modMatrix[kNumModRoutes];
    // [ADICIONADO em 2026-10-24] morph entre algoritmos: ate kMaxAlgoSlots
    // matrizes (slot 1 = `matrix`, slots 2.. = algoSlot[0..]); a posicao
    // 0..1 percorre os slots em ordem (numAlgos - 1 trechos iguais) e os pesos
    // (PM, diagonal/feedback, outputMix) sao interpolados entre os vizinhos.
    // algoStep: salta pro slot mais proximo em vez de passar pelo meio.
    // [ADICIONADO em 2026-10-24] 4 macros (fontes Macro1..4 da matriz): o valor
    // salvo no patch e o nome. O valor AO VIVO e um controlador da parte
    // (Synth::setMacro, como o mod wheel); o app manda estes valores ao carregar.
    static constexpr int kNumMacros = 4;
    float macro[kNumMacros] = {};
    std::string macroName[kNumMacros] = {"Macro 1", "Macro 2", "Macro 3", "Macro 4"};
    AlgorithmMatrix algoSlot[kMaxAlgoSlots - 1];
    uint8_t numAlgos = 1; // 1 = sem morph (como antes)
    float algoMorph = 0.0f;
    bool algoStep = false;

    AlgorithmMatrix& algo(int s) { return s == 0 ? matrix : algoSlot[s - 1]; }
    const AlgorithmMatrix& algo(int s) const { return s == 0 ? matrix : algoSlot[s - 1]; }
    int activeAlgos() const { return std::clamp<int>(numAlgos, 1, kMaxAlgoSlots); }

    LFOParams& lfo(int i) { return i == 0 ? lfo1 : i == 1 ? lfo2 : i == 2 ? lfo3 : lfo4; }
    const LFOParams& lfo(int i) const { return i == 0 ? lfo1 : i == 1 ? lfo2 : i == 2 ? lfo3 : lfo4; }

    VoiceParams() {
        // patch minimo tocavel: op0 e a unica portadora, op1 modula op0
        matrix.outputMix[0] = 1.0f;
        matrix.mod[0][1] = 1.0f;
        op[1].outputLevel = 60;
    }
};

inline AlgorithmMatrix algoUnion(const VoiceParams& v) {
    const int n = v.activeAlgos();
    if (n == 1) return v.matrix;
    AlgorithmMatrix u;
    for (int s = 0; s < n; ++s) {
        const auto& m = v.algo(s);
        for (int i = 0; i < kNumOps; ++i) {
            for (int j = 0; j < kNumOps; ++j) {
                if (m.mod[i][j] != 0.0f) u.mod[i][j] = 1.0f;
                u.ring[i][j] = u.ring[i][j] || m.ring[i][j];
            }
            if (m.outputMix[i] != 0.0f) u.outputMix[i] = 1.0f;
        }
    }
    return u;
}

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

// Onda da Wavetable: tamanho diferente de kWaveSize e reamostrado (linear,
// ciclico); com NaN/Inf ou vazia vira nullptr (= seno). So aloca se precisar.
inline std::shared_ptr<const std::vector<float>> sanWave(std::shared_ptr<const std::vector<float>> w) {
    if (!w) return w;
    const auto& v = *w;
    if (v.empty()) return nullptr;
    for (float x : v) if (!std::isfinite(x)) return nullptr;
    if (v.size() == static_cast<size_t>(kWaveSize)) {
        bool inRange = true;
        for (float x : v) inRange &= x >= -1.0f && x <= 1.0f;
        if (inRange) return w;
    }
    auto out = std::make_shared<std::vector<float>>(static_cast<size_t>(kWaveSize));
    const size_t n = v.size();
    for (int i = 0; i < kWaveSize; ++i) {
        double pos = static_cast<double>(i) * static_cast<double>(n) / kWaveSize;
        size_t i0 = static_cast<size_t>(pos) % n, i1 = (i0 + 1) % n;
        float t = static_cast<float>(pos - std::floor(pos));
        (*out)[static_cast<size_t>(i)] = std::clamp(v[i0] * (1.0f - t) + v[i1] * t, -1.0f, 1.0f);
    }
    return out;
}

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
    o.waveform = sanEnum(o.waveform, Waveform::Wavetable);
    o.wavetable.position = sanF(o.wavetable.position, 0.0f, 1.0f, 0.0f);
    o.wavetable.waveA = sanWave(o.wavetable.waveA);
    o.wavetable.waveB = sanWave(o.wavetable.waveB);
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
    o.fxKeyTrack = sanF(o.fxKeyTrack, 0.0f, 1.0f, 0.0f);
}

inline void sanitize(LFOParams& l) {
    using namespace detail;
    l.rate = sanF(l.rate, 0.01f, 50.0f, 5.0f);
    l.delaySec = sanF(l.delaySec, 0.0f, 10.0f, 0.0f);
    l.pitchDepth = sanF(l.pitchDepth, 0.0f, 1.0f, 0.0f);
    l.ampDepth = sanF(l.ampDepth, 0.0f, 1.0f, 0.0f);
    l.wave = sanEnum(l.wave, LfoWave::SmoothRandom);
    l.division = static_cast<uint8_t>(std::min<int>(l.division, kNumLfoDivisions - 1));
    l.steps = l.steps == 1 ? 0 : std::min<uint8_t>(l.steps, 32);
}

inline void sanitize(VoiceParams& v) {
    using namespace detail;
    for (auto& o : v.op) sanitize(o);
    for (auto& m : v.macro) m = sanF(m, 0.0f, 1.0f, 0.0f);
    v.numAlgos = static_cast<uint8_t>(std::clamp<int>(v.numAlgos, 1, kMaxAlgoSlots));
    v.algoMorph = sanF(v.algoMorph, 0.0f, 1.0f, 0.0f);
    for (int s = 0; s < kMaxAlgoSlots; ++s) {
        auto& m = v.algo(s);
        for (int i = 0; i < kNumOps; ++i) {
            for (int j = 0; j < kNumOps; ++j) m.mod[i][j] = sanF(m.mod[i][j], -1.0f, 1.0f, 0.0f);
            m.outputMix[i] = sanF(m.outputMix[i], 0.0f, 1.0f, 0.0f);
        }
    }
    sanEG(v.peg.rate, v.peg.level);
    v.peg.rangeOctaves = sanF(v.peg.rangeOctaves, 0.1f, 8.0f, 4.0f);
    for (int i = 0; i < kNumLfos; ++i) sanitize(v.lfo(i));
    for (auto& e : v.modEnv) {
        sanEG(e.rate, e.level);
        e.division = static_cast<uint8_t>(std::min<int>(e.division, kNumLfoDivisions - 1));
    }
    for (auto& r : v.modMatrix) {
        r.src = sanEnum(r.src, ModSource::Macro4);
        r.dest = sanEnum(r.dest, ModDest::AlgoMorph);
        r.opIndex = std::clamp(r.opIndex, 0, modDestPerLfo(r.dest) ? kNumLfos - 1 : kNumOps - 1);
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
