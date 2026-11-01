#pragma once
#include <algorithm>
#include <string>
#include <vector>

// [ADICIONADO em 2026-11-01] Escolha do dispositivo de entrada MIDI, sem ALSA,
// testada no demo: dada a lista de portas e o que esta conectado, diz o que
// conectar, desconectar e de quem aceitar eventos. O dispositivo e lembrado
// pelo NOME: os numeros do ALSA mudam quando ele e reconectado.
namespace dx7fm_app {

struct MidiAddr {
    int client = -1, port = -1;
    bool operator==(const MidiAddr& o) const { return client == o.client && port == o.port; }
};
struct MidiSource {
    MidiAddr addr;
    std::string clientName, portName;
    std::string key() const { return clientName + ":" + portName; }
};

// settings.midiDevice: "" = nenhum (so conexoes externas, como aconnect);
// "*" = todos; senao, a key() do dispositivo
inline const char* const kMidiAllDevices = "*";

struct MidiPlan {
    std::vector<MidiAddr> connect, disconnect;
    MidiAddr accept;           // so eventos desta porta...
    bool acceptAll = true;     // ...a menos que aceite todos
    bool selectedMissing = false; // o escolhido nao esta plugado
};

// current = quem esta inscrito na nossa porta; ours = quem nos mesmos conectamos
inline MidiPlan planMidiInputs(const std::vector<MidiSource>& sources, const std::vector<MidiAddr>& current,
                               const std::vector<MidiAddr>& ours, const std::string& selection) {
    MidiPlan p;
    auto has = [](const std::vector<MidiAddr>& v, const MidiAddr& a) { return std::find(v.begin(), v.end(), a) != v.end(); };
    if (selection.empty()) { // nenhum: tira so o que foi nosso; conexoes externas ficam
        for (const auto& a : current) if (has(ours, a)) p.disconnect.push_back(a);
        return p;
    }
    if (selection == kMidiAllDevices) {
        for (const auto& s : sources) if (!has(current, s.addr)) p.connect.push_back(s.addr);
        return p;
    }
    p.acceptAll = false;
    p.selectedMissing = true;
    for (const auto& s : sources)
        if (s.key() == selection) { p.accept = s.addr; p.selectedMissing = false; break; }
    for (const auto& a : current) if (!(a == p.accept)) p.disconnect.push_back(a);
    if (!p.selectedMissing && !has(current, p.accept)) p.connect.push_back(p.accept);
    return p;
}

} // namespace dx7fm_app
