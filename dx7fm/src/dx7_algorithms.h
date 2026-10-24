#pragma once
#include "dx7_params.h"
#include <vector>

// [ADICIONADO em 2026-09-21] 32 algoritmos preset (topologias de roteamento
// prontas pra AlgorithmMatrix) DESENHADOS PARA OS 8 OPERADORES deste motor.
//
// IMPORTANTE (leia antes de esperar fidelidade a um produto especifico):
// isso NAO e uma reproducao dos 32 algoritmos do DX7 real (que tem so 6
// operadores) nem do FM-X do Yamaha Montage/MODX (que tem os 8 operadores
// mas uma lista de algoritmos oficial diferente, sob copyright da Yamaha,
// que eu nao tenho memorizada com confianca suficiente pra reproduzir
// bit-exata sem checar contra uma referencia). Por pedido explicito do
// usuario ("use os do Montage com as devidas alteracoes, e um synth de 8
// operadores afinal"), o conjunto abaixo e um DESENHO PROPRIO: 32
// topologias organizadas no mesmo ESPIRITO da tradicao Yamaha (do mais
// complexo/1-portadora ao mais simples/8-portadoras-paralelas, cobrindo as
// 22 particoes inteiras de 8 mais 10 variantes com "fork" -- um modulador
// compartilhado alimentando 2+ portadoras), mas SEM pretender ser a lista
// oficial de nenhum produto Yamaha especifico.
//
// Cada algoritmo e feito de CADEIAS (chain: lista de indices de operador,
// ops[0]=portadora ... ops.back()=topo, que recebe feedback) e/ou FORKS
// (um modulador compartilhado, modChain[0], alimenta 2+ portadoras
// diretamente; modChain[1..] e uma cadeia de moduladores acima dele,
// modChain.back() recebe o feedback). applyAlgorithm() monta a
// AlgorithmMatrix a partir disso E seta um feedback default (0.3) no
// operador do topo de cada cadeia/fork -- editavel depois pelo usuario,
// como qualquer outro parametro da matriz.

