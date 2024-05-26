#ifndef __MOD_RIMELABS_TTS_H__
#define __MOD_RIMELABS_TTS_H__

#include <switch.h>
#include <speex/speex_resampler.h>

typedef struct rimelabs_data {
  char *voice_name;
  char *api_key;
  char *model_id;
  char *speed_alpha;
  char *reduce_latency;

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
  int playback_start_sent;

	void *conn;
  void *circularBuffer;
  switch_mutex_t *mutex;
  FILE *file;
  SpeexResamplerState *resampler;
} rimelabs_t;

#endif