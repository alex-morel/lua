#pragma once
#include "dx7_alloc.h"
#include "dx7_fxrack_io.h"
#include "fx_factory.h"
#include "fx_rack_logic.h"
#include "patch_browser.h"
#include "ui_widgets.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// [ADICIONADO em 2026-10-29] Aba "Efeitos": pedaleira de 2 cadeias x 4 pedais,
// mixer de envios por parte, presets (fabrica + usuario), desfazer e pedal de
// expressao. A logica testavel fica em fx_rack_logic.h; aqui so desenho.
namespace dx7fm_app::fxui {

struct FxRackHooks {
    std::filesystem::path presetDir;                         // presets do usuario (.json)
    const std::vector<fxfactory::FxFactoryPreset>* factory = nullptr;
    std::function<void(float)> learnExpression;              // desenha o botao de MIDI learn (largura)
    std::function<void(float)> sendExpression;               // posicao do pedal (0..1) pra thread de audio
};

inline const char* const kCategories[] = {"Dinamica", "Drive", "Fita", "Filtro", "Modulacao", "Pitch", "Delay", "Reverb", "Granular"};

inline ImU32 categoryColor(const char* cat, int alpha = 255) {
    const std::string c = cat ? cat : "";
    if (c == "Dinamica") return ui::rgb(246, 224, 94, alpha);
    if (c == "Drive") return ui::rgb(246, 140, 85, alpha);
    if (c == "Fita") return ui::rgb(214, 170, 110, alpha);
    if (c == "Filtro") return ui::rgb(120, 210, 120, alpha);
    if (c == "Modulacao") return ui::rgb(100, 170, 250, alpha);
    if (c == "Pitch") return ui::rgb(180, 140, 250, alpha);
    if (c == "Delay") return ui::rgb(79, 209, 197, alpha);
    if (c == "Reverb") return ui::rgb(240, 120, 180, alpha);
    if (c == "Granular") return ui::rgb(130, 220, 240, alpha);
    return ui::rgb(138, 146, 162, alpha);
}

// formato do knob: casas pela faixa + unidade ('%' escapado)
inline void knobFormat(const FxParamInfo& pi, char* buf, size_t n) {
    const float span = pi.max - pi.min;
    const char* num = span >= 100.0f ? "%.0f" : span >= 10.0f ? "%.1f" : "%.2f";
    std::string unit;
    for (const char* c = pi.unit; c && *c; ++c) { unit += *c; if (*c == '%') unit += '%'; }
    std::snprintf(buf, n, "%s%s%s", num, unit.empty() ? "" : " ", unit.c_str());
}

// knob ou combo (parametro de escolha) de um parametro do slot
inline bool ParamControl(const FxParamInfo& pi, float* v, const ui::KnobStyle& base) {
    if (pi.choices) {
        int cur = static_cast<int>(*v);
        const std::string preview = choiceAt(pi.choices, cur);
        bool changed = false;
        char lbl[64];
        std::snprintf(lbl, sizeof lbl, "##%s", pi.id);
        ImGui::TextDisabled("%s", pi.label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(lbl, preview.c_str())) {
            for (int k = 0; k < choiceCount(pi.choices); ++k)
                if (ImGui::Selectable(choiceAt(pi.choices, k).c_str(), k == cur)) { *v = static_cast<float>(k); changed = true; }
            ImGui::EndCombo();
        }
        return changed;
    }
    ui::KnobStyle st = base;
    st.logarithmic = pi.log && pi.min > 0.0f;
    char fmt[24];
    knobFormat(pi, fmt, sizeof fmt);
    return ui::Knob(pi.label, v, pi.min, pi.max, pi.def, fmt, st);
}

// combo do tipo de efeito, agrupado por categoria
inline bool KindCombo(FxSlotParams& s) {
    const FxInfo& cur = fxInfo(s.kind);
    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##kind", s.kind == FxKind::Off ? "Vazio - escolha um efeito" : cur.name, ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("Vazio", s.kind == FxKind::Off)) { s.setKind(FxKind::Off); changed = true; }
        for (const char* cat : kCategories) {
            ImGui::PushStyleColor(ImGuiCol_Text, categoryColor(cat));
            ImGui::SeparatorText(cat);
            ImGui::PopStyleColor();
            for (int k = 1; k < static_cast<int>(FxKind::Count); ++k) {
                const FxInfo& info = fxInfo(static_cast<FxKind>(k));
                if (std::strcmp(info.category, cat) != 0) continue;
                if (ImGui::Selectable(info.name, s.kind == info.kind)) {
                    if (s.kind != info.kind) { s.setKind(info.kind); s.on = true; changed = true; }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.origin);
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// footswitch com LED; devolve true se clicou
inline bool Footswitch(const char* id, bool on, ImU32 ledColor) {
    const float d = 30.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(d + 22.0f, d));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 led(p.x + 7.0f, p.y + d * 0.5f);
    if (on) dl->AddCircleFilled(led, 8.0f, (ledColor & 0x00FFFFFF) | 0x50000000, 16); // brilho
    dl->AddCircleFilled(led, 4.5f, on ? ledColor : ui::rgb(60, 30, 30), 16);
    const ImVec2 c(p.x + 22.0f + d * 0.5f, p.y + d * 0.5f);
    dl->AddCircleFilled(c, d * 0.5f, hov ? ui::rgb(120, 126, 140) : ui::rgb(96, 102, 116), 24);
    dl->AddCircleFilled(c, d * 0.5f - 4.0f, ui::rgb(160, 166, 178), 24);
    dl->AddCircle(c, d * 0.5f - 4.0f, ui::rgb(70, 74, 86), 24, 1.5f);
    if (hov) ImGui::SetTooltip(on ? "Ligado: clique pra desligar (bypass)" : "Desligado (bypass): clique pra ligar");
    return clicked;
}

inline void GainReductionMeter(float grDb, float width) {
    char text[32];
    std::snprintf(text, sizeof text, "GR -%.1f dB", grDb);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::col::kAccent2);
    ImGui::ProgressBar(std::clamp(grDb / 24.0f, 0.0f, 1.0f), ImVec2(width, 0), text);
    ImGui::PopStyleColor();
}

// diagrama de um algoritmo de roteamento, desenhado da propria tabela
// (kFxAlgorithms): envios -> blocos de 2 pedais -> saida
inline void AlgorithmDiagram(const FxRackParams& r, int a, ImVec2 size) {
    const FxAlgorithm& alg = kFxAlgorithms[a];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::col::kKnobBody, 4.0f);
    const float w = size.x, h = size.y;
    const float bw = w * 0.2f, bh = h * 0.28f;
    const float rowY[2] = {p.y + h * 0.28f, p.y + h * 0.72f};
    const float bx[2] = {p.x + w * 0.22f, p.x + w * 0.52f};
    const ImVec2 outPt(p.x + w * 0.92f, p.y + h * 0.5f);
    auto boxL = [&](int b) { return ImVec2(bx[b % 2], rowY[b / 2]); };
    auto boxR = [&](int b) { return ImVec2(bx[b % 2] + bw, rowY[b / 2]); };
    const ImU32 line = ui::col::kTextDim, act = ui::col::kAccent;
    auto wire = [&](ImVec2 a, ImVec2 b, ImU32 c) {
        const float dx = std::max(14.0f, std::fabs(b.x - a.x) * 0.5f);
        dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, c, 1.6f);
    };
    // envios (entradas) a esquerda
    const ImVec2 inPt[2] = {ImVec2(p.x + w * 0.07f, rowY[0]), ImVec2(p.x + w * 0.07f, rowY[1])};
    for (int k = 0; k < 2; ++k) {
        dl->AddCircleFilled(inPt[k], 3.0f, line);
        dl->AddText(ImVec2(inPt[k].x - 4.0f, inPt[k].y - 17.0f), ui::col::kTextDim, k ? "2" : "1");
    }
    for (int b = 0; b < kFxBlocks; ++b) {
        for (int k = 0; k < 2; ++k) {
            if (!(alg.in[b] & (k ? kFxSrcIn2 : kFxSrcIn1))) continue;
            if (b % 2 == 0 || b / 2 != k) { wire(inPt[k], boxL(b), line); continue; }
            // envio direto pro 2o bloco da fileira: contorna o 1o por fora
            const float y = rowY[k] + (k ? 1.0f : -1.0f) * (bh * 0.5f + 5.0f);
            const ImVec2 pts[4] = {inPt[k], ImVec2(bx[0] - 6.0f, y), ImVec2(bx[1] - 8.0f, y), boxL(b)};
            dl->AddPolyline(pts, 4, line, 0, 1.6f);
        }
        for (int k = 0; k < b; ++k) {
            if (!(alg.in[b] & (kFxSrcA << k))) continue;
            if (k / 2 == b / 2) { wire(boxR(k), boxL(b), act); continue; }
            // entre fileiras: de baixo da caixa de cima pro topo da de baixo
            const ImVec2 a0(bx[k % 2] + bw * 0.5f, rowY[k / 2] + bh * 0.5f), a1(bx[b % 2] + bw * 0.5f, rowY[b / 2] - bh * 0.5f);
            const float dy = (a1.y - a0.y) * 0.5f;
            dl->AddBezierCubic(a0, ImVec2(a0.x, a0.y + dy), ImVec2(a1.x, a1.y - dy), a1, act, 1.6f);
            dl->AddTriangleFilled(ImVec2(a1.x - 3.5f, a1.y - 5.0f), ImVec2(a1.x + 3.5f, a1.y - 5.0f), a1, act);
        }
        if (alg.out & (kFxSrcA << b)) wire(boxR(b), outPt, act);
    }
    dl->AddCircleFilled(outPt, 4.0f, act);
    for (int b = 0; b < kFxBlocks; ++b) { // caixas por cima dos fios
        const int c = b / 2, s0 = (b % 2) * 2;
        const ImVec2 q0(bx[b % 2], rowY[c] - bh * 0.5f), q1(bx[b % 2] + bw, rowY[c] + bh * 0.5f);
        bool any = false;
        for (int s = s0; s < s0 + 2; ++s) any = any || r.chain[c].slot[s].active();
        dl->AddRectFilled(q0, q1, any ? ui::rgb(44, 60, 66) : ui::col::kCard, 3.0f);
        dl->AddRect(q0, q1, any ? act : ui::col::kCardBorder, 3.0f);
        char t[16];
        std::snprintf(t, sizeof t, "%d.%d-%d", c + 1, s0 + 1, s0 + 2);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2((q0.x + q1.x - ts.x) * 0.5f, (q0.y + q1.y - ts.y) * 0.5f), ui::col::kText, t);
    }
}

