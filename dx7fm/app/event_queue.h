#pragma once
#include "dx7_alloc.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dx7fm_app {

// Eventos de nota/controlador SO devem ser aplicados por UMA thread (ver
// nota em ARQUITETURA.txt secao 7C: Synth::noteOn/noteOff e os metodos de
// controlador mexem em PartRuntime::heldStack/activeSlots/noteToSlot, que
// NAO tem a garantia de thread-safety que dx7_alloc.h da pro timbre). Este
// app escolhe a AUDIO thread como essa unica thread: a UI thread (teclado
// do PC) e a thread de MIDI (dx7fm_app::AlsaMidiIn) so EMPILHAM eventos
// aqui; quem de fato chama synth.noteOn/noteOff/etc e o callback de audio,
// uma vez por period, antes de renderizar as amostras.
struct AppEvent {
    enum class Kind { NoteOn, NoteOff, PitchBend, ModWheel, ChannelAftertouch, PolyAftertouch,
                      Sustain, Sostenuto, AllNotesOff, ResetControllers,
                      Macro,           // [2026-10-24] note = indice da macro (0..3), floatValue = 0..1
                      TransportStart,  // [2026-10-24] Start do clock MIDI: zera a posicao de batida
                      NotePitchBend,   // [2026-10-28] MPE: note = nota, floatValue = semitons so dela
                      NoteSlide };     // [2026-10-28] MPE: note = nota, floatValue = CC74 0..1
    Kind kind = Kind::NoteOn;
    int part = -1;        // >=0: destinado a UMA parte especifica (teclado do PC). -1: roteia por canal MIDI (ver channel1to16)
    int channel1to16 = 0; // 0 = omni (so usado quando part == -1)
    int note = 0;
    int velocity = 0;
    float floatValue = 0.0f;
    bool boolValue = false;
};

// Fila MPSC simples baseada em mutex. Nao e lock-free, mas o lock so e
// disputado uma vez por period de audio (a cada ~5-10ms) e so por
// eventos de nota/CC, que sao raros comparado a taxa de amostra -- nao
// chega a causar glitch audivel na pratica (ver ARQUITETURA.txt).
class EventQueue {
public:
    void push(const AppEvent& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(e);
    }

    void drainInto(std::vector<AppEvent>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out.insert(out.end(), queue_.begin(), queue_.end());
        queue_.clear();
    }

private:
    std::mutex mutex_;
    std::vector<AppEvent> queue_;
};

// Lembra, por (canal MIDI, nota), para QUAIS partes o noteOn foi entregue.
// O noteOff vai exatamente pra essas partes, mesmo que no meio tempo a
// parte tenha sido desabilitada ou mudado de canal (antes, o noteOff era
// roteado pelo estado ATUAL e a nota ficava presa). So a audio thread usa.
struct NoteRouting {
    uint8_t partsMask[17][128] = {}; // [canal 0=omni/1..16][nota] -> bit por parte
};

// Roteia e aplica um AppEvent no Synth. Chamado SO pela audio thread.
// enabledCache/channelCache: copia atomica (mantida pela UI thread) de
// PartParams::enabled/midiChannel, pra decidir roteamento por canal MIDI
// sem a audio thread ler a copia de staging (que a UI pode estar editando
// ao mesmo tempo) -- ver comentario acima sobre o que NAO e coberto pelo
// triple-buffer do timbre.
inline void applyAppEvent(dx7fm::Synth& synth, const AppEvent& e,
                          const std::atomic<bool> (&enabledCache)[dx7fm::kNumParts],
                          const std::atomic<int> (&channelCache)[dx7fm::kNumParts],
                          NoteRouting& noteRouting) {
    auto route = [&](auto&& fn) {
        if (e.part >= 0) { fn(e.part); return; }
        for (int p = 0; p < dx7fm::kNumParts; ++p) {
            if (!enabledCache[p].load(std::memory_order_relaxed)) continue;
            int ch = channelCache[p].load(std::memory_order_relaxed);
            if (ch == 0 || ch == e.channel1to16) fn(p);
        }
    };

    int ch = std::clamp(e.channel1to16, 0, 16);
    int note = std::clamp(e.note, 0, 127);

    switch (e.kind) {
        case AppEvent::Kind::NoteOn:
            route([&](int p) {
                synth.noteOn(p, e.note, e.velocity);
                if (e.part < 0) noteRouting.partsMask[ch][note] |= static_cast<uint8_t>(1u << p);
            });
            break;
        case AppEvent::Kind::NoteOff:
            if (e.part >= 0) { synth.noteOff(e.part, e.note); break; }
            for (int p = 0; p < dx7fm::kNumParts; ++p)
                if (noteRouting.partsMask[ch][note] & (1u << p)) synth.noteOff(p, e.note);
            noteRouting.partsMask[ch][note] = 0;
            break;
        case AppEvent::Kind::PitchBend:          route([&](int p) { synth.pitchBend(p, e.floatValue); }); break;
        case AppEvent::Kind::ModWheel:           route([&](int p) { synth.modWheel(p, e.floatValue); }); break;
        case AppEvent::Kind::Macro:              route([&](int p) { synth.setMacro(p, e.note, e.floatValue); }); break;
        case AppEvent::Kind::TransportStart:     synth.resetTransport(); break;
        case AppEvent::Kind::ChannelAftertouch:  route([&](int p) { synth.channelAftertouch(p, e.floatValue); }); break;
        case AppEvent::Kind::PolyAftertouch:     route([&](int p) { synth.polyAftertouch(p, e.note, e.floatValue); }); break;
        case AppEvent::Kind::NotePitchBend:      route([&](int p) { synth.notePitchBend(p, e.note, e.floatValue); }); break;
        case AppEvent::Kind::NoteSlide:          route([&](int p) { synth.noteSlide(p, e.note, e.floatValue); }); break;
        case AppEvent::Kind::Sustain:            route([&](int p) { synth.sustainPedal(p, e.boolValue); }); break;
        case AppEvent::Kind::Sostenuto:          route([&](int p) { synth.sostenutoPedal(p, e.boolValue); }); break;
        case AppEvent::Kind::AllNotesOff:        route([&](int p) { synth.allNotesOff(p); }); break;
        case AppEvent::Kind::ResetControllers:
            route([&](int p) {
                synth.pitchBend(p, 0.0f);
                synth.modWheel(p, 0.0f);
                synth.channelAftertouch(p, 0.0f);
                synth.sustainPedal(p, false);
                synth.sostenutoPedal(p, false);
            });
            break;
    }
}

} // namespace dx7fm_app
