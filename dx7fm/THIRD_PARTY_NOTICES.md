# Componentes de terceiros e licenças

O dx7fm inclui código e algoritmos portados de projetos open source. Como dois
deles são GPL (MVerb, GPLv3; ZamComp, GPLv2 ou posterior), **o dx7fm como um
todo é distribuído sob a GNU GPL versão 3** (texto completo em `LICENSE`).
Quem distribuir o programa (binário ou fonte) precisa oferecer o código-fonte
completo sob a GPLv3.

| Componente | Onde é usado | Autor | Licença | Forma |
|---|---|---|---|---|
| Dear ImGui | interface do app (`third_party/imgui`) | Omar Cornut | MIT | vendorizado sem mudanças |
| MVerb | reverb "Plate" (`third_party/mverb/MVerb.h`) | Martin Eastwood | GPLv3+ | vendorizado sem mudanças |
| ZamComp | compressor (`src/dx7_globalfx.h`, `ZamCompressor`) | Damien Zammit | GPLv2+ | porta (C++), com mudanças |
| Rings — Ensemble | chorus (`EnsembleChorus`) | Emilie Gillet (Mutable Instruments) | MIT | porta, com mudanças |
| Clouds — Reverb/FxEngine | reverb "Hall" (`CloudsHall`) | Emilie Gillet (Mutable Instruments) | MIT | porta, com mudanças |
| Freeverb | reverb "Room" (`FreeverbRoom`) | Jezar at Dreampoint | domínio público | porta fiel |
| Faust phaflangers.lib | phaser e flanger (`Phaser`, `Flanger`) | Julius O. Smith III | STK-4.3 (estilo MIT) | porta, com mudanças |
| Audio EQ Cookbook | EQ de 3 bandas (`Eq3`) | Robert Bristow-Johnson | fórmulas publicadas | implementação própria |
| msfa (music-synthesizer-for-android) | importação de SysEx do DX7 (`src/dx7_sysex.h`): tabela dos 32 algoritmos e fórmulas de conversão | Google Inc. (Raph Levien) | Apache-2.0 | porta (tabela + fórmulas), com mudanças |
| Airwindows (Drive, Tube2, Distortion, Density2, ToTape6, Gatelope, ClipSoftly, Pockey2, Tremolo, AutoPan, Vibrato, RingModulator) | efeitos do rack (`src/airwindows/*.h`) | Chris Johnson (airwindows) | MIT | gerados por `tools/ports/airwindows_port.py`: estado, construtor e `processReplacing` originais, sem a camada VST; só a semente do dither mudou |
| CHOW Tape Model | simulador de fita do rack (`src/chowtape/ChowTape.h`) | Jatin Chowdhury | GPLv3 | porta (histerese, tom, perdas, degrade, chew, wow/flutter), com mudanças descritas no cabeçalho |
| Surge XT — chowdsp/tape | base sem JUCE da porta do CHOW Tape | Surge Synth Team | GPLv3 | porta, com correções conferidas no original |
| ZetaCarinaeModules (processo de Ornstein-Uhlenbeck, via CHOW Tape) | variação do wow (`chowtape::WowFlutter`) | Michael Hampton | GPLv3 | porta |
| sst-effects — RotarySpeaker | rotary/Leslie do rack (`fxextra::Rotary`, `src/dx7_fx_extra.h`) | Surge Synth Team | GPLv3 | porta do algoritmo, com infraestrutura própria e chave lento/rápido com inércia |
| Faust vaeffects.lib (crybaby, autowah) | wah do rack (`fxextra::Wah`) | Julius O. Smith III | STK-4.3 (estilo MIT) | porta |
| Clouds — Grain, GranularSamplePlayer, Diffuser e tabelas | efeito granular "Micro" (`src/dx7_fx_micro.h`) | Emilie Gillet (Mutable Instruments) | MIT | porta, com mudanças (buffer float, qualquer taxa); as 11 variações por cima são próprias |
| Clouds — PitchShifter | pitch shifter do rack (`fxextra::PitchShifter`) | Emilie Gillet (Mutable Instruments) | MIT | porta, com mudanças |
| freeverb3 | base dos reverbs Hall, Room, Plate e Reflections (`third_party/freeverb3`) | Teru Kamogashira (e Fons Adriaensen em zrev) | GPLv2 ou posterior | vendorizado (a cópia do Dragonfly), com 2 mudanças mecânicas descritas em `third_party/freeverb3/README.dx7fm` |
| Dragonfly Reverb | Hall, Room, Plate e Reflections do rack (`src/dx7_fx_reverbs.h`) | Michael Willis | GPLv3 | porta da configuração e dos laços (classes NRev/NRevB), com valores dos presets do plugin; os laços do Plate são chamados direto (no plugin distribuído eles não entram) |
| Cloud Seed Core | reverb Cloud, Swell e Bloom (`third_party/cloudseed`) | Ghost Note Audio | MIT | vendorizado, com 1 mudança mecânica (`third_party/cloudseed/README.dx7fm`) |
| Surge XT — chowdsp/spring_reverb | reverb Spring (`reverbs::Spring`) | Jatin Chowdhury / Surge Synth Team | GPLv3 | porta escalar, com linhas de atraso do tamanho que usam |
| Roboto Medium | fonte da interface (`third_party/imgui/misc/fonts/Roboto-Medium.ttf`) | Google (Christian Robertson) | Apache 2.0 | arquivo distribuído com o Dear ImGui, carregado em tempo de execução |

