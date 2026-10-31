#pragma once
#include "op_fx_math.h"
#include "ui_widgets.h"
#include <cmath>
#include <cstdio>
#include <vector>

// [ADICIONADO em 2026-10-20] Painel do efeito por operador: escolha do
// efeito em botoes, grafico do que o efeito faz (resposta do filtro com a
// faixa varrida pelo FX EG, curva de transferencia + onda processada, dentes
// do comb, harmonicos deslocados, ring mod) e knobs com o nome e a unidade
// reais de cada parametro. A conta dos graficos esta em op_fx_math.h.
namespace ui {

namespace opfx_detail {

using dx7fm::FxType;
namespace opfx = dx7fm_app::opfx;

inline void wrapSameLine(float w) {
    float x2 = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w;
    if (x2 < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
}

inline void dashedV(ImDrawList* dl, float x, float y0, float y1, ImU32 c) {
    for (float y = y0; y < y1; y += 6.0f) dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 3.0f, y1)), c);
}

inline void label(ImDrawList* dl, ImVec2 p, const char* s, ImU32 c = rgb(255, 255, 255, 90)) { dl->AddText(p, c, s); }

// Grafico de resposta em frequencia (eixo log 20 Hz..20 kHz, dB): curva no
// corte maximo (FX EG em 99) e no minimo (FX EG em 0), faixa entre elas.
template <typename MagFn>
inline void freqPlot(ImDrawList* dl, ImVec2 p0, ImVec2 p1, MagFn magAt, float dbTop, float dbBottom, bool logX, float fMax,
                     ImU32 color) {
    const float fMin = logX ? 20.0f : 0.0f, w = p1.x - p0.x, h = p1.y - p0.y;
    auto xOf = [&](float f) { return p0.x + w * (logX ? std::log(f / fMin) / std::log(fMax / fMin) : f / fMax); };
    auto fOf = [&](float x) { float t = (x - p0.x) / w; return logX ? fMin * std::pow(fMax / fMin, t) : std::max(1.0f, t * fMax); };
    auto yOf = [&](float db) { return p0.y + h * std::clamp((dbTop - db) / (dbTop - dbBottom), 0.0f, 1.0f); };
    // grade
    for (float db = dbTop; db >= dbBottom; db -= 12.0f) {
        dl->AddLine(ImVec2(p0.x, yOf(db)), ImVec2(p1.x, yOf(db)), db == 0.0f ? rgb(255, 255, 255, 45) : col::kGrid);
        char b[16];
        std::snprintf(b, sizeof b, "%+.0f", db);
        if (db != dbTop && db != dbBottom) label(dl, ImVec2(p0.x + 2, yOf(db) - ImGui::GetTextLineHeight()), b);
    }
    if (logX) {
        for (float f : {100.0f, 1000.0f, 10000.0f}) {
            dl->AddLine(ImVec2(xOf(f), p0.y), ImVec2(xOf(f), p1.y), col::kGrid);
            label(dl, ImVec2(xOf(f) + 3, p1.y - ImGui::GetTextLineHeight()), f >= 1000.0f ? (f >= 10000.0f ? "10k" : "1k") : "100");
        }
    }
    // curvas
    const int n = static_cast<int>(w / 2.0f);
    std::vector<ImVec2> hi(static_cast<size_t>(n)), lo(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        float x = p0.x + w * i / (n - 1), f = fOf(x);
        hi[static_cast<size_t>(i)] = ImVec2(x, yOf(magAt(f, true)));
        lo[static_cast<size_t>(i)] = ImVec2(x, yOf(magAt(f, false)));
    }
    for (int i = 0; i + 1 < n; ++i) { // faixa varrida pelo FX EG
        const ImVec2 &a = hi[static_cast<size_t>(i)], &b = hi[static_cast<size_t>(i + 1)], &c = lo[static_cast<size_t>(i + 1)], &d = lo[static_cast<size_t>(i)];
        dl->AddQuadFilled(a, b, c, d, (color & 0x00FFFFFF) | (0x22u << 24));
    }
    dl->AddPolyline(lo.data(), n, rgb(255, 255, 255, 70), 0, 1.0f);
    dl->AddPolyline(hi.data(), n, color, 0, 2.0f);
}

