#pragma once
#include "dx7_resynth.h"
#include "spectrum.h"
#include "ui_widgets.h"
#include "wav_reader.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// [ADICIONADO em 2026-10-20] Dialogo de ressintese (analise em
// src/dx7_resynth.h): navegador de arquivos .wav, forma de onda com os
// instantes A (ataque) e B (sustain) arrastaveis, espectro dos parciais
// achados, opcoes e "Aplicar no operador".
namespace ui {

struct ResynthUiState {
    bool requestOpen = false;
    // navegador
    std::string dir;
    struct Entry { std::string name; bool isDir; };
    std::vector<Entry> entries;
    bool listed = false;
    char path[1024] = "";
    // arquivo carregado
    dx7fm::WavData wav;
    std::string loadedPath, status;
    // analise
    dx7fm::resynth::Options opt;
    dx7fm::resynth::ApplyOptions apply;
    double aSec = 0.0, bSec = 0.0;
    bool manualRef = false;
    float refHz = 440.0f;
    dx7fm::resynth::Result result;
    bool dirty = false;
    int dragging = 0; // 1 = marcador A, 2 = marcador B
    // resumo da forma de onda (min/max por coluna de pixel)
    std::vector<float> ovMin, ovMax;
};

namespace resynth_detail {

inline bool endsWithWav(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".wav";
}

inline void listDir(ResynthUiState& st) {
    namespace fs = std::filesystem;
    st.entries.clear();
    std::error_code ec;
    for (fs::directory_iterator it(st.dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code ec2;
        bool isDir = it->is_directory(ec2);
        if (isDir || endsWithWav(name)) st.entries.push_back({name, isDir});
    }
    std::sort(st.entries.begin(), st.entries.end(), [](const ResynthUiState::Entry& a, const ResynthUiState::Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        return lower(a.name) < lower(b.name);
    });
    st.listed = true;
}

inline void load(ResynthUiState& st, const std::string& path) {
    st.wav = dx7fm::readWavMono(path);
    st.loadedPath = path;
    st.ovMin.clear();
    if (!st.wav.error.empty()) { st.status = "Erro: " + st.wav.error; return; }
    st.status.clear();
    if (st.wav.truncated) st.status = "Arquivo longo: so os primeiros 5 minutos foram carregados.";
    auto env = dx7fm::resynth::analyzeEnvelope(st.wav.mono.data(), st.wav.mono.size(), st.wav.sampleRate);
    dx7fm::resynth::defaultTimes(env, st.wav.seconds(), st.opt, st.wav.sampleRate, st.aSec, st.bSec);
    st.manualRef = false;
    st.dirty = true;
}

inline void analyze(ResynthUiState& st) {
    st.result = dx7fm::resynth::analyze(st.wav.mono.data(), st.wav.mono.size(), st.wav.sampleRate, st.aSec, st.bSec, st.opt,
                                        st.manualRef ? st.refHz : 0.0f);
    if (!st.manualRef) st.refHz = st.result.refHz;
    st.dirty = false;
}

// Forma de onda (min/max por coluna) com os instantes A e B arrastaveis; a
// faixa sombreada em volta de cada marcador e o quadro que a FFT analisa.
inline void Waveform(ResynthUiState& st, ImU32 colorA, ImVec2 size) {
    const auto& x = st.wav.mono;
    const double dur = st.wav.seconds();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wave", size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const int w = std::max(1, static_cast<int>(size.x));
    if (static_cast<int>(st.ovMin.size()) != w) {
        st.ovMin.assign(static_cast<size_t>(w), 0.0f);
        st.ovMax.assign(static_cast<size_t>(w), 0.0f);
        for (int c = 0; c < w; ++c) {
            size_t i0 = x.size() * static_cast<size_t>(c) / static_cast<size_t>(w), i1 = x.size() * static_cast<size_t>(c + 1) / static_cast<size_t>(w);
            float lo = 0.0f, hi = 0.0f;
            for (size_t i = i0; i < std::max(i1, i0 + 1) && i < x.size(); ++i) { lo = std::min(lo, x[i]); hi = std::max(hi, x[i]); }
            st.ovMin[static_cast<size_t>(c)] = lo;
            st.ovMax[static_cast<size_t>(c)] = hi;
        }
    }
    auto xOf = [&](double t) { return p0.x + static_cast<float>(t / dur) * size.x; };
    auto tOf = [&](float px) { return std::clamp(static_cast<double>((px - p0.x) / size.x) * dur, 0.0, dur); };

    // arrastar o marcador mais perto do clique
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (ImGui::IsItemActivated()) st.dragging = std::fabs(m.x - xOf(st.aSec)) <= std::fabs(m.x - xOf(st.bSec)) ? 1 : 2;
    if (!ImGui::IsItemActive()) st.dragging = 0;
    if (st.dragging) {
        double t = tOf(m.x);
        if (st.dragging == 1) { st.aSec = t; st.bSec = std::max(st.bSec, t); }
        else { st.bSec = t; st.aSec = std::min(st.aSec, t); }
        st.dirty = true;
    }

    dl->AddRectFilled(p0, p1, rgb(14, 16, 22), 6.0f);
    const float mid = (p0.y + p1.y) * 0.5f, hh = size.y * 0.5f - 4.0f;
    float peak = 1e-6f;
    for (int c = 0; c < w; ++c) peak = std::max({peak, -st.ovMin[static_cast<size_t>(c)], st.ovMax[static_cast<size_t>(c)]});
    const float g = 1.0f / peak; // normaliza a altura
    for (int c = 0; c < w; ++c)
        dl->AddLine(ImVec2(p0.x + c + 0.5f, mid - st.ovMax[static_cast<size_t>(c)] * g * hh),
                    ImVec2(p0.x + c + 0.5f, mid - st.ovMin[static_cast<size_t>(c)] * g * hh + 1.0f), rgb(120, 130, 150, 200));
    const double halfWin = 0.5 * st.opt.windowSize / st.wav.sampleRate;
    const ImU32 colorB = col::kAccent2;
    for (int k = 0; k < 2; ++k) {
        double t = k == 0 ? st.aSec : st.bSec;
        ImU32 c = k == 0 ? colorA : colorB;
        dl->AddRectFilled(ImVec2(std::max(p0.x, xOf(t - halfWin)), p0.y), ImVec2(std::min(p1.x, xOf(t + halfWin)), p1.y), (c & 0x00FFFFFF) | (0x30u << 24));
        dl->AddLine(ImVec2(xOf(t), p0.y), ImVec2(xOf(t), p1.y), c, 2.0f);
        dl->AddText(ImVec2(xOf(t) + 4, p0.y + 2), c, k == 0 ? "A" : "B");
    }
    if (st.result.ok || !st.result.env.db.empty()) {
        float xo = xOf(st.result.onsetSec);
        for (float y = p0.y; y < p1.y; y += 6) dl->AddLine(ImVec2(xo, y), ImVec2(xo, y + 3), rgb(255, 255, 255, 90));
    }
    dl->AddRect(p0, p1, col::kCardBorder, 6.0f);
    if (ImGui::IsItemHovered() && !st.dragging)
        ImGui::SetTooltip("%.3f s\narraste perto de A ou B pra mover o instante analisado", tOf(m.x));
}

// Parciais achados em A e B (frequencia em escala log x amplitude em dB)
inline void PeakPlot(const ResynthUiState& st, ImU32 colorA, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const float fMin = 20.0f, fMax = std::min(20000.0f, 0.5f * static_cast<float>(st.wav.sampleRate));
    auto xOf = [&](float hz) { return p0.x + size.x * std::log(std::max(hz, fMin) / fMin) / std::log(fMax / fMin); };
    auto yOf = [&](float amp) { return p1.y - 4 - (size.y - 8) * std::clamp(1.0f + 20.0f * std::log10(std::max(amp, 1e-6f)) / 80.0f, 0.0f, 1.0f); };
    dl->AddRectFilled(p0, p1, rgb(14, 16, 22), 6.0f);
    for (float f : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
        if (f >= fMax) break;
        dl->AddLine(ImVec2(xOf(f), p0.y), ImVec2(xOf(f), p1.y), col::kGrid);
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, f >= 1000.0f ? "%gk" : "%g", f >= 1000.0f ? f / 1000.0f : f);
        dl->AddText(ImVec2(xOf(f) + 2, p0.y + 1), rgb(255, 255, 255, 80), lbl);
    }
    if (st.result.refHz > 0.0f) {
        float xr = xOf(st.result.refHz);
        for (float y = p0.y; y < p1.y; y += 6) dl->AddLine(ImVec2(xr, y), ImVec2(xr, y + 3), rgb(255, 255, 255, 120));
    }
    auto stems = [&](const std::vector<dx7fm::resynth::Peak>& pk, ImU32 c, float dx) {
        float maxA = 1e-9f;
        for (const auto& p : pk) maxA = std::max(maxA, p.amp);
        for (const auto& p : pk) dl->AddLine(ImVec2(xOf(p.hz) + dx, p1.y - 4), ImVec2(xOf(p.hz) + dx, yOf(p.amp / maxA)), c, 2.0f);
    };
    stems(st.result.peaksA, colorA, -1.0f);
    stems(st.result.peaksB, col::kAccent2, 1.0f);
    dl->AddRect(p0, p1, col::kCardBorder, 6.0f);
}

} // namespace resynth_detail

// Chamar todo quadro (dentro do editor da aditiva); abre com st.requestOpen.
// Devolve true quando aplicou no operador.
inline bool ResynthDialog(dx7fm::OperatorParams& op, int opIndex, ResynthUiState& st, ImU32 color) {
    using namespace resynth_detail;
    namespace fs = std::filesystem;
    if (st.requestOpen) {
        ImGui::OpenPopup("Ressintese de WAV");
        st.requestOpen = false;
        if (st.dir.empty()) {
            const char* home = std::getenv("HOME");
            st.dir = home ? home : ".";
        }
        st.listed = false;
    }
    ImGui::SetNextWindowSize(ImVec2(1000, 540), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Ressintese de WAV", nullptr, ImGuiWindowFlags_NoSavedSettings)) return false;
    bool applied = false;
    if (!st.listed) listDir(st);

    // --- navegador (coluna esquerda) ---
    ImGui::BeginChild("##browser", ImVec2(300, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    if (ImGui::SmallButton("^ acima")) {
        st.dir = fs::path(st.dir).parent_path().string();
        if (st.dir.empty()) st.dir = "/";
        st.listed = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", st.dir.c_str());
    ImGui::Separator();
    for (const auto& e : st.entries) {
        std::string label = (e.isDir ? "[pasta] " : "") + e.name;
        const std::string full = (fs::path(st.dir) / e.name).string();
        if (ImGui::Selectable(label.c_str(), !e.isDir && full == st.loadedPath)) {
            if (e.isDir) { st.dir = full; st.listed = false; }
            else { std::snprintf(st.path, sizeof st.path, "%s", full.c_str()); load(st, full); }
        }
    }
    if (st.entries.empty()) ImGui::TextDisabled("(nenhuma pasta ou .wav aqui)");
    ImGui::EndChild();
    ImGui::SameLine();

    // --- analise (coluna direita) ---
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(-80);
    bool enter = ImGui::InputTextWithHint("##path", "caminho de um arquivo .wav", st.path, sizeof st.path, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Abrir", ImVec2(-1, 0)) || enter) {
        load(st, st.path);
        if (st.wav.error.empty()) { st.dir = fs::path(st.path).parent_path().string(); st.listed = false; }
    }
    if (!st.status.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kAccent2), "%s", st.status.c_str());

    const bool loaded = st.wav.error.empty() && !st.wav.mono.empty();
    if (!loaded) {
        ImGui::TextDisabled("Escolha um .wav (mono ou estereo; PCM 8/16/24/32 bits ou float).\n\n"
                            "Melhor com uma nota so, isolada: o som e decomposto em ate 64\n"
                            "senoides (parciais) em dois instantes -- A no ataque e B no\n"
                            "sustain -- e o operador faz o morph de A pra B ao longo da nota.");
    } else {
        if (st.dirty) analyze(st);
        const auto& r = st.result;
        ImGui::Text("%s  |  %d Hz, %d canal(is), %d bits%s, %.2f s", fs::path(st.loadedPath).filename().string().c_str(), st.wav.sampleRate,
                    st.wav.channels, st.wav.bitsPerSample, st.wav.isFloat ? " float" : "", st.wav.seconds());
        const float w = ImGui::GetContentRegionAvail().x;
        Waveform(st, color, ImVec2(w, 110));
        ImGui::Text("A: %.3f s", st.aSec);
        ImGui::SameLine(160);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kAccent2), "B: %.3f s", st.bSec);
        ImGui::SameLine(320);
        ImGui::TextDisabled("(linha tracejada = inicio do som)");
        PeakPlot(st, color, ImVec2(w, 120));

        // opcoes
        static const int kWins[] = {2048, 4096, 8192, 16384};
        int wi = 2;
        for (int i = 0; i < 4; ++i) if (kWins[i] == st.opt.windowSize) wi = i;
        char preview[48];
        std::snprintf(preview, sizeof preview, "%d (%.0f ms)", kWins[wi], 1000.0 * kWins[wi] / st.wav.sampleRate);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Janela FFT");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("##win", preview)) {
            for (int i = 0; i < 4; ++i) {
                char item[48];
                std::snprintf(item, sizeof item, "%d (%.0f ms)", kWins[i], 1000.0 * kWins[i] / st.wav.sampleRate);
                if (ImGui::Selectable(item, i == wi)) { st.opt.windowSize = kWins[i]; st.dirty = true; }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Janela maior: separa parciais mais proximos (notas graves),\nmas mistura mais o tempo (o ataque fica borrado).");
        ImGui::SameLine(0, 18);
        ImGui::TextUnformatted("Limiar");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        st.dirty |= ImGui::SliderFloat("##thr", &st.opt.thresholdDb, -90.0f, -20.0f, "%.0f dB");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("picos mais fracos que isso (relativo ao mais forte) sao ignorados");
        ImGui::SameLine(0, 18);
        ImGui::TextUnformatted("Max parciais");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        st.dirty |= ImGui::SliderInt("##maxp", &st.opt.maxPartials, 1, dx7fm::kNumPartials);

        char note[16];
        dx7fm_app::noteName(r.detectedHz, note, sizeof note);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Referencia (razao 1x): detectada %.2f Hz (%s)", r.detectedHz, note);
        ImGui::SameLine();
        st.dirty |= ImGui::Checkbox("manual", &st.manualRef);
        if (st.manualRef) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            st.dirty |= ImGui::DragFloat("Hz##ref", &st.refHz, 0.1f, 10.0f, 8000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        }

        ImGui::Checkbox("Morph A -> B (usa o FX EG)", &st.apply.morph);
        ImGui::SameLine();
        ImGui::Checkbox("Ajustar o Amp EG ao volume do som", &st.apply.ampEG);
        ImGui::SameLine();
        ImGui::Checkbox("Operador em 1x", &st.apply.resetTuning);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("frequencia por razao 1, sem fine/detune: a nota da referencia soa na altura original");
        if (st.apply.morph && op.fxType != dx7fm::FxType::Bypass)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kAccent2), "Aviso: o FX EG tambem controla o efeito deste operador (FX diferente de Bypass).");

        char refNote[16];
        dx7fm_app::noteName(r.refHz, refNote, sizeof refNote);
        if (r.ok)
            ImGui::Text("A: %d parciais, B: %d (%d pareados).  Toque %s pra ouvir na altura original.", r.partialsA, r.partialsB, r.paired, refNote);
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kAccent2), "Nenhum parcial encontrado nesses instantes (silencio?).");
    }
    ImGui::EndGroup();

    // --- botoes ---
    const bool canApply = loaded && st.result.ok;
    ImGui::BeginDisabled(!canApply);
    char btn[48];
    std::snprintf(btn, sizeof btn, "Aplicar no operador %d", opIndex + 1);
    if (ImGui::Button(btn)) {
        dx7fm::resynth::apply(op, st.result, st.apply);
        applied = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancelar") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return applied;
}

} // namespace ui
