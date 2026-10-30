#pragma once
#include "dx7_fxrack.h"
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

// [ADICIONADO em 2026-10-29] Presets de fabrica do rack de efeitos, montados em
// codigo (banco "Fabrica" na aba Efeitos). Os parametros vao por nome
// (FxParamInfo::id), como no JSON; um nome errado conta em fxFactoryErrors()
// (o demo testa que fica em zero).
// Convencao dos envios: a Cadeia 1 e "em linha" (o mix de cada slot faz o
// seco/molhado) e a Cadeia 2, quando usada, e um retorno 100% molhado
// alimentado pelo envio 2 de cada parte -- como numa mesa.
namespace dx7fm_app::fxfactory {
using namespace dx7fm;

struct FxFactoryPreset {
    std::string name, category, description;
    FxRackParams rack;
};

inline int& fxFactoryErrors() { static int n = 0; return n; }

using P = std::initializer_list<std::pair<const char*, float>>;

inline FxSlotParams& slot(FxRackParams& r, int c, int s, FxKind k, float mix, P params = {}) {
    FxSlotParams& sp = r.chain[c].slot[s];
    sp.setKind(k);
    sp.mix = mix;
    const FxInfo& info = fxInfo(k);
    for (const auto& [id, v] : params) {
        int i = 0;
        while (i < info.nParams && std::string(info.params[i].id) != id) ++i;
        if (i == info.nParams) { ++fxFactoryErrors(); continue; }
        sp.p[i] = v;
    }
    return sp;
}

// retorno: todas as partes mandam `amount` pra cadeia 2 (o seco segue pela 1)
inline void sendAllTo2(FxRackParams& r, float amount) {
    for (auto& s : r.send) s[1] = amount;
}

inline void expression(FxRackParams& r, int c, int s, const char* id, float lo, float hi) {
    const FxInfo& info = fxInfo(r.chain[c].slot[s].kind);
    for (int i = 0; i < info.nParams; ++i)
        if (std::string(info.params[i].id) == id) { r.expr = {c, s, i, lo, hi}; return; }
    ++fxFactoryErrors();
}

inline std::vector<FxFactoryPreset> fxFactoryBank() {
    std::vector<FxFactoryPreset> out;
    auto add = [&](const char* name, const char* cat, const char* desc, auto build) {
        FxFactoryPreset p{name, cat, desc, {}};
        build(p.rack);
        sanitize(p.rack);
        out.push_back(std::move(p));
    };

    // --- basicos ---------------------------------------------------------
    add("Sala limpa", "Basicos", "so uma sala pequena, discreta", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Room, 0.22f, {{"decay", 0.7f}, {"size", 12}, {"early", 0.6f}});
    });
    add("Lead de estadio", "Basicos", "overdrive -> delay ping-pong em 1/4. -> hall", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Overdrive, 1.0f, {{"drive", 0.45f}, {"output", 0.8f}});
        slot(r, 0, 1, FxKind::DigitalDelay, 0.28f, {{"sync", 4}, {"feedback", 0.4f}, {"pingpong", 1}, {"highCut", 6000}});
        slot(r, 0, 2, FxKind::Hall, 0.3f, {{"decay", 2.8f}, {"predelay", 25}});
    });
    add("Orgao rotary", "Basicos", "valvula -> caixa rotativa (expressao: lento/rapido) -> sala", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Tube, 1.0f, {{"input", 0.55f}, {"tube", 0.6f}});
        slot(r, 0, 1, FxKind::Rotary, 1.0f, {{"doppler", 0.75f}, {"drive", 0.2f}});
        slot(r, 0, 2, FxKind::Room, 0.18f, {{"decay", 0.9f}});
        expression(r, 0, 1, "speed", 0, 1);
    });
    add("Funk wah", "Basicos", "compressor -> auto-wah -> sala curta", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Compressor, 1.0f, {{"threshold", -22}, {"ratio", 5}, {"attack", 5}, {"makeup", 6}});
        slot(r, 0, 1, FxKind::Wah, 1.0f, {{"mode", 1}, {"sensitivity", 12}});
        slot(r, 0, 2, FxKind::Room, 0.15f, {{"decay", 0.5f}});
    });
    add("Wah no pedal", "Basicos", "wah classico controlado pelo pedal de expressao", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Wah, 1.0f, {{"mode", 0}, {"position", 0.3f}});
        slot(r, 0, 1, FxKind::Overdrive, 1.0f, {{"drive", 0.3f}, {"output", 0.85f}});
        expression(r, 0, 0, "position", 0, 1);
    });
    add("Bateria comprimida", "Basicos", "compressor rapido + densidade + sala", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Compressor, 1.0f, {{"threshold", -26}, {"ratio", 8}, {"attack", 2}, {"release", 60}, {"makeup", 9}});
        slot(r, 0, 1, FxKind::Density, 0.6f, {{"density", 0.35f}});
        slot(r, 0, 2, FxKind::Room, 0.2f, {{"decay", 0.6f}, {"boost", 0.7f}});
    });

    // --- delays e fita ------------------------------------------------------
    add("Dub delay", "Delays e fita", "eco de fita realimentado + mola no retorno", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::TapeDelay, 0.4f, {{"timeL", 0.375f}, {"timeR", 0.5f}, {"feedback", 0.68f}, {"cross", 0.4f}, {"lowCut", 200}, {"highCut", 3500}, {"drive", 0.4f}, {"wow", 0.3f}});
        slot(r, 1, 0, FxKind::Spring, 1.0f, {{"size", 0.6f}, {"decay", 0.6f}});
        sendAllTo2(r, 0.35f);
    });
    add("Surf spring", "Delays e fita", "mola bem molhada + slapback", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::DigitalDelay, 0.2f, {{"time", 110}, {"feedback", 0.1f}, {"highCut", 5000}});
        slot(r, 0, 1, FxKind::Spring, 0.5f, {{"size", 0.45f}, {"decay", 0.55f}, {"reflections", 1}});
    });
    add("Fita cassete", "Delays e fita", "CHOW Tape com wow/flutter e degradacao + filtro lento", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::TapeSim, 1.0f, {{"drive", 0.7f}, {"saturation", 0.6f}, {"speed", 4.75f}, {"spacing", 4}, {"wow", 0.35f}, {"flutter", 0.3f}, {"degrade", 0.3f}, {"degradeAmount", 0.25f}});
        slot(r, 0, 1, FxKind::LoFi, 0.5f, {{"rate", 0.15f}, {"bits", 0.8f}});
        slot(r, 0, 2, FxKind::Room, 0.15f, {{"decay", 0.6f}});
    });
    add("Eco magnetico", "Delays e fita", "Magneto: varias cabecas difusas, no tempo (1/4)", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::TapeLite, 1.0f);
        slot(r, 1, 0, FxKind::Magneto, 1.0f, {{"sync", 3}, {"heads", 7}, {"feedback", 0.5f}, {"diffusion", 0.7f}, {"wow", 0.4f}});
        sendAllTo2(r, 0.5f);
    });
    add("Reverso", "Delays e fita", "delay reverso em 1/2 + reverb Nonlinear reverse", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::ReverseDelay, 0.45f, {{"sync", 2}, {"feedback", 0.35f}});
        slot(r, 1, 0, FxKind::Nonlinear, 1.0f, {{"shape", 0}, {"time", 450}});
        sendAllTo2(r, 0.3f);
    });

    // --- reverbs (maquinas) ---------------------------------------------------
    add("Ambient shimmer", "Reverbs", "chorus + Shimmer (+12/+7) no retorno", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Chorus, 0.35f);
        slot(r, 1, 0, FxKind::Shimmer, 1.0f, {{"decay", 6}, {"interval1", 12}, {"interval2", 7}, {"amount", 0.55f}});
        sendAllTo2(r, 0.6f);
    });
    add("Pad lavado", "Reverbs", "Cloud longo -> Bloom: a cauda floresce devagar", [](FxRackParams& r) {
        slot(r, 1, 0, FxKind::Cloud, 1.0f, {{"decay", 0.8f}, {"size", 0.8f}, {"mod", 0.6f}});
        slot(r, 1, 1, FxKind::Bloom, 0.5f, {{"decay", 0.8f}, {"length", 500}});
        sendAllTo2(r, 0.7f);
    });
    add("Swell de cordas", "Reverbs", "volume sobe sozinho em cada nota, dentro de um reverb", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Swell, 1.0f, {{"rise", 900}, {"reverb", 0.6f}, {"decay", 0.7f}});
    });
    add("Coral", "Reverbs", "Chorale: cauda cantando a vogal O", [](FxRackParams& r) {
        slot(r, 1, 0, FxKind::Chorale, 1.0f, {{"decay", 4}, {"vowel", 3}, {"amount", 0.8f}});
        sendAllTo2(r, 0.5f);
    });
    add("Plate vocal", "Reverbs", "plate brilhante com pre-delay", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Plate, 0.28f, {{"decay", 1.8f}, {"predelay", 30}, {"tone", 9000}});
    });
    add("Bateria gated", "Reverbs", "Nonlinear Gate: cauda cortada anos 80", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Compressor, 1.0f, {{"threshold", -20}, {"ratio", 4}, {"attack", 3}});
        slot(r, 0, 1, FxKind::Nonlinear, 0.45f, {{"shape", 3}, {"time", 250}});
    });
    add("Sala de concerto", "Reverbs", "Reflections (Concert Venue) + hall grande no retorno", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Reflections, 0.3f, {{"program", 2}});
        slot(r, 1, 0, FxKind::Hall, 1.0f, {{"decay", 3.5f}, {"size", 40}, {"predelay", 20}});
        sendAllTo2(r, 0.35f);
    });

    // --- granular (Micro) ---------------------------------------------------
    add("Mosaico", "Granular", "Micro Mosaic: micro-loops em camadas + hall", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Micro, 0.5f, {{"variation", 0}, {"activity", 0.6f}, {"repeats", 0.4f}, {"space", 0.4f}});
        slot(r, 0, 1, FxKind::Hall, 0.25f, {{"decay", 3}});
    });
    add("Nevoa", "Granular", "Micro Haze: nuvem de graos (expressao: Hold congela)", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Micro, 0.6f, {{"variation", 3}, {"activity", 0.7f}, {"spread", 0.8f}, {"space", 0.6f}});
        expression(r, 0, 0, "hold", 0, 1);
    });
    add("Arpejo granular", "Granular", "Micro Arp no tempo + delay 1/8.", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Micro, 0.5f, {{"variation", 8}, {"sync", 6}, {"activity", 0.6f}});
        slot(r, 0, 1, FxKind::DigitalDelay, 0.25f, {{"sync", 7}, {"feedback", 0.35f}, {"pingpong", 1}});
    });

    // --- roteamento por parte -------------------------------------------------
    add("Partes separadas", "Roteamento", "A: drive+delay na cadeia 1; B: hall na cadeia 2; C/D secas", [](FxRackParams& r) {
        slot(r, 0, 0, FxKind::Overdrive, 1.0f, {{"drive", 0.5f}, {"output", 0.8f}});
        slot(r, 0, 1, FxKind::DigitalDelay, 0.3f, {{"sync", 6}});
        slot(r, 1, 0, FxKind::Hall, 0.4f, {{"decay", 3}});
        r.send[0][0] = 1; r.send[0][1] = 0;
        r.send[1][0] = 0; r.send[1][1] = 1;
        for (int p = 2; p < kFxParts; ++p) { r.send[p][0] = 0; r.send[p][1] = 0; r.dry[p] = 1; }
    });
    add("Serie: lo-fi no reverb", "Roteamento", "cadeia 1 (reverb) em serie com a 2 (fita + lo-fi)", [](FxRackParams& r) {
        r.algorithm = kFxAlgoSerial;
        slot(r, 0, 0, FxKind::Cloud, 0.4f);
        slot(r, 1, 0, FxKind::TapeSim, 1.0f, {{"wow", 0.5f}, {"flutter", 0.4f}, {"speed", 3.75f}});
        slot(r, 1, 1, FxKind::FilterLfo, 1.0f, {{"type", 0}, {"cutoff", 2500}, {"rate", 0.2f}, {"depth", 1}});
    });

    // --- algoritmos de roteamento (2026-10-30) ----------------------------------
    add("Divide e junta", "Algoritmos", "compressor -> (delay | chorus) -> hall", [](FxRackParams& r) {
        r.algorithm = 3; // Divide e junta
        slot(r, 0, 0, FxKind::Compressor, 1.0f, {{"threshold", -20}, {"ratio", 3}, {"makeup", 4}});
        slot(r, 0, 2, FxKind::DigitalDelay, 1.0f, {{"sync", 7}, {"feedback", 0.45f}, {"pingpong", 1}});
        slot(r, 1, 0, FxKind::Chorus, 1.0f, {{"depth", 0.7f}});
        slot(r, 1, 2, FxKind::Hall, 0.3f, {{"decay", 3}});
        r.chain[0].level = 0.6f; // os dois ramos somam no hall
    });
    add("Drive com reverb ao lado", "Algoritmos", "Serie + direto: drive sai seco e alimenta um Cloud 100% molhado", [](FxRackParams& r) {
        r.algorithm = 2; // Serie + direto
        slot(r, 0, 0, FxKind::Overdrive, 1.0f, {{"drive", 0.55f}, {"output", 0.8f}});
        slot(r, 0, 1, FxKind::Eq3, 1.0f, {{"highGain", -4}});
        slot(r, 1, 0, FxKind::Cloud, 1.0f, {{"decay", 0.75f}});
        r.chain[1].level = 0.8f;
    });
    add("Pos comum", "Algoritmos", "fita na cadeia 1 | tremolo em 2.1 -> um reverb Plate pra tudo", [](FxRackParams& r) {
        r.algorithm = 5; // Pos comum
        slot(r, 0, 0, FxKind::TapeSim, 1.0f, {{"wow", 0.3f}, {"flutter", 0.2f}});
        slot(r, 1, 0, FxKind::Tremolo, 1.0f, {{"speed", 0.4f}});
        slot(r, 1, 2, FxKind::Plate, 0.3f, {{"decay", 2}});
        for (auto& sd : r.send) sd[1] = 1.0f; // a parte entra nos dois ramos
        r.chain[0].level = 0.5f; r.chain[1].level = 1.0f;
    });
    add("Quatro ramos", "Algoritmos", "4 efeitos lado a lado, cada um com o proprio mix", [](FxRackParams& r) {
        r.algorithm = 6; // Quatro ramos
        slot(r, 0, 0, FxKind::Room, 1.0f, {{"decay", 0.6f}});
        slot(r, 0, 2, FxKind::PitchShift, 1.0f, {{"semitones", 12}});
        slot(r, 1, 0, FxKind::DigitalDelay, 1.0f, {{"sync", 4}, {"feedback", 0.3f}});
        slot(r, 1, 2, FxKind::Micro, 1.0f, {{"variation", 3}});
        r.chain[0].level = 0.35f; r.chain[1].level = 0.35f;
        for (auto& sd : r.send) sd[1] = 1.0f;
    });
    return out;
}

} // namespace dx7fm_app::fxfactory
