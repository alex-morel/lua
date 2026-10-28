#pragma once
#include "dx7_params.h"
#include "dx7_engine.h"
#include "dx7_globalfx.h"
#include "dx7_threads.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <vector>

namespace dx7fm {

constexpr int kNumParts = 4;

struct VoiceSlot {
    Voice voice;
    int ownerPart = -1;       // -1 = livre
    int note = -1;            // nota MIDI FISICA (sem transpose)
    bool physicallyHeld = false;
    bool sustainHeld = false;   // tecla solta com o pedal de sustain (CC64) apertado
    bool sostenutoHeld = false; // tecla solta, mas capturada pelo sostenuto (CC66)
    float subPan = 0.0f;
    uint64_t triggerCounter = 0;
};

struct PartRuntime {
    // params.voice e a copia de STAGING: Synth::part(idx) devolve referencia
    // pra ela, entao editar synth.part(idx).voice.X so afeta o audio depois
    // que Synth::recompile(idx) publicar um PatchSnapshot novo (ver abaixo).
    // Os demais campos de PartParams (transpose, loKey/hiKey, voiceMode etc.)
    // continuam single-buffer, sem essa indirecao -- nao sao "o timbre" que
    // o item de thread-safety do roteiro (secao 11) pedia pra proteger.
    PartParams params;

    // [ADICIONADO em 2026-09-21] Triple buffer + indice atomico: mecanismo
    // de troca atomica de patch (o item "thread safety" do roteiro). O
    // audio thread SO le snapshots_[liveIndex_] (via live()), nunca o slot
    // de staging, entao editar params.voice de outra thread nao e uma data
    // race com o audio -- desde que quem edita va SEMPRE por
    // Synth::part(idx) + Synth::recompile(idx), nunca escrevendo direto
    // num snapshot. 3 slots (nao 2) pra nunca reescrever o slot que ACABOU
    // de ser live (poderia ainda estar em leitura numa renderStereo() em
    // andamento na outra thread).
    std::array<PatchSnapshot, 3> snapshots_{};
    std::atomic<int> liveIndex_{0};
    int stagingIndex_ = 1;

    const PatchSnapshot* live() const {
        return &snapshots_[static_cast<size_t>(liveIndex_.load(std::memory_order_acquire))];
    }

    std::vector<int> heldStack;             // notas fisicas pressionadas, ordem de chegada
    std::unordered_map<int, int> heldVelocity; // nota -> velocity (reataque mono usa a velocity original)
    std::vector<int> activeSlots;           // slots do grupo mono/unison atual
    int monoSoundingNote = -1;              // nota que o grupo mono/unison esta tocando agora
    std::unordered_map<int, int> noteToSlot;// so usado no modo Poly
    // Modo em que as notas ATUAIS foram disparadas. Pode diferir de
    // params.voiceMode se a UI trocou o modo com notas seguras: o noteOff
    // tem que ir pelo caminho do modo antigo, senao a nota fica presa.
    VoiceMode runMode = VoiceMode::Poly;
    float lastNoteHz = 0.0f;                // freq da ultima nota disparada (origem do glide)

    // cache do compileAlgorithm (so a thread que chama recompile() toca aqui)
    AlgorithmMatrix compiledFor{};
    CompiledAlgorithm compiledCache{};
    bool hasCompiledCache = false;
    uint64_t generation = 0; // contador de recompile() (ver PatchSnapshot::generation)
    // cache das tabelas da Wavetable por operador (so a thread do recompile() toca aqui)
    struct WavetableCache {
        std::shared_ptr<const std::vector<float>> waveA, waveB;
        bool spectral = true;
        WavetableBankPtr bank;
    };
    std::array<WavetableCache, kNumOps> wtCache{};
    // cache da tabela de afinacao (so a thread do recompile)
    PartParams::Tuning tuningFor;
    TuningTable tuningCache = equalTemperament();
    std::string tuningError; // ultimo erro de parse (vazio = ok)

    std::unordered_set<int> sostenutoCaptured;
    bool sostenutoDown = false;
    bool sustainDown = false;
    float bend = 0.0f;
    float modWheel = 0.0f;
    // [2026-10-24] macros: a audio thread escreve (setMacro), a UI le pra mostrar
    std::atomic<float> macro[VoiceParams::kNumMacros] = {};
    // [2026-10-24] LFOs globais (LFOParams::global): um por parte, calculados
    // pelo Synth a cada pedaco em glfoBuf; key sync reinicia na 1a nota presa
    LFO glfo[kNumLfos];
    float glfoBuf[kNumLfos][256] = {};
    bool glfoRestart = false;
    // [2026-10-24] arpejador (so a audio thread): teclas em ordem de chegada,
    // quantas estao fisicamente presas (latch), passo atual e agenda em batidas
    struct ArpKey { int note, velocity; };
    std::vector<ArpKey> arpKeys;
    int arpPhysical = 0;
    bool arpRunning = false;
    long arpStep = 0;
    double arpNextBeat = 0.0, arpOffBeat = -1.0;
    int arpSounding = -1; // nota que o arp esta tocando (-1 = nenhuma)
    float channelAT = 0.0f;
};

class Synth {
public:
    // oversample: 1 (default, sem custo extra), 2 ou 4 -- ver Voice::setOversample()
    // e ARQUITETURA.txt secao 11 (oversampling anti-aliasing).
    void init(double sampleRate, int maxVoices = 64, int oversample = 1) {
        sampleRate_ = sampleRate;
        // coeficientes one-pole constantes (antes: um exp() por amostra cada)
        polySmoothCoef_ = 1.0f - std::exp(-1.0f / (0.03f * static_cast<float>(sampleRate)));   // ~30ms
        polyDropCoef_ = 1.0f - std::exp(-1.0f / (0.001f * static_cast<float>(sampleRate)));   // ~1ms
        volumeSmoothCoef_ = 1.0f - std::exp(-1.0f / (0.02f * static_cast<float>(sampleRate))); // ~20ms
        slots_.assign(static_cast<size_t>(maxVoices), VoiceSlot{});
        jobs_.clear();
        jobs_.reserve(static_cast<size_t>(maxVoices));
        voiceBuf_.assign(static_cast<size_t>(maxVoices) * kRenderChunk, 0.0f);
        loudBuf_.assign(static_cast<size_t>(maxVoices) * kRenderChunk, 0.0f);
        panBuf_.assign(static_cast<size_t>(maxVoices) * kRenderChunk, 0.0f);
        for (auto& s : slots_) {
            s.voice.setOversample(oversample);
            s.voice.setSampleRate(sampleRate);
        }
        for (int p = 0; p < kNumParts; ++p) recompile(p);
        for (auto& pr : parts_) for (auto& g : pr.glfo) g.setSampleRate(sampleRate);
        fxChain_.setSampleRate(sampleRate);
    }

