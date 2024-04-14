#ifndef __MOD_PLAYHT_TTS_H__
#define __MOD_PLAYHT_TTS_H__

#include <switch.h>
typedef struct playht_data {
  char *voice_name;
  char *api_key;
  char *user_id;
  char *quality;
  char *speed;
  char *seed;
  char *temperature;
  char *voice_engine;
  char *emotion;
  char *voice_guidance;
  char *style_guidance;
  char *text_guidance;

  /* result data */
  long response_code;
  char *ct;
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

	void *conn;
  void *circularBuffer;
  switch_mutex_t *mutex;
  FILE *file;
} playht_t;
#endif