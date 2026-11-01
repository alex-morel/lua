#pragma once
#include "additive_editor.h"
#include "dx7_wavegen.h"
#include "dx7_wavetable.h"
#include "file_browser.h"
#include "spectrum.h"
#include "wav_reader.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// [ADICIONADO em 2026-10-22] Editor da Waveform::Wavetable: as ondas A e B
// (mini-graficos; clique escolhe qual editar), a onda na posicao do morph,
// knobs de posicao / profundidade do FX EG, modo espectral ou crossfade, e 4
// formas de criar a onda em edicao: formas prontas, desenho a mao,
// harmonicos e importacao de WAV. Toda edicao troca o shared_ptr da onda por
// um vetor NOVO (as ondas sao imutaveis: o audio pode estar lendo a antiga).
namespace ui {

struct WavetableEditorState {
    int editB = 0;
    int requestTab = -1; // 0..3: abre essa aba no proximo quadro (testes/atalhos)
    float pulseWidth = 0.25f;
    // desenho
    int lastI = -1;
    float lastV = 0.0f;
    // harmonicos
    dx7fm_app::PartialSet harm;
    AdditiveEditorState harmUi;
    const void* harmFrom = nullptr; // onda de onde os harmonicos foram lidos
    // WAV
    WavBrowser browser;
    dx7fm::WavData wav;
    std::string loadedPath, status;
    int frame = 0;
    float atSec = 0.0f, f0 = 0.0f;
    std::vector<float> candidate; // onda pronta pra "Usar"
    bool candidateDirty = false;
    // previa da posicao do morph (banco proprio da UI, so remontado quando muda)
    std::shared_ptr<const dx7fm::WavetableBank> preview;
    std::shared_ptr<const std::vector<float>> previewA, previewB;
    bool previewSpectral = true;
};

namespace wavetable_detail {

using Wave = dx7fm::wavegen::Wave;
using WavePtr = std::shared_ptr<const std::vector<float>>;

inline Wave waveOrSine(const WavePtr& w) { return w ? *w : dx7fm::wavegen::fromShape(dx7fm::Waveform::Sine); }

// um ciclo desenhado em [p0, p0+size]; `n` amostras
inline void plot(ImDrawList* dl, ImVec2 p0, ImVec2 size, const float* data, int n, ImU32 color, float thick = 1.5f) {
    const int m = std::max(2, static_cast<int>(size.x));
    std::vector<ImVec2> pts(static_cast<size_t>(m));
    const float mid = p0.y + size.y * 0.5f, hh = size.y * 0.5f - 3.0f;
    for (int i = 0; i < m; ++i) {
        int j = std::min(n - 1, i * n / m);
        pts[static_cast<size_t>(i)] = ImVec2(p0.x + static_cast<float>(i), mid - std::clamp(data[j], -1.2f, 1.2f) * hh);
    }
    dl->AddPolyline(pts.data(), m, color, 0, thick);
}

inline void frame(ImDrawList* dl, ImVec2 p0, ImVec2 size, bool selected, ImU32 color) {
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, rgb(14, 16, 22), 6.0f);
    dl->AddLine(ImVec2(p0.x + 3, p0.y + size.y * 0.5f), ImVec2(p1.x - 3, p0.y + size.y * 0.5f), col::kGrid);
    dl->AddRect(p0, p1, selected ? color : col::kCardBorder, 6.0f, 0, selected ? 2.0f : 1.0f);
}

// mini-grafico clicavel de A ou B
inline bool waveButton(const char* id, const char* label, const WavePtr& w, bool selected, ImU32 color, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    frame(dl, p0, size, selected, color);
    Wave v = waveOrSine(w);
    plot(dl, p0, size, v.data(), static_cast<int>(v.size()), selected ? color : rgb(160, 168, 184));
    dl->AddText(ImVec2(p0.x + 5, p0.y + 2), selected ? color : col::kTextDim, label);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("clique pra editar a onda %s", label);
    return clicked;
}

// canvas de desenho: botao esquerdo pinta (preenche entre quadros)
inline bool drawCanvas(WavetableEditorState& st, WavePtr& target, ImU32 color, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##draw", size);
    const bool active = ImGui::IsItemActive();
    frame(dl, p0, size, false, color);
    Wave w = waveOrSine(target);
    bool changed = false;
    if (active) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int n = static_cast<int>(w.size());
        const int i = std::clamp(static_cast<int>((m.x - p0.x) / size.x * n), 0, n - 1);
        const float v = std::clamp(1.0f - 2.0f * (m.y - p0.y - 3.0f) / (size.y - 6.0f), -1.0f, 1.0f);
        const int from = st.lastI < 0 ? i : st.lastI;
        const float fromV = st.lastI < 0 ? v : st.lastV;
        const int step = i >= from ? 1 : -1;
        for (int k = from; ; k += step) { // todas as amostras entre o quadro anterior e este
            float t = i == from ? 1.0f : static_cast<float>(k - from) / static_cast<float>(i - from);
            w[static_cast<size_t>(k)] = fromV + (v - fromV) * t;
            if (k == i) break;
        }
        st.lastI = i;
        st.lastV = v;
        target = dx7fm::wavegen::share(w);
        changed = true;
    } else {
        st.lastI = -1;
    }
    plot(dl, p0, size, w.data(), static_cast<int>(w.size()), color, 2.0f);
    if (ImGui::IsItemHovered() && !active) ImGui::SetTooltip("clique e arraste pra desenhar a onda");
    return changed;
}

