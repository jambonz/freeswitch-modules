#ifndef __MOD_VERBIO_TTS_H__
#define __MOD_VERBIO_TTS_H__

#include <switch.h>
#include <speex/speex_resampler.h>

typedef struct verbio_data {
  char *voice_name;
  char *access_token;

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
} verbio_t;

#endif