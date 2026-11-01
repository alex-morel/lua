#pragma once
#include "algo_graph.h"
#include "patch_browser.h"
#include "ui_widgets.h"
#include "imgui.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// [ADICIONADO em 2026-10-31] Editor grafico de algoritmos: janela flutuante (da
// pra tocar enquanto edita). Arraste da bolinha de baixo de um operador ate
// outro pra modular; ate a barra SAIDA pra virar portadora; o circulo do canto
// liga o feedback. Logica e presets em algo_graph.h.
namespace dx7fm_app::algoed {
using namespace dx7fm;
namespace ag = algograph;

struct AlgoEditor {
    bool open = false;
    ag::Layout layout;
    AlgorithmMatrix seen;       // ultima matriz vista: mudou por fora, reorganiza
    int seenPart = -1, seenSlot = -1;
    bool seenValid = false;
    int connectFrom = -1;       // arrastando uma conexao
    int moving = -1;            // arrastando um operador
    // selecao: conexao from -> to (from == to = feedback), ou o fio de saida de um operador
    int selFrom = -1, selTo = -1, selOut = -1;
    char name[64] = "Meu algoritmo";
    std::string current, status;
    std::vector<PatchEntry> user;
    bool userDirty = true, confirmDelete = false;
};

inline bool matrixEqual(const AlgorithmMatrix& a, const AlgorithmMatrix& b) {
    for (int i = 0; i < kNumOps; ++i) {
        if (a.outputMix[i] != b.outputMix[i]) return false;
        for (int k = 0; k < kNumOps; ++k) if (a.mod[i][k] != b.mod[i][k] || a.ring[i][k] != b.ring[i][k]) return false;
    }
    return true;
}

// menu de algoritmos: os 32 de fabrica + os do usuario. Devolve true se escolheu um.
struct AlgoChoice { int factory = -1; const PatchEntry* user = nullptr; };
inline bool PresetMenuItems(AlgoEditor& e, const std::filesystem::path& dir, AlgoChoice& out) {
    if (e.userDirty || ImGui::IsWindowAppearing()) { e.user = listPatches(dir); e.userDirty = false; }
    bool chosen = false;
    ImGui::SeparatorText("Meus algoritmos");
    if (e.user.empty()) ImGui::TextDisabled("(nenhum salvo em %s/)", dir.string().c_str());
    for (const auto& u : e.user)
        if (ImGui::Selectable(u.stem.c_str(), e.current == u.stem)) { out.user = &u; chosen = true; }
    ImGui::SeparatorText("Fábrica");
    for (int n = 1; n <= kNumAlgorithms; ++n) {
        char lbl[80];
        std::snprintf(lbl, sizeof lbl, "%d: %s", n, algorithmName(n));
        if (ImGui::Selectable(lbl)) { out.factory = n - 1; chosen = true; }
    }
    return chosen;
}

// Aplica a escolha. True se o som mudou (vira um passo de desfazer).
inline bool applyChoice(AlgoEditor& e, const AlgoChoice& c, VoiceParams& v, int slot, int* selectedFactory = nullptr) {
    VoiceParams next = v;
    if (c.user) {
        ag::Preset p;
        if (!ag::loadPresetFile(c.user->path, p)) { e.status = "Não e um preset de algoritmo: " + c.user->path.string(); return false; }
        ag::applyPreset(next, slot, p);
        e.layout = p.layout;
        e.current = c.user->stem;
        std::snprintf(e.name, sizeof e.name, "%s", c.user->stem.c_str());
        e.status = "Carregado: " + c.user->stem;
    } else if (c.factory >= 0) {
        if (v.activeAlgos() == 1) applyAlgorithm(next, c.factory + 1);
        else applyAlgorithmToMatrix(next.algo(slot), c.factory + 1);
        e.layout = ag::autoLayout(next.algo(slot));
        e.current = algorithmName(c.factory + 1);
        if (selectedFactory) *selectedFactory = c.factory;
        e.status = std::string("Carregado: ") + algorithmName(c.factory + 1);
    }
    e.seen = next.algo(slot);
    e.selFrom = e.selTo = e.selOut = -1;
    if (voiceParamsToJson(next).dump() == voiceParamsToJson(v).dump()) return false;
    v = next;
    return true;
}

inline ImVec2 lerpPos(const ag::Layout& l, int i, ImVec2 o, ImVec2 s) {
    return ImVec2(o.x + s.x * l.x[i], o.y + s.y * l.y[i]);
}

// O canvas. Desenha e trata o mouse; true se a matriz mudou.
inline bool DrawCanvas(AlgoEditor& e, VoiceParams& v, int slot, int& selectedOp, ImVec2 size) {
    AlgorithmMatrix& m = v.algo(slot);
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float nodeW = 92.0f, nodeH = 46.0f, barH = 26.0f, port = 7.0f;
    ImGui::InvisibleButton("##algocanvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), ui::rgb(14, 16, 22), 6.0f);
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);

    // area util pros centros dos operadores; a barra SAIDA fica embaixo
    const ImVec2 areaO(p0.x + nodeW * 0.5f + 8.0f, p0.y + nodeH * 0.5f + 12.0f);
    const ImVec2 areaS(size.x - nodeW - 16.0f, size.y - nodeH - barH - 44.0f);
    const float barY = p0.y + size.y - barH - 4.0f;
    const ImVec2 barMin(p0.x + 8.0f, barY), barMax(p0.x + size.x - 8.0f, barY + barH);
    ImVec2 c[kNumOps];
    for (int i = 0; i < kNumOps; ++i) c[i] = lerpPos(e.layout, i, areaO, areaS);
    auto outPort = [&](int i) { return ImVec2(c[i].x, c[i].y + nodeH * 0.5f); };
    auto inPort = [&](int i) { return ImVec2(c[i].x, c[i].y - nodeH * 0.5f); };
    auto fbSpot = [&](int i) { return ImVec2(c[i].x + nodeW * 0.5f - 9.0f, c[i].y - nodeH * 0.5f + 9.0f); };
    auto inNode = [&](int i, ImVec2 q) { return std::fabs(q.x - c[i].x) <= nodeW * 0.5f && std::fabs(q.y - c[i].y) <= nodeH * 0.5f; };
    auto near = [](ImVec2 a, ImVec2 b, float r) { const float dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy <= r * r; };
    auto curve = [&](ImVec2 a, ImVec2 b, ImU32 col, float th, bool dashed) {
        const float bend = std::max(30.0f, std::fabs(b.y - a.y) * 0.5f);
        if (!dashed) { // pontos proprios: a subdivisao automatica deixava trechos retos com espessura irregular
            ImVec2 pts[33];
            for (int k = 0; k <= 32; ++k) {
                const float t = k / 32.0f;
                pts[k] = ImVec2(ag::bezierPoint(a.x, a.x, b.x, b.x, t), ag::bezierPoint(a.y, a.y + bend, b.y - bend, b.y, t));
            }
            dl->AddPolyline(pts, 33, col, ImDrawFlags_None, th + 0.3f);
            return;
        }
        for (int s = 0; s < 24; s += 2) { // tracejada: laco, usa a amostra anterior
            const float t0 = s / 24.0f, t1 = (s + 1) / 24.0f;
            dl->AddLine(ImVec2(ag::bezierPoint(a.x, a.x, b.x, b.x, t0), ag::bezierPoint(a.y, a.y + bend, b.y - bend, b.y, t0)),
                        ImVec2(ag::bezierPoint(a.x, a.x, b.x, b.x, t1), ag::bezierPoint(a.y, a.y + bend, b.y - bend, b.y, t1)), col, th);
        }
    };

    // o que esta embaixo do mouse (prioridade: bolinha, feedback, operador, conexao, saida)
    int hitOutPort = -1, hitFb = -1, hitNode = -1, hitFrom = -1, hitTo = -1, hitWire = -1;
    if (hovered) {
        for (int i = kNumOps - 1; i >= 0 && hitOutPort < 0 && hitFb < 0 && hitNode < 0; --i) {
            if (near(mouse, outPort(i), port + 3.0f)) hitOutPort = i;
            else if (near(mouse, fbSpot(i), 8.0f)) hitFb = i;
            else if (inNode(i, mouse)) hitNode = i;
        }
        if (hitOutPort < 0 && hitFb < 0 && hitNode < 0) {
            float best = 7.0f;
            for (int to = 0; to < kNumOps; ++to)
                for (int from = 0; from < kNumOps; ++from) {
                    if (from == to || !ag::hasEdge(m, from, to)) continue;
                    const ImVec2 a = outPort(from), b = inPort(to);
                    const float d = ag::distanceToEdge(mouse.x, mouse.y, a.x, a.y, b.x, b.y);
                    if (d < best) { best = d; hitFrom = from; hitTo = to; hitWire = -1; }
                }
            for (int i = 0; i < kNumOps; ++i) {
                if (m.outputMix[i] == 0.0f) continue;
                const ImVec2 a = outPort(i);
                const float d = ag::distanceToEdge(mouse.x, mouse.y, a.x, a.y, a.x, barY);
                if (d < best) { best = d; hitWire = i; hitFrom = hitTo = -1; }
            }
        }
    }
    auto clearSel = [&] { e.selFrom = e.selTo = e.selOut = -1; };

    // mouse
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hitOutPort >= 0) e.connectFrom = hitOutPort;
        else if (hitFb >= 0) {
            const bool has = ag::feedbackOf(v, slot, hitFb) != 0.0f;
            ag::setFeedback(v, slot, hitFb, has ? 0.0f : ag::kDefaultFeedback);
            if (v.activeAlgos() > 1 && has) m.mod[hitFb][hitFb] = 0.0f;
            changed = true;
            clearSel();
            if (!has) e.selFrom = e.selTo = hitFb;
        } else if (hitNode >= 0) { e.moving = hitNode; selectedOp = hitNode; clearSel(); }
        else if (hitFrom >= 0) { clearSel(); e.selFrom = hitFrom; e.selTo = hitTo; }
        else if (hitWire >= 0) { clearSel(); e.selOut = hitWire; }
        else clearSel();
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { // botao direito apaga
        if (hitFrom >= 0) { ag::disconnect(m, hitFrom, hitTo); changed = true; clearSel(); }
        else if (hitWire >= 0) { m.outputMix[hitWire] = 0.0f; changed = true; clearSel(); }
    }
    if (hovered && io.MouseWheel != 0.0f) { // roda ajusta a quantidade
        if (hitFrom >= 0 && !m.ring[hitTo][hitFrom]) { m.mod[hitTo][hitFrom] = std::clamp(m.mod[hitTo][hitFrom] + io.MouseWheel * 0.05f, -1.0f, 1.0f); changed = true; }
        else if (hitWire >= 0) { m.outputMix[hitWire] = std::clamp(m.outputMix[hitWire] + io.MouseWheel * 0.05f, 0.01f, 1.0f); changed = true; }
    }
    if (e.moving >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            e.layout.x[e.moving] = std::clamp(e.layout.x[e.moving] + io.MouseDelta.x / std::max(1.0f, areaS.x), 0.0f, 1.0f);
            e.layout.y[e.moving] = std::clamp(e.layout.y[e.moving] + io.MouseDelta.y / std::max(1.0f, areaS.y), 0.0f, 1.0f);
        } else e.moving = -1;
    }
    if (e.connectFrom >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // soltou
        const int from = e.connectFrom;
        e.connectFrom = -1;
        bool done = false;
        for (int to = 0; to < kNumOps && !done; ++to)
            if (inNode(to, mouse)) {
                if (to == from) { if (ag::feedbackOf(v, slot, to) == 0.0f) ag::setFeedback(v, slot, to, ag::kDefaultFeedback); }
                else ag::connect(m, from, to);
                clearSel();
                e.selFrom = from; e.selTo = to;
                changed = done = true;
            }
        if (!done && mouse.y >= barMin.y - 6.0f && mouse.y <= barMax.y + 6.0f) {
            if (m.outputMix[from] == 0.0f) m.outputMix[from] = 1.0f;
            clearSel();
            e.selOut = from;
            changed = true;
        }
    }

    // barra de saida
    const bool barHot = e.connectFrom >= 0 && mouse.y >= barMin.y - 6.0f;
    dl->AddRectFilled(barMin, barMax, barHot ? ui::col::kAccent : ui::rgb(30, 60, 62), 4.0f);
    {
        const char* t = "SAÍDA";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2((barMin.x + barMax.x - ts.x) * 0.5f, barMin.y + (barH - ts.y) * 0.5f), barHot ? ui::rgb(14, 16, 22) : ui::col::kAccent, t);
    }

    // fios
    char txt[32];
    for (int i = 0; i < kNumOps; ++i) {
        if (m.outputMix[i] == 0.0f) continue;
        const bool sel = e.selOut == i, hot = hitWire == i;
        const float th = sel || hot ? 3.5f : 2.0f;
        curve(outPort(i), ImVec2(c[i].x, barY), ui::opColor(i, v.op[i].enabled ? 220 : 60), th, false);
    }
    for (int to = 0; to < kNumOps; ++to)
        for (int from = 0; from < kNumOps; ++from) {
            if (from == to || !ag::hasEdge(m, from, to)) continue;
            const bool ring = m.ring[to][from], sel = e.selFrom == from && e.selTo == to, hot = hitFrom == from && hitTo == to;
            const bool off = !v.op[from].enabled || !v.op[to].enabled;
            const ImU32 col = ring ? ui::col::kAccent2 : ui::opColor(from, off ? 60 : 230);
            const ImVec2 a = outPort(from), b = inPort(to);
            curve(a, b, col, sel || hot ? 3.5f : 2.0f, ag::closesLoop(m, from, to));
            dl->AddTriangleFilled(ImVec2(b.x - 5.0f, b.y - 8.0f), ImVec2(b.x + 5.0f, b.y - 8.0f), b, col);
            if (ring) std::snprintf(txt, sizeof txt, "ring");
            else std::snprintf(txt, sizeof txt, "%.2f", static_cast<double>(m.mod[to][from]));
            const float bend = std::max(30.0f, std::fabs(b.y - a.y) * 0.5f);
            const ImVec2 mid(ag::bezierPoint(a.x, a.x, b.x, b.x, 0.5f), ag::bezierPoint(a.y, a.y + bend, b.y - bend, b.y, 0.5f));
            const ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 3.0f, mid.y - ts.y * 0.5f - 1.0f), ImVec2(mid.x + ts.x * 0.5f + 3.0f, mid.y + ts.y * 0.5f + 1.0f), ui::rgb(14, 16, 22, 220), 3.0f);
            dl->AddText(ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), col, txt);
        }
    if (e.connectFrom >= 0) curve(outPort(e.connectFrom), mouse, ui::opColor(e.connectFrom), 2.5f, false);

    // operadores
    for (int i = 0; i < kNumOps; ++i) {
        const bool on = v.op[i].enabled, reach = ag::reachesOutput(m, i), isSel = selectedOp == i;
        const bool dropHot = e.connectFrom >= 0 && inNode(i, mouse);
        const ImVec2 a(c[i].x - nodeW * 0.5f, c[i].y - nodeH * 0.5f), b(c[i].x + nodeW * 0.5f, c[i].y + nodeH * 0.5f);
        dl->AddRectFilled(a, b, ui::opColor(i, !on ? 18 : reach ? 70 : 28), 7.0f);
        dl->AddRect(a, b, ui::opColor(i, dropHot || isSel ? 255 : on ? 170 : 70), 7.0f, 0, dropHot || isSel ? 2.5f : 1.3f);
        std::snprintf(txt, sizeof txt, "Op%d", i + 1);
        dl->AddText(ImVec2(a.x + 8.0f, a.y + 5.0f), on ? ui::col::kText : ui::col::kTextDim, txt);
        const char* sub = !on ? "desligado" : !reach ? "sem efeito" : waveformName(v.op[i].waveform);
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + 8.0f, a.y + 24.0f), ui::col::kTextDim, sub);
        dl->PopClipRect();
        // feedback
        const float fb = ag::feedbackOf(v, slot, i);
        const bool fbSel = e.selFrom == i && e.selTo == i, fbHot = hitFb == i;
        if (fb != 0.0f) dl->AddCircleFilled(fbSpot(i), 6.0f, ui::col::kAccent2, 14);
        dl->AddCircle(fbSpot(i), 6.0f, fb != 0.0f || fbHot || fbSel ? ui::col::kAccent2 : ui::rgb(120, 126, 140), 14, fbSel ? 2.5f : 1.3f);
        // bolinha de saida
        const bool portHot = hitOutPort == i || e.connectFrom == i;
        dl->AddCircleFilled(outPort(i), port, portHot ? ui::col::kText : ui::opColor(i), 16);
        dl->AddCircle(outPort(i), port, ui::rgb(14, 16, 22), 16, 1.5f);
    }
    dl->PopClipRect();

    if (hovered && e.connectFrom < 0) {
        if (hitOutPort >= 0) ImGui::SetTooltip("Arraste até outro operador pra modula-lo,\nou até a barra SAÍDA pra virar portadora");
        else if (hitFb >= 0) ImGui::SetTooltip("Feedback do Op%d: clique pra ligar/desligar", hitFb + 1);
        else if (hitFrom >= 0) ImGui::SetTooltip("Op%d modula Op%d\nClique = selecionar, roda = quantidade, direito = apagar", hitFrom + 1, hitTo + 1);
        else if (hitWire >= 0) ImGui::SetTooltip("Op%d vai pra saída\nClique = selecionar, roda = nível, direito = apagar", hitWire + 1);
    }
    return changed;
}

