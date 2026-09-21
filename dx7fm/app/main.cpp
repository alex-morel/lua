// dx7fm standalone: app de audio real-time (ALSA) com UI leve (Dear ImGui +
// SDL2/SDLRenderer2, sem exigir GPU/OpenGL) e entrada de notas via MIDI
// (ALSA sequencer) e/ou teclado do computador. Ver ARQUITETURA.txt secao 13.
#include "dx7_params.h"
#include "dx7_dsp.h"
#include "dx7_engine.h"
#include "dx7_alloc.h"
#include "dx7_globalfx.h"
#include "dx7_patch_io.h"

#include "alsa_audio.h"
#include "alsa_midi.h"
#include "event_queue.h"
#include "keyboard_piano.h"

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

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

    std::atomic<float> meterL{0.0f};
    std::atomic<float> meterR{0.0f};

    KeyboardPiano piano;
    std::unordered_map<SDL_Scancode, int> heldPcNotes;
    int currentPart = 0;
    int selectedOp = 0;
    char patchPath[kNumParts][256];
};

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
            applyAppEvent(app.synth, e, enabledTmp, channelTmp);
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
            } else if (ev.data1 == 123) { // all notes off
                ae.kind = AppEvent::Kind::AllNotesOff;
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
            if (ImGui::SliderInt("##rate", &v, 0, 99)) { eg.rate[i] = static_cast<uint8_t>(v); changed = true; }
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
            if (ImGui::SliderInt("##level", &v, 0, 99)) { eg.level[i] = static_cast<uint8_t>(v); changed = true; }
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
    static const char* kWave[] = {"Sine", "Triangle", "Saw", "Square", "Noise",
                                   "HalfSine", "AbsSine", "QuarterSine", "AlternatingSine",
                                   "CamelSine", "SquaredSine", "LogSaw"};
    static const char* kFx[] = {"Bypass", "LowPass", "BandPass", "HighPass", "RingMod",
                                 "Saturate", "Crush", "FreqShift", "Comb"};
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
            if (ImGui::Selectable(std::to_string(i).c_str(), selectedOp == i,
                                   ImGuiSelectableFlags_SpanAllColumns)) {
                selectedOp = i;
            }

            ImGui::TableSetColumnIndex(1);
            int wave = static_cast<int>(op.waveform);
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("##wave", &wave, kWave, 12)) { op.waveform = static_cast<Waveform>(wave); changed = true; }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(80);
            if (op.fixedFreq) {
                changed |= ImGui::DragFloat("##hz", &op.fixedHz, 1.0f, 0.1f, 20000.0f, "%.1f Hz");
            } else {
                changed |= ImGui::DragFloat("##ratio", &op.coarseRatio, 0.01f, 0.01f, 32.0f, "%.2f");
            }

            ImGui::TableSetColumnIndex(3);
            changed |= ImGui::Checkbox("##fixed", &op.fixedFreq);

            ImGui::TableSetColumnIndex(4);
            {
                int lvl = op.outputLevel;
                ImGui::SetNextItemWidth(70);
                if (ImGui::SliderInt("##lvl", &lvl, 0, 99)) { op.outputLevel = static_cast<uint8_t>(lvl); changed = true; }
            }

            ImGui::TableSetColumnIndex(5);
            ImGui::SetNextItemWidth(70);
            changed |= ImGui::SliderFloat("##fb", &op.feedback, 0.0f, 1.0f, "%.2f");

            ImGui::TableSetColumnIndex(6);
            int fx = static_cast<int>(op.fxType);
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##fx", &fx, kFx, 9)) { op.fxType = static_cast<FxType>(fx); changed = true; }

            ImGui::TableSetColumnIndex(7);
            ImGui::SetNextItemWidth(80);
            changed |= ImGui::DragFloat("##fx1", &op.fxParam1, 5.0f, 0.0f, 20000.0f, "%.0f");

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return changed;
}

