#include <switch.h>
#include <switch_json.h>
#include <string.h>
#include <string>
#include <mutex>
#include <thread>
#include <list>
#include <algorithm>
#include <functional>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <regex>
#include <iostream>
#include <unordered_map>

#include "mod_deepgram_transcribe.h"
#include "simple_buffer.h"
#include "parser.hpp"
#include "audio_pipe.hpp"

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320 /*which means each 20ms frame as 320 bytes at 8 khz (1 channel only)*/
#define DEEPGRAM_KEEP_ALIVE_INTERVAL_SECOND 8

namespace {
  static bool hasDefaultCredentials = false;
  static const char* defaultApiKey = nullptr;
  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = std::max(1, std::min(requestedBufferSecs ? ::atoi(requestedBufferSecs) : 2, 5));
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static unsigned int idxCallCount = 0;
  static uint32_t playCount = 0;

  /* deepgram model defaults by language */
  static const std::unordered_map<std::string, std::string> languageLookupTable = {
      {"zh", "base"},
      {"zh-CN", "base"},
      {"zh-TW", "base"},
      {"da", "nova-2"},
      {"da-DK", "nova-2"},
      {"en", "nova-2"},
      {"en-US", "nova-2"},
      {"en-AU", "nova-2"},
      {"en-GB", "nova-2"},
      {"en-IN", "nova-2"},
      {"en-NZ", "nova-2"},
      {"nl", "nova-2"},
      {"nl-BE", "nova-2"},
      {"fr", "nova-2"},
      {"fr-CA", "nova-2"},
      {"de", "nova-2"},
      {"el", "nova-2"},
      {"hi", "nova-2"},
      {"hi-Latn", "nova-2"},
      {"id", "nova-2"},
      {"it", "nova-2"},
      {"ja", "enhanced"},
      {"ko", "nova-2"},
      {"ko-KR", "nova-2"},
      {"no", "nova-2"},
      {"pl", "nova-2"},
      {"pt","nova-2"},
      {"pt-BR", "nova-2"},
      {"ru", "nova-2"},
      {"es","nova-2"},
      {"es-419","nova-2"},
      {"es-LATAM","enhanced"},
      {"sv", "nova-2"},
      {"sv-SE", "nova-2"},
      {"ta", "enhanced"},
      {"taq", "enhanced"},
      {"tr", "nova-2"},
      {"uk", "nova-2"}
  };

  static bool getLanguageInfo(const std::string& language, std::string& model) {
      auto it = languageLookupTable.find(language);
      if (it != languageLookupTable.end()) {
          model = it->second;
          return true;
      }
      return false;
  }

  static const char* emptyTranscript = 
    "\"is_final\":false,\"speech_final\":false,\"channel\":{\"alternatives\":[{\"transcript\":\"\",\"confidence\":0.0,\"words\":[]}]}";

  static void reaper(private_t *tech_pvt, bool silence_disconnect) {
    std::shared_ptr<deepgram::AudioPipe> pAp;
    pAp.reset((deepgram::AudioPipe *)tech_pvt->pAudioPipe);
    tech_pvt->pAudioPipe = nullptr;

    std::thread t([pAp, tech_pvt, silence_disconnect]{
      if (silence_disconnect) {
        pAp->finish_in_silence();
      } else {
        pAp->finish();
      }
      pAp->waitForClose();
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s (%u) got remote close\n", tech_pvt->sessionId, tech_pvt->id);
    });
    t.detach();
  }

  static void destroy_tech_pvt(private_t *tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    if (tech_pvt) {
      if (tech_pvt->pAudioPipe) {
        deepgram::AudioPipe* p = (deepgram::AudioPipe *) tech_pvt->pAudioPipe;
        delete p;
        tech_pvt->pAudioPipe = nullptr;
      }
      if (tech_pvt->resampler) {
          speex_resampler_destroy(tech_pvt->resampler);
          tech_pvt->resampler = NULL;
      }

      // NB: do not destroy the mutex here, that is caller responsibility

      /*
      if (tech_pvt->vad) {
        switch_vad_destroy(&tech_pvt->vad);
        tech_pvt->vad = nullptr;
      }
      */
    }
  }

  std::string encodeURIComponent(std::string decoded)
  {

      std::ostringstream oss;
      std::regex r("[!'\\(\\)*-.0-9A-Za-z_~:]");

      for (char &c : decoded)
      {
          if (std::regex_match((std::string){c}, r))
          {
              oss << c;
          }
          else
          {
              oss << "%" << std::uppercase << std::hex << (0xff & c);
          }
      }
      return oss.str();
  }

