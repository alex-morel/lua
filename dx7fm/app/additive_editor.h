#pragma once
#include "additive_presets.h"
#include "resynth_ui.h"
#include "ui_widgets.h"
#include <cmath>
#include <cstdio>
#include <utility>

// [ADICIONADO em 2026-10-20] Editor da forma de onda Additive (64 parciais):
// um grafico de barras onde se PINTA o espectro com o mouse, em 3 vistas
// (amplitude linear, amplitude em dB, razao de frequencia em escala log),
// mais campos numericos do parcial selecionado, presets e morph A->B.
namespace ui {

struct AdditiveEditorState {
    int editB = 0;     // espectro em edicao: 0 = A, 1 = B
    int view = 0;      // 0 = amplitude, 1 = amplitude (dB), 2 = razao
    int selected = 0;  // parcial dos campos numericos
    int lastK = 0;     // traco em andamento (mouse do quadro anterior)
    float lastT = 0.0f;
    ResynthUiState resynth; // dialogo "Ressintetizar WAV..." (resynth_ui.h)
};

namespace additive_detail {

// SameLine se um item de largura w ainda cabe na linha
inline void wrapSameLine(float w) {
    float x2 = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w;
    if (x2 < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
}

inline bool setIfChanged(float& dst, float v) {
    if (dst == v) return false;
    dst = v;
    return true;
}

} // namespace additive_detail

// Grafico de barras pintavel. Botao esquerdo pinta (Shift na vista de razao
// encaixa em harmonicos inteiros / subharmonicos 1/n); botao direito
// desliga o parcial (amplitude) ou volta a razao pro harmonico k+1.
inline bool PartialBars(const char* id, dx7fm_app::PartialSet& set, const dx7fm_app::PartialSet& ghost,
                        AdditiveEditorState& st, ImU32 color, ImVec2 size) {
    using namespace dx7fm_app;
    const bool ratioView = st.view == 2, db = st.view == 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive(), activated = ImGui::IsItemActivated();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    // barras a esquerda, coluna de rotulos do eixo a direita (fora das barras)
    const float gutter = 38.0f, padY = 6.0f, h = size.y - 2 * padY, bw = (size.x - gutter) / kNumPartials;
    const float barsX1 = p0.x + kNumPartials * bw;
    auto axisOf = [&](const PartialSet& s, int k) { return ratioView ? ratioToAxis(s.ratio[k]) : ampToAxis(s.amp[k], db); };
    auto yOf = [&](float t) { return p1.y - padY - t * h; };

    // --- edicao ---
    bool changed = false;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int mk = std::clamp(static_cast<int>((m.x - p0.x) / bw), 0, kNumPartials - 1);
    const float mt = std::clamp((p1.y - padY - m.y) / h, 0.0f, 1.0f);
    if (active) {
        if (activated) { st.lastK = mk; st.lastT = mt; }
        const bool erase = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        const bool snap = ImGui::GetIO().KeyShift;
        paintStroke(st.lastK, st.lastT, mk, mt, [&](int k, float t) {
            if (ratioView) changed |= additive_detail::setIfChanged(set.ratio[k], erase ? static_cast<float>(k + 1) : axisToRatio(t, snap));
            else changed |= additive_detail::setIfChanged(set.amp[k], erase ? 0.0f : axisToAmp(t, db));
        });
        st.lastK = mk;
        st.lastT = mt;
        st.selected = mk;
    }

    // --- desenho ---
    dl->AddRectFilled(p0, p1, rgb(14, 16, 22), 6.0f);
    char lbl[32];
    if (ratioView) {
        for (float r : {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f}) {
            float y = yOf(ratioToAxis(r));
            dl->AddLine(ImVec2(p0.x, y), ImVec2(barsX1, y), col::kGrid);
            std::snprintf(lbl, sizeof lbl, "%gx", r);
            dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(lbl).x - 4, y - ImGui::GetTextLineHeight() * 0.5f), rgb(255, 255, 255, 90), lbl);
        }
    } else {
        for (int g = 1; g <= 4; ++g) {
            float t = g / 4.0f, y = yOf(t);
            dl->AddLine(ImVec2(p0.x, y), ImVec2(barsX1, y), col::kGrid);
            if (db) std::snprintf(lbl, sizeof lbl, "%.0f dB", (t - 1.0f) * kAmpAxisDbRange);
            else std::snprintf(lbl, sizeof lbl, "%.2f", t);
            dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(lbl).x - 4, y - ImGui::GetTextLineHeight() * 0.5f), rgb(255, 255, 255, 90), lbl);
        }
    }
    for (int k = 8; k < kNumPartials; k += 8) { // marca a cada 8 parciais
        float x = p0.x + k * bw;
        dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), col::kGrid);
    }

    const float gap = bw >= 4.0f ? 1.0f : 0.0f;
    const ImU32 dimBar = rgb(90, 96, 110, 160);
    for (int k = 0; k < kNumPartials; ++k) {
        const float x0 = p0.x + k * bw + gap, x1 = p0.x + (k + 1) * bw - gap;
        const float t = axisOf(set, k);
        // na vista de razao, parcial desligado (amp 0) fica cinza: nao soa
        const ImU32 c = ratioView && set.amp[k] <= 0.0f ? dimBar : color;
        if (t > 0.0f) {
            dl->AddRectFilled(ImVec2(x0, yOf(t)), ImVec2(x1, p1.y - padY), (c & 0x00FFFFFF) | (0x90u << 24));
            dl->AddLine(ImVec2(x0, yOf(t)), ImVec2(x1, yOf(t)), c, 2.0f);
        }
        // o OUTRO espectro (A ou B), so como marca branca translucida
        const float tg = axisOf(ghost, k);
        if (tg > 0.0f) dl->AddLine(ImVec2(x0, yOf(tg)), ImVec2(x1, yOf(tg)), rgb(255, 255, 255, 110), 1.0f);
        if (k == st.selected) dl->AddRect(ImVec2(x0 - gap, p0.y + 1), ImVec2(x1 + gap, p1.y - 1), rgb(255, 255, 255, 80), 2.0f);
    }
    dl->AddRect(p0, p1, col::kCardBorder, 6.0f);

    if (hovered && !active && m.x < barsX1) {
        const float a = set.amp[mk];
        if (a > 0.0f)
            ImGui::SetTooltip("Parcial %d\namplitude %.3f (%.1f dB)\nrazao %.3fx\n(outro espectro: amp %.3f, razão %.3fx)", mk + 1, a,
                              20.0f * std::log10(a), set.ratio[mk], ghost.amp[mk], ghost.ratio[mk]);
        else
            ImGui::SetTooltip("Parcial %d (desligado)\nrazao %.3fx\n(outro espectro: amp %.3f, razão %.3fx)", mk + 1, set.ratio[mk],
                              ghost.amp[mk], ghost.ratio[mk]);
    }
    return changed;
}

