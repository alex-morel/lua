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
#include "additive_editor.h"
#include "op_fx_ui.h"
#include "wavetable_editor.h"
#include "settings.h"
#include "midi_clock.h"
#include <chrono>
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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
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
    // [2026-10-24] configuracoes do app (settings.json) e o render do audio (pra religar)
    dx7fm_app::AppSettings settings;
    std::function<void(float*, int)> render;
    // [2026-10-24] MIDI learn. ccTarget: copia de settings.cc pra thread MIDI
    // (um atomico por CC, -1 = sem alvo) -- a thread MIDI nunca le o vector.
    // learnTarget: alvo esperando um CC (-1 = nenhum); a thread MIDI grava o CC
    // recebido em learnedCc e a UI registra em settings no proximo quadro.
    std::atomic<int> ccTarget[128];
    std::atomic<int> learnTarget{-1};
    std::atomic<int> learnedCc{-1};
    int learnPending = -1; // so UI: alvo do learn em andamento
    float macroSeen[kNumParts][VoiceParams::kNumMacros] = {}; // ultimo valor ao vivo visto pela UI
    // [2026-10-24] clock MIDI: estimador so na thread MIDI; BPM e hora do ultimo
    // pulso publicados em atomicos; followClock = settings.midiClock (copia da UI)
    dx7fm_app::ClockEstimator clockEst;
    std::atomic<float> clockBpm{0.0f};
    std::atomic<int64_t> clockLastNs{0};
    std::atomic<bool> followClock{true};
    int selectedOp = 0;
    int selectedAlgo = 0; // indice (0-based) no combo de presets de algoritmo
    int algoSlot = 0;          // [2026-10-24] slot do morph de algoritmos em edicao (0 = slot 1)
    bool algoMorphView = false; // diagrama mostra a posicao do morph em vez do slot em edicao
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
    // [ADICIONADO em 2026-10-24] desfazer/refazer por parte: estados completos da
    // parte (as ondas da Wavetable sao shared_ptr: copiar nao duplica memoria).
    // committed = ultimo estado empilhado; pending = houve edicao ainda nao empilhada.
    struct PartHistory {
        std::vector<PartParams> undo, redo;
        PartParams committed;
        bool pending = false;
    };
    // A/B: o "outro" lado (so o som; o roteamento da parte fica) e o historico dele
    struct ABState {
        PartParams other;
        PartHistory otherHist;
        bool haveOther = false;
        int side = 0; // 0 = A, 1 = B
    };
    PartHistory hist[kNumParts];
    ABState ab[kNumParts];
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

// Threads extras pra renderizar vozes: ~1 por nucleo FISICO (hw/2 com SMT),
// menos o da audio thread, ate 7. DX7FM_THREADS=n forca (0 = sem threads).
static int audioWorkerThreads() {
    if (const char* env = std::getenv("DX7FM_THREADS")) return std::clamp(std::atoi(env), 0, 63);
    int hw = static_cast<int>(std::thread::hardware_concurrency());
    return std::clamp(hw / 2 - 1, hw > 1 ? 1 : 0, 7);
}

inline int64_t monoNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// clock MIDI chegando (ultimo pulso ha menos de 0.5s) e o usuario quer segui-lo
bool clockActive(const AppState& app) {
    return app.followClock.load(std::memory_order_relaxed) && app.clockBpm.load(std::memory_order_relaxed) > 0.0f &&
           monoNowNs() - app.clockLastNs.load(std::memory_order_relaxed) < 500'000'000;
}

