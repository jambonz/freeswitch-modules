#ifndef __MOD_ELEVENLABS_TTS_H__
#define __MOD_ELEVENLABS_TTS_H__

#include <switch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <speex/speex_resampler.h>

struct elevenlabs_data {
  char *session_id;
	char *voice_name;
  char* api_key;
  char* model_id;
  char* similarity_boost;
  char* stability;
  char* style;
  char* use_speaker_boost;
  char* optimize_streaming_latency;

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

	int rate;

  void *conn;
	FILE *file;
  switch_mutex_t *mutex;
  void *circularBuffer;
  int draining;
  int reads;
  int cache_audio;
  int playback_start_sent;
  SpeexResamplerState *resampler;
};

typedef struct elevenlabs_data elevenlabs_t;

#endif