    // Referencia de STAGING: editar campos aqui (inclusive .voice, o timbre)
    // so afeta a reproducao apos o proximo recompile(idx). Seguro de chamar
    // de uma thread de UI/edicao diferente da que roda renderStereo(), desde
    // que a UNICA forma de "publicar" a edicao seja via recompile(idx).
    PartParams& part(int idx) { return parts_[idx].params; }

    // [ADICIONADO em 2026-09-21] Efeitos globais POS-MIX (chorus, delay,
    // reverb -- ver dx7_globalfx.h e dx7_params.h::GlobalFxParams).
    // Aplicados uma vez na SOMA das 4 partes, dentro de renderStereo().
    // Acesso direto (nao passa pelo triple-buffer do timbre, ver secao 7C
    // do ARQUITETURA.txt sobre o que NAO esta coberto pela garantia de
    // thread safety) -- editar globalFx() enquanto renderStereo() roda
    // noutra thread tem a mesma ressalva que os demais campos "fora do
    // timbre" de PartParams.
    GlobalFxParams& globalFx() { return globalFx_; }
    // reducao de ganho atual do compressor global (dB), pra medidor na UI
    float compressorGainReductionDb() const { return fxChain_.compressorGainReductionDb(); }

    // [ADICIONADO em 2026-09-23] Volume geral (master), ganho linear 0..2
    // (1.0 = 0dB). Atomico de proposito: a UI chama setMasterVolume() de
    // outra thread enquanto renderStereo() roda na audio thread. Aplicado
    // DEPOIS dos efeitos globais e ANTES do softLimit(): aumentar o volume
    // acima de 1.0 continua protegido contra clipping. Suavizado (~20ms)
    // em renderStereo() pra mexer no slider nao dar "zipper noise".
    void setMasterVolume(float gain) {
        if (!std::isfinite(gain)) return;
        masterVolume_.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
    }
    float masterVolume() const { return masterVolume_.load(std::memory_order_relaxed); }

    // [ADICIONADO em 2026-10-22] BPM global (20..300) dos LFOs com tempoSync.
    // Atomico como o volume: a UI escreve, a audio thread le 1x por bloco.
    void setTempoBpm(float bpm) {
        if (!std::isfinite(bpm)) return;
        tempoBpm_.store(std::clamp(bpm, 20.0f, 300.0f), std::memory_order_relaxed);
    }
    float tempoBpm() const { return tempoBpm_.load(std::memory_order_relaxed); }

    // Publica params.voice (staging) como o novo patch AO VIVO: recompila o
    // algoritmo, escreve o par (voice, compiled) casado num slot de buffer
    // que o audio thread NAO esta lendo, e so entao troca o indice atomico
    // -- ver PartRuntime acima. OBRIGATORIO apos qualquer edicao de
    // part(idx).voice (matriz OU qualquer outro parametro do timbre), nao
    // so mudancas de topologia. Vozes JA soando veem o novo patch no proximo
    // renderStereo() (Synth::rebind ao vivo), preservando a edicao "ao vivo"
    // (ver ARQUITETURA.txt secao 10) sem data race.
    void recompile(int idx) {
        auto& pr = parts_[idx];
        sanitize(pr.params);
        // A busca de 8! permutacoes custa ms; a UI chama recompile a cada
        // frame enquanto um slider e arrastado, entao so recompila a
        // topologia quando a matriz de fato mudou.
        // [2026-10-24] com morph entre algoritmos, compila a UNIAO dos slots:
        // uma ordem so serve pra qualquer ponto do morph (nada e recompilado
        // durante a nota). Com 1 algoritmo, algoUnion devolve a propria matriz.
        const AlgorithmMatrix topo = algoUnion(pr.params.voice);
        if (!pr.hasCompiledCache || std::memcmp(&pr.compiledFor, &topo, sizeof(AlgorithmMatrix)) != 0) {
            pr.compiledCache = compileAlgorithm(topo);
            pr.compiledFor = topo;
            pr.hasCompiledCache = true;
        }
        int newIdx = pr.stagingIndex_;
        // tabelas da Wavetable: so remonta o operador cujas ondas/modo mudaram.
        // O cache guarda os shared_ptr das ondas (nao so o endereco): uma onda
        // liberada nao pode ter o endereco reusado por outra enquanto esta aqui.
        WavetableBanks banks{};
        for (int i = 0; i < kNumOps; ++i) {
            const auto& o = pr.params.voice.op[i];
            auto& c = pr.wtCache[static_cast<size_t>(i)];
            if (o.waveform != Waveform::Wavetable) continue; // cache fica: voltar pra Wavetable nao remonta
            if (!c.bank || c.waveA != o.wavetable.waveA || c.waveB != o.wavetable.waveB || c.spectral != o.wavetable.spectralMorph) {
                c.bank = makeWavetableBank(o);
                c.waveA = o.wavetable.waveA;
                c.waveB = o.wavetable.waveB;
                c.spectral = o.wavetable.spectralMorph;
            }
            banks[static_cast<size_t>(i)] = c.bank;
        }
        const auto& tu = pr.params.tuning;
        if (tu.sclText != pr.tuningFor.sclText || tu.kbmText != pr.tuningFor.kbmText || tu.a4Hz != pr.tuningFor.a4Hz) {
            pr.tuningFor = tu;
            pr.tuningError.clear();
            pr.tuningCache = equalTemperament(tu.a4Hz);
            if (!tu.sclText.empty()) {
                Scale sc;
                KeyMap km;
                std::string err;
                if (!parseScl(tu.sclText, sc, err)) pr.tuningError = ".scl: " + err;
                else if (!tu.kbmText.empty() && !parseKbm(tu.kbmText, km, err)) pr.tuningError = ".kbm: " + err;
                else pr.tuningCache = buildTuning(sc, tu.kbmText.empty() ? nullptr : &km, tu.a4Hz);
            }
        }
        pr.snapshots_[static_cast<size_t>(newIdx)] = PatchSnapshot{pr.params.voice, pr.compiledCache, ++pr.generation, std::move(banks), pr.tuningCache};
        int oldLive = pr.liveIndex_.exchange(newIdx, std::memory_order_acq_rel);
        pr.stagingIndex_ = 3 - newIdx - oldLive; // o terceiro indice (0+1+2=3)
    }

