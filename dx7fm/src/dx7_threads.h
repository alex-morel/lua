#pragma once
#include <atomic>
#include <cstdint>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <vector>

// [ADICIONADO em 2026-10-20] Pool de threads pra renderizar vozes em
// paralelo (Synth::renderBlock). Projeto x64/Linux.
//
// Uso: a cada bloco de audio, run(count) executa job(ctx, i) pra i em
// [0, count). A PROPRIA thread que chama (a de audio) tambem trabalha:
// pega indices como as outras e, no fim, so espera os que ja estao em
// andamento -- nunca fica parada esperando uma thread que nem acordou.
//
// Protocolo (sem trava):
//  - cursor_ (64 bits) = [ job:24 | count:20 | proximo:20 ]. Uma thread
//    so pega o indice `proximo` com um compare-exchange no cursor inteiro;
//    como o job e o count estao no MESMO atomico, uma thread que acordou
//    atrasada (ainda no job anterior) nunca pega indice do job novo: o CAS
//    falha porque o job mudou.
//  - done_ conta indices terminados; o chamador espera done_ == count
//    (acquire), o que tambem publica pra ele o que os workers escreveram.
//  - Os workers dormem num futex (std::atomic::wait) entre blocos; o
//    chamador acorda todos com notify_all.
// Tempo real: os workers tentam SCHED_FIFO (precisa de rtprio/limits do
// usuario; se nao der, ficam com prioridade normal -- funciona, so com mais
// risco de atraso sob carga).
namespace dx7fm {

class WorkerPool {
public:
    using JobFn = void (*)(void* ctx, int index);

    WorkerPool() = default;
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool() { stop(); }

    // nWorkers threads ALEM da que chama run(). 0 = tudo na thread que chama.
    void start(int nWorkers, JobFn fn, void* ctx) {
        stop();
        fn_ = fn;
        ctx_ = ctx;
        quit_.store(false, std::memory_order_relaxed);
        for (int i = 0; i < nWorkers; ++i) threads_.emplace_back([this] { workerLoop(); });
        for (auto& t : threads_) {
            sched_param sp{};
            sp.sched_priority = 70;
            pthread_setschedparam(t.native_handle(), SCHED_FIFO, &sp); // melhor esforco
        }
    }

    void stop() {
        if (threads_.empty()) return;
        quit_.store(true, std::memory_order_release);
        wake_.fetch_add(1, std::memory_order_release);
        wake_.notify_all();
        for (auto& t : threads_) t.join();
        threads_.clear();
    }

    int workers() const { return static_cast<int>(threads_.size()); }

    void run(int count) {
        if (count <= 0) return;
        if (threads_.empty() || count == 1) {
            for (int i = 0; i < count; ++i) fn_(ctx_, i);
            return;
        }
        job_ = (job_ + 1) & kJobMask;
        done_.store(0, std::memory_order_relaxed);
        cursor_.store((static_cast<uint64_t>(job_) << 40) | (static_cast<uint64_t>(count) << 20), std::memory_order_release);
        wake_.fetch_add(1, std::memory_order_release);
        wake_.notify_all();
        work(job_);
        while (done_.load(std::memory_order_acquire) < count) _mm_pause();
    }

private:
    static constexpr uint64_t kJobMask = (1u << 24) - 1, kFieldMask = (1u << 20) - 1;

    void work(uint32_t job) {
        uint64_t c = cursor_.load(std::memory_order_acquire);
        for (;;) {
            if ((c >> 40) != job) return;
            uint64_t count = (c >> 20) & kFieldMask, next = c & kFieldMask;
            if (next >= count) return;
            if (cursor_.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                fn_(ctx_, static_cast<int>(next));
                done_.fetch_add(1, std::memory_order_release);
                c = cursor_.load(std::memory_order_acquire);
            }
        }
    }

    void workerLoop() {
        uint32_t seen = wake_.load(std::memory_order_acquire);
        for (;;) {
            wake_.wait(seen, std::memory_order_acquire);
            seen = wake_.load(std::memory_order_acquire);
            if (quit_.load(std::memory_order_acquire)) return;
            work(static_cast<uint32_t>(cursor_.load(std::memory_order_acquire) >> 40));
        }
    }

    std::vector<std::thread> threads_;
    JobFn fn_ = nullptr;
    void* ctx_ = nullptr;
    uint32_t job_ = 0; // so a thread que chama run() escreve
    std::atomic<uint64_t> cursor_{0};
    std::atomic<int> done_{0};
    std::atomic<uint32_t> wake_{0};
    std::atomic<bool> quit_{false};
};

} // namespace dx7fm