  std::string& constructPath(switch_core_session_t* session, std::string& path, 
    int sampleRate, int channels, const char* language, int interim) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    const char *var ;
    const char *model = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_MODEL");
    const char *customModel = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_CUSTOM_MODEL");
    std::ostringstream oss;

    oss << "/v1/listen?";

    /* make best choice by language if model not supplied*/
    if (!model && !customModel) {
      std::string defaultModel;
      if (getLanguageInfo(language, defaultModel)) {
        oss << "&model=" << defaultModel;
      }
      else {
        oss << "model=base"; // most widely supported, though not ideal
      }
    }
    else if (model) oss << "&model=" << model;
    else if (customModel) oss << "&model=" << customModel;

    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_MODEL_VERSION")) {
     oss <<  "&version=";
     oss <<  var;
    }
    oss <<  "&language=";
    oss <<  language;

    if (channels == 2) {
     oss <<  "&multichannel=true";
     oss <<  "&channels=2";
    }

    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ENABLE_SMART_FORMAT")) {
     oss <<  "&smart_format=true";
     oss <<  "&no_delay=true";
     /**
      * see: https://github.com/orgs/deepgram/discussions/384
      * 
      */
    }
    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ENABLE_FILLER_WORDS")) {
     oss <<  "&filler_words=true";
    }
    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ENABLE_AUTOMATIC_PUNCTUATION")) {
     oss <<  "&punctuate=true";
    }
    if (switch_true(switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_PROFANITY_FILTER"))) {
     oss <<  "&profanity_filter=true";
    }
    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_REDACT")) {
     oss <<  "&redact=";
     oss <<  var;
    }
    if (switch_true(switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_DIARIZE"))) {
     oss <<  "&diarize=true";
      if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_DIARIZE_VERSION")) {
       oss <<  "&diarize_version=";
       oss <<  var;
      }
    }
    if (switch_true(switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_NER"))) {
     oss <<  "&ner=true";
    }
    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ALTERNATIVES")) {
     oss <<  "&alternatives=";
     oss <<  var;
    }
    if (switch_true(switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_NUMERALS"))) {
     oss <<  "&numerals=true";
    }

		const char* hints = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_SEARCH");
		if (hints) {
			char *phrases[500] = { 0 };
      int argc = switch_separate_string((char *)hints, ',', phrases, 500);
      for (int i = 0; i < argc; i++) {
       oss <<  "&search=";
       oss <<  encodeURIComponent(phrases[i]);
      }
		}
		const char* keywords = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_KEYWORDS");
		if (keywords) {
			char *phrases[500] = { 0 };
      int argc = switch_separate_string((char *)keywords, ',', phrases, 500);
      for (int i = 0; i < argc; i++) {
       oss <<  "&keywords=";
       oss <<  encodeURIComponent(phrases[i]);
      }
		}
		const char* replace = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_REPLACE");
		if (replace) {
			char *phrases[500] = { 0 };
      int argc = switch_separate_string((char *)replace, ',', phrases, 500);
      for (int i = 0; i < argc; i++) {
       oss <<  "&replace=";
       oss <<  encodeURIComponent(phrases[i]);
      }
		}
    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_TAG")) {
     oss <<  "&tag=";
     oss <<  var;
    }
    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_ENDPOINTING")) {
      oss <<  "&endpointing=";
      oss <<  var;
    }
    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_UTTERANCE_END_MS")) {
      oss <<  "&utterance_end_ms=";
      oss <<  var;
      interim = 1; 
      //this requires interim_results=true (https://developers.deepgram.com/docs/understanding-end-of-speech-detection)
    }
    if (interim) {
     oss <<  "&interim_results=true";
    }
    if (var = switch_channel_get_variable(channel, "DEEPGRAM_SPEECH_VAD_TURNOFF")) {
      oss <<  "&vad_turnoff=";
      oss <<  var;
    }
   oss <<  "&encoding=linear16";
   oss <<  "&sample_rate=8000";
   path = oss.str();
   return path;
  }

  static void eventCallback(const char* sessionId, const char* bugname, 
    deepgram::AudioPipe::NotifyEvent_t event, const char* message, bool finished) {
    switch_core_session_t* session = switch_core_session_locate(sessionId);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (tech_pvt) {
          switch (event) {
            case deepgram::AudioPipe::CONNECT_SUCCESS:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "connection (%s) successful\n", tech_pvt->bugname);
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_CONNECT_SUCCESS, NULL, tech_pvt->bugname, finished);
            break;
            case deepgram::AudioPipe::CONNECT_FAIL:
            {
              // first thing: we can no longer access the AudioPipe
              std::stringstream json;
              json << "{\"reason\":\"" << message << "\"}";
              tech_pvt->pAudioPipe = nullptr;
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_CONNECT_FAIL, (char *) json.str().c_str(), tech_pvt->bugname, finished);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection (%s) failed: %s\n", message, tech_pvt->bugname);
            }
            break;
            case deepgram::AudioPipe::CONNECTION_DROPPED:
              // first thing: we can no longer access the AudioPipe

              /**
               * this is a bit tricky.  If we just closed a previos connection it may be returning final transcripts
               * and then a close event here as it is shutting down (in the reaper thread above).
               * In this scenario, the fact that the connection is dropped is not significant.
               */
              if (finished) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "old connection (%s) gracefully closed by Deepgram\n", tech_pvt->bugname);
              }
              else {
                tech_pvt->pAudioPipe = nullptr;
                tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_DISCONNECT, NULL, tech_pvt->bugname, finished);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection (%s) dropped from far end\n", tech_pvt->bugname);
              }
            break;
            case deepgram::AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
              // first thing: we can no longer access the AudioPipe
              tech_pvt->pAudioPipe = nullptr;
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection (%s) closed gracefully\n", tech_pvt->bugname);
            break;
            case deepgram::AudioPipe::MESSAGE:
              if( strstr(message, emptyTranscript)) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "discarding empty deepgram transcript\n");
              }
              else {
                tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_RESULTS, message, tech_pvt->bugname, finished);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "deepgram message (%s): %s\n", tech_pvt->bugname, message);
              }
            break;

            default:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "got unexpected msg from deepgram %d:%s\n", event, message);
              break;
          }
        }
      }
      switch_core_session_rwunlock(session);
    }
  }
  switch_status_t fork_data_init(private_t *tech_pvt, switch_core_session_t *session, 
    int sampling, int desiredSampling, int channels, char *lang, int interim, 
    char* bugname, responseHandler_t responseHandler) {

    int err;
    int useTls = true;
    std::string host;
    int port = 443;
    size_t buflen = LWS_PRE + (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);

    std::ostringstream configuration_stream;
    switch_codec_implementation_t read_impl;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_core_session_get_read_impl(session, &read_impl);
  
    std::string path;
    constructPath(session, path, desiredSampling, channels, lang, interim);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "path: %s\n", path.c_str());

    const char* endpoint = switch_channel_get_variable(channel, "DEEPGRAM_URI");
    if (endpoint != nullptr) {
      std::string ep(endpoint);
      useTls = switch_true(switch_channel_get_variable(channel, "DEEPGRAM_USE_TLS"));

      size_t pos = ep.find(':');
      host = ep;
      if (pos != std::string::npos) {
        host = ep.substr(0, pos);
        std::string strPort = ep.substr(pos + 1);
        port = ::atoi(strPort.c_str());
      }
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, 
        "connecting to deepgram on-prem %s port %d, using tls? (%s)\n", host.c_str(), port, useTls ? "yes" : "no");
    } else {
      host = "api.deepgram.com";
    }

    const char* apiKey = switch_channel_get_variable(channel, "DEEPGRAM_API_KEY");
    if (!apiKey && defaultApiKey) {
      apiKey = defaultApiKey;
    } else if (!apiKey && endpoint == nullptr) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "no deepgram api key provided\n");
      return SWITCH_STATUS_FALSE;
    }

    configuration_stream <<
      host << ":" <<
      port << ";" <<
      path << ";" <<
      buflen << ";" <<
      read_impl.decoded_bytes_per_packet << ";" <<
      apiKey << ";" <<
      useTls;

    if (tech_pvt->pAudioPipe) {
      // stop sending keep alive
      tech_pvt->is_keep_alive = 0;
      if (0 != strcmp(tech_pvt->configuration, configuration_stream.str().c_str())) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "fork_data_init: stop existing deepgram connection, old configuration %s, new configuration %s\n",
          tech_pvt->configuration, configuration_stream.str().c_str());
        reaper(tech_pvt, true);
      } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "fork_data_init: enable existing deepgram connection\n");
        return SWITCH_STATUS_SUCCESS;
      }
    } else {
      memset(tech_pvt, 0, sizeof(private_t));
    }

    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
    strncpy(tech_pvt->host, host.c_str(), MAX_WS_URL_LEN);
    tech_pvt->port = port;
    strncpy(tech_pvt->path, path.c_str(), MAX_PATH_LEN);   
    strncpy(tech_pvt->configuration, configuration_stream.str().c_str(), MAX_PATH_LEN) ;
    tech_pvt->sampling = desiredSampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    tech_pvt->buffer_overrun_notified = 0;
    tech_pvt->is_keep_alive = 0;
    strncpy(tech_pvt->bugname, bugname, MAX_BUG_LEN);

    deepgram::AudioPipe* ap = new deepgram::AudioPipe(tech_pvt->sessionId, bugname, tech_pvt->host, tech_pvt->port, tech_pvt->path, 
      buflen, read_impl.decoded_bytes_per_packet, apiKey, useTls, eventCallback);
    if (!ap) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error allocating AudioPipe\n");
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->pAudioPipe = static_cast<void *>(ap);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connecting now\n");
    ap->connect();
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection in progress\n");

    if (!tech_pvt->mutex) {
      switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
    }
    
    if (!tech_pvt->resampler) {
      if (desiredSampling != sampling) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) resampling from %u to %u\n", tech_pvt->id, sampling, desiredSampling);
        tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
        if (0 != err) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
          return SWITCH_STATUS_FALSE;
        }
      }
      else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) no resampling needed for this call\n", tech_pvt->id);
      }
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_data_init\n", tech_pvt->id);

    return SWITCH_STATUS_SUCCESS;
  }

  void lws_logger(int level, const char *line) {
    switch_log_level_t llevel = SWITCH_LOG_DEBUG;

    switch (level) {
      case LLL_ERR: llevel = SWITCH_LOG_ERROR; break;
      case LLL_WARN: llevel = SWITCH_LOG_WARNING; break;
      case LLL_NOTICE: llevel = SWITCH_LOG_NOTICE; break;
      case LLL_INFO: llevel = SWITCH_LOG_INFO; break;
      break;
    }
	  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "%s\n", line);
  }
}