bool DrawOperatorDetail(OperatorParams& op) {
    bool changed = false;
    ImGui::SeparatorText("Detalhe do operador selecionado");

    changed |= ImGui::DragFloat("Fine (cents)", &op.fineCents, 1.0f, -100.0f, 100.0f);
    changed |= ImGui::DragFloat("Detune (cents)", &op.detuneCents, 1.0f, -100.0f, 100.0f);
    changed |= ImGui::SliderFloat("FX Param2", &op.fxParam2, 0.0f, 16.0f, "%.2f");
    changed |= ImGui::SliderFloat("Phase distortion", &op.phaseDistortion, 0.0f, 1.0f, "%.2f");
    {
        int rm = op.ringModSource;
        if (ImGui::SliderInt("Ring mod source (op)", &rm, -1, kNumOps - 1)) { op.ringModSource = rm; changed = true; }
    }
    {
        int rs = op.rateScaling;
        if (ImGui::SliderInt("Rate scaling", &rs, 0, 7)) { op.rateScaling = static_cast<uint8_t>(rs); changed = true; }
    }
    {
        int vs = op.velocitySens;
        if (ImGui::SliderInt("Velocity sens", &vs, 0, 7)) { op.velocitySens = static_cast<uint8_t>(vs); changed = true; }
    }
    {
        int la = op.lfoAmpModSens;
        if (ImGui::SliderInt("LFO amp sens", &la, 0, 3)) { op.lfoAmpModSens = static_cast<uint8_t>(la); changed = true; }
    }
    {
        int lp = op.lfoPitchModSens;
        if (ImGui::SliderInt("LFO pitch sens", &lp, 0, 7)) { op.lfoPitchModSens = static_cast<uint8_t>(lp); changed = true; }
    }
    changed |= ImGui::SliderFloat("PEG depth", &op.pegDepth, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::Checkbox("PEG afeta freq fixa", &op.pegAffectsFixed);
    changed |= ImGui::Checkbox("fxEG acompanha release do ampEG", &op.fxEGReleaseWithAmp);

    ImGui::Spacing();
    {
        int bp = op.levelScaling.breakpoint;
        if (ImGui::SliderInt("Level scaling: breakpoint", &bp, 0, 127)) {
            op.levelScaling.breakpoint = static_cast<uint8_t>(bp);
            changed = true;
        }
    }
    {
        int ld = op.levelScaling.leftDepth;
        if (ImGui::SliderInt("  esquerda (dB/oitava)", &ld, -12, 12)) {
            op.levelScaling.leftDepth = static_cast<int8_t>(ld);
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("  esquerda exp##lsL", &op.levelScaling.leftExp);
    {
        int rd = op.levelScaling.rightDepth;
        if (ImGui::SliderInt("  direita (dB/oitava)", &rd, -12, 12)) {
            op.levelScaling.rightDepth = static_cast<int8_t>(rd);
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("  direita exp##lsR", &op.levelScaling.rightExp);

    changed |= DrawEG("Amp EG", op.ampEG, "0-99");
    changed |= DrawEG("FX EG", op.fxEG, "0-99, modula FX1");
    return changed;
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
                changed |= ImGui::DragFloat("##m", &m.mod[i][j], 0.01f, -1.0f, 1.0f, "%.2f");
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
        changed |= ImGui::SliderFloat(("op" + std::to_string(i)).c_str(), &m.outputMix[i], 0.0f, 1.0f, "%.2f");
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
    changed |= ImGui::DragFloat("Rate (Hz)", &lfo.rate, 0.05f, 0.01f, 50.0f, "%.2f");
    int wave = static_cast<int>(lfo.wave);
    if (ImGui::Combo("Forma", &wave, kWave, 6)) { lfo.wave = static_cast<LfoWave>(wave); changed = true; }
    changed |= ImGui::DragFloat("Delay (s)", &lfo.delaySec, 0.01f, 0.0f, 10.0f, "%.2f");
    changed |= ImGui::Checkbox("Key sync", &lfo.keySync);
    changed |= ImGui::SliderFloat("Pitch depth", &lfo.pitchDepth, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Amp depth", &lfo.ampDepth, 0.0f, 1.0f, "%.2f");
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
    changed |= ImGui::DragFloat("Range (oitavas)", &peg.rangeOctaves, 0.1f, 0.1f, 8.0f, "%.1f");
    return changed;
}

bool DrawModMatrix(ModRoute (&routes)[kNumModRoutes]) {
    bool changed = false;
    static const char* kSrc[] = {"None", "LFO1", "LFO2", "ModWheel", "ChannelAT", "PolyAT", "PitchBend"};
    static const char* kDst[] = {"None", "OperatorLevel", "OperatorFx1", "OperatorFeedback", "VoicePitch"};
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
            if (ImGui::Combo("##dst", &dst, kDst, 5)) { r.dest = static_cast<ModDest>(dst); changed = true; }
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(50);
            changed |= ImGui::SliderInt("##op", &r.opIndex, 0, kNumOps - 1);
            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(80);
            changed |= ImGui::SliderFloat("##depth", &r.depth, -1.0f, 1.0f, "%.2f");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return changed;
}

void DrawPartPanel(AppState& app, int partIndex) {
    auto& pp = app.synth.part(partIndex);
    bool changed = false;

    changed |= ImGui::Checkbox("Habilitada", &pp.enabled);
    ImGui::SameLine();
    {
        int ch = pp.midiChannel;
        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderInt("Canal MIDI (0=omni)", &ch, 0, 16)) { pp.midiChannel = ch; changed = true; }
    }

    static const char* kMode[] = {"Poly", "Mono", "Unison"};
    int mode = static_cast<int>(pp.voiceMode);
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("Modo de voz", &mode, kMode, 3)) { pp.voiceMode = static_cast<VoiceMode>(mode); changed = true; }

    {
        int tr = pp.transpose;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::SliderInt("Transpose", &tr, -24, 24)) { pp.transpose = static_cast<int8_t>(tr); changed = true; }
    }

    changed |= ImGui::SliderFloat("Nivel", &pp.level, 0.0f, 1.5f, "%.2f");
    ImGui::SameLine();
    changed |= ImGui::SliderFloat("Pan", &pp.pan, -1.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Bend range (semitons)", &pp.bendRangeSemis, 0.0f, 24.0f, "%.1f");

    {
        int lo = pp.loKey, hi = pp.hiKey;
        if (ImGui::SliderInt("Key range low", &lo, 0, 127)) { pp.loKey = static_cast<uint8_t>(lo); changed = true; }
        if (ImGui::SliderInt("Key range high", &hi, 0, 127)) { pp.hiKey = static_cast<uint8_t>(hi); changed = true; }
    }

    if (pp.voiceMode == VoiceMode::Mono || pp.voiceMode == VoiceMode::Unison) {
        changed |= ImGui::Checkbox("Glide", &pp.glideEnabled);
        ImGui::SameLine();
        changed |= ImGui::DragFloat("Glide (s)", &pp.glideTimeSec, 0.01f, 0.0f, 10.0f, "%.2f");
        changed |= ImGui::Checkbox("Glide so em legato", &pp.glideLegatoOnly);
        changed |= ImGui::Checkbox("Reusa envelope em legato", &pp.monoLegatoEnvReuse);
    }
    if (pp.voiceMode == VoiceMode::Unison) {
        int uc = pp.unisonCount;
        if (ImGui::SliderInt("Unison: vozes", &uc, 1, 8)) { pp.unisonCount = uc; changed = true; }
        changed |= ImGui::DragFloat("Unison: detune (cents)", &pp.unisonDetuneCents, 0.5f, 0.0f, 50.0f, "%.1f");
        changed |= ImGui::SliderFloat("Unison: espalhamento", &pp.unisonSpread, 0.0f, 1.0f, "%.2f");
    }

    {
        int rv = pp.reservedVoices;
        if (ImGui::SliderInt("Vozes reservadas", &rv, 0, kMaxVoices)) { pp.reservedVoices = rv; changed = true; }
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("Arquivo do patch (.json)", app.patchPath[partIndex], sizeof(app.patchPath[partIndex]));
    if (ImGui::Button("Salvar patch")) {
        std::filesystem::create_directories("patches");
        if (!savePartFile(app.patchPath[partIndex], pp)) {
            std::fprintf(stderr, "[patch] falha ao salvar %s\n", app.patchPath[partIndex]);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Carregar patch")) {
        if (loadPartFile(app.patchPath[partIndex], pp)) changed = true;
        else std::fprintf(stderr, "[patch] falha ao carregar %s\n", app.patchPath[partIndex]);
    }
    ImGui::SameLine();
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
    changed |= DrawMatrix(pp.voice.matrix);
    changed |= DrawPeg(pp.voice.peg);
    changed |= DrawLfo("LFO 1", pp.voice.lfo1);
    changed |= DrawLfo("LFO 2", pp.voice.lfo2);
    changed |= DrawModMatrix(pp.voice.modMatrix);

    if (changed) {
        app.synth.recompile(partIndex);
        syncRoutingCache(app, partIndex);
    }
}

void DrawGlobalFxPanel(AppState& app) {
    auto& fx = app.synth.globalFx();
    ImGui::SeparatorText("Chorus");
    ImGui::Checkbox("Ligado##chorus", &fx.chorus.enabled);
    ImGui::SliderFloat("Rate (Hz)##chorus", &fx.chorus.rateHz, 0.05f, 5.0f, "%.2f");
    ImGui::SliderFloat("Depth (ms)##chorus", &fx.chorus.depthMs, 0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Delay base (ms)##chorus", &fx.chorus.baseDelayMs, 1.0f, 40.0f, "%.1f");
    ImGui::SliderFloat("Mix##chorus", &fx.chorus.mix, 0.0f, 1.0f, "%.2f");
    {
        int voices = fx.chorus.voices;
        if (ImGui::SliderInt("Vozes##chorus", &voices, 1, 4)) fx.chorus.voices = voices;
    }

    ImGui::SeparatorText("Delay");
    ImGui::Checkbox("Ligado##delay", &fx.delay.enabled);
    ImGui::SliderFloat("Tempo esquerdo (s)", &fx.delay.timeLeftSec, 0.01f, 2.0f, "%.3f");
    ImGui::SliderFloat("Tempo direito (s)", &fx.delay.timeRightSec, 0.01f, 2.0f, "%.3f");
    ImGui::SliderFloat("Feedback##delay", &fx.delay.feedback, 0.0f, 0.95f, "%.2f");
    ImGui::SliderFloat("Cross-feed##delay", &fx.delay.crossFeed, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Mix##delay", &fx.delay.mix, 0.0f, 1.0f, "%.2f");

    ImGui::SeparatorText("Reverb");
    ImGui::Checkbox("Ligado##reverb", &fx.reverb.enabled);
    ImGui::SliderFloat("Room size", &fx.reverb.roomSize, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Damping", &fx.reverb.damping, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Mix##reverb", &fx.reverb.mix, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Width", &fx.reverb.width, 0.0f, 1.0f, "%.2f");
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

    ImGui::Text("Voz do teclado do PC: Parte %c | Oitava base: nota %d (Z/X descem/sobem)",
                'A' + app.currentPart, app.piano.baseNote());
    ImGui::Text("Vozes ativas: %d / %d", app.synth.activeVoiceCount(), kMaxVoices);

    float l = app.meterL.load(std::memory_order_relaxed);
    float r = app.meterR.load(std::memory_order_relaxed);
    ImGui::Text("Nivel:");
    ImGui::SameLine();
    ImGui::ProgressBar(std::clamp(l, 0.0f, 1.0f), ImVec2(150, 0), "L");
    ImGui::SameLine();
    ImGui::ProgressBar(std::clamp(r, 0.0f, 1.0f), ImVec2(150, 0), "R");
}

// --- teclado do PC como piano ---------------------------------------------

void handlePianoKeyDown(AppState& app, SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_Z) { app.piano.shiftOctave(-1); return; }
    if (sc == SDL_SCANCODE_X) { app.piano.shiftOctave(1); return; }
    if (!app.piano.isPianoKey(sc)) return;
    if (app.heldPcNotes.count(sc)) return; // ja soando (evita re-trigger por key repeat do SO)
    int note = app.piano.noteForScancode(sc);
    app.heldPcNotes[sc] = note;
    AppEvent e;
    e.kind = AppEvent::Kind::NoteOn;
    e.part = app.currentPart;
    e.note = note;
    e.velocity = 100;
    app.eventQueue.push(e);
}

void handlePianoKeyUp(AppState& app, SDL_Scancode sc) {
    auto it = app.heldPcNotes.find(sc);
    if (it == app.heldPcNotes.end()) return;
    AppEvent e;
    e.kind = AppEvent::Kind::NoteOff;
    e.part = app.currentPart;
    e.note = it->second;
    app.eventQueue.push(e);
    app.heldPcNotes.erase(it);
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
    for (int p = 0; p < kNumParts; ++p) {
        std::snprintf(app.patchPath[p], sizeof(app.patchPath[p]), "patches/part_%c.json", 'a' + p);
    }

    app.synth.init(kSampleRate, kMaxVoices, /*oversample=*/1);
    for (int p = 0; p < kNumParts; ++p) syncRoutingCache(app, p);

    app.audioOut.start("default", kSampleRate, kPeriodFrames,
                        [&app](float* buf, int frames) { audioRenderCallback(app, buf, frames); });
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
            if (!io.WantCaptureKeyboard) {
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) handlePianoKeyDown(app, event.key.keysym.scancode);
                else if (event.type == SDL_KEYUP) handlePianoKeyUp(app, event.key.keysym.scancode);
            }
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
                    app.currentPart = p;
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
