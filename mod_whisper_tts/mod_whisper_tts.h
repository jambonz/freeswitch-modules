#ifndef __MOD_WHISPER_TTS_H__
#define __MOD_WHISPER_TTS_H__

#include <switch.h>
typedef struct whisper_data {
  char *voice_name;
  char *api_key;
  char *model_id;
  char *speed;

  /* result data */
  long response_code;
  char *ct;
  //whisper headers
  //openai-organization
  char *reported_organization;
  //openai-processing-ms
  char *reported_latency;
  //x-ratelimit-limit-requests
  char *reported_ratelimit_requests;
  //x-ratelimit-remaining-requests
  char *reported_ratelimit_remaining_requests;
  //x-ratelimit-reset-requests
  char *reported_ratelimit_reset_requests;
  //x-request-id
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
} whisper_t;
#endif