    void noteOn(int part, int note, int velocity) {
        auto& pr = parts_[part];
        if (!pr.params.enabled) return;
        if (note < pr.params.loKey || note > pr.params.hiKey) return;
        if (velocity < pr.params.loVel || velocity > pr.params.hiVel) return;
        if (!pr.live()->tuning.mapped[std::clamp(note + pr.params.transpose, 0, 127)]) return; // tecla sem som no .kbm

        if (pr.params.voiceMode != pr.runMode) {
            // modo trocado com notas soando: solta tudo do modo antigo antes
            releaseAllHeld(part);
            pr.runMode = pr.params.voiceMode;
        }
        if (pr.heldStack.empty() && pr.arpPhysical == 0) pr.glfoRestart = true; // 1a nota presa: key sync dos LFOs globais
        if (pr.live()->voice.arp.on) { arpKeyDown(part, note, velocity); return; }
        dispatchNoteOn(part, note, velocity);
    }

    // noteOn sem o arpejador (o arp usa pra tocar os passos)
    void dispatchNoteOn(int part, int note, int velocity) {
        auto& pr = parts_[part];
        switch (pr.runMode) {
            case VoiceMode::Poly:   polyNoteOn(part, note, velocity); break;
            case VoiceMode::Mono:   monoGroupNoteOn(part, note, velocity, 1); break;
            case VoiceMode::Unison: monoGroupNoteOn(part, note, velocity, std::max(1, pr.params.unisonCount)); break;
        }
    }

    // --- arpejador ([2026-10-24]) ----------------------------------------------
    void arpKeyDown(int part, int note, int velocity) {
        auto& pr = parts_[part];
        if (pr.arpPhysical == 0 && pr.arpRunning) pr.arpKeys.clear(); // latch: tecla nova depois de soltar tudo = padrao novo
        pr.arpKeys.erase(std::remove_if(pr.arpKeys.begin(), pr.arpKeys.end(), [&](const PartRuntime::ArpKey& k) { return k.note == note; }),
                         pr.arpKeys.end());
        pr.arpKeys.push_back({note, velocity});
        ++pr.arpPhysical;
        if (!pr.arpRunning) { // comeca ja (sem esperar a grade); os passos seguintes caem na grade
            pr.arpRunning = true;
            pr.arpStep = 0;
            pr.arpNextBeat = beatPos_;
        }
    }

    void arpKeyUp(int part, int note) {
        auto& pr = parts_[part];
        bool was = false;
        for (const auto& k : pr.arpKeys) was |= k.note == note;
        if (!was) { dispatchNoteOff(part, note); return; } // tecla que nao e do arpejo (presa antes de ligar o arp)
        pr.arpPhysical = std::max(0, pr.arpPhysical - 1);
        const bool latch = pr.live()->voice.arp.latch;
        if (!latch) pr.arpKeys.erase(std::remove_if(pr.arpKeys.begin(), pr.arpKeys.end(),
                                                    [&](const PartRuntime::ArpKey& k) { return k.note == note; }), pr.arpKeys.end());
        if (pr.arpKeys.empty()) arpStop(part);
    }

    // [2026-10-24] observador das notas que o arpejador dispara (testes, UI):
    // fn(ctx, parte, nota, ligou?, posicao em batidas). nullptr = nenhum.
    using NoteObserver = void (*)(void* ctx, int part, int note, bool on, double beat);
    void setArpObserver(NoteObserver fn, void* ctx) { arpObs_ = fn; arpObsCtx_ = ctx; }

    void arpNoteOn(int part, int note, int vel) {
        dispatchNoteOn(part, note, vel);
        if (arpObs_) arpObs_(arpObsCtx_, part, note, true, beatPos_);
    }
    void arpNoteOff(int part, int note) {
        dispatchNoteOff(part, note);
        if (arpObs_) arpObs_(arpObsCtx_, part, note, false, beatPos_);
    }

    void arpStop(int part) {
        auto& pr = parts_[part];
        if (pr.arpSounding >= 0) arpNoteOff(part, pr.arpSounding);
        pr.arpSounding = -1;
        pr.arpRunning = false;
        pr.arpKeys.clear();
        pr.arpPhysical = 0;
        pr.arpOffBeat = -1.0;
    }

    // nota do passo `step` segundo o modo (notas x oitavas)
    int arpNoteFor(const PartRuntime& pr, const ArpParams& a, long step, int& velocity) {
        std::vector<PartRuntime::ArpKey> base = pr.arpKeys;
        if (a.mode != ArpMode::AsPlayed)
            std::sort(base.begin(), base.end(), [](const PartRuntime::ArpKey& x, const PartRuntime::ArpKey& y) { return x.note < y.note; });
        std::vector<PartRuntime::ArpKey> seq;
        for (int o = 0; o < a.octaves; ++o)
            for (const auto& k : base) seq.push_back({k.note + 12 * o, k.velocity});
        if (a.mode == ArpMode::Down) std::reverse(seq.begin(), seq.end());
        if (a.mode == ArpMode::UpDown && seq.size() > 2) // sobe e desce sem repetir as pontas
            for (size_t i = seq.size() - 2; i >= 1; --i) seq.push_back(seq[i]);
        if (seq.empty()) return -1;
        size_t idx = a.mode == ArpMode::Random ? static_cast<size_t>(arpRng_() % seq.size()) : static_cast<size_t>(step % static_cast<long>(seq.size()));
        velocity = seq[idx].velocity;
        return std::clamp(seq[idx].note, 0, 127);
    }

