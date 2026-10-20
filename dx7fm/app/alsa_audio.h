#pragma once
#include <alsa/asoundlib.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

namespace dx7fm_app {

// Saida de audio ALSA minima: abre um PCM em modo interleaved, tenta
// SND_PCM_FORMAT_FLOAT_LE (evita converter float->int16 na mao) e cai pra
// S16_LE se o dispositivo nao suportar float. Roda um thread dedicado que
// chama renderFn repetidamente, escrevendo um "period" por vez.
class AlsaAudioOut {
public:
    using RenderFn = std::function<void(float* interleavedStereo, int frames)>;

    bool start(const char* deviceName, unsigned sampleRate, unsigned periodFrames, RenderFn renderFn) {
        stop();
        renderFn_ = std::move(renderFn);

        int err = snd_pcm_open(&pcm_, deviceName, SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            std::fprintf(stderr, "[audio] snd_pcm_open(\"%s\") falhou: %s\n", deviceName, snd_strerror(err));
            lastError_ = snd_strerror(err);
            return false;
        }

        snd_pcm_hw_params_t* hw = nullptr;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm_, hw);
        snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);

        format_ = SND_PCM_FORMAT_FLOAT_LE;
        if (snd_pcm_hw_params_set_format(pcm_, hw, format_) < 0) {
            format_ = SND_PCM_FORMAT_S16_LE;
            if (snd_pcm_hw_params_set_format(pcm_, hw, format_) < 0) {
                std::fprintf(stderr, "[audio] nenhum formato suportado (nem float nem s16)\n");
                lastError_ = "formato de amostra nao suportado";
                snd_pcm_close(pcm_);
                pcm_ = nullptr;
                return false;
            }
        }

        snd_pcm_hw_params_set_channels(pcm_, hw, 2);
        unsigned rate = sampleRate;
        snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr);
        snd_pcm_uframes_t period = periodFrames;
        snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr);
        snd_pcm_uframes_t bufSize = period * 4;
        snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &bufSize);

        err = snd_pcm_hw_params(pcm_, hw);
        if (err < 0) {
            std::fprintf(stderr, "[audio] hw_params falhou: %s\n", snd_strerror(err));
            lastError_ = snd_strerror(err);
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
            return false;
        }

        sampleRate_ = rate;
        periodFrames_ = static_cast<unsigned>(period);
        interleaved_.assign(static_cast<size_t>(periodFrames_) * 2, 0.0f);
        pcmBuf_.assign(static_cast<size_t>(periodFrames_) * 2 * sizeof(float), 0);
        underruns_.store(0);
        lastError_.clear();

        running_ = true;
        thread_ = std::thread([this] { threadLoop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (pcm_) {
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
        }
    }

    ~AlsaAudioOut() { stop(); }

    bool isRunning() const { return running_; }
    unsigned sampleRate() const { return sampleRate_; }
    unsigned periodFrames() const { return periodFrames_; }
    long underruns() const { return underruns_.load(); }
    const char* deviceFormatName() const { return format_ == SND_PCM_FORMAT_FLOAT_LE ? "float32" : "s16"; }
    const std::string& lastError() const { return lastError_; }

private:
    void threadLoop() {
        // Tempo real (melhor esforco: precisa de rtprio nos limits do usuario;
        // sem isso segue com prioridade normal). Mesma prioridade das threads
        // de voz do synth (dx7_threads.h), que ela espera a cada bloco.
        sched_param sp{};
        sp.sched_priority = 70;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        while (running_) {
            renderFn_(interleaved_.data(), static_cast<int>(periodFrames_));

            snd_pcm_sframes_t written;
            if (format_ == SND_PCM_FORMAT_FLOAT_LE) {
                written = snd_pcm_writei(pcm_, interleaved_.data(), periodFrames_);
            } else {
                auto* out = reinterpret_cast<int16_t*>(pcmBuf_.data());
                for (size_t i = 0; i < interleaved_.size(); ++i) {
                    float s = interleaved_[i];
                    s = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
                    out[i] = static_cast<int16_t>(s * 32767.0f);
                }
                written = snd_pcm_writei(pcm_, out, periodFrames_);
            }

            if (written < 0) {
                underruns_.fetch_add(1);
                written = snd_pcm_recover(pcm_, static_cast<int>(written), 1);
                if (written < 0) {
                    std::fprintf(stderr, "[audio] erro irrecuperavel: %s\n", snd_strerror(static_cast<int>(written)));
                    running_ = false; // UI mostra "PARADO" em vez de "ativo" com a thread morta
                    break;
                }
            }
        }
    }

    snd_pcm_t* pcm_ = nullptr;
    snd_pcm_format_t format_ = SND_PCM_FORMAT_FLOAT_LE;
    unsigned sampleRate_ = 48000;
    unsigned periodFrames_ = 256;
    std::vector<float> interleaved_;
    std::vector<uint8_t> pcmBuf_;
    RenderFn renderFn_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<long> underruns_{0};
    std::string lastError_;
};

} // namespace dx7fm_app
