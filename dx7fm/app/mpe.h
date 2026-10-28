#pragma once
#include "event_queue.h"
#include "midi_event.h"
#include <algorithm>
#include <vector>

// [ADICIONADO em 2026-10-28] Entrada MPE (MIDI Polyphonic Expression), zona
// inferior: canal 1 = mestre (bend/CC/pedal da parte inteira, pelo caminho
// normal), canais 2..1+N = uma nota por canal. O bend, a pressao (aftertouch
// de canal) e o CC74 de um canal-membro viram expressao SO da nota daquele
// canal: Synth::notePitchBend, polyAftertouch (ModSource::PolyAT) e noteSlide
// (ModSource::Slide). As notas sao entregues como se viessem do canal 1, entao
// tocam nas partes que escutam o canal 1 (ou omni).
//
// Mensagem de configuracao (MCM, RPN 6 no canal 1): N > 0 liga o MPE com N
// canais-membro, N = 0 desliga. RPN 0 num canal-membro muda a faixa do bend
// por nota (padrao do MPE: 48 semitons).
//
// Roda na thread de MIDI: so le e escreve o proprio estado e devolve
// AppEvents pra fila (quem aplica e a audio thread).
namespace dx7fm_app {

struct MpeInput {
    bool enabled = false;
    int memberChannels = 15;   // canais 2..1+N
    float noteBendRange = 48.0f;

    // estado de cada canal (0..15): nota tocando e ultimos valores recebidos.
    // O controlador manda os valores iniciais ANTES do note on; o slide
    // comeca em 0.5 (CC74 = 64, o repouso que o MPE recomenda).
    struct Channel {
        int note = -1;
        float bend = 0.0f, pressure = 0.0f, slide = 0.5f; // bend em -1..1
        int rpnMsb = 127, rpnLsb = 127;                   // RPN selecionado (127/127 = nenhum)
    };
    Channel ch[16];

    bool isMember(int channel0to15) const { return enabled && channel0to15 >= 1 && channel0to15 <= memberChannels; }

    // Trata o evento; devolve true se ele foi consumido (vira os AppEvents de
    // `out`) -- false = segue pelo caminho normal do app.
    bool handle(const MidiEvent& ev, std::vector<AppEvent>& out) {
        if (ev.channel < 0 || ev.channel > 15) return false;
        Channel& c = ch[ev.channel];
        if (ev.type == MidiEvent::Type::ControlChange && trackRpn(ev, c)) return true;
        if (!isMember(ev.channel)) return false;
        auto make = [&](AppEvent::Kind k, int note, float v) {
            AppEvent e;
            e.kind = k;
            e.channel1to16 = 1; // a zona inteira toca nas partes do canal mestre
            e.note = note;
            e.floatValue = v;
            return e;
        };
        switch (ev.type) {
            case MidiEvent::Type::NoteOn: {
                c.note = ev.data1;
                AppEvent on = make(AppEvent::Kind::NoteOn, ev.data1, 0.0f);
                on.velocity = ev.data2;
                out.push_back(on);
                // valores iniciais do canal, logo depois da nota (mesmo bloco de audio)
                out.push_back(make(AppEvent::Kind::NotePitchBend, ev.data1, c.bend * noteBendRange));
                out.push_back(make(AppEvent::Kind::PolyAftertouch, ev.data1, c.pressure));
                out.push_back(make(AppEvent::Kind::NoteSlide, ev.data1, c.slide));
                return true;
            }
            case MidiEvent::Type::NoteOff:
                out.push_back(make(AppEvent::Kind::NoteOff, ev.data1, 0.0f));
                if (c.note == ev.data1) c.note = -1;
                return true;
            case MidiEvent::Type::PitchBend:
                c.bend = std::clamp(ev.data2 / 8192.0f, -1.0f, 1.0f);
                if (c.note >= 0) out.push_back(make(AppEvent::Kind::NotePitchBend, c.note, c.bend * noteBendRange));
                return true;
            case MidiEvent::Type::ChannelAftertouch:
            case MidiEvent::Type::PolyAftertouch: // alguns controladores mandam pressao como poly AT
                c.pressure = std::clamp(ev.data2 / 127.0f, 0.0f, 1.0f);
                if (c.note >= 0) out.push_back(make(AppEvent::Kind::PolyAftertouch, c.note, c.pressure));
                return true;
            case MidiEvent::Type::ControlChange:
                if (ev.data1 == 74) {
                    c.slide = std::clamp(ev.data2 / 127.0f, 0.0f, 1.0f);
                    if (c.note >= 0) out.push_back(make(AppEvent::Kind::NoteSlide, c.note, c.slide));
                }
                return true; // outros CCs de canal-membro nao valem pra parte toda
            default:
                return false;
        }
    }

private:
    // RPN: CC 101/100 escolhem, CC 6 (data entry MSB) define. Consome o CC 6
    // so quando e um RPN que o MPE usa; 101/100 passam adiante (inofensivos).
    bool trackRpn(const MidiEvent& ev, Channel& c) {
        if (ev.data1 == 101) { c.rpnMsb = ev.data2; return false; }
        if (ev.data1 == 100) { c.rpnLsb = ev.data2; return false; }
        if (ev.data1 != 6 || c.rpnMsb != 0) return false;
        if (c.rpnLsb == 6 && ev.channel == 0) { // MCM da zona inferior
            memberChannels = std::clamp(ev.data2, 0, 15);
            enabled = memberChannels > 0;
            return true;
        }
        if (c.rpnLsb == 0 && isMember(ev.channel)) { // faixa do bend por nota
            noteBendRange = static_cast<float>(std::clamp(ev.data2, 1, 96));
            return true;
        }
        return false;
    }
};

} // namespace dx7fm_app
