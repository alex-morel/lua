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

bool DrawEG(const char* label, EGParams& eg, const char* levelDomain) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", levelDomain);
    static const char* kStage[4] = {"Ataque", "Decai1", "Sustain", "Release"};
    if (ImGui::BeginTable(label, 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Estagio");
        for (int i = 0; i < 4; ++i) ImGui::TableSetupColumn(kStage[i]);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Rate");
        for (int i = 0; i < 4; ++i) {
            ImGui::TableSetColumnIndex(i + 1);
            ImGui::PushID(i);
            int v = eg.rate[i];
            ImGui::SetNextItemWidth(60);
            if (ImGui::SliderInt("##rate", &v, 0, 99, "%d", ImGuiSliderFlags_AlwaysClamp)) { eg.rate[i] = static_cast<uint8_t>(v); changed = true; }
            ImGui::PopID();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Level");
        for (int i = 0; i < 4; ++i) {
            ImGui::TableSetColumnIndex(i + 1);
            ImGui::PushID(i + 10);
            int v = eg.level[i];
            ImGui::SetNextItemWidth(60);
            if (ImGui::SliderInt("##level", &v, 0, 99, "%d", ImGuiSliderFlags_AlwaysClamp)) { eg.level[i] = static_cast<uint8_t>(v); changed = true; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    return changed;
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
    if (ImGui::BeginTable("ops", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
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
            if (ImGui::Selectable(std::to_string(i).c_str(), selectedOp == i,
                                   ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                selectedOp = i;
            }

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

bool DrawAdditiveEditor(OperatorParams& op) {
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
        changed |= ImGui::VSliderFloat("##amp", ImVec2(46, 120), &set.amp[k], 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parcial %d: amplitude", k + 1);
        ImGui::SetNextItemWidth(46);
        changed |= ImGui::DragFloat("##ratio", &set.ratio[k], 0.005f, 0.01f, 32.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Parcial %d: razao de frequencia (x f do operador)\narraste ou ctrl+clique pra digitar", k + 1);
        ImGui::EndGroup();
        ImGui::PopID();
        if (k + 1 < kNumPartials) ImGui::SameLine();
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
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::SliderFloat("Morph A->B", &op.morphDepth, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Posicao do morph = profundidade x (FX EG / 99) + mod matrix (OperatorMorph).\n"
                          "0 = so A. Com o FX EG padrao (99 durante a nota), 1 = so B.\n"
                          "O FX EG e o mesmo que controla o efeito de FX do operador.");
    return changed;
}

bool DrawOperatorDetail(OperatorParams& op) {
    bool changed = false;
    ImGui::SeparatorText("Detalhe do operador selecionado");

    changed |= ImGui::DragFloat("Fine (cents)", &op.fineCents, 1.0f, -100.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::DragFloat("Detune (cents)", &op.detuneCents, 1.0f, -100.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    if (op.fxType == FxType::Fold)
        changed |= ImGui::SliderFloat("FX Param2 (simetria)", &op.fxParam2, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    else
        changed |= ImGui::SliderFloat("FX Param2", &op.fxParam2, 0.0f, 16.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Phase distortion", &op.phaseDistortion, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    {
        int rm = op.ringModSource;
        if (ImGui::SliderInt("Ring mod source (op)", &rm, -1, kNumOps - 1, "%d", ImGuiSliderFlags_AlwaysClamp)) { op.ringModSource = rm; changed = true; }
    }
    {
        int rs = op.rateScaling;
        if (ImGui::SliderInt("Rate scaling", &rs, 0, 7, "%d", ImGuiSliderFlags_AlwaysClamp)) { op.rateScaling = static_cast<uint8_t>(rs); changed = true; }
    }
    {
        int vs = op.velocitySens;
        if (ImGui::SliderInt("Velocity sens", &vs, 0, 7, "%d", ImGuiSliderFlags_AlwaysClamp)) { op.velocitySens = static_cast<uint8_t>(vs); changed = true; }
    }
    {
        int la = op.lfoAmpModSens;
        if (ImGui::SliderInt("LFO amp sens", &la, 0, 3, "%d", ImGuiSliderFlags_AlwaysClamp)) { op.lfoAmpModSens = static_cast<uint8_t>(la); changed = true; }
    }
    {
        int lp = op.lfoPitchModSens;
        if (ImGui::SliderInt("LFO pitch sens", &lp, 0, 7, "%d", ImGuiSliderFlags_AlwaysClamp)) { op.lfoPitchModSens = static_cast<uint8_t>(lp); changed = true; }
    }
    changed |= ImGui::SliderFloat("PEG depth", &op.pegDepth, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::Checkbox("PEG afeta freq fixa", &op.pegAffectsFixed);
    changed |= ImGui::Checkbox("fxEG acompanha release do ampEG", &op.fxEGReleaseWithAmp);

    ImGui::Spacing();
    {
        int bp = op.levelScaling.breakpoint;
        if (ImGui::SliderInt("Level scaling: breakpoint", &bp, 0, 127, "%d", ImGuiSliderFlags_AlwaysClamp)) {
            op.levelScaling.breakpoint = static_cast<uint8_t>(bp);
            changed = true;
        }
    }
    {
        int ld = op.levelScaling.leftDepth;
        if (ImGui::SliderInt("  esquerda (dB/oitava)", &ld, -12, 12, "%d", ImGuiSliderFlags_AlwaysClamp)) {
            op.levelScaling.leftDepth = static_cast<int8_t>(ld);
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("  esquerda exp##lsL", &op.levelScaling.leftExp);
    {
        int rd = op.levelScaling.rightDepth;
        if (ImGui::SliderInt("  direita (dB/oitava)", &rd, -12, 12, "%d", ImGuiSliderFlags_AlwaysClamp)) {
            op.levelScaling.rightDepth = static_cast<int8_t>(rd);
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("  direita exp##lsR", &op.levelScaling.rightExp);

    if (op.waveform == Waveform::Additive) changed |= DrawAdditiveEditor(op);

    changed |= DrawEG("Amp EG", op.ampEG, "0-99");
    changed |= DrawEG("FX EG", op.fxEG, "0-99, modula FX1");
    return changed;
}

// [ADICIONADO em 2026-09-21] Desenha a topologia de roteamento como um
// grafo de caixas+setas, generico o bastante pra visualizar QUALQUER
// AlgorithmMatrix (preset ou editada a mao na grade 8x8 acima) -- nao
// depende de layout topologico dinamico (que teria que lidar com ciclos
// de feedback arbitrarios): as 8 caixas ficam em posicoes FIXAS (grade
// 4x2), so as setas entre elas mudam. Ver ARQUITETURA.txt secao 13/14.
void DrawAlgorithmDiagram(const VoiceParams& v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    const float boxW = 70.0f, boxH = 40.0f, gapX = 40.0f, gapY = 55.0f;
    const int cols = 4;

    ImVec2 boxPos[kNumOps];
    for (int i = 0; i < kNumOps; ++i) {
        int col = i % cols, row = i / cols;
        boxPos[i] = ImVec2(origin.x + 20.0f + col * (boxW + gapX), origin.y + 20.0f + row * (boxH + gapY));
    }
    float canvasW = cols * (boxW + gapX) + 20.0f;
    float canvasH = 2.0f * (boxH + gapY) + 40.0f;
    float outputY = origin.y + canvasH - 15.0f;

    ImGui::Dummy(ImVec2(canvasW, canvasH));

    dl->AddLine(ImVec2(origin.x, outputY), ImVec2(origin.x + canvasW, outputY), IM_COL32(90, 160, 230, 160), 2.0f);
    dl->AddText(ImVec2(origin.x + canvasW - 55.0f, outputY + 4.0f), IM_COL32(140, 190, 240, 255), "saida");

    // arestas (setas) primeiro, pra ficarem atras das caixas
    for (int i = 0; i < kNumOps; ++i) {
        for (int j = 0; j < kNumOps; ++j) {
            if (i == j) continue;
            bool hasMod = v.matrix.mod[i][j] != 0.0f;
            bool hasRing = v.matrix.ring[i][j];
            if (!hasMod && !hasRing) continue;
            ImVec2 from(boxPos[j].x + boxW * 0.5f, boxPos[j].y + boxH * 0.5f);
            ImVec2 to(boxPos[i].x + boxW * 0.5f, boxPos[i].y + boxH * 0.5f);
            ImU32 color = hasRing ? IM_COL32(255, 170, 70, 210) : IM_COL32(190, 190, 200, 170);
            dl->AddLine(from, to, color, hasRing ? 2.5f : 1.5f);
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
        ImU32 fill = isCarrier ? IM_COL32(55, 95, 155, 255) : IM_COL32(65, 65, 75, 255);
        ImU32 border = hasFeedback ? IM_COL32(255, 150, 70, 255) : IM_COL32(170, 170, 180, 255);
        dl->AddRectFilled(p0, p1, fill, 4.0f);
        dl->AddRect(p0, p1, border, 4.0f, 0, hasFeedback ? 2.5f : 1.0f);

        char label[8];
        std::snprintf(label, sizeof(label), "Op%d", i + 1);
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(p0.x + (boxW - ts.x) * 0.5f, p0.y + (boxH - ts.y) * 0.5f), IM_COL32(255, 255, 255, 255), label);

        if (isCarrier) dl->AddLine(ImVec2(p0.x + boxW * 0.5f, p1.y), ImVec2(p0.x + boxW * 0.5f, outputY),
                                    IM_COL32(90, 160, 230, 150), 1.5f);
        if (hasFeedback) dl->AddCircle(ImVec2(p1.x, p0.y), 8.0f, IM_COL32(255, 150, 70, 255), 12, 1.5f);
    }
    ImGui::TextDisabled("Azul = vai pro output (portadora) | Cinza = so modula | Laranja = tem feedback | Linha laranja = ring mod");
}

bool DrawMatrix(AlgorithmMatrix& m) {
    bool changed = false;
    ImGui::SeparatorText("Matriz de algoritmo (mod[i][j]: quanto op j modula op i; diagonal = feedback)");
    if (ImGui::BeginTable("matrix", kNumOps + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("i \\ j");
        for (int j = 0; j < kNumOps; ++j) ImGui::TableSetupColumn(("op" + std::to_string(j)).c_str());
        ImGui::TableHeadersRow();
        for (int i = 0; i < kNumOps; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("op%d", i);
            for (int j = 0; j < kNumOps; ++j) {
                ImGui::TableSetColumnIndex(j + 1);
                ImGui::PushID(i * kNumOps + j);
                ImGui::SetNextItemWidth(50);
                changed |= ImGui::DragFloat("##m", &m.mod[i][j], 0.01f, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SameLine();
                bool ring = m.ring[i][j];
                if (ImGui::Checkbox("##r", &ring)) { m.ring[i][j] = ring; changed = true; }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Numero = quantidade de modulacao de fase; caixa = tambem multiplica (ring mod)");

    ImGui::SeparatorText("Output mix (quanto de cada operador vai pro bus de saida)");
    for (int i = 0; i < kNumOps; ++i) {
        ImGui::PushID(100 + i);
        ImGui::SetNextItemWidth(60);
        changed |= ImGui::SliderFloat(("op" + std::to_string(i)).c_str(), &m.outputMix[i], 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (i < kNumOps - 1) ImGui::SameLine();
        ImGui::PopID();
    }
    return changed;
}

bool DrawLfo(const char* label, LFOParams& lfo) {
    bool changed = false;
    static const char* kWave[] = {"Triangle", "SawUp", "SawDown", "Square", "Sine", "SampleHold"};
    ImGui::PushID(label);
    ImGui::SeparatorText(label);
    changed |= ImGui::DragFloat("Rate (Hz)", &lfo.rate, 0.05f, 0.01f, 50.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    int wave = static_cast<int>(lfo.wave);
    if (ImGui::Combo("Forma", &wave, kWave, 6)) { lfo.wave = static_cast<LfoWave>(wave); changed = true; }
    changed |= ImGui::DragFloat("Delay (s)", &lfo.delaySec, 0.01f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::Checkbox("Key sync", &lfo.keySync);
    changed |= ImGui::SliderFloat("Pitch depth", &lfo.pitchDepth, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Amp depth", &lfo.ampDepth, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopID();
    return changed;
}

bool DrawPeg(PEGParams& peg) {
    // PEGParams e EGParams tem o mesmo formato (rate[4]/level[4]) mas sao
    // tipos DIFERENTES -- reinterpret_cast entre eles seria UB. Copia pra
    // um EGParams temporario, reusa o widget, e copia de volta so se mudou.
    EGParams tmp;
    for (int i = 0; i < 4; ++i) { tmp.rate[i] = peg.rate[i]; tmp.level[i] = peg.level[i]; }
    bool changed = DrawEG("Pitch EG", tmp, "0-99, 50=centro");
    if (changed) {
        for (int i = 0; i < 4; ++i) { peg.rate[i] = tmp.rate[i]; peg.level[i] = tmp.level[i]; }
    }
    changed |= ImGui::DragFloat("Range (oitavas)", &peg.rangeOctaves, 0.1f, 0.1f, 8.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    return changed;
}

bool DrawModMatrix(ModRoute (&routes)[kNumModRoutes]) {
    bool changed = false;
    static const char* kSrc[] = {"None", "LFO1", "LFO2", "ModWheel", "ChannelAT", "PolyAT", "PitchBend"};
    static const char* kDst[] = {"None", "OperatorLevel", "OperatorFx1", "OperatorFeedback", "VoicePitch", "OperatorMorph"};
    ImGui::SeparatorText("Matriz de modulacao (LFO/aftertouch/mod wheel -> parametros)");
    if (ImGui::BeginTable("modmatrix", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
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
            ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            int src = static_cast<int>(r.src);
            ImGui::SetNextItemWidth(110);
            if (ImGui::Combo("##src", &src, kSrc, 7)) { r.src = static_cast<ModSource>(src); changed = true; }
            ImGui::TableSetColumnIndex(2);
            int dst = static_cast<int>(r.dest);
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("##dst", &dst, kDst, IM_ARRAYSIZE(kDst))) { r.dest = static_cast<ModDest>(dst); changed = true; }
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(50);
            changed |= ImGui::SliderInt("##op", &r.opIndex, 0, kNumOps - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(80);
            changed |= ImGui::SliderFloat("##depth", &r.depth, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
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

    ImGui::SeparatorText("Patch");

    ImGui::SetNextItemWidth(260);
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
    ImGui::SameLine();
    if (ui.dirty) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "* alteracoes nao salvas");
    else if (!ui.currentFile.empty()) ImGui::TextDisabled("salvo (%s)", ui.currentFile.filename().string().c_str());
    else ImGui::TextDisabled("nao salvo em arquivo");

    // lista de patches da pasta
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##filtro", "filtrar patches...", ui.filter, sizeof(ui.filter));
    ImGui::SameLine();
    if (ImGui::Button("Atualizar lista")) rescanPatches(app);
    ImGui::SameLine();
    ImGui::TextDisabled("pasta: %s", std::filesystem::absolute(kPatchDir).string().c_str());

    std::filesystem::path doubleClicked; // COPIA: o load pode re-varrer (e realocar) patchList
    if (ImGui::BeginListBox("##patchlist", ImVec2(260, 6.5f * ImGui::GetTextLineHeightWithSpacing()))) {
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

void DrawPartPanel(AppState& app, int partIndex) {
    auto& pp = app.synth.part(partIndex);
    bool changed = false;

    DrawPatchSection(app, partIndex);
    ImGui::SeparatorText("Parte");

    changed |= ImGui::Checkbox("Habilitada", &pp.enabled);
    ImGui::SameLine();
    {
        int ch = pp.midiChannel;
        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderInt("Canal MIDI (0=omni)", &ch, 0, 16, "%d", ImGuiSliderFlags_AlwaysClamp)) { pp.midiChannel = ch; changed = true; }
    }

    static const char* kMode[] = {"Poly", "Mono", "Unison"};
    int mode = static_cast<int>(pp.voiceMode);
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("Modo de voz", &mode, kMode, 3)) { pp.voiceMode = static_cast<VoiceMode>(mode); changed = true; }

    {
        int tr = pp.transpose;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::SliderInt("Transpose", &tr, -24, 24, "%d", ImGuiSliderFlags_AlwaysClamp)) { pp.transpose = static_cast<int8_t>(tr); changed = true; }
    }

    // largura explicita: com a largura padrao (65% da janela) o "Pan" na
    // mesma linha ficava metade fora da janela, sem dar pra clicar
    ImGui::SetNextItemWidth(200);
    changed |= ImGui::SliderFloat("Nivel", &pp.level, 0.0f, 1.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    changed |= ImGui::SliderFloat("Pan", &pp.pan, -1.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Bend range (semitons)", &pp.bendRangeSemis, 0.0f, 24.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);

    {
        int lo = pp.loKey, hi = pp.hiKey;
        if (ImGui::SliderInt("Key range low", &lo, 0, 127, "%d", ImGuiSliderFlags_AlwaysClamp)) { pp.loKey = static_cast<uint8_t>(lo); changed = true; }
        if (ImGui::SliderInt("Key range high", &hi, 0, 127, "%d", ImGuiSliderFlags_AlwaysClamp)) { pp.hiKey = static_cast<uint8_t>(hi); changed = true; }
    }

    if (pp.voiceMode == VoiceMode::Mono || pp.voiceMode == VoiceMode::Unison) {
        changed |= ImGui::Checkbox("Glide", &pp.glideEnabled);
        ImGui::SameLine();
        changed |= ImGui::DragFloat("Glide (s)", &pp.glideTimeSec, 0.01f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::Checkbox("Glide so em legato", &pp.glideLegatoOnly);
        changed |= ImGui::Checkbox("Reusa envelope em legato", &pp.monoLegatoEnvReuse);
    }
    if (pp.voiceMode == VoiceMode::Unison) {
        int uc = pp.unisonCount;
        if (ImGui::SliderInt("Unison: vozes", &uc, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp)) { pp.unisonCount = uc; changed = true; }
        changed |= ImGui::DragFloat("Unison: detune (cents)", &pp.unisonDetuneCents, 0.5f, 0.0f, 50.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        changed |= ImGui::SliderFloat("Unison: espalhamento", &pp.unisonSpread, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    }

    {
        int rv = pp.reservedVoices;
        if (ImGui::SliderInt("Vozes reservadas", &rv, 0, kMaxVoices, "%d", ImGuiSliderFlags_AlwaysClamp)) { pp.reservedVoices = rv; changed = true; }
    }

    ImGui::Spacing();
    if (ImGui::Button("Panico (all notes off)")) {
        AppEvent e;
        e.kind = AppEvent::Kind::AllNotesOff;
        e.part = partIndex;
        app.eventQueue.push(e);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Operadores");
    changed |= DrawOperatorTable(pp.voice, app.selectedOp);
    changed |= DrawOperatorDetail(pp.voice.op[app.selectedOp]);

    ImGui::SeparatorText("Algoritmo (preset de roteamento)");
    {
        static std::vector<std::string> names;
        static std::vector<const char*> namePtrs;
        if (names.empty()) {
            for (int n = 1; n <= kNumAlgorithms; ++n) {
                names.push_back(std::to_string(n) + ": " + algorithmName(n));
            }
            for (auto& s : names) namePtrs.push_back(s.c_str());
        }
        ImGui::SetNextItemWidth(500);
        ImGui::Combo("##algo", &app.selectedAlgo, namePtrs.data(), static_cast<int>(namePtrs.size()));
        ImGui::SameLine();
        if (ImGui::Button("Aplicar algoritmo")) {
            applyAlgorithm(pp.voice, app.selectedAlgo + 1);
            changed = true;
        }
        ImGui::TextDisabled("Design proprio de 32 algoritmos p/ 8 operadores (nao e a lista oficial do DX7 "
                             "nem do Montage) -- ver ARQUITETURA.txt secao 14. Aplicar sobrescreve a matriz "
                             "e o feedback de todos os operadores.");
    }
    DrawAlgorithmDiagram(pp.voice);

    changed |= DrawMatrix(pp.voice.matrix);
    changed |= DrawPeg(pp.voice.peg);
    changed |= DrawLfo("LFO 1", pp.voice.lfo1);
    changed |= DrawLfo("LFO 2", pp.voice.lfo2);
    changed |= DrawModMatrix(pp.voice.modMatrix);

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
    switch (t) {
        case GlobalFxType::Chorus: {
            auto& c = fx.chorus;
            ImGui::SliderFloat("Rate (Hz)", &c.rateHz, 0.05f, 5.0f, "%.2f", kLog);
            ImGui::SliderFloat("Profundidade", &c.depth, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Mix", &c.mix, 0.0f, 1.0f, "%.2f", kClamp);
            break;
        }
        case GlobalFxType::Phaser: {
            auto& ph = fx.phaser;
            ImGui::SliderInt("Notches", &ph.notches, 1, 8, "%d", kClamp);
            ImGui::SliderFloat("Rate (Hz)", &ph.rateHz, 0.0f, 5.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Freq. minima (Hz)", &ph.freqMinHz, 20.0f, 5000.0f, "%.0f", kLog);
            ImGui::SliderFloat("Freq. maxima (Hz)", &ph.freqMaxHz, 20.0f, 10000.0f, "%.0f", kLog);
            ImGui::SliderFloat("Razao entre notches", &ph.freqRatio, 1.0f, 4.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Largura do notch (Hz)", &ph.notchWidthHz, 10.0f, 5000.0f, "%.0f", kLog);
            ImGui::SliderFloat("Profundidade", &ph.depth, 0.0f, 2.0f, "%.2f", kClamp);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 = seco, 1 = phaser, 2 = vibrato (so a cadeia all-pass)");
            ImGui::SliderFloat("Feedback", &ph.feedback, -0.99f, 0.99f, "%.2f", kClamp);
            ImGui::Checkbox("Inverter", &ph.invert);
            break;
        }
        case GlobalFxType::Flanger: {
            auto& f = fx.flanger;
            ImGui::SliderFloat("Rate (Hz)", &f.rateHz, 0.0f, 5.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Atraso minimo (ms)", &f.delayMs, 0.05f, 10.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Varredura (ms)", &f.sweepMs, 0.0f, 10.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Profundidade", &f.depth, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Feedback", &f.feedback, -0.95f, 0.95f, "%.2f", kClamp);
            ImGui::Checkbox("Inverter", &f.invert);
            break;
        }
        case GlobalFxType::Delay: {
            auto& d = fx.delay;
            ImGui::SliderFloat("Tempo esquerdo (s)", &d.timeLeftSec, 0.01f, 2.0f, "%.3f", kClamp);
            ImGui::SliderFloat("Tempo direito (s)", &d.timeRightSec, 0.01f, 2.0f, "%.3f", kClamp);
            ImGui::SliderFloat("Feedback", &d.feedback, 0.0f, 0.95f, "%.2f", kClamp);
            ImGui::SliderFloat("Cross-feed (ping-pong)", &d.crossFeed, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Mix", &d.mix, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Corte de graves no loop (Hz)", &d.lowCutHz, 10.0f, 2000.0f, "%.0f", kLog);
            ImGui::SliderFloat("Corte de agudos no loop (Hz)", &d.highCutHz, 200.0f, 20000.0f, "%.0f", kLog);
            ImGui::SliderFloat("Drive (saturacao no loop)", &d.drive, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Wow & flutter", &d.wow, 0.0f, 1.0f, "%.2f", kClamp);
            break;
        }
        case GlobalFxType::Reverb: {
            auto& r = fx.reverb;
            static const char* kTypes[] = {"Room (Freeverb)", "Hall (Clouds)", "Plate (MVerb)"};
            int type = static_cast<int>(r.type);
            if (ImGui::Combo("Algoritmo", &type, kTypes, IM_ARRAYSIZE(kTypes))) r.type = static_cast<ReverbType>(type);
            ImGui::SliderFloat("Tamanho / decaimento", &r.roomSize, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Amortecimento", &r.damping, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Mix", &r.mix, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Largura", &r.width, 0.0f, 1.0f, "%.2f", kClamp);
            ImGui::SliderFloat("Pre-delay (ms)", &r.predelayMs, 0.0f, 250.0f, "%.0f", kClamp);
            ImGui::BeginDisabled(r.type != ReverbType::Room);
            ImGui::Checkbox("Freeze (cauda infinita)", &r.freeze);
            ImGui::EndDisabled();
            if (r.type != ReverbType::Room && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("So no algoritmo Room (modo freeze do Freeverb)");
            break;
        }
        case GlobalFxType::Eq: {
            auto& e = fx.eq;
            ImGui::SliderFloat("Graves: freq. (Hz)", &e.lowFreqHz, 20.0f, 1000.0f, "%.0f", kLog);
            ImGui::SliderFloat("Graves: ganho (dB)", &e.lowGainDb, -18.0f, 18.0f, "%.1f", kClamp);
            ImGui::SliderFloat("Medios: freq. (Hz)", &e.midFreqHz, 100.0f, 10000.0f, "%.0f", kLog);
            ImGui::SliderFloat("Medios: ganho (dB)", &e.midGainDb, -18.0f, 18.0f, "%.1f", kClamp);
            ImGui::SliderFloat("Medios: Q", &e.midQ, 0.1f, 10.0f, "%.2f", kLog);
            ImGui::SliderFloat("Agudos: freq. (Hz)", &e.highFreqHz, 1000.0f, 20000.0f, "%.0f", kLog);
            ImGui::SliderFloat("Agudos: ganho (dB)", &e.highGainDb, -18.0f, 18.0f, "%.1f", kClamp);
            if (ImGui::SmallButton("Zerar ganhos")) e.lowGainDb = e.midGainDb = e.highGainDb = 0.0f;
            break;
        }
        case GlobalFxType::Compressor: {
            auto& c = fx.compressor;
            ImGui::SliderFloat("Threshold (dB)", &c.thresholdDb, -60.0f, 0.0f, "%.1f", kClamp);
            ImGui::SliderFloat("Ratio", &c.ratio, 1.0f, 20.0f, "%.1f:1", kLog);
            ImGui::SliderFloat("Attack (ms)", &c.attackMs, 0.1f, 100.0f, "%.1f", kLog);
            ImGui::SliderFloat("Release (ms)", &c.releaseMs, 1.0f, 500.0f, "%.0f", kLog);
            ImGui::SliderFloat("Knee (dB)", &c.kneeDb, 0.0f, 8.0f, "%.1f", kClamp);
            ImGui::SliderFloat("Makeup (dB)", &c.makeupDb, 0.0f, 30.0f, "%.1f", kClamp);
            float gr = c.enabled ? app.synth.compressorGainReductionDb() : 0.0f;
            char text[32];
            std::snprintf(text, sizeof text, "-%.1f dB", gr);
            ImGui::ProgressBar(std::clamp(gr / 24.0f, 0.0f, 1.0f), ImVec2(220, 0), text);
            ImGui::SameLine();
            ImGui::TextUnformatted("reducao de ganho");
            break;
        }
    }
}

void DrawGlobalFxPanel(AppState& app) {
    auto& fx = app.synth.globalFx();

    ImGui::SeparatorText("Cadeia de efeitos (ordem de processamento)");
    ImGui::TextDisabled("O sinal passa de cima pra baixo; use as setas pra reordenar. Depois da cadeia vem o volume geral e o limitador.");
    for (int i = 0; i < kNumGlobalFx; ++i) {
        GlobalFxType t = fx.order[i];
        const auto& info = globalFxInfo(t);
        ImGui::PushID(i);
        ImGui::Text("%d.", i + 1);
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) std::swap(fx.order[i], fx.order[i - 1]);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == kNumGlobalFx - 1);
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) std::swap(fx.order[i], fx.order[i + 1]);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox(info.name, &globalFxEnabled(fx, t));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.origin);
        ImGui::PopID();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Ordem padrao")) std::copy(std::begin(kDefaultGlobalFxOrder), std::end(kDefaultGlobalFxOrder), fx.order);

    for (int i = 0; i < kNumGlobalFx; ++i) {
        GlobalFxType t = fx.order[i];
        const auto& info = globalFxInfo(t);
        bool on = globalFxEnabled(fx, t);
        char header[64];
        std::snprintf(header, sizeof header, "%d. %s%s###fxhdr%d", i + 1, info.name, on ? "" : "  (desligado)", static_cast<int>(t));
        if (ImGui::CollapsingHeader(header, on ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::PushID(static_cast<int>(t) + 100);
            ImGui::TextDisabled("%s", info.origin);
            ImGui::Checkbox("Ligado", &globalFxEnabled(fx, t));
            ImGui::PushItemWidth(260);
            DrawGlobalFxParams(app, fx, t);
            ImGui::PopItemWidth();
            ImGui::PopID();
        }
    }
}

void DrawTopBar(AppState& app) {
    ImGui::Text("Audio: %s | %u Hz | period %u | formato %s | xruns %ld",
                app.audioOut.isRunning() ? "ativo" : "PARADO",
                app.audioOut.sampleRate(), app.audioOut.periodFrames(),
                app.audioOut.deviceFormatName(), app.audioOut.underruns());
    if (!app.audioOut.isRunning() && !app.audioOut.lastError().empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "(%s)", app.audioOut.lastError().c_str());
    }

    ImGui::Text("MIDI: %s", app.midiIn.isOpen()
                                ? "porta ALSA aberta (conecte seu controlador com 'aconnect' ou no seu patchbay)"
                                : "indisponivel (so teclado do PC)");

    ImGui::Text("Teclado do PC: oitava base nota %d (Z/X descem/sobem) | toca como teclado MIDI no canal",
                app.piano.baseNote());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt("##pcch", &app.pcKeyboardChannel, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toca todas as partes habilitadas em omni (canal 0) ou neste canal,\n"
                          "igual a um controlador MIDI ligado nesse canal.");
    ImGui::Text("Vozes ativas: %d / %d", app.synth.activeVoiceCount(), kMaxVoices);

    float l = app.meterL.load(std::memory_order_relaxed);
    float r = app.meterR.load(std::memory_order_relaxed);
    ImGui::Text("Nivel:");
    ImGui::SameLine();
    ImGui::ProgressBar(std::clamp(l, 0.0f, 1.0f), ImVec2(150, 0), "L");
    ImGui::SameLine();
    ImGui::ProgressBar(std::clamp(r, 0.0f, 1.0f), ImVec2(150, 0), "R");

    // Volume geral em dB (-60..+6); o minimo (-60) e mudo de verdade.
    // Estado so da UI: o motor guarda ganho linear (Synth::setMasterVolume).
    static float volumeDb = 0.0f;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    const char* fmt = volumeDb <= -60.0f ? "mudo" : "%.1f dB";
    if (ImGui::SliderFloat("Volume geral", &volumeDb, -60.0f, 6.0f, fmt, ImGuiSliderFlags_AlwaysClamp))
        app.synth.setMasterVolume(volumeDb <= -60.0f ? 0.0f : dbToLinear(volumeDb));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { // duplo clique volta pra 0dB
        volumeDb = 0.0f;
        app.synth.setMasterVolume(1.0f);
    }
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

} // namespace

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("dx7fm standalone", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           1280, 800, SDL_WINDOW_RESIZABLE);
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
    ImGui::StyleColorsDark();
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

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("dx7fm", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        DrawTopBar(app);
        ImGui::Separator();

        if (ImGui::BeginTabBar("parts")) {
            for (int p = 0; p < kNumParts; ++p) {
                char tabLabel[16];
                std::snprintf(tabLabel, sizeof(tabLabel), "Parte %c", 'A' + p);
                if (ImGui::BeginTabItem(tabLabel)) {
                    DrawPartPanel(app, p);
                    ImGui::EndTabItem();
                }
            }
            if (ImGui::BeginTabItem("FX Globais")) {
                DrawGlobalFxPanel(app);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
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
