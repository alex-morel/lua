#pragma once
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// [ADICIONADO em 2026-10-24] Importacao de SysEx do Yamaha DX7: banco de 32
// vozes (VMEM, "F0 43 0n 09 20 00" + 4096 bytes) e voz unica (VCED, "F0 43 0n
// 00 01 1B" + 155 bytes), convertidos pro motor de 8 operadores (os 6 do DX7
// viram os operadores 0..5; 6 e 7 ficam desligados).
//
// A tabela de algoritmos e as formulas de conversao (nivel de saida, razao,
// frequencia fixa, detune, feedback, velocidade do LFO, level scaling) vem do
// msfa / "music-synthesizer-for-android" (Google, Apache-2.0 -- ver
// THIRD_PARTY_NOTICES.md), a mesma base do Dexed. Aproximacoes (o motor nao e
// um DX7): envelopes convertidos pela velocidade em dB/s (msfa a 44,1 kHz),
// curvas exponenciais de level scaling e o alcance do vibrato (aqui no maximo
// +-1 semitom).
namespace dx7fm::sysex {

// Uma voz no formato VCED "desempacotado" (155 bytes: OP6..OP1 com 21 bytes
// cada, depois os parametros globais), como na msfa.
struct Dx7Voice {
    uint8_t p[155] = {};
    std::string name() const {
        std::string n;
        for (int i = 145; i < 155; ++i) n += static_cast<char>(p[i] >= 32 && p[i] < 127 ? p[i] : ' ');
        while (!n.empty() && n.back() == ' ') n.pop_back();
        return n;
    }
};

// Algoritmos do DX7 (msfa fm_core.cc): um byte por operador, OP6 primeiro.
// bits 0-1 barramento de saida (0 = saida da voz), bit 2 soma no destino,
// bits 4-5 barramento de entrada, bit 6 entrada de feedback, bit 7 saida de feedback.
inline const uint8_t kAlgorithms[32][6] = {
    {0xc1, 0x11, 0x11, 0x14, 0x01, 0x14}, {0x01, 0x11, 0x11, 0x14, 0xc1, 0x14}, {0xc1, 0x11, 0x14, 0x01, 0x11, 0x14},
    {0x41, 0x11, 0x94, 0x01, 0x11, 0x14}, {0xc1, 0x14, 0x01, 0x14, 0x01, 0x14}, {0x41, 0x94, 0x01, 0x14, 0x01, 0x14},
    {0xc1, 0x11, 0x05, 0x14, 0x01, 0x14}, {0x01, 0x11, 0xc5, 0x14, 0x01, 0x14}, {0x01, 0x11, 0x05, 0x14, 0xc1, 0x14},
    {0x01, 0x05, 0x14, 0xc1, 0x11, 0x14}, {0xc1, 0x05, 0x14, 0x01, 0x11, 0x14}, {0x01, 0x05, 0x05, 0x14, 0xc1, 0x14},
    {0xc1, 0x05, 0x05, 0x14, 0x01, 0x14}, {0xc1, 0x05, 0x11, 0x14, 0x01, 0x14}, {0x01, 0x05, 0x11, 0x14, 0xc1, 0x14},
    {0xc1, 0x11, 0x02, 0x25, 0x05, 0x14}, {0x01, 0x11, 0x02, 0x25, 0xc5, 0x14}, {0x01, 0x11, 0x11, 0xc5, 0x05, 0x14},
    {0xc1, 0x14, 0x14, 0x01, 0x11, 0x14}, {0x01, 0x05, 0x14, 0xc1, 0x14, 0x14}, {0x01, 0x14, 0x14, 0xc1, 0x14, 0x14},
    {0xc1, 0x14, 0x14, 0x14, 0x01, 0x14}, {0xc1, 0x14, 0x14, 0x01, 0x14, 0x04}, {0xc1, 0x14, 0x14, 0x14, 0x04, 0x04},
    {0xc1, 0x14, 0x14, 0x04, 0x04, 0x04}, {0xc1, 0x05, 0x14, 0x01, 0x14, 0x04}, {0x01, 0x05, 0x14, 0xc1, 0x14, 0x04},
    {0x04, 0xc1, 0x11, 0x14, 0x01, 0x14}, {0xc1, 0x14, 0x01, 0x14, 0x04, 0x04}, {0x04, 0xc1, 0x11, 0x14, 0x04, 0x04},
    {0xc1, 0x14, 0x04, 0x04, 0x04, 0x04}, {0xc4, 0x04, 0x04, 0x04, 0x04, 0x04},
};

// Matriz do algoritmo (1..32) simulando os barramentos da msfa. Operador DX7
// n (1..6) = nosso n-1. fbAmount vai na diagonal do operador com feedback
// proprio, ou na aresta FB_OUT -> FB_IN nos algoritmos 4 e 6 (laco de 2-3
// operadores, que a msfa nao implementa e a nossa matriz livre faz).
inline AlgorithmMatrix algorithmMatrix(int algorithm1to32, float fbAmount) {
    AlgorithmMatrix m;
    const uint8_t* ops = kAlgorithms[std::clamp(algorithm1to32, 1, 32) - 1];
    std::vector<int> bus[3];
    int fbIn = -1, fbOut = -1;
    for (int k = 0; k < 6; ++k) {
        const int op = 5 - k; // ops[0] = OP6 = nosso 5
        const int flags = ops[k], in = (flags >> 4) & 3, out = flags & 3;
        if (in > 0) for (int src : bus[in]) m.mod[op][src] = 1.0f;
        if (out == 0) m.outputMix[op] = 1.0f;
        else if (flags & 4) bus[out].push_back(op);
        else bus[out] = {op};
        if (flags & 0x40) fbIn = op;
        if (flags & 0x80) fbOut = op;
    }
    if (fbIn >= 0 && fbOut >= 0) m.mod[fbIn][fbOut] = fbAmount; // fbIn == fbOut: diagonal (feedback proprio)
    return m;
}

// --- conversoes (msfa dx7note.cc / env.cc / lfo.cc) ---------------------------
// nivel do DX7 (0..99) na escala de 0.75 dB/passo: 20..99 linear, abaixo tabela
inline int scaleOutLevel(int l) {
    static const int lut[20] = {0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
    l = std::clamp(l, 0, 99);
    return l >= 20 ? 28 + l : lut[l];
}
// nosso nivel (0..99, 0.75 dB/passo, 99 = 0 dB) com o mesmo volume
inline uint8_t levelToOurs(int dxLevel) { return static_cast<uint8_t>(std::clamp(scaleOutLevel(dxLevel) - 28, 0, 99)); }

// rate do DX7 -> nosso rate, pela velocidade do envelope em niveis (0.75 dB)/s
inline uint8_t rateToOurs(int dxRate) {
    const int q = std::min(63, (std::clamp(dxRate, 0, 99) * 41) >> 6);
    // msfa: inc = (4 + q&3) << (2 + LG_N + q>>2) unidades de nivel (2^21 = 0.75 dB) por bloco de 64 amostras a 44,1 kHz
    const double unitsPerSec = (4 + (q & 3)) * std::ldexp(1.0, 8 + (q >> 2)) * (44100.0 / 64.0) / std::ldexp(1.0, 21);
    const double ours = std::log(unitsPerSec / 5.0) / std::log(1.0897); // inverso de egRateToSpeed
    return static_cast<uint8_t>(std::lround(std::clamp(ours, 0.0, 99.0)));
}

// razao (coarse 0 = 0.5, 1..31) x (1 + fine/100)
inline float ratio(int coarse, int fine) {
    const float c = coarse == 0 ? 0.5f : static_cast<float>(coarse);
    return c * (1.0f + 0.01f * static_cast<float>(fine));
}
// frequencia fixa: 10^(coarse&3) x 10^(fine/100) Hz
inline float fixedHz(int coarse, int fine) { return std::pow(10.0f, static_cast<float>(coarse & 3) + fine / 100.0f); }
// detune 0..14 (7 = centro): 12606/2^24 de oitava por passo
inline float detuneCents(int det) { return (det - 7) * (12606.0f / 16777216.0f) * 1200.0f; }
// feedback 0..7 -> peso da auto-modulacao (msfa: media das 2 ultimas saidas >> (8 - fb))
inline float feedbackAmount(int fb) { return fb <= 0 ? 0.0f : std::ldexp(1.0f, std::clamp(fb, 1, 7) - 8); }

// velocidade do LFO (0..99) em Hz (msfa lfo.cc)
inline float lfoHz(int speed) {
    int sr = speed == 0 ? 1 : (165 * std::clamp(speed, 0, 99)) >> 6;
    sr *= sr < 160 ? 11 : (11 + ((sr - 160) >> 4));
    return static_cast<float>(25190424.0 * sr / 4294967296.0);
}
// atraso do LFO (0..99) em segundos ate a modulacao estar inteira (aprox. msfa)
inline float lfoDelaySec(int delay) {
    const int a0 = 99 - std::clamp(delay, 0, 99);
    if (a0 == 99) return 0.0f;
    const int a = (16 + (a0 & 15)) << (1 + (a0 >> 4));
    return static_cast<float>(4294967296.0 / (25190424.0 * a));
}

// level scaling do DX7 (breakpoint 0..99, profundidades 0..99, curvas
// 0 -LIN, 1 -EXP, 2 +EXP, 3 +LIN) -> nosso (dB por oitava; exp = curva)
inline KeyLevelScaling levelScaling(int bp, int ld, int rd, int lc, int rc) {
    KeyLevelScaling k;
    k.breakpoint = static_cast<uint8_t>(std::clamp(bp + 17, 0, 127)); // msfa: offset = nota - bp - 17
    auto conv = [](int depth, int curve, bool& exp) {
        exp = curve == 1 || curve == 2;
        const float sign = curve < 2 ? -1.0f : 1.0f;
        // linear: depth x 329/4096 x 4 grupos/oitava x 0.75 dB; exp: ajuste a curva da tabela (2-4 oitavas)
        const float dbPerOct = exp ? 0.32f * depth : depth * 329.0f / 4096.0f * 4.0f * 0.75f;
        return static_cast<int8_t>(std::lround(std::clamp(sign * dbPerOct, -99.0f, 99.0f)));
    };
    k.leftDepth = conv(ld, lc, k.leftExp);
    k.rightDepth = conv(rd, rc, k.rightExp);
    return k;
}

// Converte uma voz do DX7 numa parte (som + nome + transpose).
inline PartParams toPart(const Dx7Voice& dx) {
    const uint8_t* p = dx.p;
    PartParams part;
    VoiceParams& v = part.voice;
    v = VoiceParams{};
    for (int k = 0; k < 6; ++k) {
        const uint8_t* o = p + k * 21;
        OperatorParams& op = v.op[5 - k];
        op = OperatorParams{};
        for (int i = 0; i < 4; ++i) {
            op.ampEG.rate[i] = rateToOurs(o[i]);
            op.ampEG.level[i] = levelToOurs(o[4 + i]);
        }
        op.levelScaling = levelScaling(o[8], o[9], o[10], o[11], o[12]);
        op.rateScaling = static_cast<uint8_t>(std::min<int>(o[13], 7));
        op.lfoAmpModSens = static_cast<uint8_t>(std::min<int>(o[14], 3));
        op.velocitySens = static_cast<uint8_t>(std::min<int>(o[15], 7));
        op.outputLevel = levelToOurs(o[16]);
        op.fixedFreq = o[17] != 0;
        if (op.fixedFreq) op.fixedHz = fixedHz(o[18], o[19]);
        else op.coarseRatio = ratio(o[18], o[19]);
        op.detuneCents = detuneCents(o[20]);
        op.lfoPitchModSens = static_cast<uint8_t>(p[143] & 7);
        op.pegDepth = 1.0f; // o pitch EG do DX7 vale pra todos os operadores
        op.feedback = 0.0f; // feedback fica na matriz (diagonal ou laco)
    }
    for (int k = 6; k < kNumOps; ++k) { v.op[k].outputLevel = 0; v.op[k].feedback = 0.0f; }
    for (int i = 0; i < 4; ++i) {
        v.peg.rate[i] = std::min<uint8_t>(p[126 + i], 99);
        v.peg.level[i] = std::min<uint8_t>(p[130 + i], 99);
    }
    v.matrix = algorithmMatrix(p[134] + 1, feedbackAmount(p[135] & 7));
    v.lfo1 = LFOParams{};
    v.lfo1.rate = lfoHz(p[137]);
    v.lfo1.delaySec = lfoDelaySec(p[138]);
    v.lfo1.pitchDepth = std::min<int>(p[139], 99) / 99.0f;
    v.lfo1.ampDepth = std::min<int>(p[140], 99) / 99.0f;
    v.lfo1.keySync = p[141] != 0;
    static const LfoWave waves[6] = {LfoWave::Triangle, LfoWave::SawDown, LfoWave::SawUp, LfoWave::Square, LfoWave::Sine, LfoWave::SampleHold};
    v.lfo1.wave = waves[std::min<int>(p[142], 5)];
    part.transpose = static_cast<int8_t>(std::clamp(static_cast<int>(p[144]) - 24, -24, 24)); // 24 = C3 (sem transpor)
    part.name = dx.name().empty() ? "DX7" : dx.name();
    sanitize(part);
    return part;
}

// --- parser ----------------------------------------------------------------------
inline uint8_t checksum(const uint8_t* data, size_t n) {
    unsigned sum = 0;
    for (size_t i = 0; i < n; ++i) sum += data[i];
    return static_cast<uint8_t>((128 - (sum & 127)) & 127);
}

// VMEM (128 bytes empacotados) -> VCED (155)
inline Dx7Voice unpackVmem(const uint8_t* b) {
    Dx7Voice v;
    uint8_t* p = v.p;
    for (int k = 0; k < 6; ++k) {
        const uint8_t* s = b + k * 17;
        uint8_t* d = p + k * 21;
        for (int i = 0; i < 11; ++i) d[i] = s[i] & 127;  // rates, levels, BP, LD, RD
        d[11] = s[11] & 3;                               // curva esquerda
        d[12] = (s[11] >> 2) & 3;                        // curva direita
        d[13] = s[12] & 7;                               // rate scaling
        d[20] = (s[12] >> 3) & 15;                       // detune
        d[14] = s[13] & 3;                               // AMS
        d[15] = (s[13] >> 2) & 7;                        // key velocity
        d[16] = s[14] & 127;                             // output level
        d[17] = s[15] & 1;                               // modo (fixo)
        d[18] = (s[15] >> 1) & 31;                       // coarse
        d[19] = s[16] & 127;                             // fine
    }
    for (int i = 0; i < 8; ++i) p[126 + i] = b[102 + i] & 127; // PEG
    p[134] = b[110] & 31;            // algoritmo
    p[135] = b[111] & 7;             // feedback
    p[136] = (b[111] >> 3) & 1;      // osc key sync
    for (int i = 0; i < 4; ++i) p[137 + i] = b[112 + i] & 127; // LFO velocidade, atraso, PMD, AMD
    p[141] = b[116] & 1;             // LFO key sync
    p[142] = (b[116] >> 1) & 7;      // forma
    p[143] = (b[116] >> 4) & 7;      // PMS
    p[144] = b[117] & 63;            // transpose
    for (int i = 0; i < 10; ++i) p[145 + i] = b[118 + i] & 127;
    return v;
}

// Le um arquivo .syx (uma ou mais mensagens). err fica vazio se deu certo.
inline std::vector<Dx7Voice> parse(const std::vector<uint8_t>& data, std::string& err) {
    std::vector<Dx7Voice> voices;
    err.clear();
    bool sawSysex = false;
    for (size_t i = 0; i + 6 <= data.size(); ++i) {
        if (data[i] != 0xF0 || data[i + 1] != 0x43) continue;
        sawSysex = true;
        const uint8_t fmt = data[i + 3];
        const size_t len = (static_cast<size_t>(data[i + 4]) << 7) | data[i + 5];
        const size_t body = i + 6;
        if ((fmt == 9 && len == 4096) || (fmt == 0 && len == 155)) {
            if (body + len + 1 > data.size()) { err = "mensagem SysEx cortada"; break; }
            if (checksum(&data[body], len) != (data[body + len] & 127)) { err = "checksum errado (arquivo corrompido)"; continue; }
            if (fmt == 9) for (int v = 0; v < 32; ++v) voices.push_back(unpackVmem(&data[body + static_cast<size_t>(v) * 128]));
            else { Dx7Voice v; for (size_t k = 0; k < 155; ++k) v.p[k] = data[body + k] & 127; voices.push_back(v); }
            i = body + len;
        }
    }
    if (!sawSysex && data.size() == 4096) // banco cru, sem cabecalho SysEx
        for (int v = 0; v < 32; ++v) voices.push_back(unpackVmem(&data[static_cast<size_t>(v) * 128]));
    if (voices.empty() && err.empty()) err = sawSysex ? "nenhuma voz de DX7 (banco ou voz unica) no arquivo" : "nao e um arquivo SysEx do DX7";
    return voices;
}

} // namespace dx7fm::sysex
