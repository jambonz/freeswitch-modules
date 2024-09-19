#ifndef __MOD_PLAYHT_TTS_H__
#define __MOD_PLAYHT_TTS_H__

#include <switch.h>
typedef struct playht_data {
  /* authentication */
  char *user_id;
  char *api_key;

  /* required */
  char *voice_name;

  /* optional (else will choose defaults in backend) */
  char *voice_engine;
  char *speed;
  char *seed;
  char *temperature;
  char *top_p;
  char *voice_guidance;
  char *style_guidance;
  char *text_guidance;
  char *repetition_penalty;
  char *language; // Only applies to Play3.0 voice engine

  /* DEPRECATED */
  char *quality; // use sample rate to adjust quality
  char *emotion;

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
  int playback_start_sent;

	void *conn;
  void *circularBuffer;
  switch_mutex_t *mutex;
  FILE *file;

  /* Play3.0 specific */
  char *url;
  int url_expires_at_ms;
} playht_t;
#endif