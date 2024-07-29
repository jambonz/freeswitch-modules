#ifndef __MOD_DEEPGRAM_TTS_H__
#define __MOD_DEEPGRAM_TTS_H__

#include <switch.h>
#include <speex/speex_resampler.h>

typedef struct deepgram_data {
  char *voice_name;
  char *api_key;
  char *endpoint;

  /* result data */
  long response_code;
  char *ct;
  // Deepgram hedaers
  //dg-model-name
  char *reported_model_name;
  //dg-model-uuid
  char *reported_model_uuid;
  //dg-char-count
  char *reported_char_count;
  //dg-request-id
  char *request_id;
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
} deepgram_t;

#endif