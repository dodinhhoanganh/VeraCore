# Native Audio Backends

VeraCore uses embedded FFmpeg libraries for compressed audio, never an
external `ffmpeg.exe` process. Direct codec SDK wrappers remain available as
fallbacks through `VeraCore.QT/NativeAudioCodecBackend.*`.

Embedded FFmpeg handles MP3, FLAC, OGG Vorbis, and WavPack when enabled by the
vcpkg manifest. Direct fallbacks use LAME/HIP, libFLAC, libogg/libvorbis, and
the WavPack SDK. Time-stretch/pitch-shift uses Elastique Pro.
Do not route FLAC through OGG Vorbis; OGG is a container path for Vorbis here.

Current wrapper state:

- WAV PCM/float import/export is built in.
- AIFF/AIFC PCM/float import/playback/render-source decode is built in.
- MP3 import/export is wired through LAME/HIP when headers/libs and feature flags are present.
- FLAC import/export is wired through libFLAC when headers/libs and feature flags are present.
- OGG Vorbis import/export is wired through libogg/libvorbis when headers/libs and feature flags are present.
- WavPack import/export is wired through the WavPack SDK when headers/libs and feature flags are present.
- Elastique is wired through an optional adapter header when headers/libs and feature flags are present.
- If Elastique is not linked, clip pitch shift falls back to native varispeed resampling during playback/render. This keeps no external-process dependency, but Elastique is still the intended high-quality backend for independent time-stretch plus pitch-shift.
- When Elastique is linked, clip sync/pitch settings are cached as native processed audio for playback/render. If processing fails, VeraCore falls back to the original decoded clip path instead of invoking external tools.

Set these MSBuild user macros when native SDKs are available:

- `NativeAudioBackendIncludeDirs`: include paths for LAME, FLAC, Vorbis, WavPack, Elastique.
- `NativeAudioBackendLibDirs`: library directories for those SDKs.
- `NativeAudioBackendLibraries`: native `.lib` names.
- `NativeAudioBackendDefinitions`: feature flags below.

Local setup: copy `VeraCore.QT/NativeAudioBackends.props.example` to
`VeraCore.QT/NativeAudioBackends.local.props`, then edit SDK include paths,
library paths, library names, and feature flags. The project imports the local
props file only when present, so machine-specific SDK paths stay out of the
shared `.vcxproj`.

Feature flags:

- `VC_AUDIO_WITH_FFMPEG=1` plus `VC_AUDIO_FFMPEG_READY=1` for embedded FFmpeg import/export.
- `VC_AUDIO_WITH_LAME=1` plus `VC_AUDIO_LAME_ENCODER_READY=1` for MP3 export.
- `VC_AUDIO_WITH_LAME=1` plus `VC_AUDIO_MP3_DECODER_READY=1` for MP3 import/playback.
- `VC_AUDIO_WITH_FLAC=1` plus `VC_AUDIO_FLAC_CODEC_READY=1` for FLAC import/export.
- `VC_AUDIO_WITH_VORBIS=1` plus `VC_AUDIO_VORBIS_CODEC_READY=1` for OGG Vorbis import/export.
- `VC_AUDIO_WITH_WAVPACK=1` plus `VC_AUDIO_WAVPACK_CODEC_READY=1` for WavPack import/export.
- `VC_AUDIO_WITH_ELASTIQUE=1` plus `VC_AUDIO_ELASTIQUE_PROCESSOR_READY=1` for time-stretch/pitch-shift through `VeraCoreElastiqueAdapter.h`.

Elastique adapter contract:

```cpp
bool VeraCoreElastiqueProcessFile(const QString& sourcePath,
                                  const QString& destinationPath,
                                  double tempoRatio,
                                  double pitchShiftSemitones,
                                  int sampleRate);
```

The adapter should return `true` only after writing a complete destination audio file.

Do not set a `*_READY` flag until the matching wrapper code is implemented and verified.
