// dx7fm standalone: app de audio real-time (ALSA) com UI leve (Dear ImGui +
// SDL2/SDLRenderer2, sem exigir GPU/OpenGL) e entrada de notas via MIDI
// (ALSA sequencer) e/ou teclado do computador. Ver ARQUITETURA.txt secao 13.
#include "dx7_params.h"
#include "dx7_dsp.h"
#include "dx7_engine.h"
#include "dx7_alloc.h"
#include "dx7_globalfx.h"
#include "dx7_patch_io.h"
#include "dx7_algorithms.h"

#include "alsa_audio.h"
#include "alsa_midi.h"
#include "event_queue.h"
#include "keyboard_piano.h"
#include "ui_widgets.h"
#include "envelope_graph.h"
#include "viz.h"
#include "patch_browser.h"

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dx7fm;
using namespace dx7fm_app;

namespace {

constexpr unsigned kSampleRate = 48000;
constexpr unsigned kPeriodFrames = 256;
constexpr int kMaxVoices = 64;

// Copia atomica de campos de PartParams que NAO fazem parte do triple
// buffer do timbre (ver ARQUITETURA.txt 7C) mas que a audio thread precisa
// ler pra rotear eventos MIDI por canal -- evita ler a copia de staging
// (que a UI thread pode estar editando ao mesmo tempo) de outra thread.
struct PartRoutingCache {
    std::atomic<bool> enabled{true};
    std::atomic<int> midiChannel{0};
    std::atomic<float> bendRangeSemis{2.0f};
};

struct AppState {
    Synth synth;
    AlsaAudioOut audioOut;
    AlsaMidiIn midiIn;
    EventQueue eventQueue;
    PartRoutingCache routing[kNumParts];
    NoteRouting noteRouting; // so a audio thread toca aqui

    std::atomic<float> meterL{0.0f};
    std::atomic<float> meterR{0.0f};
    AudioTap tap;               // saida de audio pros visualizadores (audio thread escreve, UI le)
    ui::ScopeState scope;       // estado do osciloscopio (so UI thread)
    ui::SpectrogramState spectro; // historico do espectrograma (so UI thread)

    KeyboardPiano piano;
    // O teclado do PC se comporta como um teclado MIDI: manda as notas num
    // canal (pcKeyboardChannel, 1..16) pelo MESMO roteamento das notas MIDI
    // (part=-1), entao toca toda parte habilitada em omni ou nesse canal.
    // Guarda o canal junto com a nota: se o canal mudar com a tecla
    // apertada, o noteOff ainda vai pro canal (e partes) do noteOn.
    struct HeldPcNote { int channel; int note; };
    std::unordered_map<SDL_Scancode, HeldPcNote> heldPcNotes;
    int pcKeyboardChannel = 1;
    int selectedOp = 0;
    int selectedAlgo = 0; // indice (0-based) no combo de presets de algoritmo
    // [ADICIONADO em 2026-09-23] estado do navegador de patches (so UI thread)
    struct PatchUi {
        char name[128] = {};
        char filter[64] = {};
        std::filesystem::path currentFile; // arquivo de onde veio / onde foi salvo o som atual
        std::string baseline;              // patchFingerprint no ultimo save/load/init
        bool dirty = false;                // som difere do baseline (alteracoes nao salvas)
        std::filesystem::path selected;    // item selecionado na lista
        enum class Pending { None, Load, Overwrite, Delete, Init } pending = Pending::None;
        std::filesystem::path pendingPath;
        std::string status;
        bool statusOk = true;
    } patchUi[kNumParts];
    std::vector<PatchEntry> patchList;     // compartilhada pelas 4 partes
    ui::EnvelopeGraphState envGraph[kNumParts]; // visibilidade das curvas / arrasto, por parte
    double lastPatchScan = -1e9;
};

const std::filesystem::path kPatchDir = "patches";

void syncRoutingCache(AppState& app, int part) {
    auto& pp = app.synth.part(part);
    app.routing[part].enabled.store(pp.enabled, std::memory_order_relaxed);
    app.routing[part].midiChannel.store(pp.midiChannel, std::memory_order_relaxed);
    app.routing[part].bendRangeSemis.store(pp.bendRangeSemis, std::memory_order_relaxed);
}

// --- audio callback (roda na thread de audio -- UNICA thread que chama
// synth.noteOn/noteOff/etc, ver event_queue.h) ---------------------------

void audioRenderCallback(AppState& app, float* interleaved, int frames) {
    static std::vector<AppEvent> events;
    events.clear();
    app.eventQueue.drainInto(events);

    // adapta applyAppEvent (que espera duas caches separadas) pra
    // PartRoutingCache: monta arrays temporarios de referencia
    static std::atomic<bool> enabledTmp[kNumParts];
    static std::atomic<int> channelTmp[kNumParts];
    for (int p = 0; p < kNumParts; ++p) {
        enabledTmp[p].store(app.routing[p].enabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
        channelTmp[p].store(app.routing[p].midiChannel.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    for (auto& e : events) {
        if (e.kind == AppEvent::Kind::PitchBend && e.part < 0) {
            // pitch bend MIDI cru (-1..1) precisa ser escalado pelo bendRange
            // de CADA parte que ele acabar atingindo -- rotear manualmente aqui.
            for (int p = 0; p < kNumParts; ++p) {
                if (!app.routing[p].enabled.load(std::memory_order_relaxed)) continue;
                int ch = app.routing[p].midiChannel.load(std::memory_order_relaxed);
                if (ch != 0 && ch != e.channel1to16) continue;
                float range = app.routing[p].bendRangeSemis.load(std::memory_order_relaxed);
                app.synth.pitchBend(p, e.floatValue * range);
            }
        } else {
            applyAppEvent(app.synth, e, enabledTmp, channelTmp, app.noteRouting);
        }
    }

    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float l, r;
        app.synth.renderStereo(l, r);
        interleaved[2 * i] = l;
        interleaved[2 * i + 1] = r;
        peakL = std::max(peakL, std::fabs(l));
        peakR = std::max(peakR, std::fabs(r));
    }
    app.meterL.store(peakL, std::memory_order_relaxed);
    app.meterR.store(peakR, std::memory_order_relaxed);
    app.tap.push(interleaved, frames);
}

// --- entrada MIDI (thread separada: so EMPILHA eventos) ------------------

void onMidiEvent(AppState& app, const MidiEvent& ev) {
    AppEvent ae;
    ae.part = -1;
    ae.channel1to16 = ev.channel + 1; // convencao do motor: 0=omni, 1..16=canal especifico
    switch (ev.type) {
        case MidiEvent::Type::NoteOn:
            ae.kind = AppEvent::Kind::NoteOn;
            ae.note = ev.data1;
            ae.velocity = ev.data2;
            break;
        case MidiEvent::Type::NoteOff:
            ae.kind = AppEvent::Kind::NoteOff;
            ae.note = ev.data1;
            break;
        case MidiEvent::Type::PitchBend:
            ae.kind = AppEvent::Kind::PitchBend;
            ae.floatValue = ev.data2 / 8192.0f; // -1..1 (ver audioRenderCallback pro escalonamento por bendRange)
            break;
        case MidiEvent::Type::ChannelAftertouch:
            ae.kind = AppEvent::Kind::ChannelAftertouch;
            ae.floatValue = std::clamp(ev.data2 / 127.0f, 0.0f, 1.0f);
            break;
        case MidiEvent::Type::PolyAftertouch:
            ae.kind = AppEvent::Kind::PolyAftertouch;
            ae.note = ev.data1;
            ae.floatValue = std::clamp(ev.data2 / 127.0f, 0.0f, 1.0f);
            break;
        case MidiEvent::Type::ControlChange:
            if (ev.data1 == 1) { // mod wheel
                ae.kind = AppEvent::Kind::ModWheel;
                ae.floatValue = std::clamp(ev.data2 / 127.0f, 0.0f, 1.0f);
            } else if (ev.data1 == 64) { // sustain
                ae.kind = AppEvent::Kind::Sustain;
                ae.boolValue = ev.data2 >= 64;
            } else if (ev.data1 == 66) { // sostenuto
                ae.kind = AppEvent::Kind::Sostenuto;
                ae.boolValue = ev.data2 >= 64;
            } else if (ev.data1 == 120 || ev.data1 == 123) { // all sound off / all notes off
                ae.kind = AppEvent::Kind::AllNotesOff;
            } else if (ev.data1 == 121) { // reset all controllers
                ae.kind = AppEvent::Kind::ResetControllers;
            } else {
                return; // CC nao mapeado, ignora
            }
            break;
    }
    app.eventQueue.push(ae);
}

// --- widgets de UI reutilizaveis ------------------------------------------

// Rates e levels de um EG de 4 estagios em knobs. twoRows: rates numa
// linha, levels na outra (pra colunas estreitas).
bool DrawEGKnobs(const char* id, uint8_t (&rate)[4], uint8_t (&level)[4], ImU32 color, bool twoRows = false,
                 const uint8_t* defRate = nullptr, const uint8_t* defLevel = nullptr) {
    static const uint8_t kDefRate[4] = {99, 99, 99, 50}, kDefLevel[4] = {99, 99, 99, 0};
    if (!defRate) defRate = kDefRate;
    if (!defLevel) defLevel = kDefLevel;
    static const char* kRate[4] = {"Rate 1", "Rate 2", "Rate 3", "Rate 4"};
    static const char* kLevel[4] = {"Level 1", "Level 2", "Level 3", "Level 4"};
    bool changed = false;
    ui::KnobStyle ks;
    ks.size = 38.0f;
    ks.color = color;
    ImGui::PushID(id);
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        changed |= ui::KnobU8(kRate[i], &rate[i], 0, 99, defRate[i], "%d", ks);
    }
    if (!twoRows) {
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(10, 0));
        ImGui::SameLine();
    }
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        changed |= ui::KnobU8(kLevel[i], &level[i], 0, 99, defLevel[i], "%d", ks);
    }
    ImGui::PopID();
    return changed;
}

