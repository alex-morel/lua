#pragma once
#include "factory_preset.h"
#include "dx7_sysex.h"
#include <initializer_list>
#include <string>

// [ADICIONADO em 2026-10-28] Patches de fabrica no estilo do DX7, programados
// em PARAMETROS DO DX7 (rates/levels 0-99, coarse/fine, os 32 algoritmos
// originais, feedback 0-7, LFO e pitch EG do DX7) e convertidos pelo mesmo
// caminho da importacao de SysEx (sysex::toPart). Sao programacoes proprias,
// inspiradas nos timbres classicos da ROM do DX7 -- nao copias dos dados da
// Yamaha.
namespace dx7fm_app::factory {

// Um operador nos termos do painel do DX7.
struct DxOp {
    int r[4] = {99, 99, 99, 99};
    int l[4] = {99, 99, 99, 0};
    int outputLevel = 0;
    int coarse = 1, fine = 0;   // razao = coarse x (1 + fine/100); coarse 0 = 0.5
    int detune = 7;             // 0..14, 7 = centro
    int velocity = 0;           // 0..7
    int ams = 0;                // sensibilidade a modulacao de amplitude 0..3
    int rateScaling = 0;        // 0..7
    int breakpoint = 39;        // 0..99 (39 = C3)
    int leftDepth = 0, rightDepth = 0, leftCurve = 0, rightCurve = 0; // curvas: 0 -LIN, 1 -EXP, 2 +EXP, 3 +LIN
    bool fixed = false;
};

struct DxVoice {
    std::string name;
    int algorithm = 1;          // 1..32
    int feedback = 0;           // 0..7
    DxOp op[6];                 // OP1..OP6 (como no painel)
    int pegR[4] = {99, 99, 99, 99}, pegL[4] = {50, 50, 50, 50};
    int lfoSpeed = 35, lfoDelay = 0, lfoPmd = 0, lfoAmd = 0, lfoWave = 0, lfoPms = 3;
    bool lfoSync = true;
    int transpose = 24;         // 24 = C3 (sem transpor)
};

// DxVoice -> bytes VCED (OP6 primeiro) -> PartParams pela conversao do SysEx
inline dx7fm::PartParams dxToPart(const DxVoice& d) {
    dx7fm::sysex::Dx7Voice v;
    uint8_t* p = v.p;
    for (int k = 0; k < 6; ++k) {
        const DxOp& o = d.op[5 - k];
        uint8_t* b = p + k * 21;
        for (int i = 0; i < 4; ++i) { b[i] = static_cast<uint8_t>(o.r[i]); b[4 + i] = static_cast<uint8_t>(o.l[i]); }
        b[8] = static_cast<uint8_t>(o.breakpoint);
        b[9] = static_cast<uint8_t>(o.leftDepth);
        b[10] = static_cast<uint8_t>(o.rightDepth);
        b[11] = static_cast<uint8_t>(o.leftCurve);
        b[12] = static_cast<uint8_t>(o.rightCurve);
        b[13] = static_cast<uint8_t>(o.rateScaling);
        b[14] = static_cast<uint8_t>(o.ams);
        b[15] = static_cast<uint8_t>(o.velocity);
        b[16] = static_cast<uint8_t>(o.outputLevel);
        b[17] = o.fixed ? 1 : 0;
        b[18] = static_cast<uint8_t>(o.coarse);
        b[19] = static_cast<uint8_t>(o.fine);
        b[20] = static_cast<uint8_t>(o.detune);
    }
    for (int i = 0; i < 4; ++i) { p[126 + i] = static_cast<uint8_t>(d.pegR[i]); p[130 + i] = static_cast<uint8_t>(d.pegL[i]); }
    p[134] = static_cast<uint8_t>(d.algorithm - 1);
    p[135] = static_cast<uint8_t>(d.feedback);
    p[136] = 1;
    p[137] = static_cast<uint8_t>(d.lfoSpeed);
    p[138] = static_cast<uint8_t>(d.lfoDelay);
    p[139] = static_cast<uint8_t>(d.lfoPmd);
    p[140] = static_cast<uint8_t>(d.lfoAmd);
    p[141] = d.lfoSync ? 1 : 0;
    p[142] = static_cast<uint8_t>(d.lfoWave);
    p[143] = static_cast<uint8_t>(d.lfoPms);
    p[144] = static_cast<uint8_t>(d.transpose);
    for (int i = 0; i < 10; ++i) p[145 + i] = ' ';
    dx7fm::PartParams part = dx7fm::sysex::toPart(v);
    part.name = d.name;
    return part;
}

// atalho: operador com envelope, nivel e razao
inline DxOp dxOp(std::initializer_list<int> r, std::initializer_list<int> l, int outputLevel, int coarse, int fine = 0, int detune = 7,
                 int velocity = 0) {
    DxOp o;
    int i = 0;
    for (int x : r) o.r[i++] = x;
    i = 0;
    for (int x : l) o.l[i++] = x;
    o.outputLevel = outputLevel;
    o.coarse = coarse;
    o.fine = fine;
    o.detune = detune;
    o.velocity = velocity;
    return o;
}

inline void addDxPresets(std::vector<FactoryPreset>& out) {
    auto add = [&](const DxVoice& d, const char* category, const char* description, std::vector<int> notes, double seconds = 3.0,
                   double hold = 2.2) -> FactoryPreset& {
        FactoryPreset p;
        p.part = dxToPart(d);
        p.family = "Estilo DX7";
        p.category = category;
        p.description = description;
        p.notes = std::move(notes);
        p.seconds = seconds;
        p.holdSec = hold;
        out.push_back(p);
        return out.back();
    };
    { // piano eletrico: tom (1/2), tine de razao 14 que some rapido (3/4), par desafinado com feedback pro "chorus" (5/6)
        DxVoice d; d.name = "DX Piano Eletrico"; d.algorithm = 5; d.feedback = 6;
        d.op[0] = dxOp({96, 34, 30, 67}, {99, 70, 0, 0}, 99, 1, 0, 10, 2);
        d.op[1] = dxOp({95, 50, 35, 78}, {99, 75, 0, 0}, 62, 1, 0, 7, 7);
        d.op[2] = dxOp({95, 30, 28, 60}, {99, 80, 0, 0}, 92, 1, 0, 4, 2);
        d.op[3] = dxOp({95, 62, 35, 78}, {99, 0, 0, 0}, 82, 14, 0, 7, 6);
        d.op[4] = dxOp({95, 34, 30, 67}, {99, 70, 0, 0}, 84, 1, 0, 3, 1);
        d.op[5] = dxOp({95, 50, 35, 78}, {99, 75, 0, 0}, 50, 1, 0, 11, 3);
        for (int i : {1, 3, 5}) { d.op[i].breakpoint = 48; d.op[i].rightDepth = 30; d.op[i].rightCurve = 1; d.op[i].rateScaling = 3; }
        add(d, "Teclas", "algoritmo 5 do DX7: par do tom, par do tine (razao 14) e par desafinado com feedback", {48, 60, 64, 67}).part.voice.volumeDb = -16.0f; // varias portadoras somam alto
    }
    { // metais: um modulador com feedback (6) em 3 portadoras desafinadas + par 1/2; pitch EG sobe no ataque
        DxVoice d; d.name = "DX Metais"; d.algorithm = 22; d.feedback = 7;
        d.op[0] = dxOp({72, 76, 99, 71}, {99, 88, 96, 0}, 96, 1, 0, 7, 1);
        d.op[1] = dxOp({62, 51, 29, 71}, {82, 95, 96, 0}, 80, 1, 0, 7, 3);
        d.op[2] = dxOp({72, 76, 99, 71}, {99, 88, 96, 0}, 94, 1, 0, 5, 1);
        d.op[3] = dxOp({72, 76, 99, 71}, {99, 88, 96, 0}, 94, 1, 0, 9, 1);
        d.op[4] = dxOp({72, 76, 99, 71}, {99, 88, 96, 0}, 92, 1, 0, 6, 1);
        d.op[5] = dxOp({60, 40, 41, 71}, {99, 95, 92, 0}, 82, 1, 0, 7, 3);
        d.pegR[0] = 70; d.pegR[3] = 60; d.pegL[3] = 46;
        add(d, "Metais", "algoritmo 22: um modulador com feedback em 3 portadoras desafinadas; pitch EG sobe no ataque", {55, 60, 64}).part.voice.volumeDb = -19.0f; // varias portadoras somam alto
    }
    { // cordas: duas pilhas lentas (2->1 com feedback, 6->5->4->3), desafinadas, vibrato com atraso
        DxVoice d; d.name = "DX Cordas"; d.algorithm = 2; d.feedback = 7;
        d.op[0] = dxOp({58, 24, 20, 41}, {99, 85, 80, 0}, 96, 1, 0, 9);
        d.op[1] = dxOp({60, 40, 17, 49}, {85, 90, 80, 0}, 70, 1, 0, 7);
        d.op[2] = dxOp({56, 45, 20, 45}, {99, 85, 82, 0}, 96, 1, 0, 5);
        d.op[3] = dxOp({62, 19, 20, 45}, {90, 92, 86, 0}, 74, 1, 0, 7);
        d.op[4] = dxOp({60, 19, 20, 45}, {86, 92, 86, 0}, 55, 3, 0, 7);
        d.op[5] = dxOp({55, 19, 20, 45}, {90, 92, 86, 0}, 60, 1, 0, 7);
        d.lfoSpeed = 30; d.lfoDelay = 34; d.lfoPmd = 7; d.lfoPms = 4; d.lfoWave = 4;
        add(d, "Cordas", "algoritmo 2: duas pilhas de ataque lento desafinadas, vibrato que entra depois", {48, 55, 60, 64}, 4.0, 3.0).part.voice.volumeDb = -10.5f; // varias portadoras somam alto
    }
    { // baixo: uma portadora com tres ramos (alg 16); o 4 (razao 5) estala no ataque
        DxVoice d; d.name = "DX Baixo"; d.algorithm = 16; d.feedback = 6;
        d.op[0] = dxOp({99, 45, 30, 60}, {99, 80, 60, 0}, 99, 1, 0, 7, 2);
        d.op[1] = dxOp({99, 55, 30, 60}, {99, 60, 40, 0}, 78, 1, 0, 7, 4);
        d.op[2] = dxOp({99, 70, 40, 60}, {99, 40, 20, 0}, 74, 1, 0, 7, 3);
        d.op[3] = dxOp({99, 80, 50, 60}, {99, 0, 0, 0}, 68, 5, 0, 7, 5);
        d.op[4] = dxOp({99, 50, 35, 60}, {99, 50, 30, 0}, 70, 1, 0, 7, 2);
        d.op[5] = dxOp({99, 55, 35, 60}, {99, 60, 40, 0}, 70, 1, 0, 7);
        add(d, "Baixo", "algoritmo 16: uma portadora com tres ramos de modulacao; estalo de razao 5 no ataque", {36}, 2.0, 1.2).part.voice.volumeDb = -8.0f; // varias portadoras somam alto
    }
    { // marimba: portadora + parcial 3.98 (o 4o modo da barra de madeira) e um clique de razao 7
        DxVoice d; d.name = "DX Marimba"; d.algorithm = 5;
        d.op[0] = dxOp({99, 43, 35, 50}, {99, 0, 0, 0}, 99, 1, 0, 7, 3);
        d.op[1] = dxOp({99, 78, 50, 50}, {99, 0, 0, 0}, 62, 7, 0, 7, 5);
        d.op[2] = dxOp({99, 58, 40, 50}, {99, 0, 0, 0}, 82, 2, 99, 7, 3);
        d.op[3] = dxOp({99, 70, 40, 50}, {99, 0, 0, 0}, 45, 1, 0, 7, 3);
        for (auto& o : d.op) o.rateScaling = 3;
        d.op[4].outputLevel = 0; d.op[5].outputLevel = 0;
        add(d, "Sinos", "algoritmo 5: portadora + parcial em 3.98 (o modo de uma barra de madeira) + clique", {60, 67, 72}, 2.0, 1.5).part.voice.volumeDb = -9.5f; // varias portadoras somam alto
    }
    { // sinos tubulares: pares com modulador de razao 3.5 (inarmonico), decaimento longo, um "hum" uma oitava abaixo
        DxVoice d; d.name = "DX Sinos Tubulares"; d.algorithm = 5;
        d.op[0] = dxOp({99, 26, 20, 35}, {99, 0, 0, 0}, 97, 1, 0, 7, 2);
        d.op[1] = dxOp({99, 30, 20, 35}, {99, 0, 0, 0}, 76, 3, 50, 7, 3);
        d.op[2] = dxOp({99, 28, 20, 35}, {99, 0, 0, 0}, 93, 1, 0, 10, 2);
        d.op[3] = dxOp({99, 32, 20, 35}, {99, 0, 0, 0}, 72, 3, 50, 4, 3);
        d.op[4] = dxOp({99, 22, 20, 35}, {99, 0, 0, 0}, 80, 0, 0, 7, 1);
        d.op[5] = dxOp({99, 40, 20, 35}, {99, 0, 0, 0}, 50, 1, 0, 7);
        add(d, "Sinos", "algoritmo 5: moduladores de razao 3.5 (inarmonico), decaimento longo e um hum uma oitava abaixo", {60, 67}, 5.0, 4.5).part.voice.volumeDb = -14.0f; // varias portadoras somam alto
    }
    { // cravo: tres pares brilhantes (razoes 3, 5, 7), oitava no segundo par, rate scaling (agudas decaem antes)
        DxVoice d; d.name = "DX Cravo"; d.algorithm = 5; d.feedback = 5;
        d.op[0] = dxOp({99, 40, 30, 70}, {99, 85, 0, 0}, 96, 1, 0, 7, 1);
        d.op[1] = dxOp({99, 55, 30, 70}, {99, 70, 0, 0}, 80, 3, 0, 7, 2);
        d.op[2] = dxOp({99, 40, 30, 70}, {99, 85, 0, 0}, 88, 2, 0, 8, 1);
        d.op[3] = dxOp({99, 60, 30, 70}, {99, 60, 0, 0}, 76, 5, 0, 7, 2);
        d.op[4] = dxOp({99, 40, 30, 70}, {99, 85, 0, 0}, 84, 1, 0, 9, 1);
        d.op[5] = dxOp({99, 60, 30, 70}, {99, 60, 0, 0}, 70, 7, 0, 7);
        for (auto& o : d.op) o.rateScaling = 4;
        add(d, "Teclas", "algoritmo 5: tres pares brilhantes (razoes 3, 5 e 7), rate scaling faz as agudas decairem antes", {48, 55, 60, 64}).part.voice.volumeDb = -13.5f; // varias portadoras somam alto
    }
    { // clavinete: duas pilhas de 3 (alg 3), velocity no brilho, sustain curto e release seco
        DxVoice d; d.name = "DX Clavinete"; d.algorithm = 3; d.feedback = 7;
        d.op[0] = dxOp({99, 50, 40, 85}, {99, 85, 75, 0}, 97, 1, 0, 7, 3);
        d.op[1] = dxOp({99, 60, 40, 85}, {99, 70, 60, 0}, 84, 1, 0, 7, 5);
        d.op[2] = dxOp({99, 70, 40, 85}, {99, 40, 30, 0}, 70, 3, 0, 7, 4);
        d.op[3] = dxOp({99, 55, 40, 85}, {99, 80, 70, 0}, 86, 2, 0, 8, 3);
        d.op[4] = dxOp({99, 65, 40, 85}, {99, 60, 50, 0}, 80, 5, 0, 7, 5);
        d.op[5] = dxOp({99, 60, 40, 85}, {99, 60, 50, 0}, 70, 1, 0, 7);
        add(d, "Teclas", "algoritmo 3: duas pilhas de 3 operadores, velocity abre o brilho, release seco", {48, 55, 60}, 2.0, 1.2).part.voice.volumeDb = -10.0f; // varias portadoras somam alto
    }
    { // vibrafone: pares com "ding" de razao 4, decaimento longo e tremolo (LFO na amplitude das portadoras)
        DxVoice d; d.name = "DX Vibrafone"; d.algorithm = 5;
        d.op[0] = dxOp({99, 30, 22, 40}, {99, 0, 0, 0}, 97, 1, 0, 7, 2);
        d.op[1] = dxOp({99, 62, 40, 40}, {99, 0, 0, 0}, 62, 4, 0, 7, 4);
        d.op[2] = dxOp({99, 32, 22, 40}, {99, 0, 0, 0}, 88, 1, 0, 9, 2);
        d.op[3] = dxOp({99, 66, 40, 40}, {99, 0, 0, 0}, 55, 4, 0, 7, 4);
        d.op[4] = dxOp({99, 55, 40, 40}, {99, 0, 0, 0}, 72, 4, 0, 7, 3);
        d.op[5].outputLevel = 0;
        for (int i : {0, 2}) d.op[i].ams = 2;
        d.lfoSpeed = 40; d.lfoAmd = 35; d.lfoWave = 4; d.lfoSync = false;
        add(d, "Sinos", "algoritmo 5: ding de razao 4, decaimento longo e tremolo de motor (LFO na amplitude)", {60, 64, 67}, 4.0, 3.5).part.voice.volumeDb = -13.0f; // varias portadoras somam alto
    }
    { // flauta: tom quase senoidal com ataque soprado; o 6 em frequencia fixa com feedback maximo vira o ruido do sopro
        DxVoice d; d.name = "DX Flauta"; d.algorithm = 5; d.feedback = 7;
        d.op[0] = dxOp({70, 40, 30, 55}, {99, 95, 92, 0}, 99, 1, 0, 7, 2);
        d.op[1] = dxOp({66, 40, 30, 55}, {90, 85, 80, 0}, 60, 1, 0, 7, 3);
        d.op[2] = dxOp({55, 40, 30, 55}, {99, 90, 85, 0}, 62, 2, 0, 8, 1);
        d.op[3] = dxOp({60, 40, 30, 55}, {90, 80, 75, 0}, 40, 1, 0, 7);
        d.op[4] = dxOp({85, 50, 40, 60}, {99, 72, 68, 0}, 66, 1, 0, 7, 2);
        d.op[5] = dxOp({99, 99, 99, 60}, {99, 99, 99, 0}, 80, 1, 0, 7);
        d.lfoSpeed = 35; d.lfoDelay = 40; d.lfoPmd = 5; d.lfoPms = 3; d.lfoWave = 4;
        FactoryPreset& f = add(d, "Sopros", "algoritmo 5: tom quase senoidal + sopro (o 6 virou ruido, que modula o 5 em volta da nota)", {72}, 3.0, 2.2);
        f.part.voice.op[5].waveform = Waveform::Noise; // no DX7 seria feedback maximo; aqui um ruido de verdade
        f.part.voice.volumeDb = -10.5f; // varias portadoras somam alto
    }
    { // koto: pluck brilhante (moduladores 3 e 1 que somem rapido) e corpo desafinado
        DxVoice d; d.name = "DX Koto"; d.algorithm = 5; d.feedback = 4;
        d.op[0] = dxOp({99, 45, 35, 60}, {99, 0, 0, 0}, 97, 1, 0, 7, 3);
        d.op[1] = dxOp({99, 65, 45, 60}, {99, 0, 0, 0}, 78, 3, 0, 7, 5);
        d.op[2] = dxOp({99, 40, 35, 60}, {99, 0, 0, 0}, 88, 1, 0, 9, 2);
        d.op[3] = dxOp({99, 75, 45, 60}, {99, 0, 0, 0}, 82, 1, 0, 7, 5);
        d.op[4] = dxOp({99, 60, 45, 60}, {99, 0, 0, 0}, 72, 3, 0, 7, 3);
        d.op[5] = dxOp({99, 70, 45, 60}, {99, 0, 0, 0}, 60, 5, 0, 7, 3);
        for (auto& o : d.op) o.rateScaling = 3;
        add(d, "Cordas", "algoritmo 5: pluck brilhante (moduladores que somem rapido) com corpo levemente desafinado", {62, 67, 69}, 2.5, 1.6).part.voice.volumeDb = -11.5f; // varias portadoras somam alto
    }
}

} // namespace dx7fm_app::factory