    // proximo evento do arp (passo ou fim de gate) em batidas; -1 = nenhum
    double arpNextEventBeat() const {
        double next = -1.0;
        for (const auto& pr : parts_) {
            if (pr.arpRunning) next = next < 0.0 ? pr.arpNextBeat : std::min(next, pr.arpNextBeat);
            if (pr.arpOffBeat >= 0.0) next = next < 0.0 ? pr.arpOffBeat : std::min(next, pr.arpOffBeat);
        }
        return next;
    }

    void arpProcessDue() {
        for (int p = 0; p < kNumParts; ++p) {
            auto& pr = parts_[p];
            const ArpParams& a = pr.live()->voice.arp;
            if ((pr.arpRunning || pr.arpOffBeat >= 0.0) && !a.on) { arpStop(p); continue; } // arp desligado com notas
            if (pr.arpOffBeat >= 0.0 && pr.arpOffBeat <= beatPos_ + 1e-9) { // fim do gate
                if (pr.arpSounding >= 0) arpNoteOff(p, pr.arpSounding);
                pr.arpSounding = -1;
                pr.arpOffBeat = -1.0;
            }
            if (!pr.arpRunning || pr.arpNextBeat > beatPos_ + 1e-9) continue;
            const double stepLen = lfoDivisionBeats(a.division);
            if (pr.arpSounding >= 0) { arpNoteOff(p, pr.arpSounding); pr.arpSounding = -1; } // gate 100%: legato
            int vel = 100;
            const int note = arpNoteFor(pr, a, pr.arpStep, vel);
            if (note >= 0) {
                arpNoteOn(p, note, vel);
                pr.arpSounding = note;
                pr.arpOffBeat = a.gate >= 1.0f ? -1.0 : beatPos_ + a.gate * stepLen;
            }
            ++pr.arpStep;
            // proximo passo na grade (a partir da batida 0); impar atrasado pelo swing
            const long gridIdx = static_cast<long>(std::floor(beatPos_ / stepLen + 1e-6)) + 1;
            pr.arpNextBeat = gridIdx * stepLen + ((gridIdx % 2) ? a.swing * stepLen : 0.0);
        }
    }

    // NAO filtra por key range/velocity/enabled de proposito: uma nota que
    // nao foi aceita no noteOn simplesmente nao e achada (poly: noteToSlot;
    // mono: heldStack), e uma que FOI aceita tem que poder ser solta mesmo
    // que a parte tenha sido desabilitada no meio.
    void noteOff(int part, int note) {
        auto& pr = parts_[part];
        if (pr.arpRunning || pr.arpPhysical > 0) { arpKeyUp(part, note); return; }
        dispatchNoteOff(part, note);
    }

    void dispatchNoteOff(int part, int note) {
        auto& pr = parts_[part];
        switch (pr.runMode) {
            case VoiceMode::Poly: polyNoteOff(part, note); break;
            case VoiceMode::Mono:
            case VoiceMode::Unison: monoGroupNoteOff(part, note); break;
        }
    }

    void polyAftertouch(int part, int note, float pressure01) {
        auto& pr = parts_[part];
        if (pr.runMode == VoiceMode::Poly) {
            auto it = pr.noteToSlot.find(note);
            if (it != pr.noteToSlot.end()) slots_[it->second].voice.controllers().polyAT01 = pressure01;
        } else if (pr.monoSoundingNote == note) {
            for (int idx : pr.activeSlots) slots_[idx].voice.controllers().polyAT01 = pressure01;
        }
    }

    void channelAftertouch(int part, float pressure01) {
        auto& pr = parts_[part];
        pr.channelAT = pressure01;
        for (auto& s : slots_) if (s.ownerPart == part) s.voice.controllers().channelAT01 = pressure01;
    }

    void pitchBend(int part, float semis) {
        auto& pr = parts_[part];
        pr.bend = semis;
        for (auto& s : slots_) if (s.ownerPart == part) s.voice.controllers().pitchBendSemis = semis;
    }

    // [ADICIONADO em 2026-10-24] macro k (0..3) da parte, 0..1. Chamar da thread
    // que roda o audio (como os outros controladores); macroValue() pode ser
    // lido de qualquer thread.
    void setMacro(int part, int k, float v01) {
        if (part < 0 || part >= kNumParts || k < 0 || k >= VoiceParams::kNumMacros) return;
        v01 = std::clamp(v01, 0.0f, 1.0f);
        parts_[part].macro[k].store(v01, std::memory_order_relaxed);
        for (auto& s : slots_) if (s.ownerPart == part) s.voice.controllers().macro01[k] = v01;
    }
    float macroValue(int part, int k) const { return parts_[part].macro[k].load(std::memory_order_relaxed); }

    void modWheel(int part, float v01) {
        auto& pr = parts_[part];
        pr.modWheel = v01;
        for (auto& s : slots_) if (s.ownerPart == part) s.voice.controllers().modWheel01 = v01;
    }

    // Sustain e sostenuto seguram a voz de forma INDEPENDENTE: soltar um
    // pedal so libera as vozes que nao estao presas pelo outro.
    void sustainPedal(int part, bool down) {
        auto& pr = parts_[part];
        pr.sustainDown = down;
        if (!down) {
            for (auto& s : slots_) {
                if (s.ownerPart != part || !s.sustainHeld) continue;
                s.sustainHeld = false;
                if (!s.physicallyHeld && !s.sostenutoHeld) s.voice.noteOff();
            }
        }
    }

    void sostenutoPedal(int part, bool down) {
        auto& pr = parts_[part];
        pr.sostenutoDown = down;
        pr.sostenutoCaptured.clear();
        if (down) {
            for (int n : pr.heldStack) pr.sostenutoCaptured.insert(n);
        } else {
            for (auto& s : slots_) {
                if (s.ownerPart != part || !s.sostenutoHeld) continue;
                s.sostenutoHeld = false;
                if (!s.physicallyHeld && !s.sustainHeld) s.voice.noteOff();
            }
        }
    }