inline void loadWav(WavetableEditorState& st, const std::string& path) {
    st.wav = dx7fm::readWavMono(path, 60.0);
    st.loadedPath = path;
    st.frame = 0;
    st.status = st.wav.error.empty() ? "" : "Erro: " + st.wav.error;
    st.atSec = static_cast<float>(std::min(0.3, st.wav.seconds() * 0.5));
    st.candidateDirty = true;
    if (st.wav.error.empty()) st.browser.followFile(path);
}

inline void updateCandidate(WavetableEditorState& st) {
    st.candidateDirty = false;
    st.candidate.clear();
    st.f0 = 0.0f;
    if (!st.wav.error.empty() || st.wav.mono.empty()) return;
    if (dx7fm::wavegen::wavetableFrames(st.wav.mono) > 0) {
        st.candidate = dx7fm::wavegen::fromWavFrame(st.wav.mono, st.frame);
        dx7fm::wavegen::removeDc(st.candidate);
        dx7fm::wavegen::normalize(st.candidate);
    } else {
        st.candidate = dx7fm::wavegen::fromWavPeriod(st.wav.mono, st.wav.sampleRate, st.atSec, &st.f0);
    }
}

} // namespace wavetable_detail

inline bool WavetableEditor(dx7fm::OperatorParams& op, ImU32 color, WavetableEditorState& st) {
    using namespace wavetable_detail;
    namespace wg = dx7fm::wavegen;
    auto& wt = op.wavetable;
    bool changed = false;
    ImGui::SeparatorText("Wavetable: 2 ondas com morph");

    // --- A, B, previa do morph e knobs ---
    const ImVec2 mini(120, 64);
    if (waveButton("##wa", "A", wt.waveA, st.editB == 0, color, mini)) st.editB = 0;
    ImGui::SameLine();
    if (waveButton("##wb", "B", wt.waveB, st.editB == 1, color, mini)) st.editB = 1;
    ImGui::SameLine();
    {
        if (!st.preview || st.previewA != wt.waveA || st.previewB != wt.waveB || st.previewSpectral != wt.spectralMorph) {
            st.preview = std::make_shared<const dx7fm::WavetableBank>(wt.waveA.get(), wt.waveB.get(), wt.spectralMorph);
            st.previewA = wt.waveA;
            st.previewB = wt.waveB;
            st.previewSpectral = wt.spectralMorph;
        }
        KnobStyle ks;
        ks.color = color;
        const float knobs = 2 * ks.cellWidth + 120.0f + 3 * ImGui::GetStyle().ItemSpacing.x;
        const ImVec2 big(std::max(140.0f, ImGui::GetContentRegionAvail().x - knobs), mini.y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(big);
        frame(dl, p0, big, false, color);
        float buf[512];
        st.preview->preview(wt.position, buf, 512);
        plot(dl, p0, big, buf, 512, color, 2.0f);
        char lbl[48];
        std::snprintf(lbl, sizeof lbl, "morph %.0f%%", wt.position * 100.0f);
        dl->AddText(ImVec2(p0.x + 5, p0.y + 2), col::kTextDim, lbl);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("onda na posição do knob (sem a modulação do FX EG e da matriz)");
        ImGui::SameLine();
        float pct = wt.position * 100.0f;
        if (Knob("Posição", &pct, 0.0f, 100.0f, 0.0f, "%.0f%%", ks)) { wt.position = pct / 100.0f; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0%% = onda A, 100%% = onda B");
        ImGui::SameLine();
        changed |= Knob("Morph EG", &op.morphDepth, 0.0f, 1.0f, 0.0f, "%.2f", ks);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("quanto o FX EG empurra o morph na direção de B (soma com a posição).\n"
                              "Na mod matrix, o destino OperatorMorph também move o morph (LFO, nota, velocity...).");
        ImGui::SameLine();
        ImGui::BeginGroup();
        int mode = wt.spectralMorph ? 0 : 1;
        if (ImGui::RadioButton("Espectral", &mode, 0)) { wt.spectralMorph = true; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("interpola os harmônicos: o meio do caminho e uma onda intermediaria,\nsem cancelamento (ondas de fase oposta não somem)");
        if (ImGui::RadioButton("Crossfade", &mode, 1)) { wt.spectralMorph = false; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("mistura as amostras: (1-m)*A + m*B");
        ImGui::EndGroup();
    }

    WavePtr& target = st.editB ? wt.waveB : wt.waveA;
    if (ImGui::SmallButton("Copiar A -> B")) { wt.waveB = wt.waveA; changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Trocar A <-> B")) { std::swap(wt.waveA, wt.waveB); changed = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("editando a onda %s", st.editB ? "B" : "A");

    // --- formas de criar a onda em edicao ---
    auto tabFlag = [&](int i) { return st.requestTab == i ? ImGuiTabItemFlags_SetSelected : 0; };
    if (ImGui::BeginTabBar("##wtsrc")) {
        if (ImGui::BeginTabItem("Formas", nullptr, tabFlag(0))) {
            struct Shape { const char* name; dx7fm::Waveform w; };
            static const Shape kShapes[] = {
                {"Seno", dx7fm::Waveform::Sine}, {"Triângulo", dx7fm::Waveform::Triangle}, {"Serra", dx7fm::Waveform::Saw},
                {"Quadrada", dx7fm::Waveform::Square}, {"Ruido", dx7fm::Waveform::Noise}, {"Meio seno", dx7fm::Waveform::HalfSine},
                {"Seno abs", dx7fm::Waveform::AbsSine}, {"Quarto de seno", dx7fm::Waveform::QuarterSine},
                {"Seno alternado", dx7fm::Waveform::AlternatingSine}, {"Camelo", dx7fm::Waveform::CamelSine},
                {"Seno ao quadrado", dx7fm::Waveform::SquaredSine}, {"LogSaw", dx7fm::Waveform::LogSaw}};
            for (size_t i = 0; i < std::size(kShapes); ++i) {
                if (i > 0) additive_detail::wrapSameLine(ImGui::CalcTextSize(kShapes[i].name).x + ImGui::GetStyle().FramePadding.x * 2);
                if (ImGui::SmallButton(kShapes[i].name)) {
                    Wave w = wg::fromShape(kShapes[i].w, static_cast<unsigned>(ImGui::GetFrameCount()));
                    wg::removeDc(w);
                    wg::normalize(w);
                    target = wg::share(std::move(w));
                    changed = true;
                }
            }
            if (ImGui::SmallButton("Pulso")) { target = wg::share(wg::pulse(st.pulseWidth)); changed = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            if (ImGui::SliderFloat("largura##pw", &st.pulseWidth, 0.02f, 0.98f, "%.2f")) { target = wg::share(wg::pulse(st.pulseWidth)); changed = true; }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Desenhar", nullptr, tabFlag(1))) {
            changed |= drawCanvas(st, target, color, ImVec2(ImGui::GetContentRegionAvail().x, 150.0f));
            auto apply = [&](void (*fn)(Wave&)) { Wave w = waveOrSine(target); fn(w); target = wg::share(std::move(w)); changed = true; };
            if (ImGui::SmallButton("Suavizar")) apply([](Wave& w) { wg::smooth(w, 2); });
            ImGui::SameLine();
            if (ImGui::SmallButton("Normalizar")) apply([](Wave& w) { wg::normalize(w); });
            ImGui::SameLine();
            if (ImGui::SmallButton("Inverter")) apply([](Wave& w) { wg::invert(w); });
            ImGui::SameLine();
            if (ImGui::SmallButton("Remover DC")) apply([](Wave& w) { wg::removeDc(w); });
            ImGui::SameLine();
            ImGui::TextDisabled("(cantos vivos geram harmônicos agudos; o motor corta o que passaria de Nyquist)");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Harmônicos", nullptr, tabFlag(2))) {
            // le os harmonicos da onda atual quando ela muda por fora deste editor
            if (st.harmFrom != target.get()) {
                Wave w = waveOrSine(target);
                float maxA = 0.0f;
                for (int h = 1; h <= dx7fm::kNumPartials; ++h) {
                    double re = 0.0, im = 0.0;
                    for (size_t i = 0; i < w.size(); ++i) {
                        float ph = static_cast<float>((static_cast<size_t>(h) * i) % w.size()) / static_cast<float>(w.size());
                        re += w[i] * dx7fm::fastSin01(ph + 0.25f - std::floor(ph + 0.25f));
                        im += w[i] * dx7fm::fastSin01(ph);
                    }
                    st.harm.amp[h - 1] = static_cast<float>(std::hypot(re, im));
                    st.harm.ratio[h - 1] = static_cast<float>(h);
                    maxA = std::max(maxA, st.harm.amp[h - 1]);
                }
                for (auto& a : st.harm.amp) a = maxA > 0.0f ? (a / maxA < 1e-4f ? 0.0f : a / maxA) : 0.0f;
                st.harmFrom = target.get();
            }
            if (st.harmUi.view == 2) st.harmUi.view = 0; // razoes fixas (harmonicos inteiros): sem a vista de razao
            ImGui::RadioButton("Amplitude", &st.harmUi.view, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Amplitude dB", &st.harmUi.view, 1);
            ImGui::SameLine();
            ImGui::TextDisabled("harmônicos 1..%d (fase zero); a onda e refeita a cada mudanca", dx7fm::kNumPartials);
            bool edited = PartialBars("##harm", st.harm, st.harm, st.harmUi, color, ImVec2(ImGui::GetContentRegionAvail().x, 130.0f));
            ImGui::TextUnformatted("Preset:");
            for (const auto& p : dx7fm_app::kAdditivePresets) {
                std::string n = p.name;
                if (n == "Órgão" || n == "Sino" || n == "Tambor") continue; // razoes nao inteiras: nao cabem num ciclo
                ImGui::SameLine();
                if (ImGui::SmallButton(p.name)) { p.make(st.harm); edited = true; }
            }
            if (edited) {
                target = wg::share(wg::fromHarmonics(st.harm.amp, dx7fm::kNumPartials));
                st.harmFrom = target.get();
                changed = true;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("WAV", nullptr, tabFlag(3))) {
            std::string picked = st.browser.drawList("##wtbrowser", ImVec2(300, 190), st.loadedPath);
            if (!picked.empty()) loadWav(st, picked);
            ImGui::SameLine();
            ImGui::BeginGroup();
            std::string typed = st.browser.drawPathInput("wtpath");
            if (!typed.empty()) loadWav(st, typed);
            if (!st.status.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kAccent2), "%s", st.status.c_str());
            const bool loaded = st.wav.error.empty() && !st.wav.mono.empty();
            if (!loaded) {
                ImGui::TextDisabled("Um .wav de wavetable (Serum/Vital: frames de 2048 amostras) -> escolha o frame.\n"
                                    "Qualquer outro som -> um período dele, no instante escolhido.");
            } else {
                if (st.candidateDirty) updateCandidate(st);
                const int frames = wg::wavetableFrames(st.wav.mono);
                ImGui::Text("%s  |  %.2f s%s", std::filesystem::path(st.loadedPath).filename().string().c_str(), st.wav.seconds(),
                            frames ? "" : " (som comum)");
                if (frames > 0) {
                    int f = st.frame + 1;
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::SliderInt("frame", &f, 1, frames, "%d", ImGuiSliderFlags_AlwaysClamp)) { st.frame = f - 1; st.candidateDirty = true; }
                    ImGui::SameLine();
                    ImGui::TextDisabled("de %d", frames);
                } else {
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::SliderFloat("instante", &st.atSec, 0.0f, static_cast<float>(st.wav.seconds()), "%.3f s")) st.candidateDirty = true;
                    char note[16];
                    dx7fm_app::noteName(st.f0, note, sizeof note);
                    ImGui::SameLine();
                    if (st.f0 > 0.0f) ImGui::TextDisabled("altura %.1f Hz (%s)", st.f0, note);
                    else ImGui::TextDisabled("sem altura definida aqui");
                }
                const ImVec2 sz(std::max(160.0f, ImGui::GetContentRegionAvail().x), 80.0f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::Dummy(sz);
                frame(dl, p0, sz, false, color);
                if (!st.candidate.empty()) plot(dl, p0, sz, st.candidate.data(), static_cast<int>(st.candidate.size()), color, 2.0f);
                ImGui::BeginDisabled(st.candidate.empty());
                char use[32];
                std::snprintf(use, sizeof use, "Usar na onda %s", st.editB ? "B" : "A");
                if (ImGui::Button(use)) { target = wg::share(st.candidate); changed = true; }
                ImGui::EndDisabled();
            }
            ImGui::EndGroup();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    st.requestTab = -1;
    return changed;
}

} // namespace ui
