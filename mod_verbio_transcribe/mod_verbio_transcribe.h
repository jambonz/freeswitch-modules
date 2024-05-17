#ifndef __MOD_VERBIO_TRANSCRIBE_H__
#define __MOD_VERBIO_TRANSCRIBE_H__

#include <switch.h>
#include <speex/speex_resampler.h>

#include <unistd.h>

#define MY_BUG_NAME "verbio_transcribe"
#define MAX_ENGINE_VERSION_LEN (2)
#define MAX_BUG_LEN (64)
#define MAX_SESSION_ID (256)
#define LONG_TEXT_LEN (1024)
#define MAX_LANGUAGE_LEN (6)
#define TRANSCRIBE_EVENT_RESULTS                     "verbio_transcribe::transcription"
#define TRANSCRIBE_EVENT_ERROR                       "jambonz_transcribe::error"

/* per-channel data */
typedef void (*responseHandler_t)(switch_core_session_t* session, const char* event, const char * json, const char* bugname, int finished);

struct cap_cb {
  switch_mutex_t *mutex;
  char sessionId[MAX_SESSION_ID+1];
  char bugname[MAX_BUG_LEN+1];
  char access_token[LONG_TEXT_LEN + 1];
  char language[MAX_LANGUAGE_LEN + 1];
  char inline_grammar[LONG_TEXT_LEN + 1];
  char grammar_uri[LONG_TEXT_LEN + 1];
  char label[MAX_SESSION_ID+1];
  uint32_t engine_version;
  uint32_t topic;
  uint32_t enable_formatting; 
  uint32_t enable_diarization;
  uint32_t channels;
  uint32_t interim;
  uint32_t recognition_timeout;
  uint32_t speech_complete_timeout;
  uint32_t speech_incomplete_timeout;
  uint32_t finished;
  

  SpeexResamplerState *resampler;
  void* streamer;
  responseHandler_t responseHandler;
  switch_thread_t* thread;
};

#endif