inline bool AdditiveEditor(dx7fm::OperatorParams& op, int opIndex, ImU32 color, AdditiveEditorState& st) {
    using namespace dx7fm_app;
    using additive_detail::wrapSameLine;
    bool changed = false;
    char title[96];
    std::snprintf(title, sizeof title, "Aditiva: %d parciais (A: %d ligados, B: %d)", kNumPartials, activePartials(op.additiveA),
                  activePartials(op.additiveB));
    ImGui::SeparatorText(title);

    ImGui::RadioButton("Editando A", &st.editB, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Editando B", &st.editB, 1);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::RadioButton("Amplitude", &st.view, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Amplitude dB", &st.view, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Razão", &st.view, 2);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cada barra e um oscilador senoidal: soa em (razão x frequência do operador)\n"
                          "com a sua amplitude. Razões inteiras = harmônicos; fracionarias =\n"
                          "inarmônicos (sinos, tambores).\n\n"
                          "Clique e arraste pra PINTAR as barras. Botão direito: desliga o parcial\n"
                          "(vista de amplitude) ou volta a razão pro harmônico (vista de razão).\n"
                          "Shift na vista de razão: encaixa em harmônicos inteiros.\n\n"
                          "A e B são dois espectros: o envelope de FX leva de A até B\n"
                          "(x profundidade do morph), interpolando amplitudes E razões. A marca\n"
                          "branca em cada barra e o outro espectro. Parciais acima de Nyquist\n"
                          "são desligados sozinhos (sem aliasing).");

    PartialSet& set = st.editB ? op.additiveB : op.additiveA;
    const PartialSet& ghost = st.editB ? op.additiveA : op.additiveB;
    st.selected = std::clamp(st.selected, 0, kNumPartials - 1);

    const float knobW = 62.0f; // KnobStyle::cellWidth
    const float w = std::max(200.0f, ImGui::GetContentRegionAvail().x - knobW - ImGui::GetStyle().ItemSpacing.x);
    changed |= PartialBars("##partials", set, ghost, st, color, ImVec2(w, 150.0f));
    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        KnobStyle ks;
        ks.color = color;
        changed |= Knob("Morph", &op.morphDepth, 0.0f, 1.0f, 0.0f, "%.2f", ks);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Profundidade do morph A -> B.\n"
                              "morph = profundidade x (FX EG / 99) + mod matrix (OperatorMorph)");
    }
    ImGui::EndGroup();

    // parcial selecionado (ultimo pintado/clicado): valores exatos
    {
        const int k = st.selected;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Parcial");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(36);
        int pk = k + 1;
        if (ImGui::DragInt("##sel", &pk, 0.2f, 1, kNumPartials, "%d", ImGuiSliderFlags_AlwaysClamp)) st.selected = pk - 1;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("escolhe o parcial (ou clique numa barra)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        changed |= ImGui::DragFloat("amp##selamp", &set.amp[st.selected], 0.002f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        changed |= ImGui::DragFloat("razão##selratio", &set.ratio[st.selected], 0.005f, 0.01f, kMaxPartialRatio, "%.3fx",
                                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("x frequência do operador; ctrl+clique pra digitar");
        ImGui::SameLine();
        if (ImGui::SmallButton("= harmônico")) { changed |= additive_detail::setIfChanged(set.ratio[st.selected], static_cast<float>(st.selected + 1)); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("razão = %d (o harmônico desta posição)", st.selected + 1);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Preset em %s:", st.editB ? "B" : "A");
    for (const auto& p : kAdditivePresets) {
        wrapSameLine(ImGui::CalcTextSize(p.name).x + ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::SmallButton(p.name)) { p.make(set); changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tip);
    }
    if (ImGui::SmallButton("Copiar A -> B")) { op.additiveB = op.additiveA; changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copiar B -> A")) { op.additiveA = op.additiveB; changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Trocar A <-> B")) { std::swap(op.additiveA, op.additiveB); changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Razões harmônicas")) {
        for (int k = 0; k < kNumPartials; ++k) changed |= additive_detail::setIfChanged(set.ratio[k], static_cast<float>(k + 1));
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("razão de cada parcial = sua posição (1, 2, 3 ..), amplitudes intactas");
    ImGui::SameLine();
    if (ImGui::SmallButton("Limpar")) { presets::clear(set); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("desliga todos os parciais (silêncio) e volta as razões pros harmônicos");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, (color & 0x00FFFFFF) | (0x60u << 24));
    if (ImGui::SmallButton("Ressintetizar WAV...")) st.resynth.requestOpen = true;
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Analisa um som gravado (.wav) e monta os espectros A (ataque) e B (sustain),\n"
                          "o morph pelo FX EG e, se quiser, o Amp EG a partir dele.");
    if (ResynthDialog(op, opIndex, st.resynth, color)) {
        changed = true;
        st.view = 1; // amplitude em dB: parciais fracos de um som real somem na escala linear
        st.editB = 0;
    }
    return changed;
}

} // namespace ui
