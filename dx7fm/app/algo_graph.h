#pragma once
#include "dx7_algorithms.h"
#include "dx7_params.h"
#include "dx7_patch_io.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// [ADICIONADO em 2026-10-31] Logica do editor grafico de algoritmos (app/algo_editor.h),
// sem ImGui, testada no demo: ligar/desligar conexoes, organizar os operadores
// na tela e presets de algoritmo do usuario (pasta algorithms/).
namespace dx7fm_app::algograph {
using namespace dx7fm;

// posicao do centro de cada operador no canvas, 0..1
struct Layout {
    float x[kNumOps] = {};
    float y[kNumOps] = {};
};

constexpr float kDefaultMod = 0.5f, kDefaultFeedback = 0.3f;

inline bool hasEdge(const AlgorithmMatrix& m, int from, int to) { return m.mod[to][from] != 0.0f || m.ring[to][from]; }

// from modula to. from == to = feedback (diagonal). Nao mexe numa conexao que ja existe.
inline void connect(AlgorithmMatrix& m, int from, int to) {
    if (hasEdge(m, from, to)) return;
    m.mod[to][from] = from == to ? kDefaultFeedback : kDefaultMod;
}
inline void disconnect(AlgorithmMatrix& m, int from, int to) {
    m.mod[to][from] = 0.0f;
    m.ring[to][from] = false;
}
// conexao FM <-> ring (ring multiplica; a quantidade so vale pra FM)
inline void setRing(AlgorithmMatrix& m, int from, int to, bool ring) {
    if (from == to) return;
    m.ring[to][from] = ring;
    m.mod[to][from] = ring ? 0.0f : kDefaultMod;
}
inline void clearAll(AlgorithmMatrix& m) { m = AlgorithmMatrix{}; }

// to alcanca from pelas conexoes? (a conexao from -> to fecha um laco, e o motor
// usa a amostra anterior nela)
inline bool closesLoop(const AlgorithmMatrix& m, int from, int to) {
    if (from == to) return true;
    bool seen[kNumOps] = {};
    seen[to] = true;
    for (bool grew = true; grew;) {
        grew = false;
        for (int a = 0; a < kNumOps; ++a) {
            if (!seen[a]) continue;
            for (int b = 0; b < kNumOps; ++b)
                if (!seen[b] && b != a && hasEdge(m, a, b)) { seen[b] = true; grew = true; }
        }
    }
    return seen[from];
}

// chega na saida, direto ou modulando quem chega?
inline bool reachesOutput(const AlgorithmMatrix& m, int op) {
    bool seen[kNumOps] = {};
    seen[op] = true;
    for (bool grew = true; grew;) {
        grew = false;
        for (int a = 0; a < kNumOps; ++a) {
            if (!seen[a]) continue;
            if (m.outputMix[a] != 0.0f) return true;
            for (int b = 0; b < kNumOps; ++b)
                if (!seen[b] && b != a && hasEdge(m, a, b)) { seen[b] = true; grew = true; }
        }
    }
    return false;
}

inline bool anyCarrier(const AlgorithmMatrix& m) {
    for (float o : m.outputMix) if (o != 0.0f) return true;
    return false;
}

// Camadas: portadoras embaixo, cada modulador acima de quem ele modula
// (caminho mais longo ate a saida; lacos sao cortados). Sem conexao: fileira de cima.
inline Layout autoLayout(const AlgorithmMatrix& m) {
    int layer[kNumOps];
    for (int i = 0; i < kNumOps; ++i) layer[i] = m.outputMix[i] != 0.0f ? 0 : -1;
    for (int pass = 0; pass < kNumOps; ++pass)
        for (int to = 0; to < kNumOps; ++to) {
            if (layer[to] < 0) continue;
            for (int from = 0; from < kNumOps; ++from)
                if (from != to && hasEdge(m, from, to) && layer[from] < layer[to] + 1 && !closesLoop(m, from, to))
                    layer[from] = layer[to] + 1;
        }
    int maxLayer = 0;
    for (int l : layer) maxLayer = std::max(maxLayer, l);
    for (int to = 0; to < kNumOps; ++to) // modulam alguem fora do caminho (ou lacos sem saida)
        for (int from = 0; from < kNumOps; ++from)
            if (from != to && hasEdge(m, from, to) && layer[from] < 0 && layer[to] >= 0) layer[from] = std::min(layer[to] + 1, kNumOps - 1);
    bool anyLoose = false;
    for (int l : layer) anyLoose = anyLoose || l < 0;
    for (int l : layer) maxLayer = std::max(maxLayer, l);
    const int rows = maxLayer + 1 + (anyLoose ? 1 : 0);
    Layout out;
    for (int r = 0; r < rows; ++r) {
        const int wanted = r == rows - 1 && anyLoose ? -1 : r;
        int count = 0;
        for (int l : layer) count += l == wanted;
        int k = 0;
        for (int i = 0; i < kNumOps; ++i) {
            if (layer[i] != wanted) continue;
            out.x[i] = (static_cast<float>(k) + 0.5f) / static_cast<float>(count);
            out.y[i] = rows == 1 ? 0.5f : 1.0f - static_cast<float>(r) / static_cast<float>(rows - 1);
            ++k;
        }
    }
    return out;
}

// distancia de um ponto ate a curva de uma conexao (bezier cubica com
// tangentes verticais, em 24 segmentos): selecao por clique
inline float bezierPoint(float a, float b, float c, float d, float t) {
    const float u = 1.0f - t;
    return u * u * u * a + 3.0f * u * u * t * b + 3.0f * u * t * t * c + t * t * t * d;
}
inline float distanceToEdge(float px, float py, float x0, float y0, float x1, float y1) {
    const float bend = std::max(30.0f, std::fabs(y1 - y0) * 0.5f);
    float best = 1e9f, lx = x0, ly = y0;
    for (int s = 1; s <= 24; ++s) {
        const float t = static_cast<float>(s) / 24.0f;
        const float x = bezierPoint(x0, x0, x1, x1, t), y = bezierPoint(y0, y0 + bend, y1 - bend, y1, t);
        const float dx = x - lx, dy = y - ly, len2 = dx * dx + dy * dy;
        const float k = len2 > 0.0f ? std::clamp(((px - lx) * dx + (py - ly) * dy) / len2, 0.0f, 1.0f) : 0.0f;
        const float ex = lx + k * dx - px, ey = ly + k * dy - py;
        best = std::min(best, std::sqrt(ex * ex + ey * ey));
        lx = x; ly = y;
    }
    return best;
}

// --- feedback: dois lugares no patch (OperatorParams::feedback + diagonal) --------
inline float feedbackOf(const VoiceParams& v, int slot, int op) { return v.algo(slot).mod[op][op] + v.op[op].feedback; }
// com 1 algoritmo fica em OperatorParams::feedback (o Feedback da tabela); com
// morph fica na diagonal do slot, pra fazer morph junto
inline void setFeedback(VoiceParams& v, int slot, int op, float amount) {
    if (v.activeAlgos() == 1) { v.matrix.mod[op][op] = 0.0f; v.op[op].feedback = std::clamp(amount, 0.0f, 1.0f); }
    else v.algo(slot).mod[op][op] = std::clamp(amount - v.op[op].feedback, -1.0f, 1.0f);
}

// --- presets do usuario ---------------------------------------------------------
struct Preset {
    AlgorithmMatrix matrix; // feedback sempre na diagonal
    Layout layout;
};

// O que o algoritmo do slot soa hoje, com o feedback dos operadores na diagonal
inline Preset presetFrom(const VoiceParams& v, int slot, const Layout& layout) {
    Preset p;
    p.matrix = v.algo(slot);
    for (int i = 0; i < kNumOps; ++i) p.matrix.mod[i][i] = std::clamp(feedbackOf(v, slot, i), -1.0f, 1.0f);
    p.layout = layout;
    return p;
}
// O inverso: com 1 algoritmo o feedback vai pra tabela; com morph, descontado do
// feedback dos operadores (que valem pra todos os slots), fica na diagonal.
inline void applyPreset(VoiceParams& v, int slot, const Preset& p) {
    AlgorithmMatrix& m = v.algo(slot);
    m = p.matrix;
    for (int i = 0; i < kNumOps; ++i) {
        if (v.activeAlgos() == 1) { v.op[i].feedback = std::clamp(p.matrix.mod[i][i], 0.0f, 1.0f); m.mod[i][i] = 0.0f; }
        else m.mod[i][i] = std::clamp(p.matrix.mod[i][i] - v.op[i].feedback, -1.0f, 1.0f);
    }
}

inline std::string presetToJson(const Preset& p) {
    JsonValue j = JsonValue::makeObject();
    j.set("kind", JsonValue::makeString("algorithm"));
    j.set("version", JsonValue::makeNumber(1));
    j.set("matrix", algorithmMatrixToJson(p.matrix));
    JsonValue pos = JsonValue::makeArray();
    for (int i = 0; i < kNumOps; ++i) {
        JsonValue xy = JsonValue::makeArray();
        xy.push(JsonValue::makeNumber(p.layout.x[i]));
        xy.push(JsonValue::makeNumber(p.layout.y[i]));
        pos.push(std::move(xy));
    }
    j.set("positions", std::move(pos));
    return j.dump();
}
// false = nao e um preset de algoritmo. Valores saneados; sem posicoes = organiza sozinho.
inline bool presetFromJson(const std::string& text, Preset& out) {
    JsonValue j = JsonValue::parse(text);
    if (j.stringAt("kind", "") != "algorithm" || !j.find("matrix")) return false;
    Preset p;
    p.matrix = algorithmMatrixFromJson(j.find("matrix"));
    auto san = [](float v, float lo, float hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; };
    for (int i = 0; i < kNumOps; ++i) {
        p.matrix.outputMix[i] = san(p.matrix.outputMix[i], 0.0f, 1.0f);
        for (int k = 0; k < kNumOps; ++k) p.matrix.mod[i][k] = san(p.matrix.mod[i][k], -1.0f, 1.0f);
    }
    p.layout = autoLayout(p.matrix);
    const JsonValue* pos = j.find("positions");
    if (pos && pos->arraySize() == static_cast<size_t>(kNumOps))
        for (int i = 0; i < kNumOps; ++i) {
            const JsonValue& xy = pos->at(static_cast<size_t>(i));
            if (xy.arraySize() != 2) continue;
            p.layout.x[i] = san(static_cast<float>(xy.at(0).asNumber(p.layout.x[i])), 0.0f, 1.0f);
            p.layout.y[i] = san(static_cast<float>(xy.at(1).asNumber(p.layout.y[i])), 0.0f, 1.0f);
        }
    out = p;
    return true;
}
inline bool savePresetFile(const std::filesystem::path& path, const Preset& p) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path);
    if (!f) return false;
    f << presetToJson(p) << "\n";
    return static_cast<bool>(f);
}
inline bool loadPresetFile(const std::filesystem::path& path, Preset& p) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return presetFromJson(ss.str(), p);
}

} // namespace dx7fm_app::algograph
