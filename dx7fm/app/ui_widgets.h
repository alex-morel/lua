#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// [ADICIONADO em 2026-10-18] Visual "VST moderno" do app: tema escuro com
// cores de destaque, knobs rotativos desenhados, cards de secao e medidor.
namespace ui {

// --- paleta ------------------------------------------------------------------
inline ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
inline ImVec4 rgbv(int r, int g, int b, int a = 255) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }

namespace col {
inline const ImU32 kBg = rgb(18, 20, 26);
inline const ImU32 kCard = rgb(26, 29, 37);
inline const ImU32 kCardBorder = rgb(42, 47, 59);
inline const ImU32 kAccent = rgb(79, 209, 197);    // teal
inline const ImU32 kAccent2 = rgb(246, 173, 85);   // laranja (feedback, avisos)
inline const ImU32 kText = rgb(226, 230, 238);
inline const ImU32 kTextDim = rgb(138, 146, 162);
inline const ImU32 kKnobBody = rgb(36, 40, 51);
inline const ImU32 kKnobTrack = rgb(52, 58, 74);
inline const ImU32 kGrid = rgb(255, 255, 255, 18);
} // namespace col

// Uma cor por operador -- a MESMA no grafico de envelopes, na tabela de
// operadores e no diagrama do algoritmo.
inline ImU32 opColor(int op, int alpha = 255) {
    static const int c[8][3] = {{79, 209, 197}, {246, 173, 85}, {99, 179, 237}, {246, 135, 179},
                                {154, 230, 180}, {183, 148, 244}, {252, 129, 129}, {246, 224, 94}};
    const int* k = c[op & 7];
    return rgb(k[0], k[1], k[2], alpha);
}

// --- tema --------------------------------------------------------------------
inline void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 3);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 10.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
    s.SeparatorTextBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = rgbv(226, 230, 238);
    c[ImGuiCol_TextDisabled] = rgbv(120, 128, 144);
    c[ImGuiCol_WindowBg] = rgbv(18, 20, 26);
    c[ImGuiCol_ChildBg] = rgbv(26, 29, 37);
    c[ImGuiCol_PopupBg] = rgbv(30, 33, 42, 250);
    c[ImGuiCol_Border] = rgbv(42, 47, 59);
    c[ImGuiCol_FrameBg] = rgbv(36, 40, 51);
    c[ImGuiCol_FrameBgHovered] = rgbv(46, 51, 65);
    c[ImGuiCol_FrameBgActive] = rgbv(54, 60, 77);
    c[ImGuiCol_TitleBg] = rgbv(18, 20, 26);
    c[ImGuiCol_TitleBgActive] = rgbv(26, 29, 37);
    c[ImGuiCol_MenuBarBg] = rgbv(22, 25, 32);
    c[ImGuiCol_ScrollbarBg] = rgbv(18, 20, 26);
    c[ImGuiCol_ScrollbarGrab] = rgbv(52, 58, 74);
    c[ImGuiCol_ScrollbarGrabHovered] = rgbv(66, 73, 92);
    c[ImGuiCol_ScrollbarGrabActive] = rgbv(79, 209, 197);
    c[ImGuiCol_CheckMark] = rgbv(79, 209, 197);
    c[ImGuiCol_SliderGrab] = rgbv(79, 209, 197);
    c[ImGuiCol_SliderGrabActive] = rgbv(129, 230, 217);
    c[ImGuiCol_Button] = rgbv(40, 45, 58);
    c[ImGuiCol_ButtonHovered] = rgbv(52, 59, 76);
    c[ImGuiCol_ButtonActive] = rgbv(79, 209, 197, 160);
    c[ImGuiCol_Header] = rgbv(79, 209, 197, 50);
    c[ImGuiCol_HeaderHovered] = rgbv(79, 209, 197, 80);
    c[ImGuiCol_HeaderActive] = rgbv(79, 209, 197, 120);
    c[ImGuiCol_Separator] = rgbv(42, 47, 59);
    c[ImGuiCol_SeparatorHovered] = rgbv(79, 209, 197, 120);
    c[ImGuiCol_SeparatorActive] = rgbv(79, 209, 197);
    c[ImGuiCol_ResizeGrip] = rgbv(79, 209, 197, 40);
    c[ImGuiCol_Tab] = rgbv(26, 29, 37);
    c[ImGuiCol_TabHovered] = rgbv(46, 51, 65);
    c[ImGuiCol_TabSelected] = rgbv(36, 40, 51);
    c[ImGuiCol_TabSelectedOverline] = rgbv(79, 209, 197);
    c[ImGuiCol_TabDimmed] = rgbv(26, 29, 37);
    c[ImGuiCol_TabDimmedSelected] = rgbv(36, 40, 51);
    c[ImGuiCol_TableHeaderBg] = rgbv(30, 34, 43);
    c[ImGuiCol_TableBorderStrong] = rgbv(42, 47, 59);
    c[ImGuiCol_TableBorderLight] = rgbv(34, 38, 48);
    c[ImGuiCol_TableRowBg] = rgbv(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgbv(255, 255, 255, 8);
    c[ImGuiCol_PlotHistogram] = rgbv(79, 209, 197);
    c[ImGuiCol_TextSelectedBg] = rgbv(79, 209, 197, 90);
    c[ImGuiCol_NavCursor] = rgbv(79, 209, 197);
    c[ImGuiCol_ModalWindowDimBg] = rgbv(0, 0, 0, 140);
}

