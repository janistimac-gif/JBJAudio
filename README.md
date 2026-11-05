# JBJAudio

Mešalnik zvoka za Windows (JUCE 8 / C++17), zasnovan po funkcionalnosti programa Maono Arena.

## Funkcije
- poljubno število kanalov; vsak ima svojo vhodno napravo (mikrofon, virtualni kabel, Stereo Mix ...)
- fader (-60..+6 dB), Mute, merilnik ravni, preimenovanje kanala (dvoklik na ime)
- redukcija šuma na kanalu: **Off / Gate / AI**
- **SoundPad**: 8 gumbov (wav/mp3/flac/aiff/ogg); klik = predvajaj/ustavi, desni klik = dodeli/počisti, tipke 1-8, glasnost padov; zvok gre skozi Master
- **Reverb** na celotnem miksu (drsnik v zgornji vrstici)
- **Ikona v sistemski vrstici**: zapiranje okna ga skrije, meni (desni klik): Show, Start with Windows, Quit; zagon z `--tray` ostane skrit
- **Globalne bližnjice** (delujejo tudi v ozadju): Ctrl+Alt+M = mute obeh vodil, Ctrl+Alt+Gor/Dol = glasnost ±2 dB, Ctrl+Alt+1..8 = SoundPad
- dve izhodni vodili: **Headphones** (glavna izhodna naprava) in **Stream** (druga izhodna naprava, npr. virtualni kabel, iz katerega snema OBS/Discord); vsak kanal ima gumba **HP** / **ST**, ki določata, v katero vodilo pošilja; vsako vodilo ima svoj fader, mute in merilnik
- stanje se shrani v `%APPDATA%\JBJAudio\mixer.xml`

## Prevajanje (Windows, Visual Studio 2022 + CMake)
```
cmake -S . -B build
cmake --build build --config Release
```
JUCE 8.0.4 se prenese samodejno. Z `-DJUCE_DIR=<pot>` uporabite lokalni JUCE.
Zagon: `build\JBJAudio_artefacts\Release\JBJAudio.exe`

## Opombe
- Vsi vhodi in izhod morajo imeti isto vzorčno frekvenco (npr. 48 kHz, nastavite v Windows Sound).
- Vsaka naprava ima svojo uro, zato jedro samodejno omejuje zakasnitev (FIFO) ob odstopanju.
- Za usmerjanje zvoka aplikacij (Chat, Game ...) potrebujete virtualno napravo (npr. VB-CABLE);
  JBJAudio nima lastnega gonilnika.

## Brez Visual Studia: samodejna gradnja na GitHubu
Naložite mapo na GitHub (privatni repozitorij je v redu). Delovni tok `.github/workflows/build.yml`
sam zgradi `JBJAudio.exe` na Windows; prenesete ga pod Actions → zadnji zagon → Artifacts → `JBJAudio-windows-x64`.
Če gradnja pade, je izpis napake v istem zagonu.

## Redukcija šuma
- **Gate**: vgrajen DSP, deluje vedno.
- **AI**: odprtokodni DeepFilterNet (https://github.com/Rikorose/DeepFilterNet), naložen med delovanjem.
  Poleg `JBJAudio.exe` (ali v `%APPDATA%\JBJAudio`) položite:
  1. `deepfilter.dll` (DeepFilterNet C API: df_create, df_process_frame ...; zgradite z
     `cargo build -p deep_filter --features capi --release` ali uporabite svojo obstoječo),
  2. model `DeepFilterNet3_onnx.tar.gz` (iz izdaj DeepFilterNet).
  Naprava mora delovati pri 48 kHz. Če datotek ni, program to sporoči v vrstici stanja in ostane brez AI.

## SoundPad: opombe
- Zvoki se naložijo v pomnilnik in pretvorijo na vzorčno frekvenco izhoda ob nalaganju (če zamenjate izhod z drugo frekvenco, jih znova dodelite ali ponovno zaženite program).
- Bližnjice 1-8 delujejo, ko je okno aktivno (globalnih bližnjic še ni).

- Stream naprava mora imeti isto vzorčno frekvenco kot Headphones naprava; Ctrl+Alt+Gor/Dol spreminja glasnost vodila Headphones.
