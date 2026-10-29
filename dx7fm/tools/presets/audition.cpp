// [ADICIONADO em 2026-10-29] Audicao dos patches de fabrica durante o design:
// renderiza a previa de cada preset (ou so os que contem o filtro no nome),
// grava o .wav e imprime pico, o maior RMS (janelas de 50 ms) e o envelope em
// dB relativo a ele. Uso, na pasta dx7fm/:
//   ./build/audition [pasta_dos_wav] [filtro]
#include "factory_bank.h"
#include "wav_writer.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
using namespace dx7fm;
using namespace dx7fm_app::factory;

static double rmsAt(const std::vector<float>& x, double t, double len) {
    const size_t a = static_cast<size_t>(t * 48000), b = std::min(x.size(), a + static_cast<size_t>(len * 48000));
    double e = 0.0;
    for (size_t i = a; i < b; ++i) e += static_cast<double>(x[i]) * x[i];
    return b > a ? std::sqrt(e / static_cast<double>(b - a)) : 0.0;
}

int main(int argc, char** argv) {
    const std::filesystem::path dir = argc > 1 ? argv[1] : "output/audition";
    const std::string filter = argc > 2 ? argv[2] : "";
    std::filesystem::create_directories(dir);
    std::printf("%-26s %5s %7s | dB do maior RMS em: 0.1s  0.5s    1s    2s  fim da nota\n", "", "pico", "RMSmax");
    for (const auto& f : factoryBank()) {
        if (!filter.empty() && f.part.name.find(filter) == std::string::npos) continue;
        std::vector<float> L, R;
        renderPreview(f, f.part, L, R);
        float pk = 0.0f;
        for (size_t i = 0; i < L.size(); ++i) pk = std::max({pk, std::fabs(L[i]), std::fabs(R[i])});
        double mx = 1e-9;
        for (double t = 0.0; t < f.holdSec; t += 0.05) mx = std::max(mx, rmsAt(L, t, 0.05));
        auto db = [&](double t) { return 20.0 * std::log10(rmsAt(L, t, 0.05) / mx + 1e-9); };
        std::printf("%-26s %5.2f %7.3f | %22.1f %5.1f %5.1f %5.1f %6.1f\n", f.part.name.c_str(), pk, mx, db(0.1), db(0.5), db(1.0), db(2.0),
                    db(f.holdSec - 0.06));
        std::string fn;
        for (char c : f.part.name) fn += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        writeWavStereo((dir / (fn + ".wav")).string(), L, R, 48000);
    }
}
