#pragma once
#include "dx7_alloc.h"
#include "dx7_algorithms.h"
#include "dx7_wavegen.h"
#include "additive_presets.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

// [ADICIONADO em 2026-10-28] Estrutura de um patch de fabrica + atalhos pra
// montar patches em codigo (ver factory_bank.h).
namespace dx7fm_app::factory {
using namespace dx7fm;

inline void eg(EGParams& e, std::initializer_list<int> r, std::initializer_list<int> l) {
    int i = 0;
    for (int v : r) e.rate[i++] = static_cast<uint8_t>(v);
    i = 0;
    for (int v : l) e.level[i++] = static_cast<uint8_t>(v);
}
inline void silenceAll(VoiceParams& v) { // comeca do zero: nenhum operador soando nem modulando
    v.matrix = AlgorithmMatrix{};
    for (auto& op : v.op) { op = OperatorParams{}; op.outputLevel = 0; }
}

struct FactoryPreset {
    PartParams part;
    std::string category, description;
    std::string family = "Demonstracao"; // "Demonstracao", "Estilo DX7", "Estilo SY77", "Originais"
    std::vector<int> notes; // o que tocar na previa
    double seconds = 3.0;
    double holdSec = 2.2;   // solta as notas aqui
    float bpm = 120.0f;
    float modWheel = -1.0f;
};

} // namespace dx7fm_app::factory