// curva de transferencia (quadrado a esquerda) + 2 ciclos de um seno processado
template <typename ShapeFn>
inline void shaperPlot(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ShapeFn shapeAt, bool showLow, ImU32 color,
                       const std::vector<float>* wave = nullptr) {
    const float h = p1.y - p0.y, side = h;
    const ImVec2 q0 = p0, q1(p0.x + side, p1.y);
    const float cx = (q0.x + q1.x) * 0.5f, cy = (q0.y + q1.y) * 0.5f, r = side * 0.5f - 6.0f;
    dl->AddLine(ImVec2(q0.x + 4, cy), ImVec2(q1.x - 4, cy), col::kGrid);
    dl->AddLine(ImVec2(cx, q0.y + 4), ImVec2(cx, q1.y - 4), col::kGrid);
    dl->AddLine(ImVec2(cx - r, cy + r), ImVec2(cx + r, cy - r), rgb(255, 255, 255, 35)); // identidade
    const int n = 160;
    std::vector<ImVec2> hi(n), lo(n);
    for (int i = 0; i < n; ++i) {
        float x = -1.0f + 2.0f * i / (n - 1);
        hi[static_cast<size_t>(i)] = ImVec2(cx + x * r, cy - std::clamp(shapeAt(x, true), -1.2f, 1.2f) * r);
        lo[static_cast<size_t>(i)] = ImVec2(cx + x * r, cy - std::clamp(shapeAt(x, false), -1.2f, 1.2f) * r);
    }
    if (showLow) dl->AddPolyline(lo.data(), n, rgb(255, 255, 255, 70), 0, 1.0f);
    dl->AddPolyline(hi.data(), n, color, 0, 2.0f);
    label(dl, ImVec2(q0.x + 4, q0.y + 2), "curva");

    // onda: seno de entrada (tenue) e saida
    const float wx0 = q1.x + 14, wx1 = p1.x - 4, wcy = cy, wr = r;
    if (wx1 - wx0 < 60) return;
    const int m = static_cast<int>(wx1 - wx0);
    std::vector<ImVec2> in(static_cast<size_t>(m)), out(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i) {
        float ph = 2.0f * static_cast<float>(M_PI) * 2.0f * i / (m - 1); // 2 ciclos
        float s = std::sin(ph);
        float y = wave ? (*wave)[static_cast<size_t>(i * static_cast<int>(wave->size()) / m)] : shapeAt(s, true);
        in[static_cast<size_t>(i)] = ImVec2(wx0 + static_cast<float>(i), wcy - s * wr);
        out[static_cast<size_t>(i)] = ImVec2(wx0 + static_cast<float>(i), wcy - std::clamp(y, -1.2f, 1.2f) * wr);
    }
    dl->AddLine(ImVec2(wx0, wcy), ImVec2(wx1, wcy), col::kGrid);
    dl->AddPolyline(in.data(), m, rgb(255, 255, 255, 50), 0, 1.0f);
    dl->AddPolyline(out.data(), m, color, 0, 2.0f);
    label(dl, ImVec2(wx0 + 2, q0.y + 2), "seno processado");
}

} // namespace opfx_detail

