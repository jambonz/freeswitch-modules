#include <cstdlib>
#include <algorithm>
#include <future>

#include <switch.h>
#include <switch_json.h>
#include <grpc++/grpc++.h>
#include <google/protobuf/util/json_util.h>

#include "speechcenter/recognizer/v1/recognition.grpc.pb.h"

namespace verbio_asr = speechcenter::recognizer::v1;

#include "mod_verbio_transcribe.h"
#include "simple_buffer.h"

#define CHUNKSIZE (320)

namespace {
  int case_insensitive_match(std::string s1, std::string s2) {
   std::transform(s1.begin(), s1.end(), s1.begin(), ::tolower);
   std::transform(s2.begin(), s2.end(), s2.begin(), ::tolower);
   if(s1.compare(s2) == 0)
      return 1; //The strings are same
   return 0; //not matched
  }
}

class GStreamer {
public:
  GStreamer(cap_cb *cb) : 
    m_writesDone(false), 
    m_connected(false), 
    m_interim(cb->interim),
    m_audioBuffer(CHUNKSIZE, 15) {

    strncpy(m_sessionId, cb->sessionId, 256);
    auto channelCreds = grpc::SslCredentials(grpc::SslCredentialsOptions());
    m_channel = grpc::CreateChannel(
                    "us.speechcenter.verbio.com",
                    grpc::CompositeChannelCredentials(
                    grpc::SslCredentials(grpc::SslCredentialsOptions()),
                    grpc::AccessTokenCredentials(cb->access_token)));

    if (!m_channel) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GStreamer %p failed creating grpc channel\n", this);  
      throw std::runtime_error(std::string("Error creating grpc channel"));
    }

    m_stub = std::move(verbio_asr::Recognizer::NewStub(m_channel));

    auto* config = m_request.mutable_config();
    // RecognitionParameters
    auto* params = config->mutable_parameters();
    params->set_language(cb->language);
    auto* pcm = params->mutable_pcm();
    pcm->set_sample_rate_hz(8000);
    params->set_audio_channels_number(cb->channels);
    params->set_enable_formatting(cb->enable_formatting);
    auto* resource = config->mutable_resource();
    resource->set_topic(static_cast<verbio_asr::RecognitionResource_Topic>(cb->topic));
    if (!zstr(cb->inline_grammar) || !zstr(cb->grammar_uri)) {
      auto* grammar = resource->mutable_grammar();
      if (cb->inline_grammar) {
        grammar->set_inline_grammar(cb->inline_grammar);
      } else if (cb->grammar_uri) {
        grammar->set_grammar_uri(cb->grammar_uri);
      }
    }

    config->set_version(static_cast<verbio_asr::RecognitionConfig_AsrVersion>(cb->engine_version));
    if (cb->label) {
      config->add_label(cb->label);
    }
    if (cb->recognition_timeout || cb->speech_complete_timeout || cb->speech_incomplete_timeout) {
      auto* timer = config->mutable_configuration();
      timer->set_start_input_timers(true);
      if (cb->recognition_timeout) {
        timer->set_recognition_timeout(cb->recognition_timeout);
      }
      if (cb->speech_complete_timeout) {
        timer->set_speech_complete_timeout(cb->speech_complete_timeout);
      }
      if (cb->speech_incomplete_timeout) {
        timer->set_speech_incomplete_timeout(cb->speech_incomplete_timeout);
      }
    }
  }

  ~GStreamer() {
  }

  void connect() {
    assert(!m_connected);
    // Begin a stream.

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer %p creating streamer\n", this);  
    m_streamer = m_stub->StreamingRecognize(&m_context);
    m_connected = true;

    // read thread is waiting on this
    m_promise.set_value();

    // Write the first request, containing the config only.
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer %p sending initial message\n", this);  
    bool ok = m_streamer->Write(m_request);
    m_request.clear_config();

    // send any buffered audio
    int nFrames = m_audioBuffer.getNumItems();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer %p got stream ready, %d buffered frames\n", this, nFrames);  
    if (nFrames) {
      char *p;
      do {
        p = m_audioBuffer.getNextChunk();
        if (p) {
          write(p, CHUNKSIZE);
        }
      } while (p);
    }
  }

  bool write(void* data, uint32_t datalen) {
    if (!m_connected) {
      if (datalen % CHUNKSIZE == 0) {
        m_audioBuffer.add(data, datalen);
      }
      return true;
    }
    m_request.clear_audio();
    m_request.set_audio(data, datalen); 
    bool ok = m_streamer->Write(m_request);
    return ok;
  }

  uint32_t nextMessageSize(void) {
    uint32_t size = 0;
    m_streamer->NextMessageSize(&size);
    return size;
  }

  bool read(verbio_asr::RecognitionStreamingResponse* response) {
    return m_streamer->Read(response);
  }

  grpc::Status finish() {
    return m_streamer->Finish();
  }

  void writesDone() {
    // grpc crashes if we call this twice on a stream
    if (!m_connected) {
      cancelConnect();
    }
    else if (!m_writesDone) {
      m_streamer->WritesDone();
      m_writesDone = true;
    }
  }

  bool waitForConnect() {
    std::shared_future<void> sf(m_promise.get_future());
    sf.wait();
    return m_connected;
  }

  void cancelConnect() {
    assert(!m_connected);
    m_promise.set_value();
  } 

  bool isConnected() {
    return m_connected;
  }

