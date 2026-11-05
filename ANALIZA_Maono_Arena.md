# Analiza Maono Arena (statična, objdump/strings)

**MaonoArena.exe** (x64, 27 MB): aplikacija JUCE (C++, `JUCEWindow`, `juce::dsp`, WASAPI, GUI v XML).
Razredi: MixerDevice, MixerChannel, MixerLabel, MonitorDevice, VirtualDevices, AudioDevices,
SoundPadButton, AudioFilePlay, ReverbAudioSource, DeviceAudioMeter, G2/G3SettingComponent,
VersionUpgrade, HookThread (globalne bližnjice), CustomSystemTrayIcon, AiNoiseThread.
Vsebuje tudi Rust (tract-onnx, rustfft/realfft) za AI zmanjševanje šuma.
Uvozi: WINMM, SETUPAPI/CFGMGR32 (zaznava naprav), WININET (posodobitve), dxgi, WTSAPI32.
UI: kanali System/Chat/Stream/Headphone/Game/Music/Browser/Virtual, Mute toggle, Volume control,
Fast Mute, SoundPad (wav/mp3/flac, strani, bližnjice), monitor mikrofona, Noise Reduction (AI/DSP/white/colored),
svetlobni obroč naprave, firmware upgrade, avtostart, tray.

**deepfilter.dll** (x64 Rust): DeepFilterNet (izvoz `df_create`, `df_process_frame`, `df_process_frame_raw`,
`df_set_atten_lim`, `df_set_post_filter_beta`, `df_get_frame_length`, `df_free`, `df_next_log_msg`, `df_free_log_msg`).

**uninst.exe**: NSIS 3.0 odstranjevalnik (registry + datoteke), zahteva administratorja.

Opomba: rea orodja so tekla na vaši napravi in niso videla datotek v oblaku, zato je analiza statična.