struct DragSlot { int chain, slot; };

inline void DrawPedal(FxRackUi& ui, dx7fm::Synth& synth, int c, int s) {
    FxRackParams& r = ui.edit;
    FxSlotParams& sp = r.chain[c].slot[s];
    const FxInfo& info = fxInfo(sp.kind);
    const bool empty = sp.kind == FxKind::Off;
    const ImU32 catCol = categoryColor(info.category);
    char id[24], title[64];
    std::snprintf(id, sizeof id, "##pedal%d%d", c, s);
    std::snprintf(title, sizeof title, "%d.%d  %s", c + 1, s + 1, empty ? "" : info.category);
    ImGui::PushID(c * 10 + s);
    if (ui::BeginCard(id, title, 0.0f, empty ? ui::col::kTextDim : catCol)) {
        // alca de arrastar + tipo
        ImGui::SmallButton("::");
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            ImGui::SetTooltip("Arraste pra outro pedal pra trocar de lugar\n(Ctrl ao soltar = copiar por cima)");
        if (!empty && ImGui::BeginDragDropSource()) {
            DragSlot ds{c, s};
            ImGui::SetDragDropPayload("FXSLOT", &ds, sizeof ds);
            ImGui::Text("%s (Ctrl ao soltar = copiar)", info.name);
            ImGui::EndDragDropSource();
        }
        ImGui::SameLine();
        KindCombo(sp);

        if (empty) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextDisabled("Slot vazio: o sinal passa direto.\nEscolha um efeito ou arraste\num pedal pra ca.");
        } else {
            if (!sp.on) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f); // bypass: esmaecido, mas editavel
            ui::KnobStyle ks;
            ks.size = 34.0f;
            ks.cellWidth = 54.0f;
            ks.color = catCol;
            // escolhas primeiro (combos de largura cheia), depois os knobs em fluxo
            for (int i = 0; i < info.nParams; ++i)
                if (info.params[i].choices) ParamControl(info.params[i], &sp.p[i], ks);
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            bool first = true;
            auto flow = [&] {
                if (!first) {
                    const float x = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
                    if (x + ks.cellWidth <= right) ImGui::SameLine();
                }
                first = false;
            };
            flow();
            ui::Knob("Mix", &sp.mix, 0.0f, 1.0f, info.defMix, "%.2f", ks);
            for (int i = 0; i < info.nParams; ++i) {
                if (info.params[i].choices) continue;
                flow();
                ParamControl(info.params[i], &sp.p[i], ks);
                if (info.params[i].rebuild && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s: aplicado ao soltar o knob (recria o reverb)", info.params[i].label);
            }
            if (!sp.on) ImGui::PopStyleVar();
        }

        // rodape: footswitch, expressao, medidor
        ImGui::Separator();
        if (Footswitch("##fs", sp.on && !empty, catCol) && !empty) sp.on = !sp.on;
        ImGui::SameLine();
        const bool isExpr = r.expr.chain == c && r.expr.slot == s;
        ImGui::BeginDisabled(empty);
        if (isExpr) ImGui::PushStyleColor(ImGuiCol_Button, ui::col::kAccent2);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
        if (ImGui::SmallButton("EXP")) ImGui::OpenPopup("##exp");
        if (isExpr) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(isExpr ? "Pedal de expressao: %s\n(mude a faixa no mixer)" : "Ligar o pedal de expressao a um parametro deste pedal",
                              isExpr && r.expr.param < info.nParams ? info.params[r.expr.param].label : "");
        if (ImGui::BeginPopup("##exp")) {
            ImGui::TextDisabled("Pedal de expressao controla:");
            for (int i = 0; i < info.nParams; ++i) {
                const FxParamInfo& pi = info.params[i];
                if (pi.rebuild) continue;
                if (ImGui::Selectable(pi.label, isExpr && r.expr.param == i)) r.expr = {c, s, i, pi.min, pi.max};
            }
            if (isExpr && ImGui::Selectable("(sem pedal)")) r.expr.chain = -1;
            ImGui::EndPopup();
        }
        if (sp.kind == FxKind::Compressor) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
            GainReductionMeter(sp.on ? synth.fxSlotMeter(c, s) : 0.0f, -FLT_MIN);
        }
    }
    ui::EndCard();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FXSLOT")) {
            const DragSlot ds = *static_cast<const DragSlot*>(pl->Data);
            moveSlot(r, ds.chain, ds.slot, c, s, ImGui::GetIO().KeyCtrl);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
}

