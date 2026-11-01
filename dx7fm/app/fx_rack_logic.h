#pragma once
#include "dx7_fxrack.h"
#include "patch_browser.h"
#include <string>
#include <vector>

// [ADICIONADO em 2026-10-29] Logica da pedaleira (aba Efeitos) sem ImGui, pra
// ser testada no demo: estado de edicao/publicado/desfazer, mover/copiar
// pedais e a regra dos parametros "rebuild" durante o arraste.
namespace dx7fm_app::fxui {
using namespace dx7fm;

struct FxRackUi {
    FxRackParams edit;       // o que a UI mostra/edita (fonte da verdade)
    FxRackParams live;       // o ultimo publicado no synth
    FxRackParams committed;  // ultimo passo de desfazer
    std::vector<FxRackParams> undo, redo;
    bool saveDirty = false;  // ultimo estado (fxrack.json) a salvar
    double changedAt = 0.0;
    // barra de presets
    char name[64] = "Meu preset";
    std::string current = "(sem preset)", status;
    std::vector<PatchEntry> user;
    bool userListDirty = true;
    float pedalTest = 0.0f;  // slider "pedal (teste)"
};

inline bool slotEqual(const FxSlotParams& a, const FxSlotParams& b) {
    if (a.kind != b.kind || a.on != b.on || a.mix != b.mix) return false;
    for (int i = 0; i < kFxParams; ++i) if (a.p[i] != b.p[i]) return false;
    return true;
}

inline bool rackEqual(const FxRackParams& a, const FxRackParams& b) {
    if (a.link != b.link) return false;
    for (int c = 0; c < kFxChains; ++c) {
        if (a.chain[c].level != b.chain[c].level || a.chain[c].pan != b.chain[c].pan || a.chain[c].algorithm != b.chain[c].algorithm) return false;
        for (int s = 0; s < kFxSlots; ++s) if (!slotEqual(a.chain[c].slot[s], b.chain[c].slot[s])) return false;
    }
    for (int p = 0; p < kFxParts; ++p) {
        if (a.dry[p] != b.dry[p]) return false;
        for (int c = 0; c < kFxChains; ++c) if (a.send[p][c] != b.send[p][c]) return false;
    }
    for (int k = 0; k < kFxLfos; ++k)
        if (a.lfo[k].shape != b.lfo[k].shape || a.lfo[k].rate != b.lfo[k].rate || a.lfo[k].sync != b.lfo[k].sync) return false;
    for (int k = 0; k < kFxModRoutes; ++k) {
        const FxModRoute &m = a.mod[k], &n = b.mod[k];
        if (m.src != n.src || m.part != n.part || m.chain != n.chain || m.slot != n.slot || m.param != n.param || m.depth != n.depth) return false;
    }
    const FxExpression &x = a.expr, &y = b.expr;
    return x.chain == y.chain && (x.chain < 0 || (x.slot == y.slot && x.param == y.param && x.min == y.min && x.max == y.max));
}

// Enquanto um knob "rebuild" (tamanho, pre-delay dos reverbs) e arrastado,
// publicar a cada quadro recriaria o DSP do slot (aloca, zera a cauda) dezenas
// de vezes: publica a edicao com esses parametros ainda no valor publicado;
// o valor novo vai quando o mouse solta.
inline FxRackParams withLiveRebuild(const FxRackParams& edit, const FxRackParams& live) {
    FxRackParams out = edit;
    for (int c = 0; c < kFxChains; ++c)
        for (int s = 0; s < kFxSlots; ++s) {
            FxSlotParams& o = out.chain[c].slot[s];
            const FxSlotParams& l = live.chain[c].slot[s];
            if (o.kind != l.kind) continue; // trocou de efeito: recria de qualquer jeito
            const FxInfo& info = fxInfo(o.kind);
            for (int i = 0; i < info.nParams; ++i)
                if (info.params[i].rebuild) o.p[i] = l.p[i];
        }
    return out;
}

// O efeito do pedal mudou: as rotas apontavam pra parametros de outro efeito
inline void clearSlotMods(FxRackParams& r, int chain, int slot) {
    for (auto& m : r.mod) if (m.chain == chain && m.slot == slot) m = FxModRoute{};
}

// Arrastar um pedal: copy=false troca os dois slots (o pedal de expressao vai
// junto com o seu slot); copy=true copia por cima do destino.
inline void moveSlot(FxRackParams& r, int fromChain, int fromSlot, int toChain, int toSlot, bool copy) {
    if (fromChain == toChain && fromSlot == toSlot) return;
    FxSlotParams& a = r.chain[fromChain].slot[fromSlot];
    FxSlotParams& b = r.chain[toChain].slot[toSlot];
    FxExpression& e = r.expr;
    if (copy) {
        b = a;
        if (e.chain == toChain && e.slot == toSlot) e.chain = -1; // o alvo foi sobrescrito
        clearSlotMods(r, toChain, toSlot);
        return;
    }
    std::swap(a, b);
    for (auto& m : r.mod) { // as rotas de modulacao vao junto com o pedal
        if (m.src == FxModSource::None) continue;
        if (m.chain == fromChain && m.slot == fromSlot) { m.chain = toChain; m.slot = toSlot; }
        else if (m.chain == toChain && m.slot == toSlot) { m.chain = fromChain; m.slot = fromSlot; }
    }
    if (e.chain == fromChain && e.slot == fromSlot) { e.chain = toChain; e.slot = toSlot; }
    else if (e.chain == toChain && e.slot == toSlot) { e.chain = fromChain; e.slot = fromSlot; }
}

// Troca o rack inteiro (preset, desfazer, arquivo): publica ja.
template <class SynthT>
inline void setRack(FxRackUi& ui, SynthT& synth, const FxRackParams& r, double now) {
    ui.edit = r;
    sanitize(ui.edit);
    synth.setFxRack(ui.edit);
    ui.live = ui.edit;
    ui.saveDirty = true;
    ui.changedAt = now;
}

// Fim de cada quadro da aba: publica o que mudou e fecha passos de desfazer.
// dragging = botao esquerdo apertado; anyActive = algum widget ativo.
template <class SynthT>
inline void syncRack(FxRackUi& ui, SynthT& synth, bool dragging, bool anyActive, double now) {
    sanitize(ui.edit);
    if (!rackEqual(ui.edit, ui.live)) {
        FxRackParams pub = dragging ? withLiveRebuild(ui.edit, ui.live) : ui.edit;
        if (!rackEqual(pub, ui.live)) {
            synth.setFxRack(pub);
            ui.live = pub;
            ui.saveDirty = true;
            ui.changedAt = now;
        }
    }
    if (!dragging && !anyActive && !rackEqual(ui.edit, ui.committed)) {
        ui.undo.push_back(ui.committed);
        if (ui.undo.size() > 100) ui.undo.erase(ui.undo.begin());
        ui.committed = ui.edit;
        ui.redo.clear();
    }
}

template <class SynthT>
inline void undoRack(FxRackUi& ui, SynthT& synth, double now) {
    if (!rackEqual(ui.edit, ui.committed)) { ui.undo.push_back(ui.committed); ui.committed = ui.edit; ui.redo.clear(); }
    if (ui.undo.empty()) return;
    ui.redo.push_back(ui.committed);
    ui.committed = ui.undo.back();
    ui.undo.pop_back();
    setRack(ui, synth, ui.committed, now);
}

template <class SynthT>
inline void redoRack(FxRackUi& ui, SynthT& synth, double now) {
    if (ui.redo.empty()) return;
    ui.undo.push_back(ui.committed);
    ui.committed = ui.redo.back();
    ui.redo.pop_back();
    setRack(ui, synth, ui.committed, now);
}

// "A|B|C": n-esima escolha e quantas ha
inline int choiceCount(const char* choices) {
    int n = 1;
    for (const char* c = choices; *c; ++c) n += *c == '|';
    return n;
}
inline std::string choiceAt(const char* choices, int k) {
    std::string out;
    int i = 0;
    for (const char* c = choices; *c; ++c) {
        if (*c == '|') { if (i++ == k) return out; out.clear(); continue; }
        out += *c;
    }
    return i == k ? out : std::string("?");
}

} // namespace dx7fm_app::fxui