private:
  grpc::ClientContext m_context;
  std::shared_ptr<grpc::Channel> m_channel;
  std::unique_ptr<verbio_asr::Recognizer::Stub> m_stub;
  verbio_asr::RecognitionStreamingRequest m_request;
  std::unique_ptr< grpc::ClientReaderWriterInterface<verbio_asr::RecognitionStreamingRequest, verbio_asr::RecognitionStreamingResponse> > m_streamer;
  bool m_writesDone;
  bool m_connected;
  bool m_interim;
  std::string m_language;
  std::promise<void> m_promise;
  SimpleBuffer m_audioBuffer;
  char m_sessionId[256];
};

static void *SWITCH_THREAD_FUNC grpc_read_thread(switch_thread_t *thread, void *obj) {
  struct cap_cb *cb = (struct cap_cb *) obj;
  GStreamer* streamer = (GStreamer *) cb->streamer;

  bool connected = streamer->waitForConnect();
  if (!connected) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "verbio transcribe grpc read thread exiting since we didnt connect\n") ;
    return nullptr;
  }

  // Read responses.
  verbio_asr::RecognitionStreamingResponse response;
  while (streamer->read(&response)) {  // Returns false when no more to read.
    if (response.has_error()) {
      // handle error
      const auto& error = response.error();
      auto reason = error.reason();
      cJSON* json = cJSON_CreateObject();
      cJSON_AddStringToObject(json, "type", "error");
      cJSON_AddStringToObject(json, "error", reason.c_str());
      char* json_string = cJSON_PrintUnformatted(json);

      switch_core_session_t* session = switch_core_session_locate(cb->sessionId);
      if (!session) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "grpc_read_thread: session %s is gone!\n", cb->sessionId) ;
        return nullptr;
      }
      cb->responseHandler(session, TRANSCRIBE_EVENT_ERROR, json_string, cb->bugname, cb->finished);
      switch_core_session_rwunlock(session);
      // clean
      free(json_string);
      cJSON_Delete(json);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer recognition error %s\n", reason.c_str());
      break;
    } else if (!response.has_result()) {
      // there is no available results yet.
      continue;
    } else {
      const auto& result = response.result();
      if (response.result().alternatives_size() > 0) {
        const auto& alternative = response.result().alternatives(0);
        if (alternative.words_size() == 0) {
            continue;
        }
      }
      std::string json_string;
      google::protobuf::util::JsonPrintOptions options;
      options.always_print_primitive_fields = true;
      options.preserve_proto_field_names = true;
      absl::Status status = google::protobuf::util::MessageToJsonString(result, &json_string, options);

      if (!status.ok()) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot parse verbio result, error: %s", status.ToString()) ;
        
      } else {
        switch_core_session_t* session = switch_core_session_locate(cb->sessionId);
        if (!session) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "grpc_read_thread: session %s is gone!\n", cb->sessionId) ;
          return nullptr;
        }
        cb->responseHandler(session, TRANSCRIBE_EVENT_RESULTS, json_string.c_str(), cb->bugname, cb->finished);
        switch_core_session_rwunlock(session);
      }
    }
  }
  return nullptr;
}

