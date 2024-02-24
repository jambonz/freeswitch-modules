#ifndef __MOD_AZURE_TTS_H__
#define __MOD_AZURE_TTS_H__

#include <switch.h>
#include <speex/speex_resampler.h>
typedef struct azure_data {
  char *voice_name;
  char *api_key;
  char *region;
  char *language;
  char *endpoint;
  char *endpointId;

  /* result data */
  long response_code;
  char *session_id;
  char *cache_filename;
  char *err_msg;

  int rate;
  int draining;
  int reads;
  int cache_audio;
  int flushed;

  FILE *file;
  void *audioStream;
  SpeexResamplerState *resampler;
} azure_t;

#endif