    void allNotesOff(int part) {
        auto& pr = parts_[part];
        if (pr.arpRunning || pr.arpOffBeat >= 0.0 || pr.arpPhysical > 0) arpStop(part);
        releaseAllHeld(part);
        pr.sostenutoCaptured.clear();
    }

    // Uma amostra estereo. Equivale a renderBlock(&outL, &outR, 1).
    void renderStereo(float& outL, float& outR) { renderBlock(&outL, &outR, 1); }

    // [ADICIONADO em 2026-10-20] Renderizacao em blocos: gera n amostras em
    // outL[0..n) e outR[0..n), em pedacos de ate kRenderChunk amostras:
    //  1. cada voz ativa renderiza o pedaco INTEIRO no seu buffer (amostra e
    //     loudness por amostra) -- vozes nao compartilham estado, entao isso
    //     roda em paralelo no WorkerPool (setWorkerThreads);
    //  2. a mixagem soma as vozes na ORDEM DOS SLOTS, amostra por amostra, e
    //     aplica polifonia/FX/volume/limitador como antes.
    // Por (2) a saida e bit a bit igual a renderizar amostra por amostra,
    // com qualquer numero de threads. Unica diferenca de tempo: um
    // recompile() publicado no meio do pedaco so vale no pedaco seguinte
    // (<= 256 amostras, ~5ms).
    static constexpr int kRenderChunk = 256;
    static_assert(sizeof(PartRuntime::glfoBuf[0]) / sizeof(float) >= kRenderChunk, "buffer do LFO global menor que o pedaco");

    void renderBlock(float* outL, float* outR, int n) {
        for (int done = 0; done < n;) {
            int len = std::min(kRenderChunk, n - done);
            // arpejador: corta o pedaco no instante exato do proximo passo/gate
            // (sem arp tocando, nada muda -- saida identica a antes)
            const double beatsPerSample = static_cast<double>(tempoBpm_.load(std::memory_order_relaxed)) / (60.0 * sampleRate_);
            const double next = arpNextEventBeat();
            if (next >= 0.0) {
                if (next <= beatPos_ + 1e-9) { arpProcessDue(); continue; }
                len = std::min(len, std::max(1, static_cast<int>(std::ceil((next - beatPos_) / beatsPerSample - 1e-9))));
            }
            renderChunk(outL + done, outR + done, len);
            beatPos_ += len * beatsPerSample;
            done += len;
        }
    }

    // [ADICIONADO em 2026-10-24] posicao de batida (seminimas desde o ultimo
    // Start): avanca com o BPM; LFOs globais com sync usam pra ficar no compasso.
    // Chamar da thread do audio (Start do clock MIDI -> evento TransportStart).
    void resetTransport() {
        beatPos_ = 0.0;
        for (auto& pr : parts_) if (pr.arpRunning) { pr.arpNextBeat = 0.0; pr.arpStep = 0; } // arpejo recomeca no 1
    }
    double beatPosition() const { return beatPos_; }

    // Threads de audio ALEM da que chama renderBlock() (0 = tudo serial, o
    // padrao). Chamar fora da audio thread so com o audio parado.
    void setWorkerThreads(int n) {
        n = std::clamp(n, 0, 63);
        if (n == pool_.workers()) return;
        pool_.stop();
        if (n > 0) pool_.start(n, &Synth::renderJob, this);
    }
    int workerThreads() const { return pool_.workers(); }

    // snapshot AO VIVO da parte (o que o audio le), so pra testes/depuracao
    const PatchSnapshot* debugLiveSnapshot(int part) const { return parts_[part].live(); }

    int activeVoiceCount() const {
        int c = 0;
        for (auto& s : slots_) if (s.ownerPart >= 0 && s.voice.isActive()) ++c;
        return c;
    }

    int activeVoiceCount(int part) const {
        int c = 0;
        for (auto& s : slots_) if (s.ownerPart == part && s.voice.isActive()) ++c;
        return c;
    }

    Voice& debugVoiceOfPart(int part, int k = 0) {
        int c = 0;
        for (auto& s : slots_) {
            if (s.ownerPart == part && s.voice.isActive()) {
                if (c == k) return s.voice;
                ++c;
            }
        }
        return slots_[0].voice;
    }

private:
    int countActive(int part) const { return activeVoiceCount(part); }

    int allocateSlot(int requestingPart) {
        for (size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i].voice.isActive()) return static_cast<int>(i);

        int best = -1;
        for (size_t i = 0; i < slots_.size(); ++i) {
            auto& s = slots_[i];
            if (s.ownerPart != requestingPart && s.ownerPart >= 0) {
                if (countActive(s.ownerPart) <= parts_[s.ownerPart].params.reservedVoices) continue;
            }
            if (best < 0) { best = static_cast<int>(i); continue; }
            auto& sb = slots_[static_cast<size_t>(best)];
            bool sRel = s.voice.isReleasing();
            bool bRel = sb.voice.isReleasing();
            if (sRel != bRel) {
                if (sRel) best = static_cast<int>(i);
                continue;
            }
            if (s.triggerCounter < sb.triggerCounter) best = static_cast<int>(i);
        }
        return best < 0 ? 0 : best;
    }

    void configureVoice(Voice& v, const PartRuntime& pr, bool glideActiveForThisNote) {
        v.glideEnabled_ = pr.params.glideEnabled && (glideActiveForThisNote || !pr.params.glideLegatoOnly);
        v.glideTimeSec_ = pr.params.glideTimeSec;
        v.reuseEnvOnLegato_ = pr.params.monoLegatoEnvReuse;
        v.bendRangeSemis_ = pr.params.bendRangeSemis;
    }