// SameLine se o proximo item de largura w ainda cabe na linha (layout fluido de knobs)
void flowSameLine(float w) {
    float x2 = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w;
    if (x2 < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
}

bool DrawOperatorTable(VoiceParams& v, int& selectedOp) {
    // As ultimas 7 sao a reconstrucao propria das formas de onda AFM da
    // SY77/TX81Z (ver dx7_dsp.h e ARQUITETURA.txt secao 5).
    // os combos de onda/FX mostram a lista INTEIRA (ultimo argumento do Combo):
    // com o limite padrao de 8 itens, "Fold" e "Additive" ficavam escondidos
    // atras de uma rolagem no fim da lista
    static const char* kWave[] = {"Sine", "Triangle", "Saw", "Square", "Noise",
                                   "HalfSine", "AbsSine", "QuarterSine", "AlternatingSine",
                                   "CamelSine", "SquaredSine", "LogSaw", "Additive"};
    static const char* kFx[] = {"Bypass", "LowPass", "BandPass", "HighPass", "RingMod",
                                 "Saturate", "Crush", "FreqShift", "Comb", "Fold"};
    bool changed = false;
    if (ImGui::BeginTable("ops", 8, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Op");
        ImGui::TableSetupColumn("Onda");
        ImGui::TableSetupColumn("Ratio/Hz");
        ImGui::TableSetupColumn("Fixo");
        ImGui::TableSetupColumn("Nivel");
        ImGui::TableSetupColumn("Feedback");
        ImGui::TableSetupColumn("FX");
        ImGui::TableSetupColumn("FX1");
        ImGui::TableHeadersRow();

        for (int i = 0; i < kNumOps; ++i) {
            auto& op = v.op[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            // SpanAllColumns faz o Selectable cobrir a LINHA INTEIRA; sem
            // AllowOverlap ele "rouba" o hover de todos os widgets da linha
            // (combo, sliders, checkbox ficavam impossiveis de clicar).
            ImGui::PushStyleColor(ImGuiCol_Text, ui::opColor(i));
            ImGui::PushStyleColor(ImGuiCol_Header, ui::opColor(i, 45));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::opColor(i, 30));
            if (ImGui::Selectable(std::to_string(i + 1).c_str(), selectedOp == i,
                                   ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                selectedOp = i;
            }
            ImGui::PopStyleColor(3);

            ImGui::TableSetColumnIndex(1);
            int wave = static_cast<int>(op.waveform);
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("##wave", &wave, kWave, IM_ARRAYSIZE(kWave), IM_ARRAYSIZE(kWave))) { op.waveform = static_cast<Waveform>(wave); changed = true; }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(80);
            if (op.fixedFreq) {
                changed |= ImGui::DragFloat("##hz", &op.fixedHz, 1.0f, 0.1f, 20000.0f, "%.1f Hz", ImGuiSliderFlags_AlwaysClamp);
            } else {
                changed |= ImGui::DragFloat("##ratio", &op.coarseRatio, 0.01f, 0.01f, 32.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            }

            ImGui::TableSetColumnIndex(3);
            changed |= ImGui::Checkbox("##fixed", &op.fixedFreq);

            ImGui::TableSetColumnIndex(4);
            {
                int lvl = op.outputLevel;
                ImGui::SetNextItemWidth(70);
                if (ImGui::SliderInt("##lvl", &lvl, 0, 99, "%d", ImGuiSliderFlags_AlwaysClamp)) { op.outputLevel = static_cast<uint8_t>(lvl); changed = true; }
            }

            ImGui::TableSetColumnIndex(5);
            ImGui::SetNextItemWidth(70);
            changed |= ImGui::SliderFloat("##fb", &op.feedback, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

            ImGui::TableSetColumnIndex(6);
            int fx = static_cast<int>(op.fxType);
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##fx", &fx, kFx, IM_ARRAYSIZE(kFx), IM_ARRAYSIZE(kFx))) {
                FxType newType = static_cast<FxType>(fx);
                // FX1 do Fold e drive (1..16), nao Hz: ao trocar pra Fold, o
                // default de 1000 (pensado pra corte de filtro) seria absurdo
                if (newType == FxType::Fold && op.fxType != FxType::Fold) { op.fxParam1 = 3.0f; op.fxParam2 = 0.0f; }
                op.fxType = newType;
                changed = true;
            }

            ImGui::TableSetColumnIndex(7);
            ImGui::SetNextItemWidth(80);
            if (op.fxType == FxType::Fold)
                changed |= ImGui::DragFloat("##fx1", &op.fxParam1, 0.02f, 1.0f, 16.0f, "drive %.2f", ImGuiSliderFlags_AlwaysClamp);
            else
                changed |= ImGui::DragFloat("##fx1", &op.fxParam1, 5.0f, 0.0f, 20000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return changed;
}

// [ADICIONADO em 2026-10-18] Editor da forma de onda Additive: 8
// osciladores, cada um com amplitude (slider vertical) e razao de
// frequencia (campo embaixo), nos espectros A e B.
struct AdditivePreset {
    const char* name;
    const char* tip;
    float amp[kNumPartials];
    float ratio[kNumPartials];
};
const AdditivePreset kAdditivePresets[] = {
    {"Seno", "so o parcial 1",
     {1, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 3, 4, 5, 6, 7, 8}},
    {"Serra", "harmonicos 1..8 com amplitude 1/k",
     {1, 1 / 2.f, 1 / 3.f, 1 / 4.f, 1 / 5.f, 1 / 6.f, 1 / 7.f, 1 / 8.f}, {1, 2, 3, 4, 5, 6, 7, 8}},
    {"Impares", "harmonicos impares 1, 3 .. 15 com amplitude 1/k (quadrada)",
     {1, 1 / 3.f, 1 / 5.f, 1 / 7.f, 1 / 9.f, 1 / 11.f, 1 / 13.f, 1 / 15.f}, {1, 3, 5, 7, 9, 11, 13, 15}},
    {"Orgao", "drawbars: 16', 8', 5 1/3', 4', 2 2/3', 2', 1 3/5', 1 1/3'",
     {0.8f, 1, 0.6f, 0.7f, 0.5f, 0.4f, 0.3f, 0.3f}, {0.5f, 1, 1.5f, 2, 3, 4, 5, 6}},
    {"Sino", "modos de vibracao de uma barra livre (inarmonicos): 1, 2.76, 5.40, 8.93...",
     {1, 0.7f, 0.5f, 0.35f, 0.25f, 0.18f, 0.12f, 0.08f}, {1, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.81f, 31.87f}},
    {"Tambor", "modos de uma membrana circular (inarmonicos): 1, 1.59, 2.14, 2.30...",
     {1, 0.8f, 0.6f, 0.55f, 0.45f, 0.4f, 0.3f, 0.25f}, {1, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f}},
};

bool DrawAdditiveEditor(OperatorParams& op, ImU32 color) {
    bool changed = false;
    ImGui::SeparatorText("Aditiva: 8 osciladores (amplitude + razao de frequencia)");
    static int editB = 0; // espectro em edicao (estado so de UI)
    ImGui::RadioButton("Editando A", &editB, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Editando B", &editB, 1);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cada parcial e um oscilador senoidal: soa em (razao x frequencia do operador)\n"
                          "com a amplitude do slider. Razoes inteiras = harmonicos; fracionarias =\n"
                          "inarmonicos (sinos, tambores). A e B sao dois espectros: o envelope de FX\n"
                          "leva de A ate B (x profundidade do morph), interpolando amplitudes E razoes.\n"
                          "Parciais acima de Nyquist sao desligados (sem aliasing).");

    OperatorParams::PartialSet& set = editB ? op.additiveB : op.additiveA;
    for (int k = 0; k < kNumPartials; ++k) {
        ImGui::PushID(k);
        ImGui::BeginGroup();
        ImGui::Text(" P%d", k + 1);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, color);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, color);
        changed |= ImGui::VSliderFloat("##amp", ImVec2(46, 120), &set.amp[k], 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parcial %d: amplitude", k + 1);
        ImGui::SetNextItemWidth(46);
        changed |= ImGui::DragFloat("##ratio", &set.ratio[k], 0.005f, 0.01f, 32.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Parcial %d: razao de frequencia (x f do operador)\narraste ou ctrl+clique pra digitar", k + 1);
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::SameLine();
    }
    // morph ao lado das barras
    ImGui::Dummy(ImVec2(6, 0));
    ImGui::SameLine();
    {
        ui::KnobStyle ks;
        ks.color = color;
        changed |= ui::Knob("Morph A->B", &op.morphDepth, 0.0f, 1.0f, 0.0f, "%.2f", ks);
    }

    ImGui::TextUnformatted("Preset em");
    ImGui::SameLine();
    ImGui::TextUnformatted(editB ? "B:" : "A:");
    for (const auto& p : kAdditivePresets) {
        ImGui::SameLine();
        if (ImGui::SmallButton(p.name)) {
            std::copy(std::begin(p.amp), std::end(p.amp), set.amp);
            std::copy(std::begin(p.ratio), std::end(p.ratio), set.ratio);
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tip);
    }
    if (ImGui::SmallButton("Copiar A -> B")) { op.additiveB = op.additiveA; changed = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Trocar A <-> B")) { std::swap(op.additiveA, op.additiveB); changed = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("Morph = profundidade x (FX EG/99) + mod matrix (OperatorMorph)");
    return changed;
}

bool DrawOperatorDetail(OperatorParams& op, int opIndex) {
    bool changed = false;
    ui::KnobStyle ks;
    ks.color = ui::opColor(opIndex);
    const float kw = 62.0f; // largura de um knob (pro layout fluido)

    ImGui::TextDisabled("afinacao e modulacao");
    changed |= ui::Knob("Fine", &op.fineCents, -100.0f, 100.0f, 0.0f, "%.1f c", ks);
    flowSameLine(kw); changed |= ui::Knob("Detune", &op.detuneCents, -100.0f, 100.0f, 0.0f, "%.1f c", ks);
    flowSameLine(kw); changed |= ui::Knob("Phase dist", &op.phaseDistortion, 0.0f, 1.0f, 0.0f, "%.2f", ks);
    flowSameLine(kw); changed |= ui::Knob("PEG depth", &op.pegDepth, 0.0f, 1.0f, 0.0f, "%.2f", ks);
    flowSameLine(kw); changed |= ui::KnobU8("Vel sens", &op.velocitySens, 0, 7, 0, "%d", ks);
    flowSameLine(kw); changed |= ui::KnobU8("Rate scal", &op.rateScaling, 0, 7, 0, "%d", ks);
    flowSameLine(kw); changed |= ui::KnobU8("LFO pitch", &op.lfoPitchModSens, 0, 7, 0, "%d", ks);
    flowSameLine(kw); changed |= ui::KnobU8("LFO amp", &op.lfoAmpModSens, 0, 3, 0, "%d", ks);
    flowSameLine(kw);
    if (op.fxType == FxType::Fold) changed |= ui::Knob("Simetria", &op.fxParam2, 0.0f, 1.0f, 0.0f, "%.2f", ks);
    else changed |= ui::Knob("FX P2", &op.fxParam2, 0.0f, 16.0f, 0.5f, "%.2f", ks);

    // key scaling: knobs + as opcoes de curva ao lado
    ImGui::TextDisabled("key scaling (nivel por nota, em volta do breakpoint)");
    changed |= ui::KnobU8("Breakpt", &op.levelScaling.breakpoint, 0, 127, 60, "%d", ks);
    {
        int ld = op.levelScaling.leftDepth, rd = op.levelScaling.rightDepth;
        ImGui::SameLine();
        if (ui::KnobInt("Esq dB/oit", &ld, -12, 12, 0, "%d", ks)) { op.levelScaling.leftDepth = static_cast<int8_t>(ld); changed = true; }
        ImGui::SameLine();
        if (ui::KnobInt("Dir dB/oit", &rd, -12, 12, 0, "%d", ks)) { op.levelScaling.rightDepth = static_cast<int8_t>(rd); changed = true; }
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    changed |= ImGui::Checkbox("Esquerda exponencial##lsL", &op.levelScaling.leftExp);
    changed |= ImGui::Checkbox("Direita exponencial##lsR", &op.levelScaling.rightExp);
    changed |= ImGui::Checkbox("PEG afeta freq fixa", &op.pegAffectsFixed);
    ImGui::EndGroup();
    changed |= ImGui::Checkbox("FX EG acompanha o release do Amp EG", &op.fxEGReleaseWithAmp);
    ImGui::SameLine();
    {
        int rm = op.ringModSource;
        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderInt("Ring mod: op fonte", &rm, -1, kNumOps - 1, rm < 0 ? "nenhum" : "%d", ImGuiSliderFlags_AlwaysClamp)) { op.ringModSource = rm; changed = true; }
    }

    if (op.waveform == Waveform::Additive) changed |= DrawAdditiveEditor(op, ui::opColor(opIndex));

    ImGui::Spacing();
    ImGui::TextDisabled("FX EG (modula o FX1 e o morph da Additive)");
    changed |= DrawEGKnobs("fxeg", op.fxEG.rate, op.fxEG.level, ui::opColor(opIndex, 200));
    return changed;
}
// [ADICIONADO em 2026-09-21] Desenha a topologia de roteamento como um
// grafo de caixas+setas, generico o bastante pra visualizar QUALQUER
// AlgorithmMatrix (preset ou editada a mao na grade 8x8 acima) -- nao
// depende de layout topologico dinamico (que teria que lidar com ciclos
// de feedback arbitrarios): as 8 caixas ficam em posicoes FIXAS (grade
// 4x2), so as setas entre elas mudam. Ver ARQUITETURA.txt secao 13/14.
void DrawAlgorithmDiagram(const VoiceParams& v, int& selectedOp) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    const int cols = 4;
    const float gapX = 26.0f, gapY = 46.0f, boxH = 34.0f;
    const float boxW = std::min(70.0f, (ImGui::GetContentRegionAvail().x - 24.0f - (cols - 1) * gapX) / cols);

    ImVec2 boxPos[kNumOps];
    for (int i = 0; i < kNumOps; ++i) {
        int c = i % cols, row = i / cols;
        boxPos[i] = ImVec2(origin.x + 12.0f + c * (boxW + gapX), origin.y + 14.0f + row * (boxH + gapY));
    }
    float canvasW = cols * (boxW + gapX) + 4.0f;
    float canvasH = 2.0f * (boxH + gapY) + 22.0f;
    float outputY = origin.y + canvasH - 10.0f;

    ImGui::InvisibleButton("##algodiagram", ImVec2(canvasW, canvasH));
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImVec2 m = ImGui::GetIO().MousePos;

    dl->AddLine(ImVec2(origin.x + 4, outputY), ImVec2(origin.x + canvasW, outputY), ui::col::kAccent, 2.0f);
    dl->AddText(ImVec2(origin.x + canvasW - 38.0f, outputY - 16.0f), ui::col::kAccent, "saida");

    // arestas (setas) primeiro, pra ficarem atras das caixas; na cor do operador que MODULA
    for (int i = 0; i < kNumOps; ++i) {
        for (int j = 0; j < kNumOps; ++j) {
            if (i == j) continue;
            bool hasMod = v.matrix.mod[i][j] != 0.0f;
            bool hasRing = v.matrix.ring[i][j];
            if (!hasMod && !hasRing) continue;
            ImVec2 from(boxPos[j].x + boxW * 0.5f, boxPos[j].y + boxH * 0.5f);
            ImVec2 to(boxPos[i].x + boxW * 0.5f, boxPos[i].y + boxH * 0.5f);
            ImU32 color = hasRing ? ui::col::kAccent2 : ui::opColor(j, 190);
            dl->AddLine(from, to, color, hasRing ? 2.5f : 1.8f);
            float dx = to.x - from.x, dy = to.y - from.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f) {
                dx /= len; dy /= len;
                ImVec2 tip(to.x - dx * boxH * 0.7f, to.y - dy * boxH * 0.7f);
                ImVec2 p1(tip.x - dx * 8.0f + (-dy) * 5.0f, tip.y - dy * 8.0f + dx * 5.0f);
                ImVec2 p2(tip.x - dx * 8.0f - (-dy) * 5.0f, tip.y - dy * 8.0f - dx * 5.0f);
                dl->AddTriangleFilled(tip, p1, p2, color);
            }
        }
    }

    for (int i = 0; i < kNumOps; ++i) {
        bool isCarrier = v.matrix.outputMix[i] > 0.0f;
        bool hasFeedback = (v.matrix.mod[i][i] != 0.0f) || (v.op[i].feedback > 0.0f);
        ImVec2 p0 = boxPos[i];
        ImVec2 p1(p0.x + boxW, p0.y + boxH);
        bool hot = m.x >= p0.x && m.x <= p1.x && m.y >= p0.y && m.y <= p1.y;
        if (clicked && hot) selectedOp = i;
        dl->AddRectFilled(p0, p1, ui::opColor(i, isCarrier ? 90 : 40), 6.0f);
        dl->AddRect(p0, p1, ui::opColor(i, hot || i == selectedOp ? 255 : 160), 6.0f, 0, i == selectedOp ? 2.5f : 1.2f);

        char label[8];
        std::snprintf(label, sizeof(label), "Op%d", i + 1);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(p0.x + (boxW - ts.x) * 0.5f, p0.y + (boxH - ts.y) * 0.5f), ui::col::kText, label);

        if (isCarrier) dl->AddLine(ImVec2(p0.x + boxW * 0.5f, p1.y), ImVec2(p0.x + boxW * 0.5f, outputY), ui::opColor(i, 150), 1.5f);
        if (hasFeedback) dl->AddCircle(ImVec2(p1.x, p0.y), 6.0f, ui::col::kAccent2, 12, 1.5f);
    }
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("Clique num operador pra seleciona-lo");
    ImGui::TextDisabled("Preenchido = portadora | setas = modulacao | circulo laranja = feedback | laranja = ring mod");
}
bool DrawMatrix(AlgorithmMatrix& m) {
    bool changed = false;
    ImGui::TextDisabled("mod[i][j]: quanto op j modula op i; diagonal = feedback; caixa = ring mod");
    if (ImGui::BeginTable("matrix", kNumOps + 1, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("i \\ j");
        for (int j = 0; j < kNumOps; ++j) ImGui::TableSetupColumn(("Op" + std::to_string(j + 1)).c_str());
        ImGui::TableHeadersRow();
        for (int i = 0; i < kNumOps; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::opColor(i)), "Op%d", i + 1);
            for (int j = 0; j < kNumOps; ++j) {
                ImGui::TableSetColumnIndex(j + 1);
                ImGui::PushID(i * kNumOps + j);
                ImGui::SetNextItemWidth(44);
                changed |= ImGui::DragFloat("##m", &m.mod[i][j], 0.01f, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SameLine();
                bool ring = m.ring[i][j];
                if (ImGui::Checkbox("##r", &ring)) { m.ring[i][j] = ring; changed = true; }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Output mix: quanto de cada operador vai pra saida (> 0 = portadora)");
    for (int i = 0; i < kNumOps; ++i) {
        ImGui::PushID(100 + i);
        ui::KnobStyle ks;
        ks.size = 36.0f;
        ks.color = ui::opColor(i);
        char label[16];
        std::snprintf(label, sizeof label, "Mix Op%d", i + 1);
        if (i) ImGui::SameLine();
        changed |= ui::Knob(label, &m.outputMix[i], 0.0f, 1.0f, 0.0f, "%.2f", ks);
        ImGui::PopID();
    }
    return changed;
}

bool DrawLfo(const char* label, LFOParams& lfo) {
    bool changed = false;
    static const char* kWave[] = {"Triangle", "SawUp", "SawDown", "Square", "Sine", "SampleHold"};
    ImGui::PushID(label);
    int wave = static_cast<int>(lfo.wave);
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("##forma", &wave, kWave, IM_ARRAYSIZE(kWave))) { lfo.wave = static_cast<LfoWave>(wave); changed = true; }
    ImGui::SetItemTooltip("Forma de onda do LFO");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Key sync", &lfo.keySync);
    ui::KnobStyle ks;
    ks.color = ui::col::kAccent2;
    ks.size = 38.0f;
    ks.cellWidth = 56.0f; // 4 knobs em meia coluna
    ui::KnobStyle ksLog = ks;
    ksLog.logarithmic = true;
    changed |= ui::Knob("Rate", &lfo.rate, 0.01f, 50.0f, 5.0f, "%.2f Hz", ksLog);
    ImGui::SameLine(); changed |= ui::Knob("Delay", &lfo.delaySec, 0.0f, 10.0f, 0.0f, "%.2f s", ks);
    ImGui::SameLine(); changed |= ui::Knob("Pitch", &lfo.pitchDepth, 0.0f, 1.0f, 0.0f, "%.2f", ks);
    ImGui::SameLine(); changed |= ui::Knob("Amp", &lfo.ampDepth, 0.0f, 1.0f, 0.0f, "%.2f", ks);
    ImGui::PopID();
    return changed;
}
bool DrawPeg(PEGParams& peg) {
    static const uint8_t kDefRate[4] = {99, 99, 99, 99}, kDefLevel[4] = {50, 50, 50, 50};
    bool changed = DrawEGKnobs("peg", peg.rate, peg.level, ui::col::kAccent2, true, kDefRate, kDefLevel);
    ImGui::SameLine();
    ui::KnobStyle ks;
    ks.color = ui::col::kAccent2;
    changed |= ui::Knob("Range", &peg.rangeOctaves, 0.1f, 8.0f, 4.0f, "%.1f oit", ks);
    return changed;
}
bool DrawModMatrix(ModRoute (&routes)[kNumModRoutes]) {
    bool changed = false;
    static const char* kSrc[] = {"None", "LFO1", "LFO2", "ModWheel", "ChannelAT", "PolyAT", "PitchBend"};
    static const char* kDst[] = {"None", "OperatorLevel", "OperatorFx1", "OperatorFeedback", "VoicePitch", "OperatorMorph"};
    ImGui::TextDisabled("LFO/aftertouch/mod wheel -> parametros");
    if (ImGui::BeginTable("modmatrix", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Fonte");
        ImGui::TableSetupColumn("Destino");
        ImGui::TableSetupColumn("Op");
        ImGui::TableSetupColumn("Profundidade");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kNumModRoutes; ++i) {
            auto& r = routes[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", i + 1);
            ImGui::TableSetColumnIndex(1);
            int src = static_cast<int>(r.src);
            ImGui::SetNextItemWidth(110);
            if (ImGui::Combo("##src", &src, kSrc, 7)) { r.src = static_cast<ModSource>(src); changed = true; }
            ImGui::TableSetColumnIndex(2);
            int dst = static_cast<int>(r.dest);
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("##dst", &dst, kDst, IM_ARRAYSIZE(kDst))) { r.dest = static_cast<ModDest>(dst); changed = true; }
            ImGui::TableSetColumnIndex(3);
            static const char* kOps[] = {"Op1", "Op2", "Op3", "Op4", "Op5", "Op6", "Op7", "Op8"};
            ImGui::SetNextItemWidth(62);
            bool perOp = r.dest != ModDest::VoicePitch && r.dest != ModDest::None;
            ImGui::BeginDisabled(!perOp);
            if (ImGui::Combo("##op", &r.opIndex, kOps, kNumOps)) changed = true;
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(70);
            changed |= ImGui::DragFloat("##depth", &r.depth, 0.005f, -1.0f, 1.0f, "%+.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Arraste (ou Ctrl+clique pra digitar), -1..1");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return changed;
}

// --- navegador de patches ([ADICIONADO em 2026-09-23]) -------------------

void rescanPatches(AppState& app) {
    app.patchList = listPatches(kPatchDir);
    app.lastPatchScan = ImGui::GetTime();
}

void setPatchStatus(AppState::PatchUi& ui, bool ok, std::string msg) {
    ui.statusOk = ok;
    ui.status = std::move(msg);
}

// Marca o som atual como "limpo" (acabou de ser salvo/carregado/zerado).
void markPatchClean(AppState& app, int part) {
    auto& ui = app.patchUi[part];
    ui.baseline = patchFingerprint(app.synth.part(part));
    ui.dirty = false;
}

void initPatchUi(AppState& app, int part) {
    auto& ui = app.patchUi[part];
    std::snprintf(ui.name, sizeof(ui.name), "%s", app.synth.part(part).name.c_str());
    markPatchClean(app, part);
}

void publishPart(AppState& app, int part) {
    app.synth.recompile(part);
    syncRoutingCache(app, part);
}

void doSavePatch(AppState& app, int part, const std::filesystem::path& target) {
    auto& ui = app.patchUi[part];
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (!savePartFile(target.string(), app.synth.part(part))) {
        setPatchStatus(ui, false, "Falha ao salvar " + target.string());
        return;
    }
    ui.currentFile = target;
    ui.selected = target;
    markPatchClean(app, part);
    setPatchStatus(ui, true, "Salvo em " + target.string());
    rescanPatches(app);
}

void doLoadPatch(AppState& app, int part, const std::filesystem::path& path) {
    auto& ui = app.patchUi[part];
    PartParams loaded;
    if (!loadPartFile(path.string(), loaded)) {
        setPatchStatus(ui, false, "Falha ao carregar " + path.string() + " (arquivo ausente ou invalido)");
        rescanPatches(app);
        return;
    }
    copyPatchKeepingRouting(app.synth.part(part), loaded);
    publishPart(app, part);
    std::snprintf(ui.name, sizeof(ui.name), "%s", app.synth.part(part).name.c_str());
    ui.currentFile = path;
    ui.selected = path;
    markPatchClean(app, part);
    setPatchStatus(ui, true, "Carregado " + path.filename().string());
}

void doInitPatch(AppState& app, int part) {
    auto& ui = app.patchUi[part];
    copyPatchKeepingRouting(app.synth.part(part), PartParams{});
    publishPart(app, part);
    std::snprintf(ui.name, sizeof(ui.name), "%s", app.synth.part(part).name.c_str());
    ui.currentFile.clear();
    markPatchClean(app, part);
    setPatchStatus(ui, true, "Som zerado pro patch Init");
}

void doDeletePatch(AppState& app, int part, const std::filesystem::path& path) {
    auto& ui = app.patchUi[part];
    std::error_code ec;
    if (!std::filesystem::remove(path, ec) || ec) {
        setPatchStatus(ui, false, "Falha ao excluir " + path.filename().string());
    } else {
        setPatchStatus(ui, true, "Excluido " + path.filename().string());
        for (auto& other : app.patchUi) {
            if (other.currentFile == path) other.currentFile.clear();
            if (other.selected == path) other.selected.clear();
        }
    }
    rescanPatches(app);
}

void requestSave(AppState& app, int part) {
    auto& ui = app.patchUi[part];
    std::filesystem::path target = patchPathFor(kPatchDir, ui.name);
    std::error_code ec;
    // salvar por cima do PROPRIO arquivo (o que foi carregado/salvo) e o
    // "Salvar" normal; por cima de OUTRO patch que ja existe, pergunta
    if (std::filesystem::exists(target, ec) && target != ui.currentFile) {
        ui.pending = AppState::PatchUi::Pending::Overwrite;
        ui.pendingPath = target;
    } else {
        doSavePatch(app, part, target);
    }
}

void requestLoad(AppState& app, int part, const std::filesystem::path& path) {
    auto& ui = app.patchUi[part];
    if (ui.dirty) {
        ui.pending = AppState::PatchUi::Pending::Load;
        ui.pendingPath = path;
    } else {
        doLoadPatch(app, part, path);
    }
}

bool containsCaseInsensitive(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

void DrawPatchConfirmPopup(AppState& app, int part) {
    using Pending = AppState::PatchUi::Pending;
    auto& ui = app.patchUi[part];
    const char* kPopup = "Confirmar##patch";
    if (ui.pending != Pending::None && !ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    std::string file = ui.pendingPath.filename().string();
    const char* confirmLabel = "Confirmar";
    switch (ui.pending) {
        case Pending::Load:
            ImGui::Text("A Parte %c tem alteracoes NAO salvas.", 'A' + part);
            ImGui::Text("Carregar \"%s\" descarta essas alteracoes.", file.c_str());
            confirmLabel = "Descartar e carregar";
            break;
        case Pending::Overwrite:
            ImGui::Text("Ja existe um patch \"%s\".", file.c_str());
            ImGui::Text("Substituir o arquivo pelo som atual da Parte %c?", 'A' + part);
            confirmLabel = "Substituir";
            break;
        case Pending::Delete:
            ImGui::Text("Excluir o arquivo \"%s\"?", file.c_str());
            ImGui::TextDisabled("Nao da pra desfazer.");
            confirmLabel = "Excluir";
            break;
        case Pending::Init:
            ImGui::Text("A Parte %c tem alteracoes NAO salvas.", 'A' + part);
            ImGui::Text("Zerar o som pro patch Init descarta essas alteracoes.");
            confirmLabel = "Descartar e zerar";
            break;
        case Pending::None: break;
    }
    ImGui::Spacing();
    if (ImGui::Button(confirmLabel)) {
        switch (ui.pending) {
            case Pending::Load:      doLoadPatch(app, part, ui.pendingPath); break;
            case Pending::Overwrite: doSavePatch(app, part, ui.pendingPath); break;
            case Pending::Delete:    doDeletePatch(app, part, ui.pendingPath); break;
            case Pending::Init:      doInitPatch(app, part); break;
            case Pending::None:      break;
        }
        ui.pending = Pending::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.pending = Pending::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawPatchSection(AppState& app, int part) {
    auto& ui = app.patchUi[part];
    auto& pp = app.synth.part(part);
    // re-varre a pasta de vez em quando: pega arquivos copiados/apagados por fora do app
    if (ImGui::GetTime() - app.lastPatchScan > 2.0) rescanPatches(app);

    ImGui::SetNextItemWidth(190);
    if (ImGui::InputText("Nome", ui.name, sizeof(ui.name))) {
        pp.name = ui.name;
        ui.dirty = patchFingerprint(pp) != ui.baseline;
    }
    ImGui::SameLine();
    if (ImGui::Button("Salvar")) requestSave(app, part);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Salva o som desta parte em %s", patchPathFor(kPatchDir, ui.name).string().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Init")) {
        if (ui.dirty) ui.pending = AppState::PatchUi::Pending::Init;
        else doInitPatch(app, part);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Volta o som desta parte pro patch inicial (Init)");
    if (ui.dirty) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "* alteracoes nao salvas");
    else if (!ui.currentFile.empty()) ImGui::TextDisabled("salvo (%s)", ui.currentFile.filename().string().c_str());
    else ImGui::TextDisabled("nao salvo em arquivo");

    // lista de patches da pasta
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##filtro", "filtrar patches...", ui.filter, sizeof(ui.filter));
    ImGui::SameLine();
    if (ImGui::Button("Atualizar lista")) rescanPatches(app);
    ImGui::SameLine();
    ImGui::TextDisabled("pasta: %s/", kPatchDir.string().c_str());
    ImGui::SetItemTooltip("%s", std::filesystem::absolute(kPatchDir).string().c_str());

    std::filesystem::path doubleClicked; // COPIA: o load pode re-varrer (e realocar) patchList
    if (ImGui::BeginListBox("##patchlist", ImVec2(220, 6.5f * ImGui::GetTextLineHeightWithSpacing()))) {
        int shown = 0;
        for (const auto& e : app.patchList) {
            if (ui.filter[0] && !containsCaseInsensitive(e.stem, ui.filter)) continue;
            ++shown;
            std::string label = e.stem + (e.path == ui.currentFile ? "  (atual)" : "");
            if (ImGui::Selectable(label.c_str(), ui.selected == e.path, ImGuiSelectableFlags_AllowDoubleClick)) {
                ui.selected = e.path;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) doubleClicked = e.path;
            }
        }
        if (shown == 0)
            ImGui::TextDisabled(app.patchList.empty() ? "nenhum patch salvo ainda" : "nenhum patch com esse filtro");
        ImGui::EndListBox();
    }
    if (!doubleClicked.empty()) requestLoad(app, part, doubleClicked);

    ImGui::SameLine();
    ImGui::BeginGroup();
    bool hasSel = !ui.selected.empty();
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button("Carregar", ImVec2(110, 0))) requestLoad(app, part, ui.selected);
    if (ImGui::Button("Excluir...", ImVec2(110, 0))) {
        ui.pending = AppState::PatchUi::Pending::Delete;
        ui.pendingPath = ui.selected;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("duplo clique na lista");
    ImGui::TextDisabled("tambem carrega");
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("O patch guarda o SOM da parte: operadores, matriz, envelopes, LFOs,\n"
                          "modo de voz, glide, unison, bend e transpose.\n"
                          "Carregar NAO muda o roteamento desta parte: habilitada, canal MIDI,\n"
                          "faixas de tecla/velocity, vozes reservadas, nivel e pan ficam como estao.");

    if (!ui.status.empty()) {
        ImGui::TextColored(ui.statusOk ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                           "%s", ui.status.c_str());
    }
    DrawPatchConfirmPopup(app, part);
}

// Coluna direita: patch, parte (roteamento/modo de voz), algoritmo, pitch EG,
// LFOs e matriz de modulacao.
bool DrawPartRightColumn(AppState& app, int partIndex) {
    auto& pp = app.synth.part(partIndex);
    bool changed = false;
    const float kw = 62.0f;

    if (ui::BeginCard("##card_patch", "PATCH")) DrawPatchSection(app, partIndex);
    ui::EndCard();

    if (ui::BeginCard("##card_part", "PARTE")) {
        changed |= ImGui::Checkbox("Habilitada", &pp.enabled);
        ImGui::SameLine();
        {
            int ch = pp.midiChannel;
            ImGui::SetNextItemWidth(110);
            if (ImGui::SliderInt("Canal MIDI", &ch, 0, 16, ch == 0 ? "omni" : "%d", ImGuiSliderFlags_AlwaysClamp)) { pp.midiChannel = ch; changed = true; }
        }
        static const char* kMode[] = {"Poly", "Mono", "Unison"};
        int mode = static_cast<int>(pp.voiceMode);
        ImGui::SetNextItemWidth(110);
        if (ImGui::Combo("Modo de voz", &mode, kMode, 3)) { pp.voiceMode = static_cast<VoiceMode>(mode); changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Panico")) {
            AppEvent e;
            e.kind = AppEvent::Kind::AllNotesOff;
            e.part = partIndex;
            app.eventQueue.push(e);
        }
        ImGui::SetItemTooltip("All notes off desta parte");

        changed |= ui::Knob("Nivel", &pp.level, 0.0f, 1.5f, 1.0f, "%.2f");
        flowSameLine(kw); changed |= ui::Knob("Pan", &pp.pan, -1.0f, 1.0f, 0.0f, "%.2f");
        {
            int tr = pp.transpose;
            flowSameLine(kw);
            if (ui::KnobInt("Transpose", &tr, -24, 24, 0, "%d st")) { pp.transpose = static_cast<int8_t>(tr); changed = true; }
        }
        flowSameLine(kw); changed |= ui::Knob("Bend range", &pp.bendRangeSemis, 0.0f, 24.0f, 2.0f, "%.1f st");
        if (pp.voiceMode != VoiceMode::Poly) {
            ui::KnobStyle ksLog;
            ksLog.logarithmic = true;
            float glide = std::max(pp.glideTimeSec, 0.001f);
            flowSameLine(kw);
            if (ui::Knob("Glide", &glide, 0.001f, 10.0f, 0.05f, "%.3f s", ksLog)) { pp.glideTimeSec = glide; changed = true; }
        }
        if (pp.voiceMode == VoiceMode::Unison) {
            int uc = pp.unisonCount;
            flowSameLine(kw);
            if (ui::KnobInt("Vozes", &uc, 1, 8, 1)) { pp.unisonCount = uc; changed = true; }
            flowSameLine(kw); changed |= ui::Knob("Detune", &pp.unisonDetuneCents, 0.0f, 50.0f, 10.0f, "%.1f c");
            flowSameLine(kw); changed |= ui::Knob("Espalham.", &pp.unisonSpread, 0.0f, 1.0f, 0.0f, "%.2f");
        }
        if (pp.voiceMode != VoiceMode::Poly) {
            changed |= ImGui::Checkbox("Glide", &pp.glideEnabled);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("so em legato", &pp.glideLegatoOnly);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Reusa envelope em legato", &pp.monoLegatoEnvReuse);
        }
        {
            int lo = pp.loKey, hi = pp.hiKey, rv = pp.reservedVoices;
            ImGui::SetNextItemWidth(100);
            if (ImGui::SliderInt("##lo", &lo, 0, 127, "de %d", ImGuiSliderFlags_AlwaysClamp)) { pp.loKey = static_cast<uint8_t>(lo); changed = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            if (ImGui::SliderInt("Key range", &hi, 0, 127, "ate %d", ImGuiSliderFlags_AlwaysClamp)) { pp.hiKey = static_cast<uint8_t>(hi); changed = true; }
            ImGui::SetNextItemWidth(100);
            if (ImGui::SliderInt("Vozes reservadas", &rv, 0, kMaxVoices, "%d", ImGuiSliderFlags_AlwaysClamp)) { pp.reservedVoices = rv; changed = true; }
        }
    }
    ui::EndCard();

    if (ui::BeginCard("##card_algo", "ALGORITMO")) {
        static std::vector<std::string> names;
        static std::vector<const char*> namePtrs;
        if (names.empty()) {
            for (int n = 1; n <= kNumAlgorithms; ++n) names.push_back(std::to_string(n) + ": " + algorithmName(n));
            for (auto& str : names) namePtrs.push_back(str.c_str());
        }
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 80.0f));
        ImGui::Combo("##algo", &app.selectedAlgo, namePtrs.data(), static_cast<int>(namePtrs.size()));
        ImGui::SetItemTooltip("Design proprio de 32 algoritmos p/ 8 operadores (nao e a lista oficial do DX7\n"
                              "nem do Montage) -- ver ARQUITETURA.txt secao 14.");
        ImGui::SameLine();
        if (ImGui::Button("Aplicar")) {
            applyAlgorithm(pp.voice, app.selectedAlgo + 1);
            changed = true;
        }
        ImGui::SetItemTooltip("Sobrescreve a matriz e o feedback de todos os operadores");
        DrawAlgorithmDiagram(pp.voice, app.selectedOp);
    }
    ui::EndCard();

    if (ui::BeginCard("##card_lfo", "LFOs", 0.0f, ui::col::kAccent2)) {
        if (ImGui::BeginTable("##lfos", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("LFO 1");
            changed |= DrawLfo("LFO 1", pp.voice.lfo1);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("LFO 2");
            changed |= DrawLfo("LFO 2", pp.voice.lfo2);
            ImGui::EndTable();
        }
    }
    ui::EndCard();
    if (ui::BeginCard("##card_modm", "MATRIZ DE MODULACAO", 0.0f, ui::col::kAccent2)) changed |= DrawModMatrix(pp.voice.modMatrix);
    ui::EndCard();
    return changed;
}

// Coluna esquerda: operadores, envelopes, detalhe do operador selecionado e
// matriz de algoritmo.
bool DrawPartLeftColumn(AppState& app, int partIndex) {
    auto& pp = app.synth.part(partIndex);
    bool changed = false;
    int& sel = app.selectedOp;

    if (ui::BeginCard("##card_ops", "OPERADORES")) changed |= DrawOperatorTable(pp.voice, sel);
    ui::EndCard();

    if (ui::BeginCard("##card_env", "ENVELOPES DE AMPLITUDE")) {
        changed |= ui::EnvelopeGraph("##envgraph", pp.voice, sel, app.envGraph[partIndex], 210.0f);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::opColor(sel));
        ImGui::Text("Op%d", sel + 1);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("Amp EG (arraste os pontos no grafico ou gire os knobs)");
        changed |= DrawEGKnobs("ampeg", pp.voice.op[sel].ampEG.rate, pp.voice.op[sel].ampEG.level, ui::opColor(sel));
    }
    ui::EndCard();

    char title[32];
    std::snprintf(title, sizeof title, "OPERADOR %d", sel + 1);
    if (ui::BeginCard("##card_opdetail", title, 0.0f, ui::opColor(sel))) changed |= DrawOperatorDetail(pp.voice.op[sel], sel);
    ui::EndCard();

    if (ui::BeginCard("##card_peg", "PITCH EG", 0.0f, ui::col::kAccent2)) changed |= DrawPeg(pp.voice.peg);
    ui::EndCard();

    if (ui::BeginCard("##card_matrix", nullptr)) {
        if (ImGui::CollapsingHeader("MATRIZ DE ALGORITMO (avancado)")) changed |= DrawMatrix(pp.voice.matrix);
        else ImGui::TextDisabled("Roteamento livre entre os 8 operadores e output mix -- os presets do card Algoritmo ja preenchem isto.");
    }
    ui::EndCard();
    return changed;
}

void DrawPartPanel(AppState& app, int partIndex) {
    auto& pp = app.synth.part(partIndex);
    bool changed = false;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail >= 1000.0f && ImGui::BeginTable("##partcols", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("esq", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("dir", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        changed |= DrawPartLeftColumn(app, partIndex);
        ImGui::TableSetColumnIndex(1);
        changed |= DrawPartRightColumn(app, partIndex);
        ImGui::EndTable();
    } else { // janela estreita: uma coluna so
        changed |= DrawPartRightColumn(app, partIndex);
        changed |= DrawPartLeftColumn(app, partIndex);
    }

    if (changed) {
        publishPart(app, partIndex);
        auto& ui = app.patchUi[partIndex];
        ui.dirty = patchFingerprint(pp) != ui.baseline; // desfazer uma edicao volta a "nao modificado"
    }
}
// [REESCRITO em 2026-10-18] Efeitos globais: cadeia reordenavel + painel
// por efeito (ver src/dx7_globalfx.h para a origem/licenca de cada um).
struct GlobalFxInfo { GlobalFxType type; const char* name; const char* origin; };
const GlobalFxInfo kGlobalFxInfo[kNumGlobalFx] = {
    {GlobalFxType::Chorus, "Chorus (ensemble)", "Base: Ensemble do Mutable Instruments Rings (Emilie Gillet, MIT)"},
    {GlobalFxType::Phaser, "Phaser", "Base: phaser2_stereo da phaflangers.lib do Faust (Julius O. Smith III, STK-4.3)"},
    {GlobalFxType::Flanger, "Flanger", "Base: flanger_stereo da phaflangers.lib do Faust (Julius O. Smith III, STK-4.3)"},
    {GlobalFxType::Delay, "Delay (fita)", "Implementacao propria: filtros e saturacao no loop, wow & flutter"},
    {GlobalFxType::Reverb, "Reverb", "Room: Freeverb (Jezar, dominio publico)\nHall: reverb do Mutable Instruments Clouds (MIT)\nPlate: MVerb (Martin Eastwood, GPLv3)"},
    {GlobalFxType::Eq, "EQ 3 bandas", "Biquads do Audio EQ Cookbook (Robert Bristow-Johnson)"},
    {GlobalFxType::Compressor, "Compressor", "Base: ZamComp (Damien Zammit, GPLv2+), com knee suave corrigido"},
};

const GlobalFxInfo& globalFxInfo(GlobalFxType t) { return kGlobalFxInfo[static_cast<int>(t)]; }

bool& globalFxEnabled(GlobalFxParams& fx, GlobalFxType t) {
    switch (t) {
        case GlobalFxType::Chorus:     return fx.chorus.enabled;
        case GlobalFxType::Phaser:     return fx.phaser.enabled;
        case GlobalFxType::Flanger:    return fx.flanger.enabled;
        case GlobalFxType::Delay:      return fx.delay.enabled;
        case GlobalFxType::Reverb:     return fx.reverb.enabled;
        case GlobalFxType::Eq:         return fx.eq.enabled;
        case GlobalFxType::Compressor: return fx.compressor.enabled;
    }
    return fx.chorus.enabled;
}

constexpr ImGuiSliderFlags kClamp = ImGuiSliderFlags_AlwaysClamp;
constexpr ImGuiSliderFlags kLog = ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic;

void DrawGlobalFxParams(AppState& app, GlobalFxParams& fx, GlobalFxType t) {
    static const GlobalFxParams kDef; // valores padrao (duplo clique no knob)
    ui::KnobStyle ks;
    ks.cellWidth = 58.0f;
    ui::KnobStyle ksLog = ks;
    ksLog.logarithmic = true;
    bool first = true;
    auto K = [&](const char* label, float* v, float lo, float hi, float def, const char* fmt, bool log = false) {
        if (!first) flowSameLine(58.0f);
        first = false;
        ui::Knob(label, v, lo, hi, def, fmt, log ? ksLog : ks);
    };
    auto KI = [&](const char* label, int* v, int lo, int hi, int def, const char* fmt) {
        if (!first) flowSameLine(58.0f);
        first = false;
        ui::KnobInt(label, v, lo, hi, def, fmt, ks);
    };
    switch (t) {
        case GlobalFxType::Chorus: {
            auto& c = fx.chorus;
            K("Rate", &c.rateHz, 0.05f, 5.0f, kDef.chorus.rateHz, "%.2f Hz", true);
            K("Profund.", &c.depth, 0.0f, 1.0f, kDef.chorus.depth, "%.2f");
            K("Mix", &c.mix, 0.0f, 1.0f, kDef.chorus.mix, "%.2f");
            break;
        }
        case GlobalFxType::Phaser: {
            auto& ph = fx.phaser;
            KI("Notches", &ph.notches, 1, 8, kDef.phaser.notches, "%d");
            K("Rate", &ph.rateHz, 0.0f, 5.0f, kDef.phaser.rateHz, "%.2f Hz");
            K("Freq min", &ph.freqMinHz, 20.0f, 5000.0f, kDef.phaser.freqMinHz, "%.0f Hz", true);
            K("Freq max", &ph.freqMaxHz, 20.0f, 10000.0f, kDef.phaser.freqMaxHz, "%.0f Hz", true);
            K("Razao", &ph.freqRatio, 1.0f, 4.0f, kDef.phaser.freqRatio, "%.2f");
            K("Largura", &ph.notchWidthHz, 10.0f, 5000.0f, kDef.phaser.notchWidthHz, "%.0f Hz", true);
            K("Profund.", &ph.depth, 0.0f, 2.0f, kDef.phaser.depth, "%.2f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = seco, 1 = phaser, 2 = vibrato (so a cadeia all-pass)");
            K("Feedback", &ph.feedback, -0.99f, 0.99f, kDef.phaser.feedback, "%.2f");
            ImGui::Checkbox("Inverter", &ph.invert);
            break;
        }
        case GlobalFxType::Flanger: {
            auto& f = fx.flanger;
            K("Rate", &f.rateHz, 0.0f, 5.0f, kDef.flanger.rateHz, "%.2f Hz");
            K("Atraso", &f.delayMs, 0.05f, 10.0f, kDef.flanger.delayMs, "%.2f ms");
            K("Varredura", &f.sweepMs, 0.0f, 10.0f, kDef.flanger.sweepMs, "%.2f ms");
            K("Profund.", &f.depth, 0.0f, 1.0f, kDef.flanger.depth, "%.2f");
            K("Feedback", &f.feedback, -0.95f, 0.95f, kDef.flanger.feedback, "%.2f");
            ImGui::Checkbox("Inverter", &f.invert);
            break;
        }
        case GlobalFxType::Delay: {
            auto& d = fx.delay;
            K("Tempo L", &d.timeLeftSec, 0.01f, 2.0f, kDef.delay.timeLeftSec, "%.3f s", true);
            K("Tempo R", &d.timeRightSec, 0.01f, 2.0f, kDef.delay.timeRightSec, "%.3f s", true);
            K("Feedback", &d.feedback, 0.0f, 0.95f, kDef.delay.feedback, "%.2f");
            K("Ping-pong", &d.crossFeed, 0.0f, 1.0f, kDef.delay.crossFeed, "%.2f");
            K("Mix", &d.mix, 0.0f, 1.0f, kDef.delay.mix, "%.2f");
            K("Low cut", &d.lowCutHz, 10.0f, 2000.0f, kDef.delay.lowCutHz, "%.0f Hz", true);
            K("High cut", &d.highCutHz, 200.0f, 20000.0f, kDef.delay.highCutHz, "%.0f Hz", true);
            K("Drive", &d.drive, 0.0f, 1.0f, kDef.delay.drive, "%.2f");
            K("Wow", &d.wow, 0.0f, 1.0f, kDef.delay.wow, "%.2f");
            break;
        }
        case GlobalFxType::Reverb: {
            auto& r = fx.reverb;
            static const char* kTypes[] = {"Room (Freeverb)", "Hall (Clouds)", "Plate (MVerb)"};
            int type = static_cast<int>(r.type);
            ImGui::SetNextItemWidth(170);
            if (ImGui::Combo("Algoritmo", &type, kTypes, IM_ARRAYSIZE(kTypes))) r.type = static_cast<ReverbType>(type);
            ImGui::SameLine();
            ImGui::BeginDisabled(r.type != ReverbType::Room);
            ImGui::Checkbox("Freeze", &r.freeze);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(r.type == ReverbType::Room ? "Cauda infinita (modo freeze do Freeverb)" : "So no algoritmo Room");
            K("Tamanho", &r.roomSize, 0.0f, 1.0f, kDef.reverb.roomSize, "%.2f");
            K("Amortec.", &r.damping, 0.0f, 1.0f, kDef.reverb.damping, "%.2f");
            K("Mix", &r.mix, 0.0f, 1.0f, kDef.reverb.mix, "%.2f");
            K("Largura", &r.width, 0.0f, 1.0f, kDef.reverb.width, "%.2f");
            K("Pre-delay", &r.predelayMs, 0.0f, 250.0f, kDef.reverb.predelayMs, "%.0f ms");
            break;
        }
        case GlobalFxType::Eq: {
            auto& e = fx.eq;
            K("Grave Hz", &e.lowFreqHz, 20.0f, 1000.0f, kDef.eq.lowFreqHz, "%.0f", true);
            K("Grave dB", &e.lowGainDb, -18.0f, 18.0f, 0.0f, "%+.1f");
            K("Medio Hz", &e.midFreqHz, 100.0f, 10000.0f, kDef.eq.midFreqHz, "%.0f", true);
            K("Medio dB", &e.midGainDb, -18.0f, 18.0f, 0.0f, "%+.1f");
            K("Medio Q", &e.midQ, 0.1f, 10.0f, kDef.eq.midQ, "%.2f", true);
            K("Agudo Hz", &e.highFreqHz, 1000.0f, 20000.0f, kDef.eq.highFreqHz, "%.0f", true);
            K("Agudo dB", &e.highGainDb, -18.0f, 18.0f, 0.0f, "%+.1f");
            if (ImGui::SmallButton("Zerar ganhos")) e.lowGainDb = e.midGainDb = e.highGainDb = 0.0f;
            break;
        }
        case GlobalFxType::Compressor: {
            auto& c = fx.compressor;
            K("Threshold", &c.thresholdDb, -60.0f, 0.0f, kDef.compressor.thresholdDb, "%.1f dB");
            K("Ratio", &c.ratio, 1.0f, 20.0f, kDef.compressor.ratio, "%.1f:1", true);
            K("Attack", &c.attackMs, 0.1f, 100.0f, kDef.compressor.attackMs, "%.1f ms", true);
            K("Release", &c.releaseMs, 1.0f, 500.0f, kDef.compressor.releaseMs, "%.0f ms", true);
            K("Knee", &c.kneeDb, 0.0f, 8.0f, kDef.compressor.kneeDb, "%.1f dB");
            K("Makeup", &c.makeupDb, 0.0f, 30.0f, kDef.compressor.makeupDb, "%.1f dB");
            float gr = c.enabled ? app.synth.compressorGainReductionDb() : 0.0f;
            char text[32];
            std::snprintf(text, sizeof text, "-%.1f dB", gr);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::col::kAccent2);
            ImGui::ProgressBar(std::clamp(gr / 24.0f, 0.0f, 1.0f), ImVec2(200, 0), text);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("reducao de ganho");
            break;
        }
    }
}
void DrawGlobalFxPanel(AppState& app) {
    auto& fx = app.synth.globalFx();
    if (!ImGui::BeginTable("##fxcols", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("cadeia", ImGuiTableColumnFlags_WidthFixed, 270.0f);
    ImGui::TableSetupColumn("efeitos", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();

    // cadeia: ordem + liga/desliga
    ImGui::TableSetColumnIndex(0);
    if (ui::BeginCard("##card_chain", "CADEIA DE EFEITOS")) {
        ImGui::TextDisabled("O sinal passa de cima pra baixo.");
        for (int i = 0; i < kNumGlobalFx; ++i) {
            GlobalFxType t = fx.order[i];
            const auto& info = globalFxInfo(t);
            ImGui::PushID(i);
            ImGui::TextDisabled("%d", i + 1);
            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) std::swap(fx.order[i], fx.order[i - 1]);
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(i == kNumGlobalFx - 1);
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) std::swap(fx.order[i], fx.order[i + 1]);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Checkbox(info.name, &globalFxEnabled(fx, t));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.origin);
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Ordem padrao")) std::copy(std::begin(kDefaultGlobalFxOrder), std::end(kDefaultGlobalFxOrder), fx.order);
        ImGui::Spacing();
        ImGui::TextDisabled("Depois da cadeia: volume geral\ne limitador de saida.");
    }
    ui::EndCard();

    // um card por efeito, na ordem da cadeia, em 2 colunas empilhadas de
    // forma independente (1,3,5,7 | 2,4,6): sem buracos entre cards de
    // alturas diferentes
    ImGui::TableSetColumnIndex(1);
    if (ImGui::BeginTable("##fxgrid", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        // coluna 0 inteira (1,3,5,7) e depois a 1 (2,4,6): voltar pra uma
        // celula ja usada com TableSetColumnIndex poe o cursor no topo dela
        for (int k = 0; k < kNumGlobalFx; ++k) {
            int i = k < (kNumGlobalFx + 1) / 2 ? k * 2 : (k - (kNumGlobalFx + 1) / 2) * 2 + 1;
            if (k == 0 || k == (kNumGlobalFx + 1) / 2) ImGui::TableSetColumnIndex(i % 2);
            GlobalFxType t = fx.order[i];
            const auto& info = globalFxInfo(t);
            bool on = globalFxEnabled(fx, t);
            char id[24], title[64];
            std::snprintf(id, sizeof id, "##fxcard%d", static_cast<int>(t));
            std::snprintf(title, sizeof title, "%d. %s", i + 1, info.name);
            ImGui::PushID(static_cast<int>(t) + 100);
            if (ui::BeginCard(id, title, 0.0f, on ? ui::col::kAccent : ui::col::kTextDim)) {
                ImGui::Checkbox("Ligado", &globalFxEnabled(fx, t));
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.origin);
                if (!on) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f); // desligado: esmaecido, mas editavel
                DrawGlobalFxParams(app, fx, t);
                if (!on) ImGui::PopStyleVar();
            }
            ui::EndCard();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndTable();
}
// Ponto colorido + texto (status do audio/MIDI na barra superior)
void StatusChip(bool ok, const char* text) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5, p.y + h * 0.5f), 4.0f, ok ? ui::rgb(79, 209, 140) : ui::rgb(252, 100, 100));
    ImGui::Dummy(ImVec2(12, h));
    ImGui::SameLine(0, 2);
    ImGui::TextUnformatted(text);
}

void DrawTopBar(AppState& app) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::rgb(24, 27, 35));
    ImGui::BeginChild("##header", ImVec2(0, 92), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginTable("##hdr", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("logo", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("saida", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableNextRow();

        // logo
        ImGui::TableSetColumnIndex(0);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::col::kAccent);
        ImGui::TextUnformatted("dx7fm");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::TextDisabled("8-OP FM SYNTH");

        // status
        ImGui::TableSetColumnIndex(1);
        char buf[160];
        std::snprintf(buf, sizeof buf, "Audio %s  %u Hz  buffer %u  %s  xruns %ld", app.audioOut.isRunning() ? "ativo" : "PARADO",
                      app.audioOut.sampleRate(), app.audioOut.periodFrames(), app.audioOut.deviceFormatName(), app.audioOut.underruns());
        StatusChip(app.audioOut.isRunning(), buf);
        if (!app.audioOut.isRunning() && !app.audioOut.lastError().empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "(%s)", app.audioOut.lastError().c_str());
        }
        ImGui::SameLine(0, 18);
        StatusChip(app.midiIn.isOpen(), app.midiIn.isOpen() ? "MIDI: porta ALSA dx7fm:input" : "MIDI indisponivel");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Conecte seu controlador com 'aconnect' ou no seu patchbay");
        ImGui::TextDisabled("Teclado do PC: base nota %d (Z/X oitava)  canal", app.piano.baseNote());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("##pcch", &app.pcKeyboardChannel, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toca todas as partes habilitadas em omni (canal 0) ou neste canal,\n"
                              "igual a um controlador MIDI ligado nesse canal.");
        ImGui::SameLine(0, 18);
        ImGui::TextDisabled("Vozes %d / %d", app.synth.activeVoiceCount(), kMaxVoices);

        // medidores + volume geral
        ImGui::TableSetColumnIndex(2);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, 6));
        ui::LevelMeter("L", app.meterL.load(std::memory_order_relaxed), ImVec2(200, 14));
        ui::LevelMeter("R", app.meterR.load(std::memory_order_relaxed), ImVec2(200, 14));
        ImGui::EndGroup();
        ImGui::SameLine();
        // Volume geral em dB (-60..+6); o minimo (-60) e mudo de verdade.
        // Estado so da UI: o motor guarda ganho linear (Synth::setMasterVolume).
        static float volumeDb = 0.0f;
        ui::KnobStyle ks;
        ks.size = 34.0f;
        if (ui::Knob("Volume", &volumeDb, -60.0f, 6.0f, 0.0f, volumeDb <= -60.0f ? "mudo" : "%.1f dB", ks))
            app.synth.setMasterVolume(volumeDb <= -60.0f ? 0.0f : dbToLinear(volumeDb));
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
// --- teclado do PC como piano ---------------------------------------------

void handlePianoKeyDown(AppState& app, SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_Z) { app.piano.shiftOctave(-1); return; }
    if (sc == SDL_SCANCODE_X) { app.piano.shiftOctave(1); return; }
    if (!app.piano.isPianoKey(sc)) return;
    if (app.heldPcNotes.count(sc)) return; // ja soando (evita re-trigger por key repeat do SO)
    int note = app.piano.noteForScancode(sc);
    app.heldPcNotes[sc] = {app.pcKeyboardChannel, note};
    AppEvent e;
    e.kind = AppEvent::Kind::NoteOn;
    e.part = -1; // roteia por canal, como uma nota MIDI (ver AppState::pcKeyboardChannel)
    e.channel1to16 = app.pcKeyboardChannel;
    e.note = note;
    e.velocity = 100;
    app.eventQueue.push(e);
}

void handlePianoKeyUp(AppState& app, SDL_Scancode sc) {
    auto it = app.heldPcNotes.find(sc);
    if (it == app.heldPcNotes.end()) return;
    AppEvent e;
    e.kind = AppEvent::Kind::NoteOff;
    e.part = -1;
    e.channel1to16 = it->second.channel;
    e.note = it->second.note;
    app.eventQueue.push(e);
    app.heldPcNotes.erase(it);
}

// Janela perdeu o foco: o SDL nao manda os KEYUP das teclas que estavam
// apertadas, entao solta tudo aqui pra nao deixar nota presa.
void releaseAllPianoKeys(AppState& app) {
    for (auto& [sc, held] : app.heldPcNotes) {
        AppEvent e;
        e.kind = AppEvent::Kind::NoteOff;
        e.part = -1;
        e.channel1to16 = held.channel;
        e.note = held.note;
        app.eventQueue.push(e);
    }
    app.heldPcNotes.clear();
}

// Tema e fontes do app (chamar 1x depois de ImGui::CreateContext()).
void SetupAppStyle() {
    ui::ApplyTheme();
    // Roboto Medium (Apache 2.0, distribuida com o Dear ImGui em
    // third_party/imgui/misc/fonts); se o arquivo nao existir, fica a fonte
    // padrao embutida do ImGui.
#ifdef DX7FM_FONT_DIR
    ImGuiIO& io = ImGui::GetIO();
    const std::string path = std::string(DX7FM_FONT_DIR) + "/Roboto-Medium.ttf";
    if (std::filesystem::exists(path)) io.FontDefault = io.Fonts->AddFontFromFileTTF(path.c_str(), 15.0f);
#endif
    ImGui::GetStyle().FontSizeBase = 15.0f;
}

// Faixa com osciloscopio e espectrograma da saida (recolhivel).
void DrawVisualizers(AppState& app) {
    const double sr = app.audioOut.sampleRate();
    ui::UpdateSpectrogram(app.spectro, app.tap, sr); // consome o audio novo mesmo com a faixa recolhida
    if (!ImGui::CollapsingHeader("OSCILOSCOPIO / ESPECTROGRAMA", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const float h = 140.0f;
    if (ImGui::BeginTable("##viz", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("scope", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("spectro", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        {
            static const char* kZoom[] = {"5 ms", "10 ms", "20 ms", "50 ms"};
            ImGui::TextDisabled("Osciloscopio");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            ImGui::Combo("##scopezoom", &app.scope.zoom, kZoom, IM_ARRAYSIZE(kZoom));
            ImGui::SetItemTooltip("Janela de tempo mostrada");
            ImGui::SameLine();
            ImGui::Checkbox("Disparo", &app.scope.trigger);
            ImGui::SetItemTooltip("Alinha a onda na subida do zero (fica parada na tela)");
            ImGui::SameLine();
            ImGui::Checkbox("Ganho auto", &app.scope.autoGain);
            ImGui::SetItemTooltip("Amplia sinais baixos pra ver a forma de onda");
            ui::Oscilloscope("##scope", app.tap, sr, app.scope, ImVec2(ImGui::GetContentRegionAvail().x, h));
        }

        ImGui::TableSetColumnIndex(1);
        {
            static const char* kRange[] = {"60 dB", "90 dB", "120 dB"};
            ImGui::TextDisabled("Espectrograma (20 Hz - 20 kHz, ultimos %.1f s)",
                                dx7fm_app::SpectrogramHistory::kCols * dx7fm_app::SpectrogramHistory::kHop / sr);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::Combo("##sprange", &app.spectro.range, kRange, IM_ARRAYSIZE(kRange));
            ImGui::SetItemTooltip("Faixa dinamica: o que fica mais baixo que isso aparece como fundo");
            ui::Spectrogram("##spectro", app.spectro, sr, ImVec2(ImGui::GetContentRegionAvail().x, h));
        }
        ImGui::EndTable();
    }
}

// Desenha a janela inteira do app (1x por frame). forceTab >= 0 forca a aba
// aberta (0..3 = partes, 4 = FX globais) -- usado pelo harness de screenshot.
void DrawMainUi(AppState& app, int forceTab = -1) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("dx7fm", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    DrawTopBar(app);
    DrawVisualizers(app);
    ImGui::Spacing();

    if (ImGui::BeginTabBar("parts")) {
        for (int p = 0; p < kNumParts; ++p) {
            char tabLabel[16];
            std::snprintf(tabLabel, sizeof(tabLabel), "Parte %c", 'A' + p);
            if (ImGui::BeginTabItem(tabLabel, nullptr, forceTab == p ? ImGuiTabItemFlags_SetSelected : 0)) {
                DrawPartPanel(app, p);
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("FX Globais", nullptr, forceTab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawGlobalFxPanel(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("dx7fm standalone", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           1440, 900, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow falhou: %s\n", SDL_GetError());
        return 1;
    }
    // SDL_RENDERER_SOFTWARE fica disponivel como fallback automatico do SDL
    // se nao houver aceleracao -- nao pedimos ACCELERATED explicitamente
    // pra funcionar tambem em distros minimais sem driver de GPU decente.
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer falhou: %s\n", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    SetupAppStyle();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    AppState app;

    app.synth.init(kSampleRate, kMaxVoices, /*oversample=*/1);
    // Por padrao so a parte A toca; B-D comecam desligadas (senao cada nota,
    // MIDI ou do teclado do PC, dispararia 4 camadas empilhadas -- todas em
    // omni). Liga-las e so marcar "Habilitada" na aba de cada uma.
    for (int p = 0; p < kNumParts; ++p) {
        app.synth.part(p).enabled = (p == 0);
        app.synth.recompile(p);
        syncRoutingCache(app, p);
        initPatchUi(app, p);
    }

    auto render = [&app](float* buf, int frames) { audioRenderCallback(app, buf, frames); };
    if (app.audioOut.start("default", kSampleRate, kPeriodFrames, render) &&
        app.audioOut.sampleRate() != kSampleRate) {
        // o ALSA negociou outra taxa (set_rate_near): reinicia o synth nela,
        // senao tudo sai desafinado. Para o audio antes, pro init() nao
        // correr junto com renderStereo() na audio thread.
        unsigned actualRate = app.audioOut.sampleRate();
        std::fprintf(stderr, "[audio] dispositivo usa %u Hz (pedido %u Hz), ajustando o synth\n", actualRate, kSampleRate);
        app.audioOut.stop();
        app.synth.init(actualRate, kMaxVoices, /*oversample=*/1);
        app.audioOut.start("default", actualRate, kPeriodFrames, render);
    }
    app.midiIn.start("dx7fm", "input", [&app](const MidiEvent& ev) { onMidiEvent(app, ev); });

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }

            ImGuiIO& io = ImGui::GetIO();
            // So o KEYDOWN respeita WantCaptureKeyboard. O KEYUP e sempre
            // tratado: se o foco foi pra um campo de texto com a tecla
            // apertada, a nota ainda precisa ser solta.
            if (event.type == SDL_KEYDOWN && event.key.repeat == 0 && !io.WantCaptureKeyboard)
                handlePianoKeyDown(app, event.key.keysym.scancode);
            else if (event.type == SDL_KEYUP)
                handlePianoKeyUp(app, event.key.keysym.scancode);
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                releaseAllPianoKeys(app);
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        DrawMainUi(app);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 18, 20, 26, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    app.midiIn.stop();
    app.audioOut.stop();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
