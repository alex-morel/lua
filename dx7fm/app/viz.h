#pragma once
#include "audio_tap.h"
#include "spectrum.h"
#include "spectrogram_history.h"
#include "ui_widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

// [ADICIONADO em 2026-10-18] Visualizadores da saida de audio: osciloscopio
// e espectrograma, alimentados pelo AudioTap (audio_tap.h). Desenhados com
// primitivas do ImGui (sem textura), entao funcionam com qualquer backend.
namespace ui {

// --- osciloscopio ------------------------------------------------------------
struct ScopeState {
    int zoom = 2;          // indice em kScopeWindowsMs
    bool trigger = true;   // alinha na subida do zero (onda parada na tela)
    bool autoGain = true;  // amplia sinais baixos
    float gain = 1.0f;     // ganho atual (suavizado)
    std::vector<float> buf;
};
inline const float kScopeWindowsMs[] = {5.0f, 10.0f, 20.0f, 50.0f};

inline void Oscilloscope(const char* id, const dx7fm_app::AudioTap& tap, double sr, ScopeState& st, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const float midY = (p0.y + p1.y) * 0.5f;

    const int n = std::max(16, static_cast<int>(kScopeWindowsMs[st.zoom] * 0.001 * sr));
    const int search = std::max(n, static_cast<int>(0.05 * sr)); // procura disparo em ate 50ms (periodo de ~20Hz)
    st.buf.resize(static_cast<size_t>(n + search));
    tap.readMono(tap.written(), n + search, st.buf.data());

    // disparo: a subida do zero mais RECENTE que ainda deixe n amostras depois dela
    int start = search;
    if (st.trigger) {
        for (int i = search - 1; i >= 1; --i) {
            if (st.buf[static_cast<size_t>(i - 1)] < 0.0f && st.buf[static_cast<size_t>(i)] >= 0.0f) { start = i; break; }
        }
    }
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(st.buf[static_cast<size_t>(start + i)]));
    float target = st.autoGain ? std::clamp(0.85f / std::max(peak, 0.02f), 1.0f, 40.0f) : 1.0f;
    st.gain += (target - st.gain) * (target < st.gain ? 0.5f : 0.08f); // abaixa rapido, sobe devagar

    dl->AddRectFilled(p0, p1, rgb(14, 16, 22), 6.0f);
    for (int k = 1; k < 4; ++k) {
        float x = p0.x + size.x * k / 4.0f;
        dl->AddLine(ImVec2(x, p0.y + 4), ImVec2(x, p1.y - 4), col::kGrid);
    }
    dl->AddLine(ImVec2(p0.x + 4, midY), ImVec2(p1.x - 4, midY), rgb(255, 255, 255, 30));

    // forma de onda: traco largo e translucido ("brilho") + traco fino
    std::vector<ImVec2> pts(static_cast<size_t>(n));
    const float halfH = size.y * 0.5f - 6.0f;
    for (int i = 0; i < n; ++i) {
        float v = std::clamp(st.buf[static_cast<size_t>(start + i)] * st.gain, -1.0f, 1.0f);
        pts[static_cast<size_t>(i)] = ImVec2(p0.x + 4 + (size.x - 8) * i / (n - 1), midY - v * halfH);
    }
    dl->PushClipRect(p0, p1, true);
    dl->AddPolyline(pts.data(), n, ui::opColor(0, 60), 0, 4.0f);
    dl->AddPolyline(pts.data(), n, col::kAccent, 0, 1.5f);
    dl->PopClipRect();

    char info[48];
    std::snprintf(info, sizeof info, "%.0f ms", kScopeWindowsMs[st.zoom]);
    dl->AddText(ImVec2(p0.x + 6, p0.y + 3), col::kTextDim, info);
    if (st.autoGain && st.gain > 1.05f) {
        std::snprintf(info, sizeof info, "x%.1f", st.gain);
        dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(info).x - 6, p0.y + 3), col::kTextDim, info);
    }
    if (peak <= 1e-5f) {
        const char* msg = "sem sinal";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, midY - ts.y - 4), col::kTextDim, msg);
    }
}

