#ifndef __MOD_DIALOGFLOW_CX_H__
#define __MOD_DIALOGFLOW_CX_H__

#include <switch.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#define MY_BUG_NAME "__dialogflow_cx_bug__"
#define DIALOGFLOW_CX_EVENT_INTENT "dialogflow_cx::intent"
#define DIALOGFLOW_CX_EVENT_TRANSCRIPTION "dialogflow_cx::transcription"
#define DIALOGFLOW_CX_EVENT_AUDIO_PROVIDED "dialogflow_cx::audio_provided"
#define DIALOGFLOW_CX_EVENT_END_OF_UTTERANCE "dialogflow_cx::end_of_utterance"
#define DIALOGFLOW_CX_EVENT_ERROR "dialogflow_cx::error"

#define MAX_LANG (12)
#define MAX_PROJECT_ID (128)
#define MAX_PATHLEN (256)

/* per-channel data */
typedef void (*responseHandler_t)(switch_core_session_t* session, const char * type, char* json);
typedef void (*errorHandler_t)(switch_core_session_t* session, const char * reason);

struct cap_cb {
	switch_mutex_t *mutex;
	char sessionId[256];
  SpeexResamplerState *resampler;
	void* streamer;
	responseHandler_t responseHandler;
	errorHandler_t errorHandler;
	switch_thread_t* thread;
	char lang[MAX_LANG];
	char projectId[MAX_PROJECT_ID];

};

#endif