#pragma once
#include <alsa/asoundlib.h>
#include <poll.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace dx7fm_app {

struct MidiEvent {
    enum class Type { NoteOn, NoteOff, ControlChange, PitchBend, ChannelAftertouch };
    Type type = Type::NoteOn;
    int channel = 0;  // 0..15
    int data1 = 0;    // nota ou numero do CC
    int data2 = 0;    // velocity, valor do CC (0..127), ou pitch bend cru (-8192..8191)
};

// Cria uma porta ALSA sequencer de ENTRADA (SND_SEQ_PORT_CAP_WRITE): outros
// apps/controladores MIDI se conectam nela (via `aconnect`, ou automatico
// em hosts que fazem isso). Usa poll() com timeout em vez de leitura
// bloqueante pra poder parar a thread de forma limpa (ver stop()).
class AlsaMidiIn {
public:
    using EventFn = std::function<void(const MidiEvent&)>;

    bool start(const char* clientName, const char* portName, EventFn onEvent) {
        stop();
        onEvent_ = std::move(onEvent);

        int err = snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, 0);
        if (err < 0) {
            std::fprintf(stderr, "[midi] snd_seq_open falhou: %s (MIDI desabilitado, teclado do PC ainda funciona)\n",
                          snd_strerror(err));
            lastError_ = snd_strerror(err);
            seq_ = nullptr;
            return false;
        }
        snd_seq_set_client_name(seq_, clientName);
        port_ = snd_seq_create_simple_port(seq_, portName,
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (port_ < 0) {
            std::fprintf(stderr, "[midi] create_simple_port falhou: %s\n", snd_strerror(port_));
            lastError_ = snd_strerror(port_);
            snd_seq_close(seq_);
            seq_ = nullptr;
            return false;
        }

        running_ = true;
        thread_ = std::thread([this] { threadLoop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (seq_) {
            snd_seq_close(seq_);
            seq_ = nullptr;
        }
        port_ = -1;
    }

    ~AlsaMidiIn() { stop(); }

    bool isOpen() const { return seq_ != nullptr; }
    int clientId() const { return seq_ ? snd_seq_client_id(seq_) : -1; }
    int portId() const { return port_; }
    const std::string& lastError() const { return lastError_; }

private:
    void threadLoop() {
        int npfd = snd_seq_poll_descriptors_count(seq_, POLLIN);
        if (npfd <= 0) return;
        std::vector<pollfd> pfds(static_cast<size_t>(npfd));

        while (running_) {
            snd_seq_poll_descriptors(seq_, pfds.data(), static_cast<unsigned>(npfd), POLLIN);
            int res = poll(pfds.data(), static_cast<nfds_t>(npfd), 200); // 200ms: revisita running_ periodicamente
            if (res <= 0) continue;

            snd_seq_event_t* ev = nullptr;
            while (running_ && snd_seq_event_input(seq_, &ev) >= 0 && ev) {
                dispatch(ev);
            }
        }
    }

    void dispatch(const snd_seq_event_t* ev) {
        MidiEvent me;
        bool valid = true;
        switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                me.type = ev->data.note.velocity > 0 ? MidiEvent::Type::NoteOn : MidiEvent::Type::NoteOff;
                me.channel = ev->data.note.channel;
                me.data1 = ev->data.note.note;
                me.data2 = ev->data.note.velocity;
                break;
            case SND_SEQ_EVENT_NOTEOFF:
                me.type = MidiEvent::Type::NoteOff;
                me.channel = ev->data.note.channel;
                me.data1 = ev->data.note.note;
                me.data2 = ev->data.note.velocity;
                break;
            case SND_SEQ_EVENT_CONTROLLER:
                me.type = MidiEvent::Type::ControlChange;
                me.channel = ev->data.control.channel;
                me.data1 = ev->data.control.param;
                me.data2 = static_cast<int>(ev->data.control.value);
                break;
            case SND_SEQ_EVENT_PITCHBEND:
                me.type = MidiEvent::Type::PitchBend;
                me.channel = ev->data.control.channel;
                me.data2 = static_cast<int>(ev->data.control.value); // -8192..8191
                break;
            case SND_SEQ_EVENT_CHANPRESS:
                me.type = MidiEvent::Type::ChannelAftertouch;
                me.channel = ev->data.control.channel;
                me.data2 = static_cast<int>(ev->data.control.value);
                break;
            default:
                valid = false;
        }
        if (valid && onEvent_) onEvent_(me);
    }

    snd_seq_t* seq_ = nullptr;
    int port_ = -1;
    EventFn onEvent_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::string lastError_;
};

} // namespace dx7fm_app
