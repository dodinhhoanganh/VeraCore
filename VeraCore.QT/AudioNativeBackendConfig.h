#pragma once

// Native codec/time-stretch feature gates. Keep these off unless the matching
// SDK headers and libraries are linked into the app.
#ifndef VC_AUDIO_WITH_LAME
#define VC_AUDIO_WITH_LAME 0
#endif

#ifndef VC_AUDIO_WITH_FLAC
#define VC_AUDIO_WITH_FLAC 0
#endif

#ifndef VC_AUDIO_WITH_VORBIS
#define VC_AUDIO_WITH_VORBIS 0
#endif

#ifndef VC_AUDIO_WITH_WAVPACK
#define VC_AUDIO_WITH_WAVPACK 0
#endif

#ifndef VC_AUDIO_WITH_ELASTIQUE
#define VC_AUDIO_WITH_ELASTIQUE 0
#endif

#ifndef VC_AUDIO_WITH_FFMPEG
#define VC_AUDIO_WITH_FFMPEG 0
#endif

#ifndef VC_AUDIO_LAME_ENCODER_READY
#define VC_AUDIO_LAME_ENCODER_READY 0
#endif

#ifndef VC_AUDIO_MP3_DECODER_READY
#define VC_AUDIO_MP3_DECODER_READY 0
#endif

#ifndef VC_AUDIO_FLAC_CODEC_READY
#define VC_AUDIO_FLAC_CODEC_READY 0
#endif

#ifndef VC_AUDIO_VORBIS_CODEC_READY
#define VC_AUDIO_VORBIS_CODEC_READY 0
#endif

#ifndef VC_AUDIO_WAVPACK_CODEC_READY
#define VC_AUDIO_WAVPACK_CODEC_READY 0
#endif

#ifndef VC_AUDIO_ELASTIQUE_PROCESSOR_READY
#define VC_AUDIO_ELASTIQUE_PROCESSOR_READY 0
#endif

#ifndef VC_AUDIO_FFMPEG_READY
#define VC_AUDIO_FFMPEG_READY 0
#endif