extern "C" {
  switch_status_t dg_transcribe_init() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_deepgram_transcribe: audio buffer (in secs):    %d secs\n", nAudioBufferSecs);
 
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE;
    // | LLL_INFO | LLL_PARSER | LLL_HEADER | LLL_EXT | LLL_CLIENT  | LLL_LATENCY | LLL_DEBUG ;
    
    deepgram::AudioPipe::initialize(logs, lws_logger);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::initialize completed\n");

		const char* apiKey = std::getenv("DEEPGRAM_API_KEY");
		if (NULL == apiKey) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, 
				"\"DEEPGRAM_API_KEY\" env var not set; authentication will expect channel variables of same names to be set\n");
		}
		else {
			hasDefaultCredentials = true;
      defaultApiKey = apiKey;
		}
		return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t dg_transcribe_cleanup() {
    bool cleanup = false;
    cleanup = deepgram::AudioPipe::deinitialize();
    if (cleanup == true) {
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }
	
  switch_status_t dg_transcribe_session_init(switch_core_session_t *session, 
    responseHandler_t responseHandler, uint32_t samples_per_second, uint32_t channels, 
    char* lang, int interim, char* bugname, void **ppUserData)
  {
    int err;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    private_t* tech_pvt;
    if (bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "reuse existing kep alive deepgram connection\n");
      tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    } else {
      tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
      tech_pvt->pAudioPipe = NULL;
      tech_pvt->is_keep_alive = 0;
      tech_pvt->mutex = NULL;
      tech_pvt->resampler = NULL;
    }

    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
      return SWITCH_STATUS_FALSE;
    }

    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, samples_per_second, 8000, channels, lang, interim, bugname, responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;

    return SWITCH_STATUS_SUCCESS;
  }

	switch_status_t dg_transcribe_session_stop(switch_core_session_t *session,int channelIsClosing, char* bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    const bool use_single_connection = switch_true(std::getenv("DEEPGRAM_SPEECH_USE_SINGLE_CONNECTION"));
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "dg_transcribe_session_stop: no bug - websocket conection already closed\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    uint32_t id = tech_pvt->id;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) dg_transcribe_session_stop\n", id);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    if (use_single_connection && !channelIsClosing) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "dg_transcribe_session_stop: call is running, use_single_connection is true, keep alive is activated\n", id);
      tech_pvt->is_keep_alive = 1;
      tech_pvt->frame_count = 0;
      return SWITCH_STATUS_SUCCESS;
    }
      
    // close connection and get final responses
    switch_mutex_lock(tech_pvt->mutex);
    switch_channel_set_private(channel, bugname, NULL);
    if (!channelIsClosing) switch_core_media_bug_remove(session, &bug);

    deepgram::AudioPipe *pAudioPipe = static_cast<deepgram::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe) reaper(tech_pvt, false);
    destroy_tech_pvt(tech_pvt);
    switch_mutex_unlock(tech_pvt->mutex);
    switch_mutex_destroy(tech_pvt->mutex);
    tech_pvt->mutex = nullptr;
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) dg_transcribe_session_stop\n", id);
    return SWITCH_STATUS_SUCCESS;
  }
	
	switch_bool_t dg_transcribe_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    size_t inuse = 0;
    bool dirty = false;
    char *p = (char *) "{\"msg\": \"buffer overrun\"}";
    char *keep_alive = (char *) "{\"type\": \"KeepAlive\"}";

    if (!tech_pvt) return SWITCH_TRUE;
    

    // Keep sending keep alive if there is no transcribe activity
    if (tech_pvt->is_keep_alive && tech_pvt->pAudioPipe) {
      deepgram::AudioPipe *pAudioPipe = static_cast<deepgram::AudioPipe *>(tech_pvt->pAudioPipe);
      if (++tech_pvt->frame_count * 20 /*ms*/ / 1000 >= DEEPGRAM_KEEP_ALIVE_INTERVAL_SECOND) {
        tech_pvt->frame_count = 0;
        pAudioPipe->bufferForSending(keep_alive);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "dg_transcribe_frame: sending %s to deepgram\n", keep_alive);
      }
      // remove media bug buffered data
      while (true) {
        unsigned char data[SWITCH_RECOMMENDED_BUFFER_SIZE] = {0};
        switch_frame_t frame = { 0 };
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
        switch_status_t rv = switch_core_media_bug_read(bug, &frame, SWITCH_TRUE);
        if (rv != SWITCH_STATUS_SUCCESS) break;
      }
      return SWITCH_TRUE;
    }
    
    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      if (!tech_pvt->pAudioPipe) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }
      deepgram::AudioPipe *pAudioPipe = static_cast<deepgram::AudioPipe *>(tech_pvt->pAudioPipe);
      if (pAudioPipe->getLwsState() != deepgram::AudioPipe::LWS_CLIENT_CONNECTED) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }
      pAudioPipe->lockAudioBuffer();
      size_t available = pAudioPipe->binarySpaceAvailable();
      if (NULL == tech_pvt->resampler) {
        switch_frame_t frame = { 0 };
        frame.data = pAudioPipe->binaryWritePtr();
        frame.buflen = available;
        while (true) {

          // check if buffer would be overwritten; dump packets if so
          if (available < pAudioPipe->binaryMinSpace()) {
            if (!tech_pvt->buffer_overrun_notified) {
              tech_pvt->buffer_overrun_notified = 1;
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_BUFFER_OVERRUN, NULL, tech_pvt->bugname, 0);
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets!\n", 
              tech_pvt->id);
            pAudioPipe->binaryWritePtrResetToZero();

            frame.data = pAudioPipe->binaryWritePtr();
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
          }

          switch_status_t rv = switch_core_media_bug_read(bug, &frame, SWITCH_TRUE);
          if (rv != SWITCH_STATUS_SUCCESS) break;
          if (frame.datalen) {
            pAudioPipe->binaryWritePtrAdd(frame.datalen);
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
            frame.data = pAudioPipe->binaryWritePtr();
            dirty = true;
          }
        }
      }
      else {
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = { 0 };
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen) {
            spx_uint32_t out_len = available >> 1;  // space for samples which are 2 bytes
            spx_uint32_t in_len = frame.samples;
            speex_resampler_process_interleaved_int(tech_pvt->resampler, 
              (const spx_int16_t *) frame.data, 
              (spx_uint32_t *) &in_len, 
              (spx_int16_t *) ((char *) pAudioPipe->binaryWritePtr()),
              &out_len);

            if (out_len > 0) {
              // bytes written = num samples * 2 * num channels
              size_t bytes_written = out_len << tech_pvt->channels;
              pAudioPipe->binaryWritePtrAdd(bytes_written);
              available = pAudioPipe->binarySpaceAvailable();
              dirty = true;
            }
            if (available < pAudioPipe->binaryMinSpace()) {
              if (!tech_pvt->buffer_overrun_notified) {
                tech_pvt->buffer_overrun_notified = 1;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets!\n", 
                  tech_pvt->id);
                tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_BUFFER_OVERRUN, NULL, tech_pvt->bugname, 0);
              }
              break;
            }
          }
        }
      }

      pAudioPipe->unlockAudioBuffer();
      switch_mutex_unlock(tech_pvt->mutex);
    }
    return SWITCH_TRUE;
  }
}