void audioRenderCallback(AppState& app, float* interleaved, int frames) {
    if (clockActive(app)) app.synth.setTempoBpm(std::round(app.clockBpm.load(std::memory_order_relaxed) * 10.0f) / 10.0f);
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

    // renderBlock: vozes em paralelo nas threads do synth (setWorkerThreads).
    // Os buffers so alocam no primeiro bloco (ou se o period crescer).
    static std::vector<float> bufL, bufR;
    if (bufL.size() < static_cast<size_t>(frames)) { bufL.resize(static_cast<size_t>(frames)); bufR.resize(static_cast<size_t>(frames)); }
    app.synth.renderBlock(bufL.data(), bufR.data(), frames);
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float l = bufL[static_cast<size_t>(i)], r = bufR[static_cast<size_t>(i)];
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

// tabela da thread MIDI a partir de settings.cc (so a UI chama)
void rebuildCcTable(AppState& app) {
    for (auto& t : app.ccTarget) t.store(-1, std::memory_order_relaxed);
    for (const auto& m : app.settings.cc) app.ccTarget[m.cc].store(static_cast<int>(m.target), std::memory_order_relaxed);
}

// volume geral: knob em dB (-60 = mudo .. +6); CC/knob 0..1 nessa escala
inline float volumeDbFromGain(float g) { return g <= 0.0f ? -60.0f : std::clamp(20.0f * std::log10(g), -60.0f, 6.0f); }
inline float gainFromVolumeDb(float db) { return db <= -60.0f ? 0.0f : dbToLinear(db); }

void onMidiEvent(AppState& app, const MidiEvent& ev) {
    switch (ev.type) { // [2026-10-24] tempo real: clock/start (nao viram eventos de nota)
        case MidiEvent::Type::Clock: {
            const int64_t now = monoNowNs();
            app.clockEst.tick(static_cast<double>(now) * 1e-9);
            app.clockBpm.store(static_cast<float>(app.clockEst.bpm()), std::memory_order_relaxed);
            app.clockLastNs.store(now, std::memory_order_relaxed);
            return;
        }
        case MidiEvent::Type::Start: {
            app.clockEst.reset();
            AppEvent t;
            t.kind = AppEvent::Kind::TransportStart;
            app.eventQueue.push(t);
            return;
        }
        case MidiEvent::Type::Continue:
        case MidiEvent::Type::Stop:
            return;
        default: break;
    }
    AppEvent ae;
    ae.part = -1;
    ae.channel1to16 = ev.channel + 1; // convencao do motor: 0=omni, 1..16=canal especifico
    if (ev.type == MidiEvent::Type::ControlChange && ev.data1 >= 0 && ev.data1 < 128) {
        // [2026-10-24] MIDI learn: o proximo CC vira o do alvo em espera
        if (app.learnTarget.load(std::memory_order_acquire) >= 0) {
            app.learnedCc.store(ev.data1, std::memory_order_release);
            app.learnTarget.store(-1, std::memory_order_release);
            return;
        }
        const int target = app.ccTarget[ev.data1].load(std::memory_order_relaxed);
        if (target >= 0) { // CC aprendido: tem prioridade sobre o mapeamento fixo (mod wheel etc.)
            using dx7fm_app::CcTarget;
            const float v = std::clamp(ev.data2 / 127.0f, 0.0f, 1.0f);
            if (target <= static_cast<int>(CcTarget::Macro4)) {
                ae.kind = AppEvent::Kind::Macro;
                ae.note = target;
                ae.floatValue = v;
                app.eventQueue.push(ae);
            } else if (target == static_cast<int>(CcTarget::MasterVolume)) {
                app.synth.setMasterVolume(gainFromVolumeDb(-60.0f + 66.0f * v)); // atomico
            } else if (target == static_cast<int>(CcTarget::Bpm)) {
                app.synth.setTempoBpm(std::round(20.0f + 280.0f * v)); // atomico
            }
            return;
        }
    }
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
        case MidiEvent::Type::Clock:
        case MidiEvent::Type::Start:
        case MidiEvent::Type::Continue:
        case MidiEvent::Type::Stop:
            return; // tratados no inicio da funcao
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

// botao "..." da coluna de efeito da tabela: rola ate o card EFEITO DO OPERADOR
bool g_scrollToFxCard = false;

bool DrawOperatorTable(VoiceParams& v, int& selectedOp) {
    // As ultimas 7 sao a reconstrucao propria das formas de onda AFM da
    // SY77/TX81Z (ver dx7_dsp.h e ARQUITETURA.txt secao 5).
    // os combos de onda/FX mostram a lista INTEIRA (ultimo argumento do Combo):
    // com o limite padrao de 8 itens, "Fold" e "Additive" ficavam escondidos
    // atras de uma rolagem no fim da lista
    static const char* kWave[] = {"Sine", "Triangle", "Saw", "Square", "Noise",
                                   "HalfSine", "AbsSine", "QuarterSine", "AlternatingSine",
                                   "CamelSine", "SquaredSine", "LogSaw", "Additive", "Wavetable"};
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
        ImGui::TableSetupColumn("Ajuste do FX");
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
                // FX1/FX2 mudam de significado entre efeitos (corte em Hz,
                // drive, bits...): valores iniciais sensatos pro efeito novo
                dx7fm_app::opfx::applyDefaults(op.fxType, newType, op.fxParam1, op.fxParam2);
                if (newType == FxType::RingMod && (op.ringModSource < 0 || op.ringModSource == i)) op.ringModSource = i == 0 ? 1 : 0;
                op.fxType = newType;
                changed = true;
            }

            ImGui::TableSetColumnIndex(7);
            // parametro principal do efeito editavel aqui; "..." leva ao card completo
            changed |= ui::OperatorFxQuickEdit(v, i, 120);
            ImGui::SameLine();
            if (ImGui::SmallButton("...")) { selectedOp = i; g_scrollToFxCard = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("todos os ajustes do efeito, com grafico (card EFEITO DO OPERADOR %d)", i + 1);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
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

    if (op.waveform == Waveform::Additive) {
        static ui::AdditiveEditorState additiveUi; // estado so de UI (A/B, vista, parcial selecionado)
        changed |= ui::AdditiveEditor(op, opIndex, ui::opColor(opIndex), additiveUi);
    }
    if (op.waveform == Waveform::Wavetable) {
        static ui::WavetableEditorState wavetableUi; // estado so de UI (onda em edicao, aba, WAV aberto)
        changed |= ui::WavetableEditor(op, ui::opColor(opIndex), wavetableUi);
    }
    return changed;
}

// [ADICIONADO em 2026-10-20] Card do efeito do operador: painel de
// op_fx_ui.h (escolha, grafico, knobs) + o FX EG, com o que ele controla.
bool DrawOperatorFx(VoiceParams& v, int opIndex) {
    using dx7fm_app::opfx::egSweeps;
    auto& op = v.op[opIndex];
    bool changed = ui::OperatorFxPanel(v, opIndex);

    ImGui::Spacing();
    // "additive": operador com morph A->B (Additive ou Wavetable), que o FX EG tambem move
    const bool additive = op.waveform == Waveform::Additive || op.waveform == Waveform::Wavetable, sweeps = egSweeps(op.fxType);
    const char* morphOf = op.waveform == Waveform::Wavetable ? "Wavetable" : "Additive";
    const char* what = sweeps ? (op.fxType == FxType::Fold ? "varre o drive" : "varre o corte (20% no nivel 0, 100% no 99)") : nullptr;
    if (what && additive) ImGui::TextDisabled("FX EG: %s e move o morph A->B da %s", what, morphOf);
    else if (what) ImGui::TextDisabled("FX EG: %s", what);
    else if (additive) ImGui::TextDisabled("FX EG: move o morph A->B da %s (nao afeta este efeito)", morphOf);
    else ImGui::TextDisabled("FX EG: nao afeta este efeito (so filtros, Wavefolder e o morph da Additive/Wavetable)");
    ImGui::BeginDisabled(!sweeps && !additive);
    changed |= DrawEGKnobs("fxeg", op.fxEG.rate, op.fxEG.level, ui::opColor(opIndex, 200));
    changed |= ImGui::Checkbox("FX EG acompanha o release do Amp EG", &op.fxEGReleaseWithAmp);
    ImGui::EndDisabled();
    return changed;
}
// [ADICIONADO em 2026-09-21] Desenha a topologia de roteamento como um
// grafo de caixas+setas, generico o bastante pra visualizar QUALQUER
// AlgorithmMatrix (preset ou editada a mao na grade 8x8 acima) -- nao
// depende de layout topologico dinamico (que teria que lidar com ciclos
// de feedback arbitrarios): as 8 caixas ficam em posicoes FIXAS (grade
// 4x2), so as setas entre elas mudam. Ver ARQUITETURA.txt secao 13/14.
// [2026-10-24] desenha a matriz `m` (o slot em edicao ou o ponto interpolado
// do morph); weighted: espessura/opacidade das setas pelo peso (|PM|).
void DrawAlgorithmDiagram(const VoiceParams& v, const AlgorithmMatrix& m, int& selectedOp, bool weighted = false) {
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
    ImVec2 mouse = ImGui::GetIO().MousePos;

    dl->AddLine(ImVec2(origin.x + 4, outputY), ImVec2(origin.x + canvasW, outputY), ui::col::kAccent, 2.0f);
    dl->AddText(ImVec2(origin.x + canvasW - 38.0f, outputY - 16.0f), ui::col::kAccent, "saida");

    // arestas (setas) primeiro, pra ficarem atras das caixas; na cor do operador que MODULA
    for (int i = 0; i < kNumOps; ++i) {
        for (int j = 0; j < kNumOps; ++j) {
            if (i == j) continue;
            bool hasMod = m.mod[i][j] != 0.0f;
            bool hasRing = m.ring[i][j];
            if (!hasMod && !hasRing) continue;
            ImVec2 from(boxPos[j].x + boxW * 0.5f, boxPos[j].y + boxH * 0.5f);
            ImVec2 to(boxPos[i].x + boxW * 0.5f, boxPos[i].y + boxH * 0.5f);
            const float w = weighted && !hasRing ? std::min(1.0f, std::fabs(m.mod[i][j])) : 1.0f;
            ImU32 color = hasRing ? ui::col::kAccent2 : ui::opColor(j, static_cast<int>(40 + 150 * w));
            dl->AddLine(from, to, color, hasRing ? 2.5f : 0.8f + 2.0f * w);
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
        bool isCarrier = m.outputMix[i] > 0.0f;
        bool hasFeedback = (m.mod[i][i] != 0.0f) || (v.op[i].feedback > 0.0f);
        ImVec2 p0 = boxPos[i];
        ImVec2 p1(p0.x + boxW, p0.y + boxH);
        bool hot = mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y;
        if (clicked && hot) selectedOp = i;
        dl->AddRectFilled(p0, p1, ui::opColor(i, isCarrier ? static_cast<int>(40 + 50 * (weighted ? std::min(1.0f, m.outputMix[i]) : 1.0f)) : 40), 6.0f);
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
// [ADICIONADO em 2026-10-24] Morph entre algoritmos: fileira de slots (qual
// editar, +/-), regua da posicao (arrastavel, com os slots marcados), Saltar
// e "Ver posicao" (o diagrama mostra o ponto interpolado).
bool DrawAlgoSlots(AppState& app, VoiceParams& v) {
    bool changed = false;
    int n = v.activeAlgos();
    app.algoSlot = std::clamp(app.algoSlot, 0, n - 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Algoritmos");
    ImGui::SetItemTooltip("Ate 8 algoritmos por patch: a posicao do morph passa por eles em ordem,\n"
                          "interpolando as conexoes. Clique num numero pra editar aquele algoritmo.");
    for (int s = 0; s < n; ++s) {
        ImGui::SameLine(0, 4);
        char b[8];
        std::snprintf(b, sizeof b, "%d", s + 1);
        const bool sel = s == app.algoSlot;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? ui::col::kAccent : ui::rgb(40, 44, 56));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ui::rgb(14, 16, 22) : ui::col::kText);
        ImGui::PushID(s);
        if (ImGui::SmallButton(b)) { app.algoSlot = s; app.algoMorphView = false; }
        ImGui::PopID();
        ImGui::PopStyleColor(2);
    }
    ImGui::SameLine(0, 10);
    ImGui::BeginDisabled(n >= kMaxAlgoSlots);
    if (ImGui::SmallButton("+")) { // novo slot = copia do que esta em edicao, logo depois dele
        for (int s = n; s > app.algoSlot + 1; --s) v.algo(s) = v.algo(s - 1);
        v.algo(app.algoSlot + 1) = v.algo(app.algoSlot);
        v.numAlgos = static_cast<uint8_t>(n + 1);
        ++app.algoSlot;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Novo algoritmo (copia do que esta em edicao)");
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(n <= 1);
    if (ImGui::SmallButton("-")) { // remove o slot em edicao
        for (int s = app.algoSlot; s < n - 1; ++s) v.algo(s) = v.algo(s + 1);
        v.algo(n - 1) = AlgorithmMatrix{};
        v.numAlgos = static_cast<uint8_t>(n - 1);
        app.algoSlot = std::min(app.algoSlot, n - 2);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Remove o algoritmo em edicao");
    n = v.activeAlgos();
    if (n < 2) return changed;

    // regua da posicao: 0 = algoritmo 1, 1 = o ultimo; arrastar move o morph
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = std::max(160.0f, ImGui::GetContentRegionAvail().x - 170.0f), h = 26.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##morphruler", ImVec2(w, h));
    const float x0 = p0.x + 10, x1 = p0.x + w - 10, cy = p0.y + 9;
    if (ImGui::IsItemActive()) {
        float pos = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        if (pos != v.algoMorph) { v.algoMorph = pos; changed = true; }
        app.algoMorphView = true;
    }
    dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), ui::rgb(90, 96, 110), 3.0f);
    for (int s = 0; s < n; ++s) {
        float x = x0 + (x1 - x0) * static_cast<float>(s) / static_cast<float>(n - 1);
        dl->AddCircleFilled(ImVec2(x, cy), s == app.algoSlot ? 5.0f : 3.5f, s == app.algoSlot ? ui::col::kAccent : ui::rgb(150, 156, 170));
        char b[4];
        std::snprintf(b, sizeof b, "%d", s + 1);
        dl->AddText(ImVec2(x - ImGui::CalcTextSize(b).x * 0.5f, cy + 5), ui::col::kTextDim, b);
    }
    const float hx = x0 + (x1 - x0) * v.algoMorph;
    dl->AddTriangleFilled(ImVec2(hx, cy - 2), ImVec2(hx - 6, cy - 10), ImVec2(hx + 6, cy - 10), ui::col::kAccent2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("posicao do morph: %.0f%% (arraste). A matriz de modulacao soma no destino AlgoMorph.", v.algoMorph * 100.0f);
    ImGui::SameLine();
    ImGui::BeginGroup();
    changed |= ImGui::Checkbox("Saltar", &v.algoStep);
    ImGui::SetItemTooltip("Pula de algoritmo em algoritmo em vez de passar pelo meio");
    ImGui::Checkbox("Ver posicao", &app.algoMorphView);
    ImGui::SetItemTooltip("Diagrama mostra o ponto do morph (setas mais grossas = conexao mais forte)\nem vez do algoritmo em edicao");
    ImGui::EndGroup();
    return changed;
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
    static const char* kWave[] = {"Triangle", "SawUp", "SawDown", "Square", "Sine", "SampleHold", "SmoothRandom"};
    ImGui::PushID(label);
    int wave = static_cast<int>(lfo.wave);
    ImGui::SetNextItemWidth(92);
    if (ImGui::Combo("##forma", &wave, kWave, IM_ARRAYSIZE(kWave), IM_ARRAYSIZE(kWave))) { lfo.wave = static_cast<LfoWave>(wave); changed = true; }
    ImGui::SetItemTooltip("Forma de onda do LFO (SmoothRandom = aleatorio com transicao suave)");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Key sync", &lfo.keySync);
    ImGui::SetItemTooltip("Reinicia o ciclo a cada nota");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Sync", &lfo.tempoSync);
    ImGui::SetItemTooltip("Velocidade em divisoes do BPM (knob BPM no topo) em vez de Hz");
    ui::KnobStyle ks;
    ks.color = ui::col::kAccent2;
    ks.size = 32.0f;
    ks.cellWidth = 48.0f; // 5 knobs em meia coluna
    ui::KnobStyle ksLog = ks;
    ksLog.logarithmic = true;
    if (lfo.tempoSync) {
        int div = lfo.division; // 4/1 (lento) .. 1/32 (rapido); "." = pontuada, "T" = tercina
        if (ui::KnobInt("Divisao", &div, 0, kNumLfoDivisions - 1, 6, lfoDivisionName(div), ks)) { lfo.division = static_cast<uint8_t>(div); changed = true; }
    } else {
        changed |= ui::Knob("Rate", &lfo.rate, 0.01f, 50.0f, 5.0f, "%.2f Hz", ksLog);
    }
    const float gap = 4.0f; // 5 knobs em meia coluna: espaco menor entre eles
    ImGui::SameLine(0, gap); changed |= ui::Knob("Delay", &lfo.delaySec, 0.0f, 10.0f, 0.0f, "%.2f s", ks);
    ImGui::SameLine(0, gap); changed |= ui::Knob("Pitch", &lfo.pitchDepth, 0.0f, 1.0f, 0.0f, "%.2f", ks);
    ImGui::SameLine(0, gap); changed |= ui::Knob("Amp", &lfo.ampDepth, 0.0f, 1.0f, 0.0f, "%.2f", ks);
    ImGui::SameLine(0, gap);
    {
        int steps = lfo.steps;
        if (ui::KnobInt("Degraus", &steps, 0, 32, 0, steps < 2 ? "suave" : "%d", ks)) { lfo.steps = static_cast<uint8_t>(steps == 1 ? 0 : steps); changed = true; }
        ImGui::SetItemTooltip("Quantiza o LFO em N niveis (escada); 0 = suave");
    }
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
// [ADICIONADO em 2026-10-24] Macros: 4 knobs por patch, fontes Macro1..4 da
// matriz. O valor ao vivo e um controlador da parte (Synth::setMacro, via fila
// de eventos); o do patch acompanha (fica salvo). CC aprendido mexe no ao vivo.
void LearnButton(AppState& app, dx7fm_app::CcTarget target, float width); // junto do popup de configuracoes

bool DrawMacros(AppState& app, int part) {
    auto& v = app.synth.part(part).voice;
    bool changed = false;
    ImGui::TextDisabled("Fontes Macro1..4 da matriz de modulacao. Nome editavel; botao embaixo = MIDI learn.");
    ui::KnobStyle ks;
    ks.color = ui::col::kAccent;
    ks.cellWidth = 96.0f;
    for (int k = 0; k < VoiceParams::kNumMacros; ++k) {
        if (k) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(k);
        // o valor ao vivo so muda por CC ou por um evento nosso ja aplicado: so
        // adota quando ELE mudou (senao o knob voltaria enquanto o evento do
        // proprio knob ainda esta na fila)
        const float live = app.synth.macroValue(part, k);
        if (live != app.macroSeen[part][k]) {
            app.macroSeen[part][k] = live;
            if (std::fabs(live - v.macro[k]) > 1e-6f) { v.macro[k] = live; changed = true; }
        }
        float shown = v.macro[k];
        if (ui::Knob(v.macroName[k].c_str(), &shown, 0.0f, 1.0f, 0.0f, "%.2f", ks)) {
            v.macro[k] = shown;
            AppEvent e;
            e.kind = AppEvent::Kind::Macro;
            e.part = part;
            e.note = k;
            e.floatValue = shown;
            app.eventQueue.push(e);
            changed = true;
        }
        char name[25];
        std::snprintf(name, sizeof name, "%s", v.macroName[k].c_str());
        ImGui::SetNextItemWidth(ks.cellWidth);
        if (ImGui::InputText("##name", name, sizeof name)) { v.macroName[k] = name; changed = true; }
        LearnButton(app, static_cast<dx7fm_app::CcTarget>(k), ks.cellWidth);
        ImGui::PopID();
        ImGui::EndGroup();
    }
    return changed;
}

// [ADICIONADO em 2026-10-24] Arpejador do patch (o motor fica em Synth:
// arpKeyDown/arpProcessDue; os passos caem na grade do BPM).
bool DrawArp(ArpParams& a) {
    bool changed = false;
    changed |= ImGui::Checkbox("Ligado##arp", &a.on);
    ImGui::SetItemTooltip("As teclas da parte montam o padrao; o arpejador toca uma nota por passo no tempo do BPM");
    ImGui::SameLine();
    static const char* kModes[] = {"Sobe", "Desce", "Sobe/desce", "Aleatorio", "Como tocado"};
    int mode = static_cast<int>(a.mode);
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##arpmode", &mode, kModes, IM_ARRAYSIZE(kModes))) { a.mode = static_cast<ArpMode>(mode); changed = true; }
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Latch", &a.latch);
    ImGui::SetItemTooltip("Soltar todas as teclas mantem o arpejo; a proxima tecla comeca um padrao novo");
    ui::KnobStyle ks;
    ks.color = ui::col::kAccent;
    int div = a.division;
    if (ui::KnobInt("Divisao", &div, 0, kNumLfoDivisions - 1, 12, lfoDivisionName(div), ks)) { a.division = static_cast<uint8_t>(div); changed = true; }
    ImGui::SameLine();
    int oct = a.octaves;
    if (ui::KnobInt("Oitavas", &oct, 1, 4, 1, "%d", ks)) { a.octaves = static_cast<uint8_t>(oct); changed = true; }
    ImGui::SameLine();
    float gate = a.gate * 100.0f;
    if (ui::Knob("Gate", &gate, 5.0f, 100.0f, 50.0f, "%.0f%%", ks)) { a.gate = gate / 100.0f; changed = true; }
    ImGui::SetItemTooltip("Quanto do passo a nota fica soando (100%% = legato)");
    ImGui::SameLine();
    float swing = a.swing * 100.0f;
    if (ui::Knob("Swing", &swing, 0.0f, 75.0f, 0.0f, "%.0f%%", ks)) { a.swing = swing / 100.0f; changed = true; }
    ImGui::SetItemTooltip("Atrasa os passos impares (33%% = tercina)");
    return changed;
}

// manda os valores de macro do patch pro motor (ao carregar/zerar um patch)
void sendPatchMacros(AppState& app, int part) {
    for (int k = 0; k < VoiceParams::kNumMacros; ++k) {
        AppEvent e;
        e.kind = AppEvent::Kind::Macro;
        e.part = part;
        e.note = k;
        e.floatValue = app.synth.part(part).voice.macro[k];
        app.eventQueue.push(e);
    }
}

// [ADICIONADO em 2026-10-22] Envelopes de modulacao (estilo DX, comecam no
// level 4 como o PEG): uma aba por envelope, curva + knobs. So fontes da
// matriz (Env1..Env3) -- sozinhos nao fazem nada.
void DrawEnvCurve(const EGParams& eg, ImU32 color, ImVec2 size, bool loop = false, const char* syncDiv = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, ui::rgb(14, 16, 22), 6.0f);
    const auto t = dx7fm_app::egTimes(eg, eg.level[3]);
    const float hold = std::max(0.15f, 0.25f * t.untilSustain()); // trecho de sustain so pra visualizar
    const float total = t.untilSustain() + hold + t.stage[3];
    auto pt = [&](float sec, float level) {
        return ImVec2(p0.x + 4 + (size.x - 8) * sec / std::max(total, 1e-3f), p1.y - 4 - (size.y - 8) * level / 99.0f);
    };
    float sec = 0.0f;
    ImVec2 pts[6];
    pts[0] = pt(0.0f, eg.level[3]);
    for (int k = 0; k < 3; ++k) { sec += t.stage[k]; pts[k + 1] = pt(sec, eg.level[k]); }
    sec += hold;
    pts[4] = pt(sec, eg.level[2]);
    pts[5] = pt(sec + t.stage[3], eg.level[3]);
    dl->AddPolyline(pts, 6, color, 0, 2.0f);
    dl->AddLine(ImVec2(pts[4].x, p0.y + 3), ImVec2(pts[4].x, p1.y - 3), ui::rgb(255, 255, 255, 50));
    if (loop) { // trecho que repete (do inicio do estagio 1 ao sustain), com seta de volta
        const ImU32 lc = (color & 0x00FFFFFF) | (0x30u << 24);
        dl->AddRectFilled(ImVec2(pts[0].x, p0.y + 2), ImVec2(pts[3].x, p1.y - 2), lc);
        const float y = p1.y - 8; // seta de volta no pe do trecho, longe da curva e do texto
        dl->AddLine(ImVec2(pts[3].x, y), ImVec2(pts[0].x + 6, y), color, 1.5f);
        dl->AddTriangleFilled(ImVec2(pts[0].x, y), ImVec2(pts[0].x + 7, y - 4), ImVec2(pts[0].x + 7, y + 4), color);
        dl->AddText(ImVec2(pts[3].x + 4, y - ImGui::GetTextLineHeight() * 0.5f), color, "loop");
    }
    char lbl[48];
    if (syncDiv) std::snprintf(lbl, sizeof lbl, "sync: ciclo do loop = %s do BPM", syncDiv);
    else std::snprintf(lbl, sizeof lbl, "%.2fs ate o sustain", t.untilSustain());
    dl->AddText(ImVec2(p0.x + 6, p0.y + 3), ui::col::kTextDim, lbl);
}

bool DrawModEnvelopes(VoiceParams& v) {
    bool changed = false;
    ImGui::TextDisabled("Fontes Env1..Env3 da matriz de modulacao (0..1 = level/99)");
    if (ImGui::BeginTabBar("##modenvs")) {
        for (int k = 0; k < kNumModEnvs; ++k) {
            char tab[16];
            std::snprintf(tab, sizeof tab, "Env %d", k + 1);
            if (ImGui::BeginTabItem(tab)) {
                ImGui::PushID(k);
                auto& e = v.modEnv[k];
                DrawEnvCurve(e, ui::col::kAccent2, ImVec2(ImGui::GetContentRegionAvail().x, 70.0f), e.loop,
                             e.tempoSync ? lfoDivisionName(e.division) : nullptr);
                changed |= ImGui::Checkbox("Loop", &e.loop);
                ImGui::SetItemTooltip("Enquanto a nota esta presa, volta do sustain (L3) pro estagio 1: L3 -> L1 -> L2 -> L3 ...\n"
                                      "O note off sai do loop e faz o release normal.");
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Sync", &e.tempoSync);
                ImGui::SetItemTooltip("O envelope anda mais rapido ou devagar pro ciclo L3 -> L1 -> L2 -> L3 durar a divisao do BPM");
                if (e.tempoSync) {
                    ImGui::SameLine();
                    int div = e.division;
                    ImGui::SetNextItemWidth(90);
                    if (ImGui::SliderInt("##div", &div, 0, kNumLfoDivisions - 1, lfoDivisionName(div), ImGuiSliderFlags_AlwaysClamp)) {
                        e.division = static_cast<uint8_t>(div);
                        changed = true;
                    }
                    ImGui::SetItemTooltip("duracao do ciclo (4/1 = 4 compassos ... 1/32)");
                }
                static const uint8_t kDefRate[4] = {60, 45, 45, 45}, kDefLevel[4] = {99, 70, 50, 0};
                changed |= DrawEGKnobs("modenv", v.modEnv[k].rate, v.modEnv[k].level, ui::col::kAccent2, true, kDefRate, kDefLevel);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    return changed;
}

bool DrawModMatrix(ModRoute (&routes)[kNumModRoutes]) {
    bool changed = false;
    static const char* kSrc[] = {"None", "LFO1", "LFO2", "ModWheel", "ChannelAT", "PolyAT", "PitchBend", "Nota", "Velocity",
                                 "LFO3", "LFO4", "Env1", "Env2", "Env3", "Macro1", "Macro2", "Macro3", "Macro4"};
    static const char* kDst[] = {"None", "OperatorLevel", "OperatorFx1", "OperatorFeedback", "VoicePitch", "OperatorMorph",
                                 "VoicePan", "VoiceLevel", "OperatorPitch", "OperatorFx2", "LfoRate", "LfoDepth", "AlgoMorph"};
    static const char* kDstTip[] = {"", "nivel do operador x (1 + quantidade)", "FX1 do operador x (1 + quantidade): corte, taxa, afinacao, drive",
                                    "feedback do operador + quantidade", "afinacao da voz inteira, quantidade x 12 semitons",
                                    "morph A->B da Additive/Wavetable + quantidade", "pan da voz + quantidade (-1 esquerda, +1 direita)",
                                    "volume da voz x (1 + quantidade)", "afinacao SO do operador, quantidade x 12 semitons",
                                    "FX2 do operador x (1 + quantidade): Q, drive, bits, feedback, simetria",
                                    "velocidade do LFO escolhido x 2^(3 x quantidade): +-3 oitavas",
                                    "saida do LFO escolhido x (1 + quantidade): -1 desliga",
                                    "posicao do morph entre algoritmos (card Algoritmo) + quantidade"};
    // so as rotas em uso + 1 vazia (as 16 com o checkbox)
    static bool showAll = false;
    int lastUsed = -1;
    for (int i = 0; i < kNumModRoutes; ++i)
        if (routes[i].src != ModSource::None || routes[i].dest != ModDest::None) lastUsed = i;
    const int visible = showAll ? kNumModRoutes : std::min(kNumModRoutes, std::max(lastUsed + 2, 4));
    ImGui::TextDisabled("LFOs/envelopes/controladores/nota/velocity -> parametros");
    ImGui::SameLine();
    ImGui::Checkbox("Mostrar as 16", &showAll);
    if (ImGui::BeginTable("modmatrix", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Fonte");
        ImGui::TableSetupColumn("Destino");
        ImGui::TableSetupColumn("Op");
        ImGui::TableSetupColumn("Profundidade");
        ImGui::TableHeadersRow();
        for (int i = 0; i < visible; ++i) {
            auto& r = routes[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", i + 1);
            ImGui::TableSetColumnIndex(1);
            int src = static_cast<int>(r.src);
            ImGui::SetNextItemWidth(110);
            if (ImGui::Combo("##src", &src, kSrc, IM_ARRAYSIZE(kSrc), IM_ARRAYSIZE(kSrc))) { r.src = static_cast<ModSource>(src); changed = true; }
            if (ImGui::IsItemHovered() && (r.src == ModSource::KeyNote || r.src == ModSource::Velocity))
                ImGui::SetTooltip(r.src == ModSource::KeyNote ? "Nota: 0 no C4, +1 cinco oitavas acima (C9), -1 cinco abaixo"
                                                              : "Velocity: 0..1 (velocity / 127), fixa durante a nota");
            ImGui::TableSetColumnIndex(2);
            int dst = static_cast<int>(r.dest);
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("##dst", &dst, kDst, IM_ARRAYSIZE(kDst), IM_ARRAYSIZE(kDst))) {
                r.dest = static_cast<ModDest>(dst);
                if (modDestPerLfo(r.dest)) r.opIndex = std::min(r.opIndex, kNumLfos - 1);
                changed = true;
            }
            if (ImGui::IsItemHovered() && r.dest != ModDest::None) ImGui::SetTooltip("%s", kDstTip[static_cast<int>(r.dest)]);
            ImGui::TableSetColumnIndex(3);
            static const char* kOps[] = {"Op1", "Op2", "Op3", "Op4", "Op5", "Op6", "Op7", "Op8"};
            static const char* kLfos[] = {"LFO1", "LFO2", "LFO3", "LFO4"};
            ImGui::SetNextItemWidth(62);
            if (modDestPerLfo(r.dest)) { // o alvo e um LFO, nao um operador
                if (ImGui::Combo("##lfo", &r.opIndex, kLfos, kNumLfos)) changed = true;
            } else {
                ImGui::BeginDisabled(!modDestPerOperator(r.dest));
                if (ImGui::Combo("##op", &r.opIndex, kOps, kNumOps)) changed = true;
                ImGui::EndDisabled();
            }
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
    app.hist[part].committed = app.synth.part(part);
}

// --- desfazer/refazer + A/B ([2026-10-24]) -----------------------------------
constexpr size_t kMaxUndo = 100;

bool sameState(const PartParams& a, const PartParams& b) {
    return partParamsToJson(a).dump() == partParamsToJson(b).dump();
}

// empilha o estado anterior (committed) se a parte mudou desde ele
void historyCommit(AppState& app, int part) {
    auto& h = app.hist[part];
    const PartParams& now = app.synth.part(part);
    h.pending = false;
    if (sameState(h.committed, now)) return;
    h.undo.push_back(h.committed);
    if (h.undo.size() > kMaxUndo) h.undo.erase(h.undo.begin());
    h.redo.clear();
    h.committed = now;
}

void sendPatchMacros(AppState& app, int part);
void publishPart(AppState& app, int part);

// coloca um estado inteiro na parte e publica (motor, macros, indicador de "nao salvo")
void restorePart(AppState& app, int part, const PartParams& p) {
    app.synth.part(part) = p;
    publishPart(app, part);
    sendPatchMacros(app, part);
    auto& ui = app.patchUi[part];
    std::snprintf(ui.name, sizeof(ui.name), "%s", p.name.c_str());
    ui.dirty = patchFingerprint(app.synth.part(part)) != ui.baseline;
}

void undoPart(AppState& app, int part) {
    auto& h = app.hist[part];
    if (h.pending) historyCommit(app, part);
    if (h.undo.empty()) return;
    h.redo.push_back(app.synth.part(part));
    restorePart(app, part, h.undo.back());
    h.undo.pop_back();
    h.committed = app.synth.part(part);
}

void redoPart(AppState& app, int part) {
    auto& h = app.hist[part];
    if (h.redo.empty()) return;
    h.undo.push_back(app.synth.part(part));
    restorePart(app, part, h.redo.back());
    h.redo.pop_back();
    h.committed = app.synth.part(part);
}

// troca A <-> B: so o som (o roteamento da parte continua o mesmo); cada lado tem o seu historico
void switchAB(AppState& app, int part) {
    auto& ab = app.ab[part];
    historyCommit(app, part);
    PartParams current = app.synth.part(part);
    if (!ab.haveOther) { ab.other = current; ab.otherHist = {}; ab.otherHist.committed = current; ab.haveOther = true; }
    PartParams next = current;
    copyPatchKeepingRouting(next, ab.other);
    restorePart(app, part, next);
    ab.other = current;
    std::swap(app.hist[part], ab.otherHist);
    app.hist[part].committed = app.synth.part(part);
    ab.side ^= 1;
}

void copyToOtherSide(AppState& app, int part) {
    auto& ab = app.ab[part];
    ab.other = app.synth.part(part);
    ab.otherHist = {};
    ab.otherHist.committed = ab.other;
    ab.haveOther = true;
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
    historyCommit(app, part);
    copyPatchKeepingRouting(app.synth.part(part), loaded);
    publishPart(app, part);
    sendPatchMacros(app, part);
    historyCommit(app, part); // carregar e um passo de desfazer
    std::snprintf(ui.name, sizeof(ui.name), "%s", app.synth.part(part).name.c_str());
    ui.currentFile = path;
    ui.selected = path;
    markPatchClean(app, part);
    setPatchStatus(ui, true, "Carregado " + path.filename().string());
}

void doInitPatch(AppState& app, int part) {
    auto& ui = app.patchUi[part];
    historyCommit(app, part);
    copyPatchKeepingRouting(app.synth.part(part), PartParams{});
    publishPart(app, part);
    sendPatchMacros(app, part);
    historyCommit(app, part);
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
    { // [2026-10-24] desfazer/refazer + A/B
        auto& h = app.hist[part];
        ImGui::BeginDisabled(h.undo.empty() && !h.pending);
        if (ImGui::SmallButton("Desfazer")) undoPart(app, part);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ctrl+Z (%zu passos)", h.undo.size() + (h.pending ? 1 : 0));
        ImGui::SameLine();
        ImGui::BeginDisabled(h.redo.empty());
        if (ImGui::SmallButton("Refazer")) redoPart(app, part);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Ctrl+Y ou Ctrl+Shift+Z");
        ImGui::SameLine(0, 18);
        auto& ab = app.ab[part];
        for (int sd = 0; sd < 2; ++sd) {
            if (sd) ImGui::SameLine(0, 2);
            const bool on = ab.side == sd;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ui::col::kAccent : ui::rgb(40, 44, 56));
            ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::rgb(14, 16, 22) : ui::col::kText);
            if (ImGui::SmallButton(sd ? "B" : "A") && !on) switchAB(app, part);
            ImGui::PopStyleColor(2);
        }
        ImGui::SetItemTooltip("Compara duas versoes do som: o outro lado guarda o som e o historico dele");
        ImGui::SameLine();
        if (ImGui::SmallButton(ab.side == 0 ? "Copiar A -> B" : "Copiar B -> A")) copyToOtherSide(app, part);
    }
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
        changed |= DrawAlgoSlots(app, pp.voice);
        const int nAlgo = pp.voice.activeAlgos();
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 80.0f));
        ImGui::Combo("##algo", &app.selectedAlgo, namePtrs.data(), static_cast<int>(namePtrs.size()));
        ImGui::SetItemTooltip("Design proprio de 32 algoritmos p/ 8 operadores (nao e a lista oficial do DX7\n"
                              "nem do Montage) -- ver ARQUITETURA.txt secao 14.");
        ImGui::SameLine();
        if (ImGui::Button("Aplicar")) {
            if (nAlgo == 1) applyAlgorithm(pp.voice, app.selectedAlgo + 1);
            else applyAlgorithmToMatrix(pp.voice.algo(app.algoSlot), app.selectedAlgo + 1);
            changed = true;
        }
        if (nAlgo == 1) ImGui::SetItemTooltip("Sobrescreve a matriz e o feedback de todos os operadores");
        else ImGui::SetItemTooltip("Sobrescreve so o algoritmo %d (o feedback do preset vai pra diagonal dele, pra fazer morph junto;\n"
                                   "o Feedback de cada operador na tabela continua valendo em todos os algoritmos)", app.algoSlot + 1);
        if (nAlgo > 1 && app.algoMorphView) {
            // ponto interpolado na posicao do knob (como o motor faz, sem a modulacao)
            const auto& v = pp.voice;
            float x = v.algoMorph * static_cast<float>(nAlgo - 1);
            int sl = std::min(static_cast<int>(x), nAlgo - 2);
            float t = x - static_cast<float>(sl);
            if (v.algoStep) t = t < 0.5f ? 0.0f : 1.0f;
            AlgorithmMatrix mid;
            const auto &a = v.algo(sl), &b = v.algo(sl + 1);
            for (int i = 0; i < kNumOps; ++i) {
                for (int j = 0; j < kNumOps; ++j) {
                    mid.mod[i][j] = a.mod[i][j] + (b.mod[i][j] - a.mod[i][j]) * t;
                    mid.ring[i][j] = t < 0.5f ? a.ring[i][j] : b.ring[i][j];
                }
                mid.outputMix[i] = a.outputMix[i] + (b.outputMix[i] - a.outputMix[i]) * t;
            }
            DrawAlgorithmDiagram(pp.voice, mid, app.selectedOp, true);
        } else {
            DrawAlgorithmDiagram(pp.voice, pp.voice.algo(app.algoSlot), app.selectedOp);
        }
    }
    ui::EndCard();

    if (ui::BeginCard("##card_macros", "MACROS", 0.0f, ui::col::kAccent)) changed |= DrawMacros(app, partIndex);
    ui::EndCard();
    if (ui::BeginCard("##card_arp", "ARPEJADOR", 0.0f, ui::col::kAccent)) changed |= DrawArp(pp.voice.arp);
    ui::EndCard();
    if (ui::BeginCard("##card_lfo", "LFOs", 0.0f, ui::col::kAccent2)) {
        // [2026-10-22] 4 LFOs (2x2). Pitch/Amp = vibrato/tremolo direto; os 4 tambem sao fontes da matriz
        if (ImGui::BeginTable("##lfos", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH)) {
            for (int k = 0; k < kNumLfos; ++k) {
                if (k % 2 == 0) ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(k % 2);
                char name[8];
                std::snprintf(name, sizeof name, "LFO %d", k + 1);
                ImGui::TextDisabled("%s", name);
                ImGui::SameLine();
                ImGui::PushID(k);
                changed |= ImGui::Checkbox("Global", &pp.voice.lfo(k).global);
                ImGui::PopID();
                ImGui::SetItemTooltip("Um LFO so pra parte inteira: todas as notas ondulam juntas (acordes).\n"
                                      "Com Sync, fica alinhado ao compasso (e ao Start do clock MIDI).\n"
                                      "Key sync reinicia na primeira nota tocada; LfoRate da matriz nao se aplica.");
                changed |= DrawLfo(name, pp.voice.lfo(k));
            }
            ImGui::EndTable();
        }
    }
    ui::EndCard();
    if (ui::BeginCard("##card_modenv", "ENVELOPES DE MODULACAO", 0.0f, ui::col::kAccent2)) changed |= DrawModEnvelopes(pp.voice);
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

    std::snprintf(title, sizeof title, "EFEITO DO OPERADOR %d", sel + 1);
    if (g_scrollToFxCard) { ImGui::SetScrollHereY(0.0f); g_scrollToFxCard = false; } // botao "..." da tabela
    if (ui::BeginCard("##card_opfx", title, 0.0f, ui::opColor(sel))) changed |= DrawOperatorFx(pp.voice, sel);
    ui::EndCard();

    if (ui::BeginCard("##card_peg", "PITCH EG", 0.0f, ui::col::kAccent2)) changed |= DrawPeg(pp.voice.peg);
    ui::EndCard();

    if (ui::BeginCard("##card_matrix", nullptr)) {
        char hdr[64];
        if (pp.voice.activeAlgos() > 1) std::snprintf(hdr, sizeof hdr, "MATRIZ DE ALGORITMO (avancado) -- algoritmo %d###mtx", app.algoSlot + 1);
        else std::snprintf(hdr, sizeof hdr, "MATRIZ DE ALGORITMO (avancado)###mtx");
        if (ImGui::CollapsingHeader(hdr)) changed |= DrawMatrix(pp.voice.algo(std::min(app.algoSlot, pp.voice.activeAlgos() - 1)));
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
        app.hist[partIndex].pending = true;
    }
    // a edicao vira um passo de desfazer quando termina (arrastar um knob = 1 passo)
    if (app.hist[partIndex].pending && !ImGui::IsAnyItemActive()) historyCommit(app, partIndex);

    // Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z (fora de campos de texto)
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redoPart(app, partIndex); else undoPart(app, partIndex); }
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redoPart(app, partIndex);
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

// [ADICIONADO em 2026-10-24] Troca o oversampling: o motor precisa de um
// init novo (buffers/decimadores por voz), entao para o audio, reinicia o
// synth na mesma taxa (as notas soando sao cortadas; patches e partes ficam),
// religa e salva em settings.json.
void applyOversample(AppState& app, int os) {
    const unsigned rate = app.audioOut.sampleRate();
    const bool wasRunning = app.audioOut.isRunning();
    app.audioOut.stop();
    app.synth.init(rate, kMaxVoices, os);
    for (int p = 0; p < kNumParts; ++p) app.synth.allNotesOff(p);
    if (wasRunning) app.audioOut.start("default", rate, kPeriodFrames, app.render);
    app.settings.oversample = os;
    dx7fm_app::saveSettings(app.settings);
}

// MIDI learn: botao que mostra o CC do alvo ("CC 21"), "aprender" ou
// "mova um CC..." durante o learn. Clique = aprender; direito = esquecer.
void LearnButton(AppState& app, dx7fm_app::CcTarget target, float width) {
    const int t = static_cast<int>(target);
    char lbl[32];
    const int cc = app.settings.ccFor(target);
    if (app.learnPending == t) std::snprintf(lbl, sizeof lbl, "mova um CC...##l%d", t);
    else if (cc >= 0) std::snprintf(lbl, sizeof lbl, "CC %d##l%d", cc, t);
    else std::snprintf(lbl, sizeof lbl, "aprender##l%d", t);
    const bool waiting = app.learnPending == t;
    if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, ui::col::kAccent2);
    if (ImGui::Button(lbl, ImVec2(width, 0))) {
        if (waiting) { app.learnPending = -1; app.learnTarget.store(-1); }
        else { app.learnPending = t; app.learnedCc.store(-1); app.learnTarget.store(t, std::memory_order_release); }
    }
    if (waiting) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && cc >= 0) {
        app.settings.forget(target);
        dx7fm_app::saveSettings(app.settings);
        rebuildCcTable(app);
    }
    ImGui::SetItemTooltip("Clique e mova um knob/fader do controlador MIDI pra ligar o CC dele aqui.\nBotao direito: esquecer o CC.");
}

// a cada quadro (UI): registra o CC que a thread MIDI capturou
void PollMidiLearn(AppState& app) {
    const int cc = app.learnedCc.exchange(-1, std::memory_order_acq_rel);
    if (cc < 0 || app.learnPending < 0) return;
    app.settings.learn(static_cast<dx7fm_app::CcTarget>(app.learnPending), cc);
    app.learnPending = -1;
    dx7fm_app::saveSettings(app.settings);
    rebuildCcTable(app);
}

void DrawSettingsPopup(AppState& app) {
    if (!ImGui::BeginPopup("Configuracoes")) return;
    ImGui::TextUnformatted("Oversampling (anti-aliasing)");
    ImGui::TextDisabled("Calcula as vozes a 2x/4x a taxa e filtra: menos aliasing em FM agressiva e notas agudas.\n"
                        "Custa ~2x/~4x de CPU nas vozes. Trocar corta as notas que estiverem soando.");
    int os = app.settings.oversample;
    for (int v : {1, 2, 4}) {
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "%dx", v);
        if (v != 1) ImGui::SameLine();
        if (ImGui::RadioButton(lbl, os == v) && os != v) applyOversample(app, v);
    }
    ImGui::Separator();
    if (ImGui::Checkbox("Seguir o clock MIDI quando ele chegar", &app.settings.midiClock)) dx7fm_app::saveSettings(app.settings);
    ImGui::Separator();
    ImGui::TextUnformatted("MIDI learn (CC -> controle)");
    ImGui::TextDisabled("As macros valem pra parte que recebe o canal do CC; volume e BPM sao globais.");
    for (int t = 0; t <= static_cast<int>(dx7fm_app::CcTarget::Bpm); ++t) {
        const auto target = static_cast<dx7fm_app::CcTarget>(t);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(dx7fm_app::ccTargetName(target));
        ImGui::SameLine(90);
        LearnButton(app, target, 140);
    }
    ImGui::TextDisabled("arquivo: %s", dx7fm_app::settingsPath().string().c_str());
    ImGui::EndPopup();
}

void DrawTopBar(AppState& app) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::rgb(24, 27, 35));
    ImGui::BeginChild("##header", ImVec2(0, 92), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginTable("##hdr", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("logo", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("saida", ImGuiTableColumnFlags_WidthFixed, 380.0f); // medidores (200) + knobs BPM e Volume
        ImGui::TableNextRow();

        // logo
        ImGui::TableSetColumnIndex(0);
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::col::kAccent);
        ImGui::TextUnformatted("dx7fm");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::TextDisabled("8-OP FM SYNTH");
        if (ImGui::SmallButton("Configuracoes")) ImGui::OpenPopup("Configuracoes");
        DrawSettingsPopup(app);

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
        float volumeDb = volumeDbFromGain(app.synth.masterVolume()); // CC aprendido tambem mexe
        ui::KnobStyle ks;
        ks.size = 34.0f;
        // [2026-10-22] BPM global dos LFOs com Sync (estado do app, nao do patch)
        float bpm = app.synth.tempoBpm();
        const bool ext = clockActive(app);
        ImGui::BeginDisabled(ext);
        if (ui::Knob(ext ? "BPM (MIDI)" : "BPM", &bpm, 20.0f, 300.0f, 120.0f, ext ? "%.1f" : "%.0f", ks)) app.synth.setTempoBpm(std::round(bpm));
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(ext ? "Seguindo o clock MIDI (desligue em Configuracoes)"
                                  : "Tempo dos LFOs/envelopes com Sync e do arpejador (todas as partes).\nArraste, ou botao direito pra digitar.");
        ImGui::SameLine();
        if (ui::Knob("Volume", &volumeDb, -60.0f, 6.0f, 0.0f, volumeDb <= -60.0f ? "mudo" : "%.1f dB", ks))
            app.synth.setMasterVolume(gainFromVolumeDb(volumeDb));
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
    PollMidiLearn(app);
    app.followClock.store(app.settings.midiClock, std::memory_order_relaxed);
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

    app.settings = dx7fm_app::loadSettings();
    rebuildCcTable(app);
    app.synth.init(kSampleRate, kMaxVoices, app.settings.oversample);
    app.synth.setWorkerThreads(audioWorkerThreads());
    // Por padrao so a parte A toca; B-D comecam desligadas (senao cada nota,
    // MIDI ou do teclado do PC, dispararia 4 camadas empilhadas -- todas em
    // omni). Liga-las e so marcar "Habilitada" na aba de cada uma.
    for (int p = 0; p < kNumParts; ++p) {
        app.synth.part(p).enabled = (p == 0);
        app.synth.recompile(p);
        syncRoutingCache(app, p);
        initPatchUi(app, p);
    }

    app.render = [&app](float* buf, int frames) { audioRenderCallback(app, buf, frames); };
    auto& render = app.render;
    if (app.audioOut.start("default", kSampleRate, kPeriodFrames, render) &&
        app.audioOut.sampleRate() != kSampleRate) {
        // o ALSA negociou outra taxa (set_rate_near): reinicia o synth nela,
        // senao tudo sai desafinado. Para o audio antes, pro init() nao
        // correr junto com renderStereo() na audio thread.
        unsigned actualRate = app.audioOut.sampleRate();
        std::fprintf(stderr, "[audio] dispositivo usa %u Hz (pedido %u Hz), ajustando o synth\n", actualRate, kSampleRate);
        app.audioOut.stop();
        app.synth.init(actualRate, kMaxVoices, app.settings.oversample);
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
            if (event.type == SDL_KEYDOWN && event.key.repeat == 0 && !io.WantCaptureKeyboard && !(SDL_GetModState() & KMOD_CTRL))
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
