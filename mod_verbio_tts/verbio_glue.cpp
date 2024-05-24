#include "mod_verbio_tts.h"
#include <switch.h>

#include <boost/circular_buffer.hpp>
#include <grpc++/grpc++.h>
#include "tts_grpc_gateway/v1/verbio-speech-center-synthesizer.grpc.pb.h"
#include "tts_grpc_gateway/v1/verbio-speech-center-synthesizer.pb.h"
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>

#define BUFFER_SIZE 8129

using tts_grpc_gateway::v1::TextToSpeech;
using tts_grpc_gateway::v1::StreamingSynthesisRequest;
using tts_grpc_gateway::v1::StreamingSynthesisResponse;
using tts_grpc_gateway::v1::VoiceSamplingRate;
using tts_grpc_gateway::v1::AudioFormat;
using tts_grpc_gateway::v1::EndOfUtterance;

typedef boost::circular_buffer<uint16_t> CircularBuffer_t;

static std::string fullDirPath;

class GStreamer {
  public:

  GStreamer(verbio_t* v) {
    auto channelCreds = grpc::SslCredentials(grpc::SslCredentialsOptions());
    m_channel = grpc::CreateChannel(
                  "us.speechcenter.verbio.com",
                  grpc::CompositeChannelCredentials(
                  grpc::SslCredentials(grpc::SslCredentialsOptions()),
                  grpc::AccessTokenCredentials(v->access_token)));
    if (!m_channel) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Verbio failed creating grpc channel\n");	
      throw std::runtime_error(std::string("Error creating Verbio grpc channel"));
    }
    m_stub = std::move(TextToSpeech::NewStub(m_channel));
    grpc::ClientContext context;
    m_streamer = m_stub->StreamingSynthesizeSpeech(&context);
    auto* config = m_request.mutable_config();
    config->set_voice(v->voice_name);
    config->set_sampling_rate(VoiceSamplingRate::VOICE_SAMPLING_RATE_8KHZ);

    // Send synthesis config
    m_streamer->Write(m_request);
    m_request.clear_config();
  }

  ~GStreamer() {

  }

  bool synthesize(const char* text) {
    // send text
    m_request.clear_end_of_utterance();
    m_request.set_text(text);
    bool ok = m_streamer->Write(m_request);

    if (ok) {
      // send end signal
      m_request.clear_text();
      m_request.set_allocated_end_of_utterance(new EndOfUtterance());
      ok = m_streamer->Write(m_request);
      if (!ok) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Verbio failed sending EndOfUtterance\n");	
      }
    } else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Verbio failed sending text\n");	
    }

    return ok;
  }

  bool read(StreamingSynthesisResponse* response) {
    return m_streamer->Read(response);
  }

  grpc::Status finish() {
    return m_streamer->Finish();
  }

  private:
    grpc::ClientContext m_context;
    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<TextToSpeech::Stub> m_stub;
    StreamingSynthesisRequest m_request;
    std::unique_ptr<
      grpc::ClientReaderWriter<StreamingSynthesisRequest, StreamingSynthesisResponse>,
      std::default_delete<grpc::ClientReaderWriter<StreamingSynthesisRequest, StreamingSynthesisResponse>>> m_streamer;

};

static void start_synthesis(verbio_t* v) {
  GStreamer* streamer = (GStreamer *) v->streamer;
  StreamingSynthesisResponse response;
  CircularBuffer_t *cBuffer = (CircularBuffer_t *) v->circularBuffer;
  while(streamer->read(&response)) {
    if (v->flushed) break;
    if (response.has_streaming_audio()) {
        const auto& audioData = response.streaming_audio().audio_samples();
        const uint16_t* begin = reinterpret_cast<const uint16_t*>(audioData.data());
        const uint16_t* end = reinterpret_cast<const uint16_t*>(audioData.data() + audioData.size());

          /* lock as briefly as possible */
        switch_mutex_lock(v->mutex);
        if (cBuffer->capacity() - cBuffer->size() < audioData.size()) {
          cBuffer->set_capacity(cBuffer->size() + std::max( audioData.size(), (size_t)BUFFER_SIZE));
        }
        cBuffer->insert(cBuffer->end(), begin, end);
        switch_mutex_unlock(v->mutex);
    }
    else if (response.has_end_of_utterance()) {
        break;
    }
  }
  v->draining = 1;
}