As mudanças feitas em cada porta estão descritas no comentário de cabeçalho
do bloco correspondente em `src/dx7_globalfx.h`.

## Origem dos arquivos (baixados em 2026-10-18)

- MVerb: <https://github.com/martineastwood/mverb> (`MVerb.h`, `LICENSE`)
- ZamComp: <https://github.com/zamaudio/zam-plugins> (`plugins/ZamComp/ZamCompPlugin.cpp`)
- Mutable Instruments: <https://github.com/pichenettes/eurorack> (`rings/dsp/fx/ensemble.h`,
  `clouds/dsp/fx/reverb.h`, `clouds/dsp/fx/fx_engine.h`; parâmetros do reverb conforme
  `clouds/dsp/granular_processor.cc`) e <https://github.com/pichenettes/stmlib>
  (`dsp/cosine_oscillator.h`, só como referência do LFO)
- Freeverb (distribuição original de Jezar): <https://github.com/chiptunes-swift/freeverb>
  (`freeverb/Components/*`)
- Faust: <https://github.com/grame-cncm/faustlibraries> (`phaflangers.lib`, `licenses/stk-4.3.0.md`)
- Baixados em 2026-10-29 (rack de efeitos):
  - Airwindows: <https://github.com/airwindows/airwindows> (`plugins/LinuxVST/src/<Nome>/<Nome>.h`,
    `<Nome>.cpp`, `<Nome>Proc.cpp`; `LICENSE`)
  - CHOW Tape: <https://github.com/jatinchowdhury18/AnalogTapeModel>
    (`Plugin/Source/Processors/Timing_Effects/*`, `Loss_Effects/LossFilter.cpp`, `Degrade/DegradeProcessor.cpp`)
  - Surge XT: <https://github.com/surge-synthesizer/surge> (`src/common/dsp/effects/chowdsp/tape/*`,
    `chowdsp/shared/Shelf.h`, `BilinearUtils.h`, `chowdsp/TapeEffect.cpp`)
  - sst-effects: <https://github.com/surge-synthesizer/sst-effects> (`include/sst/effects/RotarySpeaker.h`)
  - Faust: `vaeffects.lib` (crybaby, autowah) do mesmo repositório de cima
  - Clouds: `clouds/dsp/fx/pitch_shifter.h` do mesmo repositório da Mutable de cima; e `clouds/dsp/grain.h`,
    `granular_sample_player.h`, `fx/diffuser.h`, `clouds/resources/lookup_tables.py` (efeito Micro)
  - Dragonfly Reverb: <https://github.com/michaelwillis/dragonfly-reverb> (`common/freeverb/*`,
    `common/fv3_config.h`, `plugins/dragonfly-*/DSP.cpp`, `DistrhoPluginInfo.h`)
  - Cloud Seed Core: <https://github.com/GhostNoteAudio/CloudSeedCore> (`DSP/*`, `Parameters.*`, `LICENSE`)
  - Surge XT: `src/common/dsp/effects/chowdsp/spring_reverb/*` e `chowdsp/shared/chowdsp_DelayInterpolation.h`
