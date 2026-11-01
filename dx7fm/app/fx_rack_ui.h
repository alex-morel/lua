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

inline const char* const kCategories[] = {"Dinâmica", "Drive", "Fita", "Filtro", "Modulação", "Pitch", "Delay", "Reverb", "Granular"};

inline ImU32 categoryColor(const char* cat, int alpha = 255) {
    const std::string c = cat ? cat : "";
    if (c == "Dinâmica") return ui::rgb(246, 224, 94, alpha);
    if (c == "Drive") return ui::rgb(246, 140, 85, alpha);
    if (c == "Fita") return ui::rgb(214, 170, 110, alpha);
    if (c == "Filtro") return ui::rgb(120, 210, 120, alpha);
    if (c == "Modulação") return ui::rgb(100, 170, 250, alpha);
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

// diagrama do algoritmo de uma cadeia, desenhado da propria tabela
// (kFxChainAlgorithms): entrada -> pedais (coluna = profundidade) -> saida
inline void ChainAlgorithmDiagram(const FxChainParams& ch, int a, ImVec2 size, bool names) {
    const FxChainAlgorithm& alg = kFxChainAlgorithms[a];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::col::kKnobBody, 4.0f);
    // coluna de cada pedal = caminho mais longo desde a entrada
    int depth[kFxSlots] = {}, maxDepth = 1, rows[kFxSlots + 1] = {}, row[kFxSlots] = {};
    for (int s = 0; s < kFxSlots; ++s) {
        depth[s] = 1;
        for (int k = 0; k < s; ++k) if (alg.in[s] & (kFxSrc1 << k)) depth[s] = std::max(depth[s], depth[k] + 1);
        maxDepth = std::max(maxDepth, depth[s]);
        row[s] = rows[depth[s]]++;
    }
    const float padX = 18.0f, colW = (size.x - 2.0f * padX) / static_cast<float>(maxDepth);
    const float bw = std::min(colW - 14.0f, names ? 110.0f : 34.0f), bh = std::min(18.0f, size.y / 4.0f - 2.0f);
    auto center = [&](int s) {
        const int n = rows[depth[s]];
        const float x = p.x + padX + colW * (static_cast<float>(depth[s]) - 0.5f);
        const float y = p.y + size.y * (static_cast<float>(row[s]) + 0.5f) / static_cast<float>(n);
        return ImVec2(x, y);
    };
    const ImVec2 inPt(p.x + 7.0f, p.y + size.y * 0.5f), outPt(p.x + size.x - 7.0f, p.y + size.y * 0.5f);
    const ImU32 act = ui::col::kAccent;
    auto wire = [&](ImVec2 a0, ImVec2 a1) {
        const float dx = std::max(8.0f, (a1.x - a0.x) * 0.5f);
        dl->AddBezierCubic(a0, ImVec2(a0.x + dx, a0.y), ImVec2(a1.x - dx, a1.y), a1, act, 1.5f);
    };
    for (int s = 0; s < kFxSlots; ++s) {
        const ImVec2 c = center(s), left(c.x - bw * 0.5f, c.y), right(c.x + bw * 0.5f, c.y);
        if (alg.in[s] & kFxSrcIn) wire(inPt, left);
        for (int k = 0; k < s; ++k) if (alg.in[s] & (kFxSrc1 << k)) { const ImVec2 ck = center(k); wire(ImVec2(ck.x + bw * 0.5f, ck.y), left); }
        if (alg.out & (kFxOut1 << s)) wire(right, outPt);
    }
    dl->AddCircleFilled(inPt, 3.5f, ui::col::kTextDim);
    dl->AddCircleFilled(outPt, 3.5f, act);
    for (int s = 0; s < kFxSlots; ++s) { // caixas por cima dos fios
        const ImVec2 c = center(s), q0(c.x - bw * 0.5f, c.y - bh * 0.5f), q1(c.x + bw * 0.5f, c.y + bh * 0.5f);
        const FxSlotParams& sp = ch.slot[s];
        const bool on = sp.active();
        dl->AddRectFilled(q0, q1, on ? ui::rgb(40, 52, 58) : ui::col::kCard, 3.0f);
        dl->AddRect(q0, q1, on ? categoryColor(fxInfo(sp.kind).category) : ui::col::kCardBorder, 3.0f);
        char t[48];
        if (names && sp.kind != FxKind::Off) std::snprintf(t, sizeof t, "%d %s", s + 1, fxInfo(sp.kind).name);
        else std::snprintf(t, sizeof t, "%d", s + 1);
        dl->PushClipRect(q0, q1, true);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(std::max(q0.x + 3.0f, c.x - ts.x * 0.5f), c.y - ts.y * 0.5f), on ? ui::col::kText : ui::col::kTextDim, t);
        dl->PopClipRect();
    }
}