extern "C" {
  switch_status_t verbio_speech_load() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_loading..\n");

    /* create temp folder for cache files */
    const char* baseDir = std::getenv("JAMBONZ_TMP_CACHE_FOLDER");
    if (!baseDir) {
      baseDir = "/tmp/";
    }
    if (strcmp(baseDir, "/") == 0) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create folder %s\n", baseDir);
      return SWITCH_STATUS_FALSE;
    }

    fullDirPath = std::string(baseDir) + "tts-cache-files";

    // Create the directory with read, write, and execute permissions for everyone
    mode_t oldMask = umask(0);
    int result = mkdir(fullDirPath.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
    umask(oldMask);
    if (result != 0) {
      if (errno != EEXIST) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create folder %s\n", fullDirPath.c_str());
        fullDirPath = "";
      }
      else switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "folder %s already exists\n", fullDirPath.c_str());
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "created folder %s\n", fullDirPath.c_str());
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_loaded..\n");

    return SWITCH_STATUS_SUCCESS;
  }

    switch_status_t verbio_speech_open(verbio_t* verbio) {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_feed_tts(verbio_t* v, char* text, switch_speech_flag_t *flags) {
    const int MAX_CHARS = 20;
    char tempText[MAX_CHARS + 4]; // +4 for the ellipsis and null terminator

    if (strlen(text) > MAX_CHARS) {
        strncpy(tempText, text, MAX_CHARS);
        strcpy(tempText + MAX_CHARS, "...");
    } else {
        strcpy(tempText, text);
    }

    /* open cache file */
    if (v->cache_audio && fullDirPath.length() > 0) {
      switch_uuid_t uuid;
      char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
      char outfile[512] = "";
      int fd;

      switch_uuid_get(&uuid);
      switch_uuid_format(uuid_str, &uuid);

      switch_snprintf(outfile, sizeof(outfile), "%s%s%s.r8", fullDirPath.c_str(), SWITCH_PATH_SEPARATOR, uuid_str);
      v->cache_filename = strdup(outfile);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "writing audio cache file to %s\n", v->cache_filename);

      mode_t oldMask = umask(0);
      fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
      umask(oldMask);
      if (fd == -1 ) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
      }
      else {
        v->file = fdopen(fd, "wb");
        if (!v->file) {
          close(fd);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
        }
      }
    }

    if (!v->access_token) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "verbio_speech_feed_tts: no access_key provided\n");
      return SWITCH_STATUS_FALSE;
    }

    if (!v->voice_name) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "verbio_speech_feed_tts: no voice_name provided\n");
      return SWITCH_STATUS_FALSE;
    }

    if (v->rate != 8000 /*Hz*/) {
      int err;
      v->resampler = speex_resampler_init(1, 8000, v->rate, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }

    std::chrono::time_point<std::chrono::high_resolution_clock>* ptr = new std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::high_resolution_clock::now());
    v->startTime = ptr;

    v->circularBuffer = (void *) new CircularBuffer_t(BUFFER_SIZE);

    

    try {
      std::thread(start_synthesis, v).detach();
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "verbio_speech_feed_tts sent synthesize request\n");
    } catch (const std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_verbio_tts: Exception: %s\n", e.what());
      return SWITCH_STATUS_FALSE;
    }
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_read_tts(verbio_t* v, void *data, size_t *datalen, switch_speech_flag_t *flags) {
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) v->circularBuffer;
    std::vector<uint16_t> pcm_data;

    if (v->response_code > 0 && v->response_code != 200) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_read_tts, returning failure\n") ;
      return SWITCH_STATUS_FALSE;
    }
    if (v->flushed) {
      return SWITCH_STATUS_BREAK;
    }
    switch_mutex_lock(v->mutex);
    size_t bufSize = cBuffer->size();
    if (cBuffer->empty()) {
      switch_mutex_unlock(v->mutex);
      if (v->draining) {
        return SWITCH_STATUS_BREAK;
      }
      /* no audio available yet so send silence */
      memset(data, 255, *datalen);
      return SWITCH_STATUS_SUCCESS;
    }
    // verbio returned 8000hz 16 bit data, we have to take enough data based on call sample rate.
    size_t size = std::min((*datalen/(2 * v->rate / 8000)), bufSize);
    pcm_data.insert(pcm_data.end(), cBuffer->begin(), cBuffer->begin() + size);
    cBuffer->erase(cBuffer->begin(), cBuffer->begin() + size);
    switch_mutex_unlock(v->mutex);

    size_t data_size = pcm_data.size();

    if (v->resampler) {
        std::vector<int16_t> in(pcm_data.begin(), pcm_data.end());

        std::vector<int16_t> out((*datalen));
        spx_uint32_t in_len = data_size;
        spx_uint32_t out_len = out.size();

        speex_resampler_process_interleaved_int(v->resampler, in.data(), &in_len, out.data(), &out_len);

        if (out_len > out.size()) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Resampler output exceeded maximum buffer size!\n");
          return SWITCH_STATUS_FALSE;
        }

        memcpy(data, out.data(), out_len * sizeof(int16_t));
        *datalen = out_len * sizeof(int16_t);
    } else {
        memcpy(data, pcm_data.data(), data_size * sizeof(int16_t));
        *datalen = data_size * sizeof(int16_t);
    }

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_flush_tts(verbio_t* v) {
    bool download_complete = v->response_code == 200;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_flush_tts, download complete? %s\n", download_complete ? "yes" : "no") ;

    CircularBuffer_t *cBuffer = (CircularBuffer_t *) v->circularBuffer;
    delete cBuffer;
    v->circularBuffer = nullptr ;
    delete static_cast<std::chrono::time_point<std::chrono::high_resolution_clock>*>(v->startTime);
    v->startTime = nullptr;

    GStreamer* streamer = (GStreamer *) v->streamer;
    streamer->finish();
    delete streamer;
    v->streamer = nullptr;

    v->flushed = 1;
    if (!download_complete) {
      if (v->file) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "closing audio cache file %s because download was interrupted\n", v->cache_filename);
        if (fclose(v->file) != 0) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error closing audio cache file\n");
        }
        v->file = nullptr ;
      }

      if (v->cache_filename) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing audio cache file %s because download was interrupted\n", v->cache_filename);
        if (unlink(v->cache_filename) != 0) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "cleanupConn: error removing audio cache file %s: %d:%s\n", 
            v->cache_filename, errno, strerror(errno));
        }
        free(v->cache_filename);
        v->cache_filename = nullptr ;
      }
    }
    if (v->session_id) {
      switch_core_session_t* session = switch_core_session_locate(v->session_id);
      if (session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);

        /* unlock as quickly as possible */
        switch_core_session_rwunlock(session);
        if (channel) {
          switch_event_t *event;
          if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_STOP) == SWITCH_STATUS_SUCCESS) {
            switch_channel_event_set_data(channel, event);
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_verbio_response_code", std::to_string(v->response_code).c_str());
            if (v->cache_filename && download_complete) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", v->cache_filename);
            }
            if (!download_complete && v->err_msg) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_error", v->err_msg);
            }
            switch_event_fire(&event);
          }
          else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: failed to create event\n");
          }
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: channel not found\n");
        }
      }
    }

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_close(verbio_t* v) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_close\n") ;
    if (v->resampler) {
      speex_resampler_destroy(v->resampler);
    }

    v->resampler = NULL;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_unload() {
    return SWITCH_STATUS_SUCCESS;
  }

} 