// --- card (secao com fundo arredondado e titulo) --------------------------------
// BeginCard/EndCard: child window que cresce na altura do conteudo.
inline bool BeginCard(const char* id, const char* title, float width = 0.0f, ImU32 titleColor = col::kAccent) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    bool open = ImGui::BeginChild(id, ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PopStyleVar();
    if (title && *title) {
        ImGui::PushStyleColor(ImGuiCol_Text, titleColor);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 2),
                                            col::kCardBorder, 1.0f);
        ImGui::Dummy(ImVec2(0, 2));
    }
    return open;
}
inline void EndCard() {
    ImGui::EndChild();
}

// --- knob rotativo ------------------------------------------------------------
// Arrastar pra cima/baixo muda o valor (Shift = fino); roda do mouse tambem;
// duplo clique volta ao padrao; botao direito abre um campo pra digitar.
// Faixa bipolar (min < 0 < max) desenha o arco a partir do centro.
struct KnobStyle {
    float size = 44.0f;     // diametro
    ImU32 color = col::kAccent;
    bool logarithmic = false;
    float cellWidth = 62.0f; // largura reservada (rotulo e valor centralizados embaixo)
};

inline float knobNorm(float v, float vmin, float vmax, bool logarithmic) {
    if (logarithmic && vmin > 0.0f) return std::log(v / vmin) / std::log(vmax / vmin);
    return (v - vmin) / (vmax - vmin);
}
inline float knobValue(float n, float vmin, float vmax, bool logarithmic) {
    n = std::clamp(n, 0.0f, 1.0f);
    if (logarithmic && vmin > 0.0f) return vmin * std::pow(vmax / vmin, n);
    return vmin + (vmax - vmin) * n;
}

inline bool Knob(const char* label, float* v, float vmin, float vmax, float vdefault, const char* fmt = "%.2f",
                 KnobStyle st = KnobStyle()) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float labelH = ImGui::GetTextLineHeight();
    const float w = std::max(st.size, st.cellWidth);
    bool changed = false;

    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 center(p0.x + w * 0.5f, p0.y + st.size * 0.5f);
    float r = st.size * 0.5f;

    // a area clicavel e o proprio knob; o rotulo do botao invisivel e o nome
    // do parametro (aparece pros hooks de teste/acessibilidade do ImGui)
    ImGui::SetCursorScreenPos(ImVec2(center.x - r, p0.y));
    ImGui::InvisibleButton(label, ImVec2(st.size, st.size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    float n = knobNorm(*v, vmin, vmax, st.logarithmic);

    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.MouseDelta.y != 0.0f) {
        float speed = io.KeyShift ? 0.0008f : 0.005f;
        n = std::clamp(n - io.MouseDelta.y * speed, 0.0f, 1.0f);
        *v = knobValue(n, vmin, vmax, st.logarithmic);
        changed = true;
    }
    if (hovered && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY) && io.MouseWheel != 0.0f) {
        n = std::clamp(n + io.MouseWheel * (io.KeyShift ? 0.005f : 0.03f), 0.0f, 1.0f);
        *v = knobValue(n, vmin, vmax, st.logarithmic);
        changed = true;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = vdefault;
        changed = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##edit");
    if (ImGui::BeginPopup("##edit")) {
        ImGui::TextDisabled("%s", label);
        ImGui::SetNextItemWidth(120);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        float tmp = *v;
        if (ImGui::InputFloat("##value", &tmp, 0.0f, 0.0f, fmt, ImGuiInputTextFlags_EnterReturnsTrue)) {
            *v = std::clamp(tmp, vmin, vmax);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    n = knobNorm(*v, vmin, vmax, st.logarithmic);

    // desenho: corpo, trilha de 270 graus, arco de valor, ponteiro
    const float a0 = IM_PI * 0.75f, a1 = IM_PI * 2.25f;
    float an = a0 + (a1 - a0) * n;
    float trackR = r - 3.0f;
    dl->AddCircleFilled(center, r - 7.0f, hovered || active ? col::kKnobTrack : col::kKnobBody, 32);
    dl->PathArcTo(center, trackR, a0, a1, 40);
    dl->PathStroke(col::kKnobTrack, 0, 3.5f);
    bool bipolar = vmin < 0.0f && vmax > 0.0f && !st.logarithmic;
    float from = bipolar ? a0 + (a1 - a0) * knobNorm(0.0f, vmin, vmax, false) : a0;
    if (std::fabs(an - from) > 0.01f) {
        dl->PathArcTo(center, trackR, std::min(from, an), std::max(from, an), 40);
        dl->PathStroke(st.color, 0, 3.5f);
    }
    ImVec2 dir(std::cos(an), std::sin(an));
    dl->AddLine(ImVec2(center.x + dir.x * (r - 16.0f), center.y + dir.y * (r - 16.0f)),
                ImVec2(center.x + dir.x * (r - 8.0f), center.y + dir.y * (r - 8.0f)), col::kText, 2.0f);

    // rotulo e valor (centralizados embaixo)
    char value[32];
    std::snprintf(value, sizeof value, fmt, *v);
    auto centered = [&](const char* text, float y, ImU32 color) {
        const char* end = ImGui::FindRenderedTextEnd(text);
        float tw = ImGui::CalcTextSize(text, end).x;
        float x = std::max(p0.x, center.x - tw * 0.5f);
        dl->PushClipRect(ImVec2(p0.x - 4, y), ImVec2(p0.x + w + 4, y + labelH), true);
        dl->AddText(ImVec2(x, y), color, text, end);
        dl->PopClipRect();
    };
    centered(label, p0.y + st.size + 2.0f, col::kTextDim);
    centered(value, p0.y + st.size + 2.0f + labelH, hovered || active ? st.color : col::kText);

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, st.size + 4.0f + labelH * 2.0f));
    ImGui::EndGroup();
    if (hovered && !active)
        ImGui::SetItemTooltip("%s: %s\nArraste (Shift = fino), roda do mouse,\nduplo clique = padrao, botao direito = digitar", label, value);
    ImGui::PopID();
    return changed;
}

