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
  char *reported_latency;
  char *request_id;
  char *history_item_id;
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
} whisper_t;
#endif