    // Desliga o slot de quem o possuia ANTES de ele ser reusado por outra
    // nota. Sem isso a nota antiga continuava apontando pro slot (noteToSlot
    // / activeSlots) e o noteOff DELA soltava a nota nova que roubou a voz.
    void detachSlot(int idx) {
        auto& s = slots_[static_cast<size_t>(idx)];
        if (s.ownerPart < 0) return;
        auto& old = parts_[s.ownerPart];
        for (auto it = old.noteToSlot.begin(); it != old.noteToSlot.end();) {
            if (it->second == idx) {
                auto& hs = old.heldStack;
                hs.erase(std::remove(hs.begin(), hs.end(), it->first), hs.end());
                it = old.noteToSlot.erase(it);
            } else {
                ++it;
            }
        }
        auto& as = old.activeSlots;
        as.erase(std::remove(as.begin(), as.end(), idx), as.end());
        s.ownerPart = -1;
    }

    // Solta todas as vozes da parte e zera o estado de teclas seguras
    // (usado por allNotesOff e pela troca de voiceMode).
    void releaseAllHeld(int part) {
        auto& pr = parts_[part];
        for (auto& s : slots_) {
            if (s.ownerPart == part) {
                s.voice.noteOff();
                s.physicallyHeld = false;
                s.sustainHeld = false;
                s.sostenutoHeld = false;
            }
        }
        pr.heldStack.clear();
        pr.heldVelocity.clear();
        pr.activeSlots.clear();
        pr.noteToSlot.clear();
        pr.monoSoundingNote = -1;
    }

    void triggerSlot(int idx, int part, int note, int velocity, float extraDetuneCents, bool legato,
                     float glideFromHz) {
        auto& pr = parts_[part];
        auto& s = slots_[static_cast<size_t>(idx)];
        configureVoice(s.voice, pr, legato);
        int soundingNote = std::clamp(note + pr.params.transpose, 0, 127);
        s.ownerPart = part;
        s.note = note;
        s.physicallyHeld = true;
        s.sustainHeld = false;
        s.sostenutoHeld = false;
        s.triggerCounter = ++triggerCounter_;
        const PatchSnapshot* snap = pr.live(); // notas NOVAS sempre pegam o patch AO VIVO mais recente
        s.voice.bindWavetables(&snap->wavetables);
        s.voice.bindTuning(&snap->tuning);
        s.voice.noteOn(&snap->voice, &snap->compiled, soundingNote, velocity, extraDetuneCents, legato, glideFromHz,
                       snap->generation);
        s.voice.controllers().pitchBendSemis = pr.bend;
        s.voice.controllers().modWheel01 = pr.modWheel;
        for (int k = 0; k < VoiceParams::kNumMacros; ++k) s.voice.controllers().macro01[k] = pr.macro[k].load(std::memory_order_relaxed);
        s.voice.controllers().channelAT01 = pr.channelAT;
    }

    float soundingHz(const PartRuntime& pr, int note) const {
        return pr.live()->tuning.hz[std::clamp(note + pr.params.transpose, 0, 127)];
    }

public:
    // ultimo erro ao ler a afinacao da parte (vazio = ok), pra UI
    const std::string& tuningError(int part) const { return parts_[part].tuningError; }

private:

    void releaseSlot(int idx, int part, int note) {
        auto& pr = parts_[part];
        auto& s = slots_[static_cast<size_t>(idx)];
        s.physicallyHeld = false;
        if (pr.sustainDown) s.sustainHeld = true;
        if (pr.sostenutoDown && pr.sostenutoCaptured.count(note) > 0) s.sostenutoHeld = true;
        if (!s.sustainHeld && !s.sostenutoHeld) s.voice.noteOff();
    }

    void polyNoteOn(int part, int note, int velocity) {
        auto& pr = parts_[part];
        auto it = pr.noteToSlot.find(note);
        bool retrigger = (it != pr.noteToSlot.end());
        int idx = retrigger ? it->second : allocateSlot(part);
        if (!retrigger) detachSlot(idx);
        pr.noteToSlot[note] = idx;
        triggerSlot(idx, part, note, velocity, 0.0f, false, pr.lastNoteHz);
        pr.lastNoteHz = soundingHz(pr, note);
        auto& hs = pr.heldStack;
        hs.erase(std::remove(hs.begin(), hs.end(), note), hs.end());
        hs.push_back(note);
    }

    void polyNoteOff(int part, int note) {
        auto& pr = parts_[part];
        auto it = pr.noteToSlot.find(note);
        if (it == pr.noteToSlot.end()) return;
        int idx = it->second;
        pr.noteToSlot.erase(it);
        auto& hs = pr.heldStack;
        hs.erase(std::remove(hs.begin(), hs.end(), note), hs.end());
        releaseSlot(idx, part, note);
    }

    static float unisonOffset(int k, int voiceCount) {
        return voiceCount > 1 ? ((static_cast<float>(k) / static_cast<float>(voiceCount - 1)) - 0.5f) * 2.0f : 0.0f;
    }

    // Usado tanto por Mono (voiceCount=1) quanto Unison (voiceCount=unisonCount).
    void monoGroupNoteOn(int part, int note, int velocity, int voiceCount) {
        auto& pr = parts_[part];
        auto& hs = pr.heldStack;
        hs.erase(std::remove(hs.begin(), hs.end(), note), hs.end());
        hs.push_back(note);
        pr.heldVelocity[note] = velocity;
        if (!pr.activeSlots.empty()) monoGroupRetrigger(part, note, velocity);
        else monoGroupStart(part, note, velocity, voiceCount);
    }

    // Grupo novo (nenhuma voz do grupo soando): aloca voiceCount slots.
    void monoGroupStart(int part, int note, int velocity, int voiceCount) {
        auto& pr = parts_[part];
        pr.activeSlots.clear();
        float glideFrom = pr.lastNoteHz;
        for (int k = 0; k < voiceCount; ++k) {
            int idx = allocateSlot(part);
            detachSlot(idx);
            float t = unisonOffset(k, voiceCount);
            triggerSlot(idx, part, note, velocity, t * pr.params.unisonDetuneCents, false, glideFrom);
            slots_[static_cast<size_t>(idx)].subPan = t * pr.params.unisonSpread;
            pr.activeSlots.push_back(idx);
        }
        pr.monoSoundingNote = note;
        pr.lastNoteHz = soundingHz(pr, note);
    }

