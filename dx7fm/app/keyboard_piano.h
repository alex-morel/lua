#pragma once
#include <SDL.h>
#include <algorithm>
#include <unordered_map>

namespace dx7fm_app {

// Mapeamento classico de "teclado do PC como piano" (layout de posicao
// fisica via SDL_Scancode, entao funciona igual independente do layout de
// idioma do teclado): A W S E D F T G Y H U J = uma oitava cromatica a
// partir de C, K O L P ; continuam mais 5 semitons acima. Z/X descem/sobem
// a oitava base.
class KeyboardPiano {
public:
    void setBaseNote(int note) { baseNote_ = std::clamp(note, 0, 127); }
    int baseNote() const { return baseNote_; }
    void shiftOctave(int deltaOctaves) { baseNote_ = std::clamp(baseNote_ + deltaOctaves * 12, 0, 127); }

    // Retorna a nota MIDI pro scancode, ou -1 se a tecla nao for mapeada.
    int noteForScancode(SDL_Scancode sc) const {
        auto it = kMap.find(sc);
        if (it == kMap.end()) return -1;
        return std::clamp(baseNote_ + it->second, 0, 127);
    }

    // true se a tecla e usada pro mapeamento de piano (pra nao confundir
    // com atalhos de UI, ex.: se algum dia usarmos letras pra outra coisa).
    bool isPianoKey(SDL_Scancode sc) const { return kMap.count(sc) > 0; }

private:
    static const std::unordered_map<SDL_Scancode, int> kMap;
    int baseNote_ = 60; // C4
};

inline const std::unordered_map<SDL_Scancode, int> KeyboardPiano::kMap = {
    {SDL_SCANCODE_A, 0},  {SDL_SCANCODE_W, 1},  {SDL_SCANCODE_S, 2},  {SDL_SCANCODE_E, 3},
    {SDL_SCANCODE_D, 4},  {SDL_SCANCODE_F, 5},  {SDL_SCANCODE_T, 6},  {SDL_SCANCODE_G, 7},
    {SDL_SCANCODE_Y, 8},  {SDL_SCANCODE_H, 9},  {SDL_SCANCODE_U, 10}, {SDL_SCANCODE_J, 11},
    {SDL_SCANCODE_K, 12}, {SDL_SCANCODE_O, 13}, {SDL_SCANCODE_L, 14}, {SDL_SCANCODE_P, 15},
    {SDL_SCANCODE_SEMICOLON, 16},
};

} // namespace dx7fm_app