namespace dx7fm {

struct AlgorithmFork {
    std::vector<int> carriers;  // 2+ operadores que recebem o MESMO modulador direto
    std::vector<int> modChain;  // modChain[0] modula todas as carriers; modChain[1..] empilham acima; back() recebe feedback
};

// Oposto do fork: UMA portadora recebendo de 2+ cadeias de modulador
// SEPARADAS (cada cadeia liga direto na portadora, nao umas nas outras).
struct AlgorithmMerge {
    int carrier;
    std::vector<std::vector<int>> modChains; // cada uma: [0]=modula `carrier` direto .. back()=topo(feedback)
};

struct AlgorithmDef {
    const char* name;
    std::vector<std::vector<int>> chains; // cada uma: ops[0]=portadora .. back()=topo(feedback)
    std::vector<AlgorithmFork> forks;
    std::vector<AlgorithmMerge> merges;
};

// Indices 0-7 (nao 1-8) na definicao -- a UI/nomes exibem op+1 pro usuario.
inline const std::vector<AlgorithmDef>& algorithmTable() {
    static const std::vector<AlgorithmDef> table = {
        // --- 1 portadora --------------------------------------------------
        {"1 cadeia unica (8 em serie)", {{0,1,2,3,4,5,6,7}}, {}, {}},

        // --- 2 portadoras ---------------------------------------------------
        {"2 cadeias: 7+1", {{0,1,2,3,4,5,6},{7}}, {}, {}},
        {"2 cadeias: 6+2", {{0,1,2,3,4,5},{6,7}}, {}, {}},
        {"2 cadeias: 5+3", {{0,1,2,3,4},{5,6,7}}, {}, {}},
        {"2 cadeias: 4+4", {{0,1,2,3},{4,5,6,7}}, {}, {}},

        // --- 3 portadoras -----------------------------------------------
        {"3 cadeias: 6+1+1", {{0,1,2,3,4,5},{6},{7}}, {}, {}},
        {"3 cadeias: 5+2+1", {{0,1,2,3,4},{5,6},{7}}, {}, {}},
        {"3 cadeias: 4+3+1", {{0,1,2,3},{4,5,6},{7}}, {}, {}},
        {"3 cadeias: 4+2+2", {{0,1,2,3},{4,5},{6,7}}, {}, {}},
        {"3 cadeias: 3+3+2", {{0,1,2},{3,4,5},{6,7}}, {}, {}},

        // --- 4 portadoras -----------------------------------------------
        {"4 cadeias: 5+1+1+1", {{0,1,2,3,4},{5},{6},{7}}, {}, {}},
        {"4 cadeias: 4+2+1+1", {{0,1,2,3},{4,5},{6},{7}}, {}, {}},
        {"4 cadeias: 3+3+1+1", {{0,1,2},{3,4,5},{6},{7}}, {}, {}},
        {"4 cadeias: 3+2+2+1", {{0,1,2},{3,4},{5,6},{7}}, {}, {}},
        {"4 cadeias: 2+2+2+2", {{0,1},{2,3},{4,5},{6,7}}, {}, {}},

        // --- 5 portadoras -----------------------------------------------
        {"5 cadeias: 4+1+1+1+1", {{0,1,2,3},{4},{5},{6},{7}}, {}, {}},
        {"5 cadeias: 3+2+1+1+1", {{0,1,2},{3,4},{5},{6},{7}}, {}, {}},
        {"5 cadeias: 2+2+2+1+1", {{0,1},{2,3},{4,5},{6},{7}}, {}, {}},

        // --- 6 portadoras -----------------------------------------------
        {"6 cadeias: 3+1+1+1+1+1", {{0,1,2},{3},{4},{5},{6},{7}}, {}, {}},
        {"6 cadeias: 2+2+1+1+1+1", {{0,1},{2,3},{4},{5},{6},{7}}, {}, {}},

        // --- 7 portadoras -----------------------------------------------
        {"7 cadeias: 2+1x6", {{0,1},{2},{3},{4},{5},{6},{7}}, {}, {}},

        // --- 8 portadoras (totalmente paralelo) --------------------------
        {"8 portadoras em paralelo (aditivo puro)", {{0},{1},{2},{3},{4},{5},{6},{7}}, {}, {}},

        // --- forks: 1 modulador compartilhado alimentando 2+ portadoras ---
        {"Fork: 2 portadoras (0,1) <- cadeia de 6 (2..7)", {}, {{{0,1}, {2,3,4,5,6,7}}}, {}},
        {"Fork: 2 portadoras (0,1)<-2<-3(fb); + cadeia 4<-5<-6<-7(fb)",
         {{4,5,6,7}}, {{{0,1}, {2,3}}}, {}},
        {"Fork: 3 portadoras (0,1,2)<-3<-4<-5(fb); + cadeia 6<-7(fb)",
         {{6,7}}, {{{0,1,2}, {3,4,5}}}, {}},
        {"2 forks simetricos: (0,1)<-2<-3(fb) e (4,5)<-6<-7(fb)",
         {}, {{{0,1},{2,3}}, {{4,5},{6,7}}}, {}},
        {"Fork: 4 portadoras (0,1,2,3)<-cadeia de 4 (4..7)", {}, {{{0,1,2,3}, {4,5,6,7}}}, {}},
        {"Convergente: portadora0 <- (1<-3(fb)) e <- (2<-4(fb)); + cadeia 5<-6<-7(fb)",
         {{5,6,7}}, {}, {{0, {{1,3},{2,4}}}}},
        {"Convergente triplo: portadora0 <- 1(fb), <- 2(fb), <- 3(fb); + cadeia 4<-5<-6<-7(fb)",
         {{4,5,6,7}}, {}, {{0, {{1},{2},{3}}}}},
        {"Fork: 3 portadoras (0,1,2)<-3<-4(fb); + cadeia 5<-6<-7(fb)",
         {{5,6,7}}, {{{0,1,2}, {3,4}}}, {}},
        {"Fork: 2 portadoras (0,1)<-2<-3<-4(fb); + cadeia 5<-6<-7(fb)",
         {{5,6,7}}, {{{0,1}, {2,3,4}}}, {}},
        {"Fork: 5 portadoras (0,1,2,3,4)<-5<-6<-7(fb)", {}, {{{0,1,2,3,4}, {5,6,7}}}, {}},
    };
    return table;
}

constexpr int kNumAlgorithms = 32;

inline const char* algorithmName(int number) {
    const auto& t = algorithmTable();
    if (number < 1 || number > static_cast<int>(t.size())) return "?";
    return t[static_cast<size_t>(number - 1)].name;
}

// Aplica o algoritmo `number` (1..32) em v.matrix + v.op[*].feedback.
// NAO mexe em outputLevel/ampEG/waveform/etc dos operadores -- so a
// topologia (quem modula quem, quem e portadora, onde tem feedback).
// Escreve a topologia do algoritmo `number` em `m`; os operadores no fim de
// cada cadeia ganham feedback 0.3 via setFeedback(op). [2026-10-24: separado
// de applyAlgorithm pra servir tambem os slots do morph de algoritmos.]
template <typename SetFeedback>
inline bool writeAlgorithm(AlgorithmMatrix& m, int number, SetFeedback setFeedback) {
    const auto& t = algorithmTable();
    if (number < 1 || number > static_cast<int>(t.size())) return false;
    const AlgorithmDef& def = t[static_cast<size_t>(number - 1)];
    m = AlgorithmMatrix{};
    for (const auto& chain : def.chains) {
        if (chain.empty()) continue;
        m.outputMix[chain[0]] = 1.0f;
        for (size_t i = 0; i + 1 < chain.size(); ++i) m.mod[chain[i]][chain[i + 1]] = 1.0f;
        setFeedback(chain.back());
    }
    for (const auto& fork : def.forks) {
        if (fork.modChain.empty()) continue;
        int sharedMod = fork.modChain[0];
        for (int carrier : fork.carriers) {
            m.outputMix[carrier] = 1.0f;
            m.mod[carrier][sharedMod] = 1.0f;
        }
        for (size_t i = 0; i + 1 < fork.modChain.size(); ++i)
            m.mod[fork.modChain[i]][fork.modChain[i + 1]] = 1.0f;
        setFeedback(fork.modChain.back());
    }
    for (const auto& merge : def.merges) {
        m.outputMix[merge.carrier] = 1.0f;
        for (const auto& modChain : merge.modChains) {
            if (modChain.empty()) continue;
            m.mod[merge.carrier][modChain[0]] = 1.0f;
            for (size_t i = 0; i + 1 < modChain.size(); ++i)
                m.mod[modChain[i]][modChain[i + 1]] = 1.0f;
            setFeedback(modChain.back());
        }
    }
    return true;
}

// Slot 1 (VoiceParams::matrix): feedback vai pro op.feedback de cada operador (como sempre foi).
inline void applyAlgorithm(VoiceParams& v, int number) {
    if (number < 1 || number > static_cast<int>(algorithmTable().size())) return;
    for (auto& op : v.op) op.feedback = 0.0f;
    writeAlgorithm(v.matrix, number, [&](int op) { v.op[op].feedback = 0.3f; });
}

// Qualquer slot do morph: feedback na DIAGONAL da matriz (soma com op.feedback
// no motor), pra ele fazer morph junto com o resto da topologia.
inline void applyAlgorithmToMatrix(AlgorithmMatrix& m, int number) {
    writeAlgorithm(m, number, [&](int op) { m.mod[op][op] = 0.3f; });
}

} // namespace dx7fm
