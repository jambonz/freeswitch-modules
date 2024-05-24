#ifndef __MOD_VERBIO_TTS_H__
#define __MOD_VERBIO_TTS_H__

#include <switch.h>
#include <speex/speex_resampler.h>

typedef struct verbio_data {
  char *voice_name;
  char *access_token;

  /* result data */
  long response_code;
  char *ct;
  char *name_lookup_time_ms;
  char *connect_time_ms;
  char *final_response_time_ms;
  char *err_msg;
  char *cache_filename;
  char *session_id;

  int rate;
  int draining;
  int reads;
  int cache_audio;

	void *conn;
  void *circularBuffer;
  switch_mutex_t *mutex;
  FILE *file;
  SpeexResamplerState *resampler;
} verbio_t;

#endif