extern "C" {

  switch_status_t verbio_speech_init() {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_cleanup() {
    return SWITCH_STATUS_SUCCESS;
  }
  switch_status_t verbio_speech_session_init(switch_core_session_t *session, responseHandler_t responseHandler, 
    uint32_t channels, char* lang, int interim, char* bugname, void **ppUserData) {

    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_memory_pool_t *pool = switch_core_session_get_pool(session);
    auto read_codec = switch_core_session_get_read_codec(session);
    uint32_t sampleRate = read_codec->implementation->actual_samples_per_second;
    struct cap_cb *cb;
    int err;

    cb =(struct cap_cb *) switch_core_session_alloc(session, sizeof(*cb));
    strncpy(cb->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
    strncpy(cb->bugname, bugname, MAX_BUG_LEN);
    cb->channels = channels;
    cb->interim = interim;
    cb->finished = 0;

    // Read Verbio configuration from channel variables
    const char* var;
    if (var = switch_channel_get_variable(channel, "VERBIO_ACCESS_TOKEN")) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Using channel vars for verbio authentication\n");
      strncpy(cb->access_token, var, LONG_TEXT_LEN);
    }
    else if (std::getenv("VERBIO_ACCESS_TOKEN")) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Using env vars for verbio authentication\n");
      strncpy(cb->access_token, std::getenv("VERBIO_ACCESS_TOKEN"), LONG_TEXT_LEN);
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "No channel vars or env vars for verbio authentication. Stop initiating Verbio connection\n");
      return SWITCH_STATUS_FALSE;
    }
    cb->enable_formatting = switch_true(switch_channel_get_variable(channel, "VERBIO_ENABLE_FORMATTING"));
    cb->enable_diarization = switch_true(switch_channel_get_variable(channel, "VERBIO_ENABLE_DIARIZATION"));
    strncpy(cb->language, lang, MAX_LANGUAGE_LEN);
    if (var = switch_channel_get_variable(channel, "VERBIO_ENGINE_VERSION")) {
      cb->engine_version = atoi(var);
    }
    if (var = switch_channel_get_variable(channel, "VERBIO_TOPIC")) {
      cb->topic = atoi(var);
    } else {
      cb->topic = 0;
    }
    if (var = switch_channel_get_variable(channel, "VERBIO_INLINE_GRAMMAR")) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "xhoaluu1 %s\n", var);
      strncpy(cb->inline_grammar, var, LONG_TEXT_LEN);
    }
    if (var = switch_channel_get_variable(channel, "VERBIO_GRAMMAR_URI")) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "xhoaluu2 %s\n", var);
      strncpy(cb->grammar_uri, var, LONG_TEXT_LEN);
    }
    if (var = switch_channel_get_variable(channel, "VERBIO_LABEL")) {
      strncpy(cb->label, var, MAX_SESSION_ID);
    }
    if (var = switch_channel_get_variable(channel, "VERBIO_RECOGNITION_TIMEOUT")) {
      cb->recognition_timeout = atoi(var);
    }
    if (var = switch_channel_get_variable(channel, "VERBIO_SPEECH_COMPLETE_TIMEOUT")) {
      cb->speech_complete_timeout = atoi(var);
    }
    if (var = switch_channel_get_variable(channel, "VERBIO_SPEECH_INCOMPLETE_TIMEOUT")) {
      cb->speech_incomplete_timeout = atoi(var);
    }

    if (switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mutex\n");
      return SWITCH_STATUS_FALSE;
    }

    if (sampleRate != 8000) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "verbio_speech_session_init:  initializing resampler\n");
        cb->resampler = speex_resampler_init(channels, sampleRate, 8000, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing resampler: %s.\n",
          switch_channel_get_name(channel), speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    } else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s: no resampling needed for this call\n", switch_channel_get_name(channel));
    }
    cb->responseHandler = responseHandler;

    GStreamer *streamer = NULL;
    try {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "verbio_speech_session_init:  allocating streamer\n");
      streamer = new GStreamer(cb);
      cb->streamer = streamer;
    } catch (std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing gstreamer: %s.\n", 
        switch_channel_get_name(channel), e.what());
      return SWITCH_STATUS_FALSE;
    }

    streamer->connect();

    // create the read thread
    switch_threadattr_t *thd_attr = NULL;
    switch_threadattr_create(&thd_attr, pool);
    switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
    switch_thread_create(&cb->thread, thd_attr, grpc_read_thread, cb, pool);

    *ppUserData = cb;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_session_cleanup(switch_core_session_t *session, int channelIsClosing, char* bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (bug) {
      struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
      switch_mutex_lock(cb->mutex);

      switch_channel_set_private(channel, cb->bugname, NULL);

      // close connection and get final responses
      GStreamer* streamer = (GStreamer *) cb->streamer;

      if (streamer) {
        streamer->writesDone();
        cb->finished = 1;

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "verbio_speech_session_cleanup: GStreamer (%p) waiting for read thread to complete\n", (void*)streamer);
        switch_status_t st;
        switch_thread_join(&st, cb->thread);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "verbio_speech_session_cleanup:  GStreamer (%p) read thread completed\n", (void*)streamer);

        delete streamer;
        cb->streamer = NULL;
      }

      if (cb->resampler) {
        speex_resampler_destroy(cb->resampler);
      }

      if (!channelIsClosing) {
        switch_core_media_bug_remove(session, &bug);
      }

      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "verbio_speech_session_cleanup: Closed stream\n");

      switch_mutex_unlock(cb->mutex);
      switch_mutex_destroy(cb->mutex);
      cb->mutex = nullptr;

      return SWITCH_STATUS_SUCCESS;
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached.\n", switch_channel_get_name(channel));
    return SWITCH_STATUS_FALSE;
  }

  switch_bool_t verbio_speech_frame(switch_media_bug_t *bug, void* user_data) {
      switch_core_session_t *session = switch_core_media_bug_get_session(bug);
      struct cap_cb *cb = (struct cap_cb *) user_data;
      if (cb->streamer) {
        GStreamer* streamer = (GStreamer *) cb->streamer;
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = {};
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
          while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
            if (frame.datalen) {

              if (cb->resampler) {
                spx_int16_t out[SWITCH_RECOMMENDED_BUFFER_SIZE];
                spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
                spx_uint32_t in_len = frame.samples;
                size_t written;

                speex_resampler_process_interleaved_int(cb->resampler,
                  (const spx_int16_t *) frame.data,
                  (spx_uint32_t *) &in_len,
                  &out[0],
                  &out_len);
                streamer->write( &out[0], sizeof(spx_int16_t) * out_len);
              }
              else {
                streamer->write( frame.data, sizeof(spx_int16_t) * frame.samples);
              }
            }
          }
          switch_mutex_unlock(cb->mutex);
        }
      }
      return SWITCH_TRUE;
    }
}