- msfa (baixado em 2026-10-24): <https://github.com/google/music-synthesizer-for-android>
  (`app/src/main/jni/fm_core.cc`, `dx7note.cc`, `env.cc`, `lfo.cc`)

---

## Textos de licença e avisos exigidos

### Mutable Instruments (Rings, Clouds) — MIT

```
Copyright 2014 Emilie Gillet.

Author: Emilie Gillet (emilie.o.gillet@gmail.com)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### Airwindows — MIT

```
MIT License

Copyright (c) 2018 Chris Johnson

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Cloud Seed Core — MIT

```
MIT License

Copyright (c) 2024 Ghost Note Audio

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### freeverb3 — GNU GPL v2 ou posterior

Copyright (C) 2006-2018 Teru Kamogashira; partes (zrev) Copyright (C)
2003-2010 Fons Adriaensen. Licenciado "GPL versão 2 ou (a seu critério)
qualquer versão posterior" — usado aqui sob a GPLv3. Texto da GPLv2 em
`third_party/freeverb3/COPYING`.

### CHOW Tape Model, Surge XT, sst-effects, ZetaCarinaeModules, Dragonfly Reverb — GNU GPL v3

O código portado desses projetos é GPLv3, a mesma licença do dx7fm (texto
completo em `LICENSE`). Avisos de copyright: CHOW Tape Model © Jatin
Chowdhury; Surge XT © 2018-2024 Surge Synth Team e autores (originalmente
© Claes Johanson / Vember Audio 2004-2018); sst-effects © Surge Synth Team;
ZetaCarinaeModules © Michael Hampton; Dragonfly Reverb © Michael Willis.

### Faust phaflangers.lib e vaeffects.lib (funções de Julius O. Smith III) — STK-4.3

Copyright (C) 2003-2017 Julius O. Smith III, liberado sob a licença STK-4.3:

```
The Synthesis ToolKit in C++ (STK)
Copyright (c) 1995--2017 Perry R. Cook and Gary P. Scavone

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

Any person wishing to distribute modifications to the Software is
asked to send the modifications to the original developer so that they
can be incorporated into the canonical version.  This is, however, not
a binding provision of this license.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### msfa (music-synthesizer-for-android) — Apache License 2.0

```
Copyright 2012 Google Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

Mudanças: a tabela de algoritmos (`algorithms[32]` de `fm_core.cc`) é
interpretada por simulação dos barramentos pra montar a matriz livre deste
motor, incluindo o laço de feedback entre operadores dos algoritmos 4 e 6
(que a msfa não implementa); as fórmulas de `dx7note.cc`, `env.cc` e
`lfo.cc` foram reescritas como conversões pros parâmetros deste motor. Texto
completo da licença em `third_party/licenses/Apache-2.0.txt` (compatível com
a GPLv3 do projeto).

### Freeverb — domínio público

```
Written by Jezar at Dreampoint, June 2000
http://www.dreampoint.co.uk
This code is public domain
```

### ZamComp — GNU GPL v2 ou posterior

```
ZamComp mono compressor
Copyright (C) 2014  Damien Zammit <damien@zamaudio.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of
the License, or (at your option) any later version.
```

Usado aqui sob a opção "versão posterior", ou seja, GPLv3 (ver `LICENSE`).

### MVerb — GNU GPL v3 ou posterior

```
Copyright (c) 2010 Martin Eastwood
This code is distributed under the terms of the GNU General Public License
```

Texto completo em `third_party/mverb/LICENSE` (idêntico a `LICENSE`).

### Dear ImGui — MIT

Ver `third_party/imgui/LICENSE.txt`.

### Roboto — Apache License 2.0

A fonte Roboto (Copyright Google, desenhada por Christian Robertson) é
licenciada sob a Apache License, Version 2.0
(<https://www.apache.org/licenses/LICENSE-2.0>), compatível com a GPLv3. O app
carrega o arquivo `Roboto-Medium.ttf` que vem com o Dear ImGui; sem ele, usa a
fonte embutida do ImGui.