    // Reataque legato das vozes JA alocadas do grupo. O espalhamento de
    // detune usa o tamanho real do grupo (activeSlots), nao o unisonCount
    // atual, que pode ter mudado com a nota segura.
    void monoGroupRetrigger(int part, int note, int velocity) {
        auto& pr = parts_[part];
        int voiceCount = static_cast<int>(pr.activeSlots.size());
        for (int k = 0; k < voiceCount; ++k) {
            int idx = pr.activeSlots[static_cast<size_t>(k)];
            float t = unisonOffset(k, voiceCount);
            triggerSlot(idx, part, note, velocity, t * pr.params.unisonDetuneCents, true, 0.0f);
        }
        pr.monoSoundingNote = note;
        pr.lastNoteHz = soundingHz(pr, note);
    }

    void monoGroupNoteOff(int part, int note) {
        auto& pr = parts_[part];
        auto& hs = pr.heldStack;
        auto found = std::find(hs.begin(), hs.end(), note);
        if (found == hs.end()) return; // nota nunca aceita por esta parte (fora do range, parte desabilitada...)
        hs.erase(found);
        pr.heldVelocity.erase(note);

        // soltar uma nota que NAO e a que esta soando (ex.: a de baixo, com
        // outra segura por cima) nao muda nada no que se ouve
        if (note != pr.monoSoundingNote) return;

        if (!hs.empty()) {
            int prevNote = hs.back();
            auto v = pr.heldVelocity.find(prevNote);
            int velocity = v != pr.heldVelocity.end() ? v->second : 100;
            if (!pr.activeSlots.empty()) {
                monoGroupRetrigger(part, prevNote, velocity);
            } else {
                // as vozes do grupo foram roubadas por outra nota/parte
                int voiceCount = pr.runMode == VoiceMode::Unison ? std::max(1, pr.params.unisonCount) : 1;
                monoGroupStart(part, prevNote, velocity, voiceCount);
            }
        } else {
            for (int idx : pr.activeSlots) releaseSlot(idx, part, note);
            pr.activeSlots.clear();
            pr.monoSoundingNote = -1;
        }
    }

    struct RenderJob {
        int slot;
        float gL, gR, level;
        int activeLen; // amostras do pedaco em que a voz estava ativa
        // [2026-10-22] patch com rota pra ModDest::VoicePan: o pan muda por
        // amostra (panBuf_) e os ganhos sao calculados na mixagem. Sem a rota,
        // o caminho antigo (ganhos fixos no pedaco) -- saida identica a antes.
        bool panMod;
        float basePan; // pan da parte + subPan do unison (sem clamp)
    };

    static void renderJob(void* ctx, int index) {
        auto* self = static_cast<Synth*>(ctx);
        RenderJob& job = self->jobs_[static_cast<size_t>(index)];
        Voice& v = self->slots_[static_cast<size_t>(job.slot)].voice;
        float* smp = &self->voiceBuf_[static_cast<size_t>(index) * kRenderChunk];
        float* loud = &self->loudBuf_[static_cast<size_t>(index) * kRenderChunk];
        float* pan = &self->panBuf_[static_cast<size_t>(index) * kRenderChunk];
        const int len = self->chunkLen_;
        int i = 0;
        // isActive() e checado ANTES de cada amostra, como no laco antigo:
        // a voz que morre no meio do pedaco para ali (activeLen).
        for (; i < len && v.isActive(); ++i) {
            smp[i] = v.process();
            loud[i] = v.loudness();
            if (job.panMod) pan[i] = v.panMod();
        }
        job.activeLen = i;
    }

    void renderChunk(float* outL, float* outR, int len) {
        // 1 load atomico (acquire) por parte e por pedaco, nao por voz
        const PatchSnapshot* live[kNumParts];
        for (int p = 0; p < kNumParts; ++p) live[p] = parts_[p].live();

        bool panRoutes[kNumParts];
        for (int p = 0; p < kNumParts; ++p) {
            panRoutes[p] = false;
            for (const auto& r : live[p]->voice.modMatrix) panRoutes[p] |= r.dest == ModDest::VoicePan && r.src != ModSource::None;
        }
        const float bpm = tempoBpm_.load(std::memory_order_relaxed);

        // LFOs globais: uma conta por parte pro pedaco inteiro, antes das vozes
        const float* glfoPtr[kNumParts][kNumLfos] = {};
        const double beatsPerSample = bpm / (60.0 * sampleRate_);
        for (int p = 0; p < kNumParts; ++p) {
            auto& pr = parts_[p];
            const bool restart = pr.glfoRestart;
            pr.glfoRestart = false;
            for (int k = 0; k < kNumLfos; ++k) {
                const LFOParams& lp = live[p]->voice.lfo(k);
                if (!lp.global) continue;
                LFO& g = pr.glfo[k];
                g.configure(lp);
                g.setTempo(bpm);
                if (restart) g.noteOn();
                float* buf = pr.glfoBuf[k];
                if (lp.tempoSync) {
                    const double cycle = lfoDivisionBeats(lp.division);
                    for (int i = 0; i < len; ++i) {
                        const double b = (beatPos_ + i * beatsPerSample) / cycle;
                        buf[i] = g.processAtPhase(b - std::floor(b));
                    }
                } else {
                    for (int i = 0; i < len; ++i) buf[i] = g.process();
                }
                glfoPtr[p][k] = buf;
            }
        }

        jobs_.clear();
        for (size_t idx = 0; idx < slots_.size(); ++idx) {
            auto& s = slots_[idx];
            if (s.ownerPart < 0 || !s.voice.isActive()) continue;
            auto& pr = parts_[s.ownerPart];
            const PatchSnapshot* snap = live[s.ownerPart];
            s.voice.rebind(&snap->voice, &snap->compiled, snap->generation); // "ao vivo": pega qualquer recompile() publicado ate agora
            s.voice.bindWavetables(&snap->wavetables);
            s.voice.setTempo(bpm);
            s.voice.bindGlobalLfos(glfoPtr[s.ownerPart]);
            float pan = std::clamp(pr.params.pan + s.subPan, -1.0f, 1.0f);
            jobs_.push_back({static_cast<int>(idx), 1.0f - std::max(0.0f, pan), 1.0f + std::min(0.0f, pan), pr.params.level, 0,
                             panRoutes[s.ownerPart], pr.params.pan + s.subPan});
        }
        chunkLen_ = len;
        const int count = static_cast<int>(jobs_.size());
        // sem threads, ou bloco curto (renderStereo): acordar threads custaria mais que renderizar
        if (pool_.workers() > 0 && len >= 32) pool_.run(count);
        else for (int j = 0; j < count; ++j) renderJob(this, j);

        for (int i = 0; i < len; ++i) {
            float l = 0.0f, r = 0.0f, voiceLoad = 0.0f;
            for (int j = 0; j < count; ++j) {
                const RenderJob& job = jobs_[static_cast<size_t>(j)];
                if (i >= job.activeLen) continue;
                const size_t at = static_cast<size_t>(j) * kRenderChunk + static_cast<size_t>(i);
                voiceLoad += loudBuf_[at];
                float lvl = job.level * voiceBuf_[at];
                if (job.panMod) {
                    float p = std::clamp(job.basePan + panBuf_[at], -1.0f, 1.0f);
                    l += lvl * (1.0f - std::max(0.0f, p));
                    r += lvl * (1.0f + std::min(0.0f, p));
                } else {
                    l += lvl * job.gL;
                    r += lvl * job.gR;
                }
            }
            mixSample(l, r, voiceLoad);
            outL[i] = l;
            outR[i] = r;
        }
    }

