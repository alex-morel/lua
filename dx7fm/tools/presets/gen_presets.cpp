// [ADICIONADO em 2026-10-28] Grava o banco de fabrica (app/factory_bank.h) em
// patches/ (formato do app) e uma previa .wav de cada patch em
// output/presets/, conferindo que soam (sem silencio, sem NaN, sem clipar).
// Uso, na pasta dx7fm/:
//   ./build/gen_presets [pasta_dos_patches] [pasta_das_previas]
// Os patches sao conferidos por tools/presets/patch_tests.cpp.
#include "factory_bank.h"
#include "dx7_patch_io.h"
#include "wav_writer.h"
#include "patch_browser.h"
#include <cstdio>
#include <filesystem>
using namespace dx7fm;
using namespace dx7fm_app::factory;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const fs::path patchDir = argc > 1 ? argv[1] : "patches", wavDir = argc > 2 ? argv[2] : "output/presets";
    fs::create_directories(patchDir);
    fs::create_directories(wavDir);
    int bad = 0;
    for (auto& p : factoryBank()) {
        sanitize(p.part);
        const fs::path file = dx7fm_app::patchPathFor(patchDir, p.part.name);
        if (!savePartFile(file.string(), p.part)) { std::printf("FALHOU ao salvar %s\n", file.c_str()); ++bad; continue; }
        // previa: carrega do arquivo (testa o round trip) e toca
        PartParams loaded;
        loadPartFile(file.string(), loaded);
        std::vector<float> L, R;
        renderPreview(p, loaded, L, R);
        const int total = static_cast<int>(L.size());
        double sum = 0.0; float peak = 0.0f; bool finite = true;
        for (int i = 0; i < total; ++i) {
            finite &= std::isfinite(L[i]) && std::isfinite(R[i]);
            peak = std::max({peak, std::fabs(L[i]), std::fabs(R[i])});
            sum += static_cast<double>(L[i]) * L[i];
        }
        const double rms = std::sqrt(sum / total);
        writeWavStereo((wavDir / (file.stem().string() + ".wav")).string(), L, R, 48000);
        const bool ok = finite && rms > 0.01 && peak <= 1.0f;
        bad += !ok;
        std::printf("%-26s %-34s pico %.2f  RMS %.3f %s\n", p.part.name.c_str(), file.filename().c_str(), peak, rms, ok ? "" : "<-- PROBLEMA");
    }
    return bad;
}