inline void OperatorFxGraph(const dx7fm::VoiceParams& v, int opIndex, ImVec2 size) {
    using namespace opfx_detail;
    const auto& op = v.op[opIndex];
    const ImU32 color = opColor(opIndex);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, rgb(14, 16, 22), 6.0f);
    dl->PushClipRect(p0, p1, true);
    const ImVec2 i0(p0.x + 6, p0.y + 4), i1(p1.x - 6, p1.y - 4);
    const float sr = 48000.0f;
    const float p1v = op.fxParam1, p2v = op.fxParam2;

    switch (op.fxType) {
        case FxType::Bypass: {
            const char* s = "sem efeito";
            ImVec2 ts = ImGui::CalcTextSize(s);
            label(dl, ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), s, col::kTextDim);
            break;
        }
        case FxType::LowPass: case FxType::BandPass: case FxType::HighPass: case FxType::Notch: {
            freqPlot(dl, i0, i1, [&](float f, bool top) { return opfx::filterMagnitudeDb(op.fxType, op.fxModel, f, top ? p1v : 0.2f * p1v, p2v, sr); },
                     24.0f, -48.0f, true, 20000.0f, color);
            float xc = i0.x + (i1.x - i0.x) * std::log(std::clamp(p1v, 20.0f, 20000.0f) / 20.0f) / std::log(1000.0f);
            dashedV(dl, xc, i0.y, i1.y, color);
            break;
        }
        case FxType::Comb: {
            const float fd = std::max(p1v, 20.0f), fMax = std::min(20000.0f, fd * 8.0f);
            freqPlot(dl, i0, i1, [&](float f, bool) { return opfx::combMagnitudeDb(f, fd, p2v); }, 24.0f, -24.0f, false, fMax, color);
            for (int k = 1; k <= 7; ++k) {
                float x = i0.x + (i1.x - i0.x) * k * fd / fMax;
                char b[24];
                if (k == 1) std::snprintf(b, sizeof b, "%.0f Hz", static_cast<double>(fd));
                else std::snprintf(b, sizeof b, "%dx", k);
                label(dl, ImVec2(x + 2, i1.y - ImGui::GetTextLineHeight()), b);
            }
            break;
        }
        case FxType::Saturate: case FxType::Fold: case FxType::Waveshape:
            shaperPlot(dl, i0, i1, [&](float x, bool top) { return opfx::shape(op.fxType, x, p1v, p2v, top ? 1.0f : 0.0f, op.fxShape); },
                       op.fxType != FxType::Saturate, color);
            break;
        case FxType::Crush: {
            // a onda inclui o sample & hold, num seno de 220 Hz amostrado a 48 kHz
            // (mesmo sample & hold do Crusher: segura a entrada a cada 1/taxa s)
            const float hold = 1.0f / std::max(1.0f, p1v);
            const int total = static_cast<int>(2.0f / 220.0f * sr); // 2 ciclos
            std::vector<float> wave(static_cast<size_t>(total));
            float timer = hold, held = 0.0f;
            for (int i = 0; i < total; ++i) {
                if (timer >= hold) { timer -= hold; held = std::sin(2.0f * static_cast<float>(M_PI) * 220.0f * static_cast<float>(i) / sr); }
                timer += 1.0f / sr;
                wave[static_cast<size_t>(i)] = opfx::shape(FxType::Crush, held, p1v, p2v);
            }
            shaperPlot(dl, i0, i1, [&](float x, bool) { return opfx::shape(FxType::Crush, x, p1v, p2v); }, false, color, &wave);
            break;
        }
        case FxType::FreqShift: {
            // harmonicos 1..10 de 220 Hz (tenues) e para onde o shift leva cada um
            const float base = 220.0f, fMax = 220.0f * 10.5f + std::max(0.0f, p1v);
            auto xOf = [&](float f) { return i0.x + (i1.x - i0.x) * f / fMax; };
            for (int k = 1; k <= 10; ++k) {
                float f = base * k, g = f + p1v, y = i1.y - (i1.y - i0.y - 18) / std::sqrt(static_cast<float>(k));
                dl->AddLine(ImVec2(xOf(f), i1.y), ImVec2(xOf(f), y), rgb(255, 255, 255, 60), 2.0f);
                if (g < fMax) {
                    dl->AddLine(ImVec2(xOf(g), i1.y), ImVec2(xOf(g), y), color, 2.0f);
                    if (p1v > 1.0f) dl->AddLine(ImVec2(xOf(f), y + 4), ImVec2(xOf(g), y + 4), (color & 0x00FFFFFF) | (0x60u << 24));
                }
            }
            char b[64];
            std::snprintf(b, sizeof b, "harmonicos de 220 Hz (cinza) +%.0f Hz", p1v);
            label(dl, ImVec2(i0.x + 2, i0.y), b);
            break;
        }
        case FxType::RingMod: {
            const int src = op.ringModSource;
            if (src < 0 || src >= dx7fm::kNumOps || src == opIndex) {
                const char* s = src == opIndex ? "a fonte nao pode ser o proprio operador" : "escolha o operador fonte";
                ImVec2 ts = ImGui::CalcTextSize(s);
                label(dl, ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), s, col::kAccent2);
                break;
            }
            // as duas ondas (razoes dos operadores) e o produto
            const auto& so = v.op[src];
            const float ra = op.fixedFreq ? op.fixedHz / 220.0f : op.coarseRatio, rb = so.fixedFreq ? so.fixedHz / 220.0f : so.coarseRatio;
            const int m = static_cast<int>(i1.x - i0.x);
            const float lane = (i1.y - i0.y) / 3.0f;
            std::vector<ImVec2> a(static_cast<size_t>(m)), b(static_cast<size_t>(m)), c(static_cast<size_t>(m));
            for (int i = 0; i < m; ++i) {
                float t = 2.0f * static_cast<float>(M_PI) * 2.0f * i / (m - 1);
                float x = i0.x + static_cast<float>(i), sa = std::sin(t * ra), sb = std::sin(t * rb);
                a[static_cast<size_t>(i)] = ImVec2(x, i0.y + lane * 0.5f - sa * lane * 0.4f);
                b[static_cast<size_t>(i)] = ImVec2(x, i0.y + lane * 1.5f - sb * lane * 0.4f);
                c[static_cast<size_t>(i)] = ImVec2(x, i0.y + lane * 2.5f - sa * sb * lane * 0.45f);
            }
            dl->AddPolyline(a.data(), m, (color & 0x00FFFFFF) | (0x90u << 24), 0, 1.0f);
            dl->AddPolyline(b.data(), m, (opColor(src) & 0x00FFFFFF) | (0x90u << 24), 0, 1.0f);
            dl->AddPolyline(c.data(), m, color, 0, 2.0f);
            char s[48];
            std::snprintf(s, sizeof s, "Op%d", opIndex + 1);
            label(dl, ImVec2(i0.x + 2, i0.y), s, color);
            std::snprintf(s, sizeof s, "x Op%d", src + 1);
            label(dl, ImVec2(i0.x + 2, i0.y + lane), s, opColor(src));
            label(dl, ImVec2(i0.x + 2, i0.y + 2 * lane), "= saida");
            break;
        }
    }
    dl->PopClipRect();
    dl->AddRect(p0, p1, col::kCardBorder, 6.0f);
}