    // Pos-mix de uma amostra: polifonia, FX globais, volume, limitador.
    void mixSample(float& outL, float& outR, float voiceLoad) {
        // [ADICIONADO em 2026-09-21] Compensacao de polifonia: 1/sqrt(N) e a
        // normalizacao classica pra somar N fontes de amplitude comparavel e
        // NAO-correlacionadas entre si (fases/timbres diferentes) -- preserva
        // a POTENCIA/RMS total aproximadamente CONSTANTE conforme N cresce,
        // ao contrario de 1/N (que fica exagerado/fraco demais ja com 2-3
        // vozes) ou de nao compensar nada (que e o que causava o clipping
        // documentado no bug da secao 7B item 5). Com N=1 (nota unica),
        // ganho=1.0 -- NADA muda em relacao ao comportamento de antes.
        // Suavizada (~30ms, one-pole) pra nao dar "zipper noise"/degrau
        // audivel quando uma nota entra ou sai de um acorde.
        // N nao e a contagem crua de vozes, e a soma de Voice::loudness():
        // vozes em release (ou presas no pedal com sustain 0) pesam menos,
        // senao cada nota nova de um acorde com pedal saia mais baixa.
        // [2026-10-28] Assimetrica: quando a carga SOBE (acorde atacado de
        // uma vez) o ganho cai em ~1ms -- com 30ms o acorde passava ~15ms
        // acima do limite; a carga ja segue o envelope de amplitude de cada
        // voz, entao nao ha degrau. Quando a carga desce, os 30ms de antes.
        float targetGain = 1.0f / std::sqrt(std::max(1.0f, voiceLoad));
        polyphonyGain_ += (targetGain - polyphonyGain_) * (targetGain < polyphonyGain_ ? polyDropCoef_ : polySmoothCoef_);
        outL *= polyphonyGain_;
        outR *= polyphonyGain_;

        // Efeitos globais pos-mix, na soma das 4 partes, na ordem de
        // globalFx_.order (ver dx7_globalfx.h; bloco desligado = identidade).
        fxChain_.process(globalFx_, outL, outR);

        // Volume geral (ver setMasterVolume): depois dos FX, antes do limitador.
        float volTarget = masterVolume_.load(std::memory_order_relaxed);
        masterGain_ += (volTarget - masterGain_) * volumeSmoothCoef_;
        if (std::fabs(masterGain_ - volTarget) < 1e-6f) masterGain_ = volTarget; // encaixa: volume 0 vira silencio EXATO
        outL *= masterGain_;
        outR *= masterGain_;

        // Limitador de master bus, camada 2 (rede de seguranca): mesmo com a
        // compensacao de polifonia acima, um patch com feedback pesado, um
        // filtro ressonante, ou os efeitos globais ainda PODEM empurrar
        // transientes pra alem de +-1.0 -- softLimit() garante que isso
        // nunca vira clipping duro, so uma compressao suave (ver secao 5).
        outL = softLimit(outL);
        outR = softLimit(outR);
    }

    double sampleRate_ = 48000.0;
    std::vector<VoiceSlot> slots_;
    PartRuntime parts_[kNumParts];
    uint64_t triggerCounter_ = 0;

    GlobalFxParams globalFx_;
    GlobalFxChain fxChain_;
    float polyphonyGain_ = 1.0f; // suavizado, ver renderStereo() (compensacao 1/sqrt(N))
    float polySmoothCoef_ = 0.0f, polyDropCoef_ = 0.0f, volumeSmoothCoef_ = 0.0f;
    std::atomic<float> masterVolume_{1.0f}; // alvo, escrito pela UI (setMasterVolume)
    std::atomic<float> tempoBpm_{120.0f};    // setTempoBpm (LFOs com tempoSync)
    double beatPos_ = 0.0;                   // so a audio thread (resetTransport, renderBlock)
    std::minstd_rand arpRng_{12345};         // modo Random do arpejador (semente fixa: testes reproduziveis)
    NoteObserver arpObs_ = nullptr;
    void* arpObsCtx_ = nullptr;
    float masterGain_ = 1.0f;               // valor suavizado, so a audio thread toca

    // renderBlock: buffers por voz (alocados em init, nunca na audio thread)
    std::vector<RenderJob> jobs_;
    std::vector<float> voiceBuf_, loudBuf_, panBuf_;
    int chunkLen_ = 0;
    WorkerPool pool_; // ultimo membro: destruido primeiro (para as threads antes do resto)
};

} // namespace dx7fm
