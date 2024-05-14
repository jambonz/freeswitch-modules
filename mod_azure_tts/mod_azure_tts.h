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
  char *http_proxy_ip;
  char *http_proxy_port;

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

  void *startTime;

  FILE *file;
  SpeexResamplerState *resampler;
  void *circularBuffer;
  switch_mutex_t *mutex;

  int has_last_byte;
  uint8_t last_byte;
} azure_t;

#endif