// Edicao rapida na tabela de operadores: o parametro PRINCIPAL de cada efeito
// (com nome e unidade); o resto fica no card EFEITO DO OPERADOR.
inline bool OperatorFxQuickEdit(dx7fm::VoiceParams& v, int opIndex, float width) {
    using namespace opfx_detail;
    auto& op = v.op[opIndex];
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    const ImGuiSliderFlags log = ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic;
    switch (op.fxType) {
        case FxType::Bypass: ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("-"); break;
        case FxType::LowPass: case FxType::BandPass: case FxType::HighPass: case FxType::Notch:
            changed = ImGui::DragFloat("##fxq", &op.fxParam1, 5.0f, 20.0f, 20000.0f, "corte %.0f Hz", log); break;
        case FxType::Saturate: changed = ImGui::DragFloat("##fxq", &op.fxParam2, 0.02f, 0.0f, 16.0f, "drive %.2f", ImGuiSliderFlags_AlwaysClamp); break;
        case FxType::Crush: changed = ImGui::DragFloat("##fxq", &op.fxParam1, 10.0f, 100.0f, 20000.0f, "taxa %.0f Hz", log); break;
        case FxType::FreqShift: changed = ImGui::DragFloat("##fxq", &op.fxParam1, 1.0f, 1.0f, 5000.0f, "+%.0f Hz", log); break;
        case FxType::Comb: changed = ImGui::DragFloat("##fxq", &op.fxParam1, 2.0f, 20.0f, 20000.0f, "afin. %.0f Hz", log); break;
        case FxType::Waveshape:
        case FxType::Fold: changed = ImGui::DragFloat("##fxq", &op.fxParam1, 0.02f, 1.0f, 16.0f, "drive %.2f", ImGuiSliderFlags_AlwaysClamp); break;
        case FxType::RingMod: {
            char prev[16];
            std::snprintf(prev, sizeof prev, op.ringModSource >= 0 ? "x Op%d" : "fonte?", op.ringModSource + 1);
            if (ImGui::BeginCombo("##fxq", prev)) {
                for (int s = 0; s < dx7fm::kNumOps; ++s) {
                    if (s == opIndex) continue;
                    char item[16];
                    std::snprintf(item, sizeof item, "x Op%d", s + 1);
                    if (ImGui::Selectable(item, op.ringModSource == s)) { op.ringModSource = s; changed = true; }
                }
                ImGui::EndCombo();
            }
            break;
        }
    }
    if (op.fxType != FxType::Bypass && ImGui::IsItemHovered()) {
        char sum[48];
        opfx::summary(op, sum, sizeof sum);
        ImGui::SetTooltip("%s: %s\narraste (ctrl+clique digita); todos os ajustes no card EFEITO DO OPERADOR", opfx::info(op.fxType).name, sum);
    }
    return changed;
}