// --- espectrograma -------------------------------------------------------------
// Dados em dx7fm_app::SpectrogramHistory (spectrogram_history.h); aqui so a
// faixa dinamica escolhida na UI e o desenho.
struct SpectrogramState {
    dx7fm_app::SpectrogramHistory data;
    int range = 1; // indice em kSpectroRangesDb
};
inline const float kSpectroRangesDb[] = {-60.0f, -90.0f, -120.0f};

inline void UpdateSpectrogram(SpectrogramState& st, const dx7fm_app::AudioTap& tap, double sr) { st.data.update(tap, sr); }

inline void Spectrogram(const char* id, SpectrogramState& st, double sr, ImVec2 size) {
    using S = dx7fm_app::SpectrogramHistory;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const float dbMin = kSpectroRangesDb[st.range];
    const float cw = size.x / S::kCols, bh = size.y / S::kBands;

    dl->AddRectFilled(p0, p1, rgb(12, 14, 20), 6.0f);
    dl->PushClipRect(p0, p1, true);
    // cada celula com gradiente vertical entre a faixa e a de cima: sem "escada"
    for (int c = 0; c < S::kCols; ++c) {
        const float* col = st.data.column(c);
        float x0 = p0.x + c * cw, x1 = x0 + cw + 0.5f;
        for (int b = 0; b < S::kBands; ++b) {
            float lo = col[b], hi = col[std::min(b + 1, S::kBands - 1)];
            if (lo <= dbMin && hi <= dbMin) continue; // abaixo do piso: fica o fundo
            uint8_t r0, g0, b0, r1, g1, b1;
            dx7fm_app::spectrogramColor(lo, dbMin, r0, g0, b0);
            dx7fm_app::spectrogramColor(hi, dbMin, r1, g1, b1);
            float yb = p1.y - b * bh, yt = yb - bh - 0.5f;
            ImU32 cb = IM_COL32(r0, g0, b0, 255), ct = IM_COL32(r1, g1, b1, 255);
            dl->AddRectFilledMultiColor(ImVec2(x0, yt), ImVec2(x1, yb), ct, ct, cb, cb);
        }
    }
    // marcas de frequencia
    const double ratio = std::log(S::kFMax / S::kFMin);
    for (float f : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
        float y = p1.y - size.y * static_cast<float>(std::log(f / S::kFMin) / ratio);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x + 6, y), rgb(255, 255, 255, 90));
        char lbl[24];
        if (f >= 1000.0f) std::snprintf(lbl, sizeof lbl, "%gk", f / 1000.0f);
        else std::snprintf(lbl, sizeof lbl, "%g", f);
        dl->AddText(ImVec2(p0.x + 8, y - ImGui::GetTextLineHeight() * 0.5f), rgb(255, 255, 255, 110), lbl);
    }
    dl->PopClipRect();

    if (hovered) {
        ImVec2 m = ImGui::GetIO().MousePos;
        int c = std::clamp(static_cast<int>((m.x - p0.x) / cw), 0, S::kCols - 1);
        float yN = std::clamp((p1.y - m.y) / size.y, 0.0f, 1.0f);
        float hz = S::kFMin * static_cast<float>(std::exp(ratio * yN));
        int b = std::clamp(static_cast<int>(yN * S::kBands), 0, S::kBands - 1);
        float db = st.data.column(c)[b];
        float agoSec = static_cast<float>((S::kCols - 1 - c) * S::kHop / sr);
        char note[16];
        dx7fm_app::noteName(hz, note, sizeof note);
        ImGui::SetTooltip("%.0f Hz (%s)\n%.0f dB\nha %.2f s", hz, note, std::max(db, -140.0f), agoSec);
    }
}

} // namespace ui
