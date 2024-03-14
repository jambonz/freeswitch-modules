#ifndef __MOD_DUB_H__
#define __MOD_DUB_H__

#include <switch.h>
#include <speex/speex_resampler.h>

#include <mpg123.h>

#include <unistd.h>

#define MY_BUG_NAME "_dub_"
#define MAX_SESSION_ID (256)
#define MAX_BUG_LEN (64)
#define MAX_URL_LEN (1024)
#define MAX_DUB_TRACKS (2)

/* per-channel data */
typedef void (*responseHandler_t)(switch_core_session_t* session, const char* json, const char* bugname, const char* details);

typedef enum {
  DUB_TRACK_STATE_INACTIVE = 0,
  DUB_TRACK_STATE_READY,
  DUB_TRACK_STATE_ACTIVE,
  DUB_TRACK_STATE_PAUSED
} dub_state_t;

typedef enum {
  DUB_GENERATOR_TYPE_UNKNOWN = 0,
  DUB_GENERATOR_TYPE_HTTP,
  DUB_GENERATOR_TYPE_FILE,
  DUB_GENERATOR_TYPE_TTS
} dub_generator_t;

typedef enum {
  DUB_TRACK_EVENT_PLAY = 0,
  DUB_TRACK_EVENT_STOP,
  DUB_TRACK_EVENT_PAUSE,
  DUB_TRACK_EVENT_RESUME
} dub_event_t;


typedef struct dub_track {
  dub_state_t state;
  dub_generator_t generator;
  void* cmdQueue;
  char* trackName;
  int sampleRate;
  int gain;
  void* circularBuffer;
  int generatorId;
} dub_track_t;

struct cap_cb {
	switch_mutex_t *mutex;
  int gain;
  dub_track_t tracks[MAX_DUB_TRACKS];
};


#endif