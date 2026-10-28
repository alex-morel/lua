#pragma once

// [2026-10-28] Evento MIDI de entrada, separado de alsa_midi.h pra ser usado
// sem ALSA (mpe.h, testes do demo.cpp).
namespace dx7fm_app {

struct MidiEvent {
    enum class Type { NoteOn, NoteOff, ControlChange, PitchBend, ChannelAftertouch, PolyAftertouch,
                      Clock, Start, Continue, Stop }; // [2026-10-24] mensagens de tempo real (clock MIDI)
    Type type = Type::NoteOn;
    int channel = 0;  // 0..15
    int data1 = 0;    // nota ou numero do CC
    int data2 = 0;    // velocity, valor do CC (0..127), ou pitch bend cru (-8192..8191)
};

} // namespace dx7fm_app
