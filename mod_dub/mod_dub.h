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


struct cap_cb {
	switch_mutex_t *mutex;
  int gain;
  void *tracks[MAX_DUB_TRACKS];
};


#endif