// Variantes inteiras (parametros 0..99, 0..7 etc.): o knob trabalha em
// float e arredonda.
inline bool KnobInt(const char* label, int* v, int vmin, int vmax, int vdefault, const char* fmt = "%d", KnobStyle st = KnobStyle()) {
    float f = static_cast<float>(*v);
    char ffmt[16];
    std::snprintf(ffmt, sizeof ffmt, "%s", fmt);
    // fmt inteiro ("%d") nao serve pra float: troca por "%.0f" mantendo prefixo/sufixo
    char buf[32];
    const char* d = std::strstr(ffmt, "%d");
    if (d) {
        std::snprintf(buf, sizeof buf, "%.*s%%.0f%s", static_cast<int>(d - ffmt), ffmt, d + 2);
    } else {
        std::snprintf(buf, sizeof buf, "%s", ffmt);
    }
    bool changed = Knob(label, &f, static_cast<float>(vmin), static_cast<float>(vmax), static_cast<float>(vdefault), buf, st);
    int nv = std::clamp(static_cast<int>(std::lround(f)), vmin, vmax);
    if (changed && nv != *v) { *v = nv; return true; }
    return false;
}
inline bool KnobU8(const char* label, uint8_t* v, int vmin, int vmax, int vdefault, const char* fmt = "%d", KnobStyle st = KnobStyle()) {
    int i = *v;
    if (KnobInt(label, &i, vmin, vmax, vdefault, fmt, st)) { *v = static_cast<uint8_t>(i); return true; }
    return false;
}

// --- medidor de nivel (L/R) ------------------------------------------------------
inline void LevelMeter(const char* id, float level, ImVec2 size) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::kKnobBody, 3.0f);
    // escala em dB (-48..0): sinal baixo continua visivel
    float db = level > 1e-6f ? 20.0f * std::log10(level) : -100.0f;
    float n = std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f);
    float x1 = p.x + size.x * n;
    float xY = p.x + size.x * (36.0f / 48.0f), xR = p.x + size.x * (45.0f / 48.0f); // -12dB e -3dB
    auto seg = [&](float a, float b, ImU32 c) {
        if (x1 > a) dl->AddRectFilled(ImVec2(a, p.y + 1), ImVec2(std::min(x1, b), p.y + size.y - 1), c, 2.0f);
    };
    seg(p.x + 1, xY, rgb(79, 209, 140));
    seg(xY, xR, rgb(246, 224, 94));
    seg(xR, p.x + size.x - 1, rgb(252, 100, 100));
    dl->AddText(ImVec2(p.x + 4, p.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f), rgb(10, 12, 16, 200), id);
    ImGui::Dummy(size);
    ImGui::PopID();
}

} // namespace ui
