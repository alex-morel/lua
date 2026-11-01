#pragma once
#include "midi_event.h"
#include "midi_select.h"
#include <alsa/asoundlib.h>
#include <poll.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace dx7fm_app {

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

        // NONBLOCK: com o modo bloqueante, depois de esvaziar a fila o
        // snd_seq_event_input() ficava BLOQUEADO esperando o proximo evento,
        // e o join() do stop() travava o fechamento do app.
        int err = snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
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

    // [2026-11-01] Portas que podem mandar MIDI pra gente. Chamadas da thread da
    // UI: consultas e inscricoes sao ioctls, nao tocam no buffer de entrada que
    // a threadLoop le.
    std::vector<MidiSource> listSources() const {
        std::vector<MidiSource> out;
        if (!seq_) return out;
        snd_seq_client_info_t* ci;
        snd_seq_port_info_t* pi;
        snd_seq_client_info_alloca(&ci);
        snd_seq_port_info_alloca(&pi);
        const int self = snd_seq_client_id(seq_);
        const unsigned need = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
        snd_seq_client_info_set_client(ci, -1);
        while (snd_seq_query_next_client(seq_, ci) >= 0) {
            const int c = snd_seq_client_info_get_client(ci);
            if (c == self || c == SND_SEQ_CLIENT_SYSTEM) continue;
            snd_seq_port_info_set_client(pi, c);
            snd_seq_port_info_set_port(pi, -1);
            while (snd_seq_query_next_port(seq_, pi) >= 0) {
                const unsigned caps = snd_seq_port_info_get_capability(pi);
                if ((caps & need) != need || (caps & SND_SEQ_PORT_CAP_NO_EXPORT)) continue;
                out.push_back({{c, snd_seq_port_info_get_port(pi)}, snd_seq_client_info_get_name(ci), snd_seq_port_info_get_name(pi)});
            }
        }
        return out;
    }
    // quem esta inscrito na nossa porta (inclusive por aconnect)
    std::vector<MidiAddr> subscribers() const {
        std::vector<MidiAddr> out;
        if (!seq_) return out;
        snd_seq_query_subscribe_t* q;
        snd_seq_query_subscribe_alloca(&q);
        snd_seq_addr_t root{static_cast<unsigned char>(snd_seq_client_id(seq_)), static_cast<unsigned char>(port_)};
        snd_seq_query_subscribe_set_root(q, &root);
        snd_seq_query_subscribe_set_type(q, SND_SEQ_QUERY_SUBS_WRITE);
        snd_seq_query_subscribe_set_index(q, 0);
        while (snd_seq_query_port_subscribers(seq_, q) >= 0) {
            const snd_seq_addr_t* a = snd_seq_query_subscribe_get_addr(q);
            out.push_back({a->client, a->port});
            snd_seq_query_subscribe_set_index(q, snd_seq_query_subscribe_get_index(q) + 1);
        }
        return out;
    }
    bool connectFrom(const MidiAddr& a) { return seq_ && snd_seq_connect_from(seq_, port_, a.client, a.port) >= 0; }
    bool disconnectFrom(const MidiAddr& a) { return seq_ && snd_seq_disconnect_from(seq_, port_, a.client, a.port) >= 0; }
    // so aceita eventos desta porta (desconectar nao basta: um app pode mandar direto)
    void acceptOnly(const MidiAddr& a) { accept_.store((a.client << 8) | (a.port & 0xFF), std::memory_order_relaxed); }
    void acceptNone() { accept_.store(kAcceptNone, std::memory_order_relaxed); }
    void acceptAll() { accept_.store(kAcceptAll, std::memory_order_relaxed); }
    // quando chegou o ultimo evento (segundos, relogio monotonico), pra luz de atividade
    double lastEventSec() const { return lastEventSec_.load(std::memory_order_relaxed); }

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
            while (running_) {
                int r = snd_seq_event_input(seq_, &ev);
                if (r == -ENOSPC) continue; // fila do kernel transbordou: eventos perdidos, segue lendo
                if (r < 0 || !ev) break;    // -EAGAIN: fila vazia, volta pro poll()
                dispatch(ev);
            }
        }
    }

    void dispatch(const snd_seq_event_t* ev) {
        const int accept = accept_.load(std::memory_order_relaxed);
        if (accept != kAcceptAll && accept != ((ev->source.client << 8) | ev->source.port)) return;
        if (ev->type != SND_SEQ_EVENT_CLOCK) { // o clock chega sem parar: a luz nao diria nada
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            lastEventSec_.store(static_cast<double>(ts.tv_sec) + 1e-9 * static_cast<double>(ts.tv_nsec), std::memory_order_relaxed);
        }
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
            case SND_SEQ_EVENT_KEYPRESS:
                me.type = MidiEvent::Type::PolyAftertouch;
                me.channel = ev->data.note.channel;
                me.data1 = ev->data.note.note;
                me.data2 = ev->data.note.velocity; // pressao
                break;
            case SND_SEQ_EVENT_CLOCK:    me.type = MidiEvent::Type::Clock; break;
            case SND_SEQ_EVENT_START:    me.type = MidiEvent::Type::Start; break;
            case SND_SEQ_EVENT_CONTINUE: me.type = MidiEvent::Type::Continue; break;
            case SND_SEQ_EVENT_STOP:     me.type = MidiEvent::Type::Stop; break;
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
    static constexpr int kAcceptAll = -1, kAcceptNone = -2;
    std::atomic<int> accept_{kAcceptAll};
    std::atomic<double> lastEventSec_{0.0};
};

} // namespace dx7fm_app