// painel da direita: edita o que esta selecionado
inline bool DrawInspector(AlgoEditor& e, VoiceParams& v, int slot, int selectedOp) {
    AlgorithmMatrix& m = v.algo(slot);
    bool changed = false;
    ImGui::TextUnformatted("Selecionado");
    ImGui::Separator();
    if (e.selFrom >= 0 && e.selFrom == e.selTo) {
        const int op = e.selFrom;
        ImGui::Text("Feedback do Op%d", op + 1);
        float fb = ag::feedbackOf(v, slot, op);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##fb", &fb, 0.0f, 1.0f, "%.2f")) { ag::setFeedback(v, slot, op, fb); changed = true; }
        if (ImGui::Button("Tirar feedback")) { ag::setFeedback(v, slot, op, 0.0f); e.selFrom = e.selTo = -1; changed = true; }
    } else if (e.selFrom >= 0 && ag::hasEdge(m, e.selFrom, e.selTo)) {
        const int from = e.selFrom, to = e.selTo;
        ImGui::Text("Op%d modula Op%d", from + 1, to + 1);
        bool ring = m.ring[to][from];
        if (ImGui::RadioButton("FM", !ring) && ring) { ag::setRing(m, from, to, false); changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Ring", ring) && !ring) { ag::setRing(m, from, to, true); changed = true; }
        ImGui::SetItemTooltip("Ring: multiplica os sinais em vez de modular a fase");
        if (!ring) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            float amt = m.mod[to][from];
            if (ImGui::SliderFloat("##amt", &amt, -1.0f, 1.0f, "quantidade %.2f", ImGuiSliderFlags_AlwaysClamp)) {
                m.mod[to][from] = amt == 0.0f ? 0.001f : amt; // 0 apagaria a conexao no meio do arraste
                changed = true;
            }
        }
        if (ag::closesLoop(m, from, to)) { ImGui::PushTextWrapPos(0.0f); ImGui::TextDisabled("Tracejada: fecha um laco, então usa a amostra anterior."); ImGui::PopTextWrapPos(); }
        if (ImGui::Button("Apagar conexão")) { ag::disconnect(m, from, to); e.selFrom = e.selTo = -1; changed = true; }
    } else if (e.selOut >= 0 && m.outputMix[e.selOut] != 0.0f) {
        const int op = e.selOut;
        ImGui::Text("Op%d na saída", op + 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        float mix = m.outputMix[op];
        if (ImGui::SliderFloat("##mix", &mix, 0.01f, 1.0f, "nível %.2f", ImGuiSliderFlags_AlwaysClamp)) { m.outputMix[op] = mix; changed = true; }
        if (ImGui::Button("Tirar da saída")) { m.outputMix[op] = 0.0f; e.selOut = -1; changed = true; }
    } else {
        ImGui::Text("Op%d", selectedOp + 1);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::col::kTextDim);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Clique numa conexão ou no fio de saída pra ajustar.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (ImGui::Checkbox("Ligado", &v.op[selectedOp].enabled)) changed = true;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::col::kTextDim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Como usar\n"
                           "- Arraste da bolinha de baixo de um operador até outro: ele passa a modular o outro.\n"
                           "- Até a barra SAÍDA: vira portadora.\n"
                           "- Circulo no canto: feedback.\n"
                           "- Arraste o operador pra organizar.\n"
                           "- Na conexão: roda ajusta, botão direito (ou Delete) apaga.\n"
                           "- Ctrl+Z desfaz.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    return changed;
}

// A janela. slot = algoritmo em edicao (morph); selectedFactory = indice do combo do card.
inline bool DrawAlgoEditor(AlgoEditor& e, VoiceParams& v, int part, int slot, int& selectedOp, int& selectedFactory, const std::filesystem::path& presetDir) {
    if (!e.open) return false;
    bool changed = false;
    AlgorithmMatrix& m = v.algo(slot);
    // mudou por fora (Ctrl+Z, card, outra parte): reorganiza, se a topologia mudou
    if (!e.seenValid || part != e.seenPart || slot != e.seenSlot || !matrixEqual(m, e.seen)) {
        bool sameShape = e.seenValid && part == e.seenPart && slot == e.seenSlot;
        for (int i = 0; sameShape && i < kNumOps; ++i) {
            sameShape = (m.outputMix[i] != 0.0f) == (e.seen.outputMix[i] != 0.0f);
            for (int k = 0; sameShape && k < kNumOps; ++k) if (k != i) sameShape = ag::hasEdge(m, k, i) == ag::hasEdge(e.seen, k, i);
        }
        if (!sameShape) e.layout = ag::autoLayout(m);
        e.seen = m;
        e.seenPart = part;
        e.seenSlot = slot;
        e.seenValid = true;
    }
    ImGui::SetNextWindowSize(ImVec2(980, 660), ImGuiCond_Once);
    ImGui::SetNextWindowPos(ImVec2(230, 110), ImGuiCond_Once);
    char title[80];
    if (v.activeAlgos() > 1) std::snprintf(title, sizeof title, "Editor de algoritmo: parte %c, algoritmo %d###algoeditor", 'A' + part, slot + 1);
    else std::snprintf(title, sizeof title, "Editor de algoritmo: parte %c###algoeditor", 'A' + part);
    if (!ImGui::Begin(title, &e.open, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return false; }

    // barra de presets
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(250);
    if (ImGui::BeginCombo("##algopreset", e.current.empty() ? "(editado)" : e.current.c_str(), ImGuiComboFlags_HeightLarge)) {
        AlgoChoice ch;
        if (PresetMenuItems(e, presetDir, ch)) changed |= applyChoice(e, ch, v, slot, &selectedFactory);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    ImGui::InputText("##algoname", e.name, sizeof e.name);
    ImGui::SameLine();
    const std::filesystem::path target = patchPathFor(presetDir, e.name);
    std::error_code ec;
    const bool exists = std::filesystem::exists(target, ec);
    if (ImGui::Button(exists ? "Sobrescrever" : "Salvar")) {
        if (ag::savePresetFile(target, ag::presetFrom(v, slot, e.layout))) {
            e.current = target.stem().string();
            e.status = "Salvo em " + target.string();
            e.userDirty = true;
        } else e.status = "Não consegui salvar " + target.string();
        e.confirmDelete = false;
    }
    ImGui::SetItemTooltip("Grava conexões, feedback, níveis de saída e a posição dos operadores em %s", target.string().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!exists);
    if (ImGui::Button(e.confirmDelete ? "Confirmar exclusão" : "Excluir")) {
        if (!e.confirmDelete) e.confirmDelete = true;
        else {
            std::filesystem::remove(target, ec);
            e.status = ec ? "Não consegui excluir " + target.string() : "Excluido: " + target.stem().string();
            if (!ec && e.current == target.stem().string()) e.current.clear();
            e.userDirty = true;
            e.confirmDelete = false;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Organizar")) e.layout = ag::autoLayout(m);
    ImGui::SetItemTooltip("Portadoras embaixo, cada modulador acima de quem ele modula");
    ImGui::SameLine();
    if (ImGui::Button("Limpar")) {
        ag::clearAll(m);
        if (v.activeAlgos() == 1) for (auto& o : v.op) o.feedback = 0.0f;
        e.layout = ag::autoLayout(m);
        e.selFrom = e.selTo = e.selOut = -1;
        e.current.clear();
        changed = true;
    }
    ImGui::SetItemTooltip("Tira todas as conexões e saídas (Ctrl+Z desfaz)");
    if (!ag::anyCarrier(m)) { ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::col::kAccent2), "Nenhum operador vai pra SAÍDA: este algoritmo não soa."); }
    else if (!e.status.empty()) ImGui::TextDisabled("%s", e.status.c_str());
    else ImGui::TextDisabled("Toque enquanto edita: a mudanca vale na hora.");

    // canvas | painel
    const float inspW = 240.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const bool canvasChanged = DrawCanvas(e, v, slot, selectedOp, ImVec2(std::max(300.0f, avail.x - inspW - 8.0f), std::max(280.0f, avail.y)));
    changed |= canvasChanged;
    ImGui::SameLine();
    ImGui::BeginChild("##algoinsp", ImVec2(inspW, 0));
    changed |= DrawInspector(e, v, slot, selectedOp);
    ImGui::EndChild();

    // Delete apaga a selecao (fora de campos de texto)
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        if (e.selFrom >= 0 && e.selFrom == e.selTo) { ag::setFeedback(v, slot, e.selFrom, 0.0f); changed = true; }
        else if (e.selFrom >= 0) { ag::disconnect(m, e.selFrom, e.selTo); changed = true; }
        else if (e.selOut >= 0) { m.outputMix[e.selOut] = 0.0f; changed = true; }
        e.selFrom = e.selTo = e.selOut = -1;
    }
    ImGui::End();
    if (canvasChanged) e.current.clear();
    e.seen = m; // o que o editor mudou nao e "mudanca por fora"
    return changed;
}

} // namespace dx7fm_app::algoed