// Escolha do efeito + grafico + knobs. Os knobs do FX EG ficam com quem
// chama (DrawEGKnobs, em main.cpp).
inline bool OperatorFxPanel(dx7fm::VoiceParams& v, int opIndex) {
    using namespace opfx_detail;
    auto& op = v.op[opIndex];
    const ImU32 color = opColor(opIndex);
    bool changed = false;

    // botoes de escolha (os efeitos, na ordem do enum)
    for (int t = 0; t <= static_cast<int>(FxType::Notch); ++t) {
        const FxType type = static_cast<FxType>(t);
        const char* name = opfx::info(type).name;
        if (t > 0) wrapSameLine(ImGui::CalcTextSize(name).x + ImGui::GetStyle().FramePadding.x * 2);
        const bool sel = op.fxType == type;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? (color & 0x00FFFFFF) | (0xB0u << 24) : rgb(40, 44, 56));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (color & 0x00FFFFFF) | (0x70u << 24));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? rgb(14, 16, 22) : col::kText);
        ImGui::PushID(t);
        if (ImGui::SmallButton(name) && !sel) {
            opfx::applyDefaults(op.fxType, type, op.fxParam1, op.fxParam2);
            if (type == FxType::RingMod && (op.ringModSource < 0 || op.ringModSource == opIndex))
                op.ringModSource = opIndex == 0 ? 1 : 0;
            op.fxType = type;
            changed = true;
        }
        ImGui::PopID();
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opfx::info(type).about);
    }
    ImGui::TextDisabled("%s", opfx::info(op.fxType).about);
    // [2026-10-30] modelo do filtro / curva do waveshaper
    if (opfx::hasModel(op.fxType)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Modelo");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("##fxmodel", opfx::filterModelLabel(op.fxModel))) {
            for (int m = 0; m <= static_cast<int>(dx7fm::FilterModel::Ms20); ++m) {
                const auto fm = static_cast<dx7fm::FilterModel>(m);
                if (ImGui::Selectable(opfx::filterModelLabel(fm), fm == op.fxModel) && fm != op.fxModel) { op.fxModel = fm; changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("SVF 12 dB: limpo (o de sempre)\nSVF 24 dB: limpo, corte mais ingreme\nLadder: 24 dB, gordo; auto-oscila com Q no maximo\n"
                              "Diodo: estilo baixo acido, ressonancia estreita\nSallen-Key: 12 dB agressivo, estilo MS-20");
    } else if (op.fxType == FxType::Waveshape) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Curva");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("##fxshape", opfx::waveshapeLabel(op.fxShape))) {
            for (int c = 0; c <= static_cast<int>(dx7fm::WaveshapeCurve::Rectify); ++c) {
                const auto wc = static_cast<dx7fm::WaveshapeCurve>(c);
                if (ImGui::Selectable(opfx::waveshapeLabel(wc), wc == op.fxShape) && wc != op.fxShape) { op.fxShape = wc; changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Suave e Clip: harmonicos impares, com mais drive = mais forte\nSeno: dobra suave\nValvula: assimetrica, entram os pares\n"
                              "Chebyshev 3: seno cheio vira o 3o harmonico\nRetificador: oitava acima");
    }

    // grafico | knobs
    KnobStyle ks;
    ks.color = color;
    const float knobsW = 3 * ks.cellWidth + 2 * ImGui::GetStyle().ItemSpacing.x;
    const float gw = std::max(220.0f, ImGui::GetContentRegionAvail().x - knobsW - ImGui::GetStyle().ItemSpacing.x);
    OperatorFxGraph(v, opIndex, ImVec2(gw, 130.0f));
    ImGui::SameLine();
    ImGui::BeginGroup();
    KnobStyle logk = ks;
    logk.logarithmic = true;
    switch (op.fxType) {
        case FxType::Bypass: ImGui::TextDisabled("nada a ajustar"); break;
        case FxType::LowPass: case FxType::BandPass: case FxType::HighPass: case FxType::Notch:
            changed |= Knob("Corte", &op.fxParam1, 20.0f, 20000.0f, 2000.0f, "%.0f Hz", logk);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("corte com o FX EG no maximo (99); no nivel 0 o corte cai pra 20%%");
            ImGui::SameLine();
            { KnobStyle qk = logk; changed |= Knob("Q", &op.fxParam2, 0.1f, 16.0f, 0.707f, "%.2f", qk); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("ressonancia: 0.7 = sem pico; acima disso, pico no corte");
            break;
        case FxType::RingMod: {
            ImGui::TextUnformatted("Operador fonte");
            bool first = true;
            for (int s = 0; s < dx7fm::kNumOps; ++s) {
                if (s == opIndex) continue;
                if (!first) ImGui::SameLine(0, 4);
                first = false;
                char b[8];
                std::snprintf(b, sizeof b, "%d", s + 1);
                const bool sel = op.ringModSource == s;
                ImGui::PushStyleColor(ImGuiCol_Button, sel ? opColor(s, 200) : opColor(s, 50));
                ImGui::PushStyleColor(ImGuiCol_Text, sel ? rgb(14, 16, 22) : col::kText);
                if (ImGui::SmallButton(b)) { op.ringModSource = s; changed = true; }
                ImGui::PopStyleColor(2);
            }
            ImGui::TextDisabled("(usa a saida da amostra\nanterior do operador fonte)");
            break;
        }
        case FxType::Saturate:
            changed |= Knob("Drive", &op.fxParam2, 0.0f, 16.0f, 1.0f, "%.2f", ks);
            break;
        case FxType::Crush: {
            changed |= Knob("Taxa", &op.fxParam1, 100.0f, 20000.0f, 8000.0f, "%.0f Hz", logk);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("taxa de amostragem reduzida (sample & hold)");
            ImGui::SameLine();
            int bits = static_cast<int>(std::lround(std::clamp(op.fxParam2 * 16.0f, 1.0f, 24.0f)));
            if (KnobInt("Bits", &bits, 1, 24, 8, "%d", ks)) { op.fxParam2 = static_cast<float>(bits) / 16.0f; changed = true; }
            break;
        }
        case FxType::FreqShift:
            changed |= Knob("Desloc.", &op.fxParam1, 1.0f, 5000.0f, 50.0f, "+%.0f Hz", logk);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hz somados a todas as frequencias (nao e transposicao:\nos intervalos entre harmonicos mudam)");
            break;
        case FxType::Comb:
            changed |= Knob("Afinacao", &op.fxParam1, 20.0f, 20000.0f, 440.0f, "%.0f Hz", logk);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("frequencia de ressonancia (atraso = 1 / afinacao)");
            ImGui::SameLine();
            { float fb = std::min(op.fxParam2, 0.99f);
              if (Knob("Feedback", &fb, 0.0f, 0.99f, 0.7f, "%.2f", ks)) { op.fxParam2 = fb; changed = true; } }
            break;
        case FxType::Waveshape:
            changed |= Knob("Drive", &op.fxParam1, 1.0f, 16.0f, 2.0f, "%.2f", ks);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("drive com o FX EG no maximo; no nivel 0 cai pra 20%% do excesso");
            ImGui::SameLine();
            changed |= Knob("Bias", &op.fxParam2, 0.0f, 1.0f, 0.0f, "%.2f", ks);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("desloca o sinal na curva: entram harmonicos pares");
            break;
        case FxType::Fold:
            changed |= Knob("Drive", &op.fxParam1, 1.0f, 16.0f, 3.0f, "%.2f", ks);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("drive com o FX EG no maximo; no nivel 0 cai pra 20%% do excesso");
            ImGui::SameLine();
            changed |= Knob("Simetria", &op.fxParam2, 0.0f, 1.0f, 0.0f, "%.2f", ks);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = so harmonicos impares; acima disso entram os pares");
            break;
    }
    if (opfx::keyTracks(op.fxType)) {
        ImGui::SameLine();
        float pct = op.fxKeyTrack * 100.0f;
        if (Knob("Key track", &pct, 0.0f, 100.0f, 0.0f, "%.0f%%", ks)) { op.fxKeyTrack = pct / 100.0f; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("O valor em Hz acompanha a nota, em volta do C4:\n"
                              "100%% = uma oitava acima dobra (o comb/filtro fica afinado com a nota),\n"
                              "50%% = meia oitava por oitava, 0%% = Hz fixo.\nSegue o glide; nao segue pitch bend nem vibrato.");
    }
    ImGui::EndGroup();
    return changed;
}

} // namespace ui