// cabecalho de uma cadeia na pedaleira: algoritmo (combo + setas) e diagrama
inline void ChainHeader(FxChainParams& ch, int c) {
    ImGui::PushID(200 + c);
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::col::kAccent);
    ImGui::Text("CADEIA %d", c + 1);
    ImGui::PopStyleColor();
    char cur[48];
    std::snprintf(cur, sizeof cur, "%d. %s", ch.algorithm + 1, kFxChainAlgorithms[ch.algorithm].name);
    ImGui::SetNextItemWidth(170);
    if (ImGui::BeginCombo("##algo", cur, ImGuiComboFlags_HeightLarge)) {
        for (int a = 0; a < kFxNumChainAlgorithms; ++a) {
            char lbl[48];
            std::snprintf(lbl, sizeof lbl, "%d. %s", a + 1, kFxChainAlgorithms[a].name);
            if (ImGui::Selectable(lbl, a == ch.algorithm)) ch.algorithm = static_cast<uint8_t>(a);
            if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
                ChainAlgorithmDiagram(ch, a, ImVec2(200, 64), false);
                ImGui::TextUnformatted(kFxChainAlgorithms[a].description);
                ImGui::EndTooltip();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Algoritmo da cadeia: como os 4 pedais se ligam.\nOnde ramos se juntam, o sinal e a media deles.");
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) ch.algorithm = static_cast<uint8_t>((ch.algorithm + kFxNumChainAlgorithms - 1) % kFxNumChainAlgorithms);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) ch.algorithm = static_cast<uint8_t>((ch.algorithm + 1) % kFxNumChainAlgorithms);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kFxChainAlgorithms[ch.algorithm].description);
    ImGui::EndGroup();
    ImGui::SameLine(0, 16);
    ChainAlgorithmDiagram(ch, ch.algorithm, ImVec2(std::min(560.0f, ImGui::GetContentRegionAvail().x), 74), true);
    ImGui::PopID();
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
        if (KindCombo(sp)) clearSlotMods(r, c, s); // as rotas apontavam pra parametros do efeito anterior

        if (empty) {
            ImGui::TextDisabled("Vazio: o sinal passa direto.");
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

        // rodape: footswitch, expressao, medidor (pedal vazio nao tem o que ligar)
        if (!empty) {
        ImGui::Separator();
        if (Footswitch("##fs", sp.on, catCol)) sp.on = !sp.on;
        ImGui::SameLine();
        const bool isExpr = r.expr.chain == c && r.expr.slot == s;
        ImGui::BeginDisabled(empty);
        if (isExpr) ImGui::PushStyleColor(ImGuiCol_Button, ui::col::kAccent2);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
        if (ImGui::SmallButton("EXP")) ImGui::OpenPopup("##exp");
        if (isExpr) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(isExpr ? "Pedal de expressão: %s\n(mude a faixa no mixer)" : "Ligar o pedal de expressão a um parâmetro deste pedal",
                              isExpr && r.expr.param < info.nParams ? info.params[r.expr.param].label : "");
        if (ImGui::BeginPopup("##exp")) {
            ImGui::TextDisabled("Pedal de expressão controla:");
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
        ImGui::TextDisabled("Seco = direto pra saída;\n-> 1 / -> 2 = envio pra cada cadeia.");
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
        // ligacao entre as cadeias (o algoritmo de cada uma fica no cabecalho dela)
        ImGui::TextUnformatted("Ligação entre as cadeias");
        for (int l = 0; l < kFxNumLinks; ++l) {
            int cur = r.link;
            if (ImGui::RadioButton(kFxLinks[l].name, &cur, l)) r.link = static_cast<uint8_t>(l);
            ImGui::SetItemTooltip("%s", kFxLinks[l].description);
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", kFxLinks[r.link].description);
        ImGui::PopTextWrapPos();
        for (int c = 0; c < kFxChains; ++c) {
            ImGui::PushID(100 + c);
            ImGui::Text("Cadeia %d", c + 1);
            ui::Knob("Nível", &r.chain[c].level, 0.0f, 2.0f, 1.0f, "%.2f", ks);
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

    if (ui::BeginCard("##fxexpr", "PEDAL DE EXPRESSÃO")) {
        FxExpression& e = r.expr;
        if (e.chain < 0) {
            ImGui::TextDisabled("Sem alvo: use o botão EXP de um pedal.");
        } else {
            const FxInfo& info = fxInfo(r.chain[e.chain].slot[e.slot].kind);
            const FxParamInfo& pi = info.params[e.param];
            ImGui::Text("%d.%d %s: %s", e.chain + 1, e.slot + 1, info.name, pi.label);
            if (pi.choices) {
                ImGui::TextDisabled("escolha: o pedal percorre as opções");
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
        ImGui::SetItemTooltip("Simula o pedal pela tela (o CC aprendido faz o mesmo).\nAntes do primeiro valor o parâmetro fica como no pedal.");
    }
    ui::EndCard();
}

// [2026-11-01] Modulacao dos efeitos: 2 LFOs do rack e as rotas
inline void DrawFxModCard(FxRackUi& ui) {
    FxRackParams& r = ui.edit;
    if (!ui::BeginCard("##fxmod", "MODULAÇÃO DOS EFEITOS", 0.0f, ui::col::kAccent2)) { ui::EndCard(); return; }
    ImGui::TextDisabled("Qualquer parâmetro de um pedal pode ser destino. Tamanho e pre-delay de alguns reverbs não: recriam o efeito.");
    for (int k = 0; k < kFxLfos; ++k) {
        FxLfoParams& l = r.lfo[k];
        ImGui::PushID(k);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("LFO %c", 'A' + k);
        ImGui::SameLine(60);
        ImGui::SetNextItemWidth(100);
        if (ImGui::BeginCombo("##shape", fxLfoShapeName(l.shape))) {
            for (int s = 0; s < static_cast<int>(FxLfoShape::Count); ++s)
                if (ImGui::Selectable(fxLfoShapeName(s), s == l.shape)) l.shape = static_cast<uint8_t>(s);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::BeginCombo("##sync", l.sync < 0 ? "Livre" : lfoDivisionName(l.sync))) {
            if (ImGui::Selectable("Livre", l.sync < 0)) l.sync = -1;
            for (int d = 0; d < kNumLfoDivisions; ++d)
                if (ImGui::Selectable(lfoDivisionName(d), d == l.sync)) l.sync = d;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Livre, ou uma divisão do BPM");
        ImGui::SameLine();
        ImGui::BeginDisabled(l.sync >= 0);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderFloat("##rate", &l.rate, 0.01f, 20.0f, "%.2f Hz", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    int lastUsed = -1;
    for (int k = 0; k < kFxModRoutes; ++k) if (r.mod[k].src != FxModSource::None) lastUsed = k;
    const int visible = std::min(kFxModRoutes, std::max(lastUsed + 2, 2));
    if (ImGui::BeginTable("##fxmodt", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Fonte");
        ImGui::TableSetupColumn("Parte");
        ImGui::TableSetupColumn("Pedal");
        ImGui::TableSetupColumn("Parâmetro");
        ImGui::TableSetupColumn("Profundidade");
        ImGui::TableHeadersRow();
        for (int k = 0; k < visible; ++k) {
            FxModRoute& m = r.mod[k];
            ImGui::PushID(100 + k);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("##src", fxModSourceInfo(m.src).name, ImGuiComboFlags_HeightLarge)) {
                for (int s = 0; s < static_cast<int>(FxModSource::Count); ++s) {
                    const FxModSource fs = static_cast<FxModSource>(s);
                    if (ImGui::Selectable(fxModSourceInfo(fs).name, fs == m.src)) {
                        m.src = fs;
                        if (fs != FxModSource::None && m.depth == 0.0f) m.depth = 0.5f;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(50);
            if (fxModSourceInfo(m.src).perPart) {
                char prev[4];
                std::snprintf(prev, sizeof prev, "%c", 'A' + m.part);
                if (ImGui::BeginCombo("##part", prev)) {
                    for (int p = 0; p < kFxParts; ++p) {
                        char it[4];
                        std::snprintf(it, sizeof it, "%c", 'A' + p);
                        if (ImGui::Selectable(it, p == m.part)) m.part = p;
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("De qual parte vem o controlador ou a macro");
            } else ImGui::TextDisabled("-");
            ImGui::BeginDisabled(m.src == FxModSource::None);
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(190);
            char prev[64];
            std::snprintf(prev, sizeof prev, "%d.%d %s", m.chain + 1, m.slot + 1, fxInfo(r.chain[m.chain].slot[m.slot].kind).name);
            if (ImGui::BeginCombo("##pedal", prev, ImGuiComboFlags_HeightLarge)) {
                for (int c = 0; c < kFxChains; ++c)
                    for (int s = 0; s < kFxSlots; ++s) {
                        char it[64];
                        std::snprintf(it, sizeof it, "%d.%d %s", c + 1, s + 1, fxInfo(r.chain[c].slot[s].kind).name);
                        if (ImGui::Selectable(it, c == m.chain && s == m.slot)) { m.chain = c; m.slot = s; m.param = kFxModMix; }
                    }
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(130);
            const FxInfo& info = fxInfo(r.chain[m.chain].slot[m.slot].kind);
            if (ImGui::BeginCombo("##param", m.param == kFxModMix ? "Mix" : info.params[m.param].label, ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("Mix", m.param == kFxModMix)) m.param = kFxModMix;
                for (int i = 0; i < info.nParams; ++i) {
                    if (info.params[i].rebuild) continue;
                    if (ImGui::Selectable(info.params[i].label, i == m.param)) m.param = i;
                }
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(90);
            ImGui::DragFloat("##depth", &m.depth, 0.005f, -1.0f, 1.0f, "%+.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("-1..+1 = a faixa inteira do knob. Ctrl+clique digita");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
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
                if (pr.category != lastCat) { ImGui::SeparatorText(("Fábrica: " + pr.category).c_str()); lastCat = pr.category; }
                if (ImGui::Selectable(pr.name.c_str(), ui.current == pr.name)) loadPresetInto(ui, synth, pr.rack, pr.name, now);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pr.description.c_str());
            }
        }
        ImGui::SeparatorText("Usuário");
        if (ui.user.empty()) ImGui::TextDisabled("(nenhum salvo em %s/)", hooks.presetDir.string().c_str());
        for (const auto& e : ui.user)
            if (ImGui::Selectable(e.stem.c_str(), ui.current == e.stem)) {
                FxRackParams r;
                if (loadFxRackFile(e.path, r)) loadPresetInto(ui, synth, r, e.stem, now);
                else ui.status = "Não e um preset de efeitos: " + e.path.string();
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
            ui.status = "Não consegui salvar " + target.string();
        }
    }
    ImGui::SetItemTooltip("Grava o rack inteiro (pedais, envios, pedal de expressão) em %s", target.string().c_str());
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
            if (c == 1) ImGui::TextDisabled(ui.edit.link == kFxLinkParallel ? "(em paralelo com a cadeia 1)" : "(recebe a saída da cadeia 1)");
            ChainHeader(ui.edit.chain[c], c);
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
        DrawFxModCard(ui);
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