inline void DrawRackMixer(FxRackUi& ui, dx7fm::Synth& synth, const FxRackHooks& hooks) {
    FxRackParams& r = ui.edit;
    static const FxRackParams kDef;
    ui::KnobStyle ks;
    ks.size = 32.0f;
    ks.cellWidth = 52.0f;

    if (ui::BeginCard("##fxmix", "ENVIOS POR PARTE")) {
        ImGui::TextDisabled("Seco = direto pra saida;\n-> 1 / -> 2 = envio pra cada cadeia.");
        ui::KnobStyle kp = ks;
        kp.size = 28.0f;
        kp.cellWidth = 46.0f;
        // grade: colunas = partes, linhas = seco / envio 1 / envio 2
        static const char* const kRows[3] = {"Seco", "-> 1", "-> 2"};
        const float x0 = ImGui::GetCursorPosX();
        const float col = kp.cellWidth + ImGui::GetStyle().ItemSpacing.x;
        for (int p = 0; p < kFxParts; ++p) {
            if (p) ImGui::SameLine(x0 + col * p);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::opColor(p));
            ImGui::SetCursorPosX(x0 + col * p + kp.cellWidth * 0.5f - 4.0f);
            ImGui::Text("%c", 'A' + p);
            ImGui::PopStyleColor();
        }
        for (int row = 0; row < 3; ++row)
            for (int p = 0; p < kFxParts; ++p) {
                ImGui::PushID(row * 10 + p);
                if (p) ImGui::SameLine(x0 + col * p);
                float* v = row == 0 ? &r.dry[p] : &r.send[p][row - 1];
                const float def = row == 0 ? kDef.dry[p] : kDef.send[p][row - 1];
                ui::Knob(kRows[row], v, 0.0f, 2.0f, def, "%.2f", kp);
                ImGui::PopID();
            }
    }
    ui::EndCard();

    if (ui::BeginCard("##fxchains", "CADEIAS")) {
        // algoritmo de roteamento: combo (cada item mostra o diagrama no tooltip) + diagrama atual
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Algoritmo");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        char cur[48];
        std::snprintf(cur, sizeof cur, "%d. %s", r.algorithm + 1, kFxAlgorithms[r.algorithm].name);
        if (ImGui::BeginCombo("##fxalgo", cur)) {
            for (int a = 0; a < kFxNumAlgorithms; ++a) {
                char lbl[48];
                std::snprintf(lbl, sizeof lbl, "%d. %s", a + 1, kFxAlgorithms[a].name);
                if (ImGui::Selectable(lbl, a == r.algorithm)) r.algorithm = static_cast<uint8_t>(a);
                if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
                    AlgorithmDiagram(r, a, ImVec2(220, 76));
                    ImGui::TextUnformatted(kFxAlgorithms[a].description);
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndCombo();
        }
        // setas: anterior/proximo (tocar e ir trocando)
        if (ImGui::ArrowButton("##algoprev", ImGuiDir_Left)) r.algorithm = static_cast<uint8_t>((r.algorithm + kFxNumAlgorithms - 1) % kFxNumAlgorithms);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##algonext", ImGuiDir_Right)) r.algorithm = static_cast<uint8_t>((r.algorithm + 1) % kFxNumAlgorithms);
        ImGui::SameLine();
        ImGui::TextDisabled("1 e 2 = envios");
        AlgorithmDiagram(r, r.algorithm, ImVec2(ImGui::GetContentRegionAvail().x, 80));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", kFxAlgorithms[r.algorithm].description);
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("Nivel/pan: o que sai da fileira.");
        for (int c = 0; c < kFxChains; ++c) {
            ImGui::PushID(100 + c);
            ImGui::Text("Cadeia %d", c + 1);
            ui::Knob("Nivel", &r.chain[c].level, 0.0f, 2.0f, 1.0f, "%.2f", ks);
            ImGui::SameLine();
            ui::Knob("Pan", &r.chain[c].pan, -1.0f, 1.0f, 0.0f, "%.2f", ks);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0, 10));
            char lbl[8];
            std::snprintf(lbl, sizeof lbl, "%d", c + 1);
            ui::LevelMeter(lbl, synth.fxChainPeak(c), ImVec2(std::max(40.0f, ImGui::GetContentRegionAvail().x), 16));
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
    ui::EndCard();

    if (ui::BeginCard("##fxexpr", "PEDAL DE EXPRESSAO")) {
        FxExpression& e = r.expr;
        if (e.chain < 0) {
            ImGui::TextDisabled("Sem alvo: use o botao EXP de um pedal.");
        } else {
            const FxInfo& info = fxInfo(r.chain[e.chain].slot[e.slot].kind);
            const FxParamInfo& pi = info.params[e.param];
            ImGui::Text("%d.%d %s: %s", e.chain + 1, e.slot + 1, info.name, pi.label);
            if (pi.choices) {
                ImGui::TextDisabled("escolha: o pedal percorre as opcoes");
            } else {
                ui::KnobStyle st = ks;
                st.logarithmic = pi.log && pi.min > 0.0f;
                st.color = ui::col::kAccent2;
                char fmt[24];
                knobFormat(pi, fmt, sizeof fmt);
                ui::Knob("Calcanhar", &e.min, pi.min, pi.max, pi.min, fmt, st);
                ImGui::SameLine();
                ui::Knob("Ponta", &e.max, pi.min, pi.max, pi.max, fmt, st);
            }
            if (ImGui::SmallButton("Remover")) e.chain = -1;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("CC");
        ImGui::SameLine();
        if (hooks.learnExpression) hooks.learnExpression(140.0f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##pedaltest", &ui.pedalTest, 0.0f, 1.0f, "pedal (teste) %.2f") && hooks.sendExpression)
            hooks.sendExpression(ui.pedalTest);
        ImGui::SetItemTooltip("Simula o pedal pela tela (o CC aprendido faz o mesmo).\nAntes do primeiro valor o parametro fica como no pedal.");
    }
    ui::EndCard();
}

inline void loadPresetInto(FxRackUi& ui, dx7fm::Synth& synth, const FxRackParams& r, const std::string& name, double now) {
    setRack(ui, synth, r, now);
    ui.current = name;
    std::snprintf(ui.name, sizeof ui.name, "%s", name.c_str());
    ui.status = "Carregado: " + name + " (Ctrl+Z volta)";
}

inline void DrawPresetBar(FxRackUi& ui, dx7fm::Synth& synth, const FxRackHooks& hooks, double now) {
    if (ui.userListDirty) { ui.user = listPatches(hooks.presetDir); ui.userListDirty = false; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("##fxpreset", ui.current.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (hooks.factory) {
            std::string lastCat;
            for (const auto& pr : *hooks.factory) {
                if (pr.category != lastCat) { ImGui::SeparatorText(("Fabrica: " + pr.category).c_str()); lastCat = pr.category; }
                if (ImGui::Selectable(pr.name.c_str(), ui.current == pr.name)) loadPresetInto(ui, synth, pr.rack, pr.name, now);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pr.description.c_str());
            }
        }
        ImGui::SeparatorText("Usuario");
        if (ui.user.empty()) ImGui::TextDisabled("(nenhum salvo em %s/)", hooks.presetDir.string().c_str());
        for (const auto& e : ui.user)
            if (ImGui::Selectable(e.stem.c_str(), ui.current == e.stem)) {
                FxRackParams r;
                if (loadFxRackFile(e.path, r)) loadPresetInto(ui, synth, r, e.stem, now);
                else ui.status = "Nao e um preset de efeitos: " + e.path.string();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##fxname", ui.name, sizeof ui.name);
    ImGui::SameLine();
    const std::filesystem::path target = patchPathFor(hooks.presetDir, ui.name);
    std::error_code ec;
    const bool exists = std::filesystem::exists(target, ec);
    if (ImGui::Button(exists ? "Sobrescrever" : "Salvar")) {
        if (saveFxRackFile(target, ui.edit)) {
            ui.status = "Salvo em " + target.string();
            ui.current = target.stem().string();
            ui.userListDirty = true;
        } else {
            ui.status = "Nao consegui salvar " + target.string();
        }
    }
    ImGui::SetItemTooltip("Grava o rack inteiro (pedais, envios, pedal de expressao) em %s", target.string().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Init")) loadPresetInto(ui, synth, FxRackParams{}, "(init)", now);
    ImGui::SetItemTooltip("Rack vazio: todas as partes entram na cadeia 1, sem efeitos");
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.undo.empty() && rackEqual(ui.edit, ui.committed));
    if (ImGui::Button("Desfazer")) undoRack(ui, synth, now);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Z (%zu passos)", ui.undo.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.redo.empty());
    if (ImGui::Button("Refazer")) redoRack(ui, synth, now);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Y ou Ctrl+Shift+Z");
    if (!ui.status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", ui.status.c_str()); }
}

inline void DrawFxRackPanel(FxRackUi& ui, dx7fm::Synth& synth, const FxRackHooks& hooks) {
    const double now = ImGui::GetTime();
    DrawPresetBar(ui, synth, hooks, now);
    ImGui::Spacing();
    if (ImGui::BeginTable("##fxrack", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("mixer", ImGuiTableColumnFlags_WidthFixed, 250.0f);
        ImGui::TableSetupColumn("pedais", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        DrawRackMixer(ui, synth, hooks);
        ImGui::TableSetColumnIndex(1);
        for (int c = 0; c < kFxChains; ++c) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::col::kAccent);
            ImGui::Text("CADEIA %d", c + 1);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (c == 0) ImGui::TextDisabled("algoritmo %d: %s", ui.edit.algorithm + 1, kFxAlgorithms[ui.edit.algorithm].name);
            char tid[16];
            std::snprintf(tid, sizeof tid, "##chain%d", c);
            if (ImGui::BeginTable(tid, kFxSlots, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextRow();
                for (int s = 0; s < kFxSlots; ++s) {
                    ImGui::TableSetColumnIndex(s);
                    DrawPedal(ui, synth, c, s);
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTable();
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redoRack(ui, synth, now); else undoRack(ui, synth, now); }
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redoRack(ui, synth, now);
    }
    syncRack(ui, synth, ImGui::IsMouseDown(ImGuiMouseButton_Left), ImGui::IsAnyItemActive(), now);
}

} // namespace dx7fm_app::fxui
