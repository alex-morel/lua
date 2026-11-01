#pragma once
#include "dx7_alloc.h"
#include "dx7_sequencer_io.h"
#include "patch_browser.h"
#include "ui_widgets.h"
#include "imgui.h"
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// [ADICIONADO em 2026-11-01] Aba Sequencer: 4 tracks (uma por parte) em grade de
// 32 passos e, embaixo, nota, velocity, gate e modulacao da track selecionada.
namespace dx7fm_app::sequi {
using namespace dx7fm;

struct SeqUi {
    SeqParams edit, committed;
    std::vector<SeqParams> undo, redo;
    bool pending = false;     // edicao aberta, vira passo de desfazer ao soltar
    bool saveDirty = false;
    double changedAt = 0.0;
    int track = 0, step = 0;  // selecionados
    int paintRow = -1;        // arrastando na grade
    bool paintOn = false;
    int noteDrag = -1;        // passo com a nota sendo arrastada
    float noteDragY = 0.0f;
    char name[64] = "Meu padrão";
    std::string current = "(sem nome)", status;
    std::vector<PatchEntry> user;
    bool userDirty = true, confirmClear = false;
};

struct SeqHooks {
    std::filesystem::path dir;                  // padroes do usuario
    std::function<bool(int)> partEnabled;
    std::function<void(int)> enablePart;
};

inline std::string noteName(int n) {
    static const char* k[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(k[((n % 12) + 12) % 12]) + std::to_string(n / 12 - 1);
}

// nota de um passo que acabou de ser ligado: a do ultimo passo ligado antes dele
inline void turnOn(SeqTrack& t, int s) {
    t.step[s].on = true;
    for (int k = s - 1; k >= 0; --k)
        if (t.step[k].on) { t.step[s].note = t.step[k].note; return; }
}
inline void shiftTrack(SeqTrack& t, int dir) { // gira dentro do comprimento
    const int n = t.length;
    if (n < 2) return;
    SeqStep tmp[kSeqSteps];
    for (int i = 0; i < n; ++i) tmp[(i + dir + n) % n] = t.step[i];
    for (int i = 0; i < n; ++i) t.step[i] = tmp[i];
}

inline void publish(SeqUi& u, Synth& s, double now) {
    sanitize(u.edit);
    s.setSequence(u.edit);
    u.saveDirty = true;
    u.changedAt = now;
}
inline void setAll(SeqUi& u, Synth& s, const SeqParams& p, double now) {
    u.edit = p;
    publish(u, s, now);
}
inline void commit(SeqUi& u) {
    u.undo.push_back(u.committed);
    if (u.undo.size() > 100) u.undo.erase(u.undo.begin());
    u.committed = u.edit;
    u.redo.clear();
    u.pending = false;
}
inline void undoSeq(SeqUi& u, Synth& s, double now) {
    if (u.pending) commit(u);
    if (u.undo.empty()) return;
    u.redo.push_back(u.committed);
    u.committed = u.undo.back();
    u.undo.pop_back();
    setAll(u, s, u.committed, now);
}
inline void redoSeq(SeqUi& u, Synth& s, double now) {
    if (u.redo.empty()) return;
    u.undo.push_back(u.committed);
    u.committed = u.redo.back();
    u.redo.pop_back();
    setAll(u, s, u.committed, now);
}

// Fileira de barras alinhada com a grade: arrastar pinta o valor de cada passo.
// bipolar: o zero fica no meio. Botao direito volta ao padrao.
template <class Get, class Set, class Lit>
inline bool BarLane(const char* id, ImVec2 origin, float cellW, float h, int len, ImU32 color, bool bipolar, float vmin, float vmax, float def, Get get, Set set, Lit lit) {
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton(id, ImVec2(cellW * kSeqSteps, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool changed = false;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int cell = std::clamp(static_cast<int>((m.x - origin.x) / cellW), 0, kSeqSteps - 1);
    if (cell < len) {
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float n = std::clamp(1.0f - (m.y - origin.y) / h, 0.0f, 1.0f);
            const float v = vmin + (vmax - vmin) * n;
            if (v != get(cell)) { set(cell, v); changed = true; }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { set(cell, def); changed = true; }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + cellW * kSeqSteps, origin.y + h), ui::rgb(14, 16, 22), 3.0f);
    const float zero = origin.y + h * (bipolar ? 0.5f : 1.0f);
    if (bipolar) dl->AddLine(ImVec2(origin.x, zero), ImVec2(origin.x + cellW * kSeqSteps, zero), ui::col::kCardBorder);
    for (int i = 0; i < kSeqSteps; ++i) {
        const float x0 = origin.x + cellW * static_cast<float>(i);
        if (i >= len) { dl->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x0 + cellW, origin.y + h), ui::rgb(0, 0, 0, 90)); continue; }
        if (i % 4 == 0) dl->AddLine(ImVec2(x0, origin.y), ImVec2(x0, origin.y + h), ui::col::kCardBorder);
        const float n = (get(i) - vmin) / (vmax - vmin);
        const float y = origin.y + h * (1.0f - n);
        dl->AddRectFilled(ImVec2(x0 + 2.0f, std::min(y, zero)), ImVec2(x0 + cellW - 2.0f, std::max(y, zero) + 0.5f), lit(i) ? color : ui::rgb(70, 76, 90), 2.0f); // passo sem nota: apagado
    }
    return changed;
}

inline void DrawSequencerPanel(SeqUi& u, Synth& synth, const SeqHooks& hooks, float bpm) {
    const double now = ImGui::GetTime();
    bool changed = false;
    const ImGuiIO& io = ImGui::GetIO();
    if (u.userDirty) { u.user = listPatches(hooks.dir); u.userDirty = false; }

    // --- transporte e padroes ---
    const bool playing = synth.seqIsPlaying();
    ImGui::PushStyleColor(ImGuiCol_Button, playing ? ui::col::kAccent : ui::rgb(38, 43, 55));
    ImGui::PushStyleColor(ImGuiCol_Text, playing ? ui::rgb(14, 16, 22) : ui::col::kText);
    if (ImGui::Button(playing ? "Stop##seq" : "Play##seq", ImVec2(70, 0))) synth.seqPlay(!playing);
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Espaço também toca e para, com esta aba aberta");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%.0f BPM", static_cast<double>(bpm));
    ImGui::SameLine(0, 24);
    ImGui::TextUnformatted("Padrão");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##seqpreset", u.current.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) u.user = listPatches(hooks.dir);
        if (u.user.empty()) ImGui::TextDisabled("(nenhum salvo em %s/)", hooks.dir.string().c_str());
        for (const auto& e : u.user)
            if (ImGui::Selectable(e.stem.c_str(), u.current == e.stem)) {
                SeqParams p;
                if (loadSeqFile(e.path, p)) {
                    u.edit = p;
                    changed = true;
                    u.current = e.stem;
                    std::snprintf(u.name, sizeof u.name, "%s", e.stem.c_str());
                    u.status = "Carregado: " + e.stem;
                } else u.status = "Não é um padrão do sequencer: " + e.path.string();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##seqname", u.name, sizeof u.name);
    ImGui::SameLine();
    const std::filesystem::path target = patchPathFor(hooks.dir, u.name);
    std::error_code ec;
    if (ImGui::Button(std::filesystem::exists(target, ec) ? "Sobrescrever" : "Salvar")) {
        if (saveSeqFile(target, u.edit)) { u.status = "Salvo em " + target.string(); u.current = target.stem().string(); u.userDirty = true; }
        else u.status = "Não consegui salvar " + target.string();
    }
    ImGui::SameLine();
    if (ImGui::Button(u.confirmClear ? "Confirmar: limpar tudo" : "Limpar tudo")) {
        if (u.confirmClear) { u.edit = SeqParams{}; changed = true; u.current = "(sem nome)"; u.confirmClear = false; }
        else u.confirmClear = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(u.undo.empty() && !u.pending);
    if (ImGui::Button("Desfazer")) undoSeq(u, synth, now);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(u.redo.empty());
    if (ImGui::Button("Refazer")) redoSeq(u, synth, now);
    ImGui::EndDisabled();
    if (!u.status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", u.status.c_str()); }
    ImGui::Spacing();

    // --- grade ---
    const float leftW = 318.0f, cellH = 34.0f;
    const float avail = ImGui::GetContentRegionAvail().x - leftW - 8.0f;
    const float cellW = std::max(16.0f, avail / static_cast<float>(kSeqSteps));
    float gridX = 0.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int t = 0; t < kSeqTracks; ++t) {
        SeqTrack& tr = u.edit.track[t];
        const ImU32 col = ui::opColor(t);
        ImGui::PushID(t);
        const ImVec2 rowP = ImGui::GetCursorScreenPos();
        if (t == u.track) dl->AddRectFilled(ImVec2(rowP.x - 4.0f, rowP.y - 2.0f), ImVec2(rowP.x + leftW + cellW * kSeqSteps + 8.0f, rowP.y + cellH + 2.0f), ui::rgb(255, 255, 255, 10), 4.0f);
        // nome da parte: selecionar a track
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "%c##sel", 'A' + t);
        ImGui::PushStyleColor(ImGuiCol_Button, t == u.track ? (col & 0x00FFFFFFu) | (0xC0u << 24) : (col & 0x00FFFFFFu) | (0x40u << 24));
        ImGui::PushStyleColor(ImGuiCol_Text, t == u.track ? ui::rgb(14, 16, 22) : ui::col::kText);
        if (ImGui::Button(lbl, ImVec2(30, cellH))) u.track = t;
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Track da Parte %c: clique pra editar nota, velocity, gate e modulação", 'A' + t);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Button, tr.mute ? ui::col::kAccent2 : ui::rgb(38, 43, 55));
        ImGui::PushStyleColor(ImGuiCol_Text, tr.mute ? ui::rgb(14, 16, 22) : ui::col::kText);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);
        if (ImGui::Button("M##mute", ImVec2(26, 0))) { tr.mute = !tr.mute; changed = true; }
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Mudo: cala as notas; a modulação continua");
        ImGui::SameLine();
        float len = tr.length;
        if (ui::ValueBar("len", &len, 1.0f, 32.0f, 16.0f, "%.0f", 62.0f, col)) { tr.length = static_cast<uint8_t>(std::lround(len)); changed = true; }
        ImGui::SetItemTooltip("Passos da track. Tamanhos diferentes criam polirritmia");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::BeginCombo("##div", lfoDivisionName(tr.division))) {
            for (int d = 0; d < kNumLfoDivisions; ++d)
                if (ImGui::Selectable(lfoDivisionName(d), d == tr.division)) { tr.division = static_cast<uint8_t>(d); changed = true; }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Duração de cada passo");
        ImGui::SameLine();
        float swing = tr.swing * 100.0f;
        if (ui::ValueBar("swing", &swing, 0.0f, 75.0f, 0.0f, "sw %.0f%%", 62.0f, ui::col::kAccent2)) { tr.swing = swing / 100.0f; changed = true; }
        ImGui::SetItemTooltip("Swing: atrasa os passos pares. 33%% = tercina");
        ImGui::EndGroup();
        // celulas
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(rowP.x + leftW, rowP.y));
        const ImVec2 g = ImGui::GetCursorScreenPos();
        gridX = g.x;
        ImGui::InvisibleButton("##cells", ImVec2(cellW * kSeqSteps, cellH));
        const bool hot = ImGui::IsItemHovered();
        const int hoverCell = std::clamp(static_cast<int>((io.MousePos.x - g.x) / cellW), 0, kSeqSteps - 1);
        if (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoverCell < tr.length) {
            u.paintRow = t;
            u.paintOn = !tr.step[hoverCell].on;
            u.track = t;
            u.step = hoverCell;
        }
        if (u.paintRow == t) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (hoverCell < tr.length && tr.step[hoverCell].on != u.paintOn) {
                    if (u.paintOn) turnOn(tr, hoverCell);
                    else tr.step[hoverCell].on = false;
                    changed = true;
                }
            } else u.paintRow = -1;
        }
        const int head = synth.seqPlayhead(t);
        for (int i = 0; i < kSeqSteps; ++i) {
            const ImVec2 a(g.x + cellW * static_cast<float>(i) + 1.0f, g.y + 1.0f), b(g.x + cellW * static_cast<float>(i + 1) - 1.0f, g.y + cellH - 1.0f);
            const SeqStep& st = tr.step[i];
            if (i >= tr.length) { dl->AddRectFilled(a, b, ui::rgb(255, 255, 255, 4), 3.0f); continue; }
            const bool beat = (i / 4) % 2 == 0;
            ImU32 fill = st.on ? (tr.mute ? ui::rgb(90, 96, 110) : (col & 0x00FFFFFFu) | (static_cast<ImU32>(90 + st.velocity) << 24))
                               : beat ? ui::rgb(38, 43, 55) : ui::rgb(30, 34, 44);
            dl->AddRectFilled(a, b, fill, 3.0f);
            if (st.mod != 0.0f) { // risco: o passo tem modulacao
                const float y = st.mod > 0.0f ? a.y + 2.0f : b.y - 3.0f;
                dl->AddLine(ImVec2(a.x + 3.0f, y), ImVec2(b.x - 3.0f, y), ui::col::kAccent2, 1.5f);
            }
            if (i == head) dl->AddRect(a, b, ui::col::kText, 3.0f, 0, 2.0f);
            else if (t == u.track && i == u.step) dl->AddRect(a, b, ui::rgb(255, 255, 255, 110), 3.0f);
            if (st.on && cellW >= 26.0f) {
                const std::string nn = noteName(st.note);
                const ImVec2 ts = ImGui::CalcTextSize(nn.c_str());
                dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), ui::rgb(14, 16, 22), nn.c_str());
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(rowP.x, rowP.y + cellH + 4.0f));
        ImGui::PopID();
    }

    // partes desligadas nao soam: um aviso so, sem desalinhar as fileiras
    bool anyOff = false;
    for (int t = 0; t < kSeqTracks; ++t) {
        if (hooks.partEnabled(t)) continue;
        bool hasNotes = false;
        for (const auto& st : u.edit.track[t].step) hasNotes = hasNotes || st.on;
        if (!hasNotes) continue;
        if (!anyOff) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::col::kAccent2), "Tracks com notas em partes desligadas não soam:");
            anyOff = true;
        }
        char b[24];
        std::snprintf(b, sizeof b, "Ligar Parte %c", 'A' + t);
        ImGui::SameLine();
        if (ImGui::SmallButton(b)) hooks.enablePart(t);
    }

    // --- passos da track selecionada ---
    ImGui::Spacing();
    SeqTrack& tr = u.edit.track[u.track];
    const ImU32 col = ui::opColor(u.track);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::Text("Track %c", 'A' + u.track);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("<<")) { shiftTrack(tr, -1); changed = true; }
    ImGui::SetItemTooltip("Desloca os passos um pra trás");
    ImGui::SameLine();
    if (ImGui::SmallButton(">>")) { shiftTrack(tr, 1); changed = true; }
    ImGui::SetItemTooltip("Desloca os passos um pra frente");
    ImGui::SameLine();
    if (ImGui::SmallButton("Limpar track")) { const SeqTrack old = tr; tr = SeqTrack{}; tr.length = old.length; tr.division = old.division; tr.swing = old.swing; tr.mute = old.mute; changed = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("Arraste nas fileiras; botão direito volta ao padrão. A modulação vira a fonte Sequencer na matriz e nos efeitos.");

    const float laneX = gridX;
    auto label = [&](const char* text, float y) {
        dl->AddText(ImVec2(laneX - 90.0f, y), ui::col::kTextDim, text);
    };
    float y = ImGui::GetCursorScreenPos().y + 2.0f;
    // Nota: arraste vertical por semitom, roda tambem
    {
        const float h = 74.0f;
        label("Nota", y);
        const ImVec2 o(laneX, y);
        ImGui::SetCursorScreenPos(o);
        ImGui::InvisibleButton("##notes", ImVec2(cellW * kSeqSteps, h));
        const int cell = std::clamp(static_cast<int>((io.MousePos.x - o.x) / cellW), 0, kSeqSteps - 1);
        if (ImGui::IsItemHovered() && cell < tr.length) {
            if (io.MouseWheel != 0.0f) { tr.step[cell].note = static_cast<uint8_t>(std::clamp<int>(tr.step[cell].note + static_cast<int>(io.MouseWheel), 0, 127)); changed = true; }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { u.noteDrag = cell; u.noteDragY = io.MousePos.y; u.step = cell; }
            ImGui::SetTooltip("%s\nArraste na vertical ou use a roda", noteName(tr.step[cell].note).c_str());
        }
        if (u.noteDrag >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const int d = static_cast<int>((u.noteDragY - io.MousePos.y) / 6.0f);
                if (d != 0) {
                    tr.step[u.noteDrag].note = static_cast<uint8_t>(std::clamp<int>(tr.step[u.noteDrag].note + d, 0, 127));
                    u.noteDragY -= static_cast<float>(d) * 6.0f;
                    changed = true;
                }
            } else u.noteDrag = -1;
        }
        dl->AddRectFilled(o, ImVec2(o.x + cellW * kSeqSteps, o.y + h), ui::rgb(14, 16, 22), 3.0f);
        // a janela de notas acompanha o padrao
        int lo = 127, hi = 0;
        for (int i = 0; i < tr.length; ++i) if (tr.step[i].on) { lo = std::min<int>(lo, tr.step[i].note); hi = std::max<int>(hi, tr.step[i].note); }
        if (lo > hi) { lo = 48; hi = 72; }
        lo = std::max(0, lo - 3);
        hi = std::min(127, std::max(hi + 3, lo + 12));
        const float bh = 13.0f;
        for (int i = 0; i < kSeqSteps; ++i) {
            const float x0 = o.x + cellW * static_cast<float>(i);
            if (i >= tr.length) { dl->AddRectFilled(ImVec2(x0, o.y), ImVec2(x0 + cellW, o.y + h), ui::rgb(0, 0, 0, 90)); continue; }
            if (i % 4 == 0) dl->AddLine(ImVec2(x0, o.y), ImVec2(x0, o.y + h), ui::col::kCardBorder);
            const SeqStep& st = tr.step[i];
            const float n = static_cast<float>(st.note - lo) / static_cast<float>(hi - lo);
            const float cy = o.y + (h - bh) * (1.0f - std::clamp(n, 0.0f, 1.0f));
            dl->AddRectFilled(ImVec2(x0 + 1.0f, cy), ImVec2(x0 + cellW - 1.0f, cy + bh), st.on ? col : ui::rgb(70, 76, 90), 2.0f);
        }
        y += h + 6.0f;
    }
    label("Velocity", y);
    changed |= BarLane("##vel", ImVec2(laneX, y), cellW, 48.0f, tr.length, col, false, 1.0f, 127.0f, 100.0f,
                       [&](int i) { return static_cast<float>(tr.step[i].velocity); },
                       [&](int i, float v) { tr.step[i].velocity = static_cast<uint8_t>(std::lround(v)); }, [&](int i) { return tr.step[i].on; });
    y += 54.0f;
    label("Gate", y);
    changed |= BarLane("##gate", ImVec2(laneX, y), cellW, 48.0f, tr.length, col, false, 0.01f, 1.0f, 0.5f,
                       [&](int i) { return tr.step[i].gate; }, [&](int i, float v) { tr.step[i].gate = v; }, [&](int i) { return tr.step[i].on; });
    y += 54.0f;
    label("Modulação", y);
    changed |= BarLane("##mod", ImVec2(laneX, y), cellW, 62.0f, tr.length, ui::col::kAccent2, true, -1.0f, 1.0f, 0.0f,
                       [&](int i) { return tr.step[i].mod; }, [&](int i, float v) { tr.step[i].mod = std::fabs(v) < 0.03f ? 0.0f : v; }, [](int) { return true; });
    y += 68.0f;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, y));
    ImGui::Dummy(ImVec2(1, 1));

    if (changed) { publish(u, synth, now); u.pending = true; }
    if (u.pending && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) commit(u);
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) synth.seqPlay(!synth.seqIsPlaying());
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redoSeq(u, synth, now); else undoSeq(u, synth, now); }
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redoSeq(u, synth, now);
    }
}

} // namespace dx7fm_app::sequi
