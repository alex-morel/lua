#pragma once
#include "envelope_math.h"
#include "ui_widgets.h"
#include "dx7_params.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

// [ADICIONADO em 2026-10-18] Grafico dos Amp EG dos 8 operadores
// sobrepostos (uma cor por operador, ui::opColor). O do operador
// selecionado fica em destaque, com area preenchida e 4 pontos arrastaveis:
// fim do ataque, do decai 1, do decai 2 (inicio do sustain) e do release.
// Arrastar na vertical muda o LEVEL do estagio; na horizontal, o tempo do
// estagio (convertido de volta em RATE, ver envelope_math.h). Shift trava o
// tempo (so level). Clicar perto de outra curva seleciona aquele operador.
//
// Dois paineis: a esquerda, da nota ate o sustain; a direita (depois da
// linha "note off"), o release. Eixo de tempo comprimido (log perto de 0)
// pra 4ms e 20s caberem juntos; a escala congela durante o arrasto.
namespace ui {

// Operadores que chegam na saida (portadoras + quem as modula, direta ou
// indiretamente) -- usado pelo botao "So usados" da legenda.
inline void usedOperators(const dx7fm::VoiceParams& v, bool (&used)[dx7fm::kNumOps]) {
    using dx7fm::kNumOps;
    for (int i = 0; i < kNumOps; ++i) used[i] = v.matrix.outputMix[i] != 0.0f;
    for (bool grew = true; grew;) {
        grew = false;
        for (int i = 0; i < kNumOps; ++i) {
            if (!used[i]) continue;
            for (int j = 0; j < kNumOps; ++j) {
                bool feeds = j != i && (v.matrix.mod[i][j] != 0.0f || v.matrix.ring[i][j]);
                if (feeds && !used[j]) { used[j] = true; grew = true; }
            }
        }
    }
}

struct EnvelopeGraphState {
    bool visible[dx7fm::kNumOps] = {true, true, true, true, true, true, true, true};
    int dragNode = -1;          // 0..3 enquanto arrasta um ponto do operador selecionado
    float frozenTA = 0, frozenTR = 0; // escala do eixo congelada durante o arrasto
};

inline bool EnvelopeGraph(const char* id, dx7fm::VoiceParams& v, int& selectedOp, EnvelopeGraphState& st, float height) {
    using namespace dx7fm_app;
    using dx7fm::kNumOps;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool changed = false;

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float W = std::max(ImGui::GetContentRegionAvail().x, 200.0f);
    ImGui::InvisibleButton(id, ImVec2(W, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    // area de plotagem e os 2 paineis
    const float left = 30.0f, bottom = 18.0f, top = 16.0f, right = 8.0f, gap = 14.0f;
    const float px0 = p0.x + left, px1 = p0.x + W - right;
    const float py0 = p0.y + top, py1 = p0.y + height - bottom;
    const float paneA = (px1 - px0 - gap) * 0.72f, paneR = (px1 - px0 - gap) - paneA;
    const float rx0 = px0 + paneA + gap;

    // escala de tempo: maior duracao entre os operadores visiveis (+ folga)
    float tA = 0.05f, tR = 0.05f;
    for (int i = 0; i < kNumOps; ++i) {
        if (!st.visible[i] && i != selectedOp) continue;
        EgTimes t = egTimes(v.op[i].ampEG);
        tA = std::max(tA, t.untilSustain());
        tR = std::max(tR, t.stage[3]);
    }
    tA *= 1.35f;
    tR *= 1.35f;
    if (st.dragNode >= 0) { tA = st.frozenTA; tR = st.frozenTR; }

    auto xA = [&](float t) { return px0 + egTimeToX(t, tA) * paneA; };
    auto xR = [&](float t) { return rx0 + egTimeToX(t, tR) * paneR; };
    auto yL = [&](float level) { return py1 - std::clamp(level, 0.0f, 99.0f) / 99.0f * (py1 - py0); };

    // fundo, grade e rotulos
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + height), rgb(20, 23, 30), 6.0f);
    char buf[64];
    for (int lv : {0, 25, 50, 75, 99}) {
        float y = yL(static_cast<float>(lv));
        dl->AddLine(ImVec2(px0, y), ImVec2(px1, y), col::kGrid, 1.0f);
        std::snprintf(buf, sizeof buf, "%d", lv);
        dl->AddText(ImVec2(p0.x + 6, y - ImGui::GetTextLineHeight() * 0.5f), col::kTextDim, buf);
    }
    auto timeTicks = [&](float x0, float paneW, float tMax) {
        static const float ticks[] = {0.01f, 0.1f, 1.0f, 10.0f};
        static const char* names[] = {"10ms", "100ms", "1s", "10s"};
        for (int k = 0; k < 4; ++k) {
            if (ticks[k] > tMax) break;
            float x = x0 + egTimeToX(ticks[k], tMax) * paneW;
            dl->AddLine(ImVec2(x, py0), ImVec2(x, py1), col::kGrid, 1.0f);
            dl->AddText(ImVec2(x - 12, py1 + 2), col::kTextDim, names[k]);
        }
    };
    timeTicks(px0, paneA, tA);
    timeTicks(rx0, paneR, tR);
    dl->AddText(ImVec2(px0 + 4, p0.y + 1), col::kTextDim, "nota -> sustain");
    // linha tracejada do note off
    for (float y = py0; y < py1; y += 8.0f) dl->AddLine(ImVec2(rx0 - gap * 0.5f, y), ImVec2(rx0 - gap * 0.5f, std::min(y + 4.0f, py1)), col::kTextDim, 1.0f);
    dl->AddText(ImVec2(rx0 + 4, p0.y + 1), col::kTextDim, "note off -> release");

    // pontos (no espaco da tela) da curva de um operador: amostra cada
    // segmento linear no tempo, pra mostrar a curvatura do eixo comprimido
    struct Pt { float x, y; };
    auto buildCurve = [&](int op, Pt* out, int& n, ImVec2 (&nodes)[4]) {
        const auto& eg = v.op[op].ampEG;
        EgTimes t = egTimes(eg);
        n = 0;
        float t0 = 0.0f, l0 = 0.0f;
        constexpr int kSub = 20;
        for (int s = 0; s < 3; ++s) {
            float t1 = t0 + t.stage[s], l1 = eg.level[s];
            for (int k = (s == 0 ? 0 : 1); k <= kSub; ++k) {
                float a = static_cast<float>(k) / kSub;
                float tt = t0 + (t1 - t0) * a;
                out[n++] = {xA(tt), yL(l0 + (l1 - l0) * a)};
            }
            nodes[s] = ImVec2(xA(t1), yL(l1));
            t0 = t1; l0 = l1;
        }
        out[n++] = {px0 + paneA, yL(l0)}; // sustain segura ate o note off
        float rl0 = eg.level[2], rl1 = eg.level[3];
        for (int k = 0; k <= kSub; ++k) {
            float a = static_cast<float>(k) / kSub;
            out[n++] = {xR(t.stage[3] * a), yL(rl0 + (rl1 - rl0) * a)};
        }
        nodes[3] = ImVec2(xR(t.stage[3]), yL(rl1));
        out[n++] = {px1, yL(rl1)};
    };

    Pt pts[kNumOps][96];
    int npts[kNumOps] = {};
    ImVec2 nodes[kNumOps][4];
    for (int i = 0; i < kNumOps; ++i) buildCurve(i, pts[i], npts[i], nodes[i]);

    // curvas nao selecionadas primeiro (por baixo)
    dl->PushClipRect(ImVec2(p0.x, p0.y), ImVec2(p0.x + W, p0.y + height), true);
    for (int i = 0; i < kNumOps; ++i) {
        if (i == selectedOp || !st.visible[i]) continue;
        for (int k = 0; k + 1 < npts[i]; ++k) {
            if (pts[i][k + 1].x < pts[i][k].x) continue; // salto entre os paineis
            dl->AddLine(ImVec2(pts[i][k].x, pts[i][k].y), ImVec2(pts[i][k + 1].x, pts[i][k + 1].y), opColor(i, 150), 1.6f);
        }
    }
    // selecionada: area preenchida + linha grossa + pontos
    {
        int i = selectedOp;
        for (int k = 0; k + 1 < npts[i]; ++k) {
            const Pt& a = pts[i][k];
            const Pt& b = pts[i][k + 1];
            if (b.x < a.x || b.x - a.x > paneA) continue;
            dl->AddQuadFilled(ImVec2(a.x, a.y), ImVec2(b.x, b.y), ImVec2(b.x, py1), ImVec2(a.x, py1), opColor(i, 34));
            dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), opColor(i), 2.6f);
        }
    }
    dl->PopClipRect();

    // interacao
    auto nearestNode = [&](ImVec2 m) {
        int best = -1;
        float bestD = 9.0f * 9.0f;
        for (int k = 0; k < 4; ++k) {
            float dx = nodes[selectedOp][k].x - m.x, dy = nodes[selectedOp][k].y - m.y;
            if (dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; best = k; }
        }
        return best;
    };
    auto nearestCurve = [&](ImVec2 m) {
        int best = -1;
        float bestD = 7.0f;
        for (int i = 0; i < kNumOps; ++i) {
            if (!st.visible[i] && i != selectedOp) continue;
            for (int k = 0; k + 1 < npts[i]; ++k) {
                const Pt& a = pts[i][k];
                const Pt& b = pts[i][k + 1];
                if (b.x < a.x) continue;
                float vx = b.x - a.x, vy = b.y - a.y, wx = m.x - a.x, wy = m.y - a.y;
                float len2 = vx * vx + vy * vy;
                float tt = len2 > 0 ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f) : 0.0f;
                float dx = a.x + vx * tt - m.x, dy = a.y + vy * tt - m.y;
                float d = std::sqrt(dx * dx + dy * dy);
                if (d < bestD) { bestD = d; best = i; }
            }
        }
        return best;
    };

    static const char* kStage[4] = {"ataque", "decai 1", "decai 2 (sustain)", "release"};
    ImVec2 m = io.MousePos;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        int node = nearestNode(m);
        if (node >= 0) {
            st.dragNode = node;
            st.frozenTA = tA;
            st.frozenTR = tR;
        } else {
            int curve = nearestCurve(m);
            if (curve >= 0) selectedOp = curve;
        }
    }
    if (!active) st.dragNode = -1;

    if (active && st.dragNode >= 0) {
        auto& eg = v.op[selectedOp].ampEG;
        int k = st.dragNode;
        // level pela posicao vertical
        float level = std::round((py1 - m.y) / (py1 - py0) * 99.0f);
        level = std::clamp(level, 0.0f, 99.0f);
        if (static_cast<int>(level) != eg.level[k]) { eg.level[k] = static_cast<uint8_t>(level); changed = true; }
        // tempo pela posicao horizontal -> rate (Shift trava o tempo)
        if (!io.KeyShift) {
            EgTimes t = egTimes(eg);
            float from, stageSec;
            if (k < 3) {
                float startT = 0.0f;
                for (int s = 0; s < k; ++s) startT += t.stage[s];
                float tNode = egXToTime((m.x - px0) / paneA, tA);
                stageSec = std::max(tNode - startT, 0.0f);
                from = k == 0 ? 0.0f : static_cast<float>(eg.level[k - 1]);
            } else {
                stageSec = egXToTime((m.x - rx0) / paneR, tR);
                from = eg.level[2];
            }
            float rate = egRateForSeconds(from, eg.level[k], stageSec);
            if (rate >= 0.0f && static_cast<int>(std::lround(rate)) != eg.rate[k]) {
                eg.rate[k] = static_cast<uint8_t>(std::lround(rate));
                changed = true;
            }
        }
    }

    // pontos do selecionado (por cima de tudo)
    int hoverNode = hovered && st.dragNode < 0 ? nearestNode(m) : st.dragNode;
    for (int k = 0; k < 4; ++k) {
        ImVec2 c = nodes[selectedOp][k];
        bool hot = k == hoverNode;
        dl->AddCircleFilled(c, hot ? 6.0f : 4.5f, opColor(selectedOp));
        dl->AddCircle(c, hot ? 7.5f : 5.5f, hot ? col::kText : rgb(0, 0, 0, 160), 16, 1.5f);
    }
    if (hoverNode >= 0 && (hovered || active)) {
        const auto& eg = v.op[selectedOp].ampEG;
        EgTimes t = egTimes(eg);
        float sec = t.stage[hoverNode];
        ImGui::SetTooltip("Op%d - %s\nrate %d, level %d (%.0f ms)\nArraste: vertical = level, horizontal = tempo\n(Shift: só level)",
                          selectedOp + 1, kStage[hoverNode], eg.rate[hoverNode], eg.level[hoverNode], sec * 1000.0f);
    } else if (hovered) {
        int curve = nearestCurve(m);
        if (curve >= 0 && curve != selectedOp) ImGui::SetTooltip("Op%d - clique pra selecionar", curve + 1);
    }

    // legenda: clique seleciona (e mostra), botao direito liga/desliga a curva
    bool used[kNumOps];
    usedOperators(v, used);
    for (int i = 0; i < kNumOps; ++i) {
        ImGui::PushID(i);
        bool sel = i == selectedOp;
        ImU32 c = opColor(i, st.visible[i] || sel ? 255 : 70);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? opColor(i, 70) : rgb(36, 40, 51));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, opColor(i, 110));
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "Op%d", i + 1);
        if (ImGui::SmallButton(lbl)) { selectedOp = i; st.visible[i] = true; }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) st.visible[i] = !st.visible[i];
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("Op%d%s\nclique: selecionar | botão direito: mostrar/esconder", i + 1, used[i] ? "" : " (não chega na saída)");
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("Todos")) for (bool& b : st.visible) b = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Só usados")) for (int i = 0; i < kNumOps; ++i) st.visible[i] = used[i];
    return changed;
}

} // namespace ui
