#include "mod_google_tts.h"
#include <switch.h>

#include <boost/circular_buffer.hpp>

#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>

#include "google/cloud/texttospeech/v1/cloud_tts.grpc.pb.h"
#include <grpc++/grpc++.h>

using google::cloud::texttospeech::v1::SynthesizeSpeechRequest;
using google::cloud::texttospeech::v1::SynthesizeSpeechResponse;
using google::cloud::texttospeech::v1::TextToSpeech;
using google::cloud::texttospeech::v1::Voice;
using google::cloud::texttospeech::v1::SsmlVoiceGender;
using google::cloud::texttospeech::v1::SsmlVoiceGender_Name;
using google::cloud::texttospeech::v1::SynthesisInput;
using google::cloud::texttospeech::v1::AudioEncoding;

#define BUFFER_SIZE 8129

typedef boost::circular_buffer<uint16_t> CircularBuffer_t;

static std::string fullDirPath;

static void start_synthesis(const char* text, google_t* g) {
    try {
      SynthesizeSpeechRequest request;
      SynthesizeSpeechResponse response;
      grpc::ClientContext context;
      auto input = request.mutable_input();
      auto voice = request.mutable_voice();
      auto custom_voice = voice->mutable_custom_voice();
      auto audio_config = request.mutable_audio_config();
      auto channelCreds = grpc::SslCredentials(grpc::SslCredentialsOptions());
      auto callCreds = grpc::ServiceAccountJWTAccessCredentials(g->credential);
      auto creds = grpc::CompositeChannelCredentials(channelCreds, callCreds);
      auto channel = grpc::CreateChannel("texttospeech.googleapis.com", creds);
      auto stub = TextToSpeech::NewStub(channel);

      if (strstr(text, "<speak") == text) {
        input->set_ssml(text);
      }
      else {
        input->set_text(text);
      }
      voice->set_name(g->voice_name);
      
      if (strcmp(g->gender, "MALE") == 0) {
        voice->set_ssml_gender(google::cloud::texttospeech::v1::SsmlVoiceGender::MALE);
      } else if (strcmp(g->gender, "FEMALE") == 0) {
        voice->set_ssml_gender(google::cloud::texttospeech::v1::SsmlVoiceGender::FEMALE);
      } else if (strcmp(g->gender, "NEUTRAL") == 0) {
        voice->set_ssml_gender(google::cloud::texttospeech::v1::SsmlVoiceGender::NEUTRAL);
      } else {
        voice->set_ssml_gender(google::cloud::texttospeech::v1::SsmlVoiceGender::SSML_VOICE_GENDER_UNSPECIFIED);
      }
      if (g->model) {
        custom_voice->set_model(g->model);
        if (strcmp(g->reported_usage, "OFFLINE") == 0) {
          custom_voice->set_reported_usage(google::cloud::texttospeech::v1::CustomVoiceParams_ReportedUsage::CustomVoiceParams_ReportedUsage_OFFLINE);
        } else if (strcmp(g->reported_usage, "REALTIME") == 0) {
          custom_voice->set_reported_usage(google::cloud::texttospeech::v1::CustomVoiceParams_ReportedUsage::CustomVoiceParams_ReportedUsage_REALTIME);
        } else {
          custom_voice->set_reported_usage(google::cloud::texttospeech::v1::CustomVoiceParams_ReportedUsage::CustomVoiceParams_ReportedUsage_REPORTED_USAGE_UNSPECIFIED);
        }
      }
      voice->set_language_code(g->language);
      audio_config->set_audio_encoding(AudioEncoding::LINEAR16);
      audio_config->set_sample_rate_hertz(8000);
      grpc::Status status = stub->SynthesizeSpeech(&context, request, &response);
      if (!status.ok()) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, 
          "start_synthesis: error synthesizing speech: %s: details: %s\n", 
          status.error_message().c_str(), status.error_details().c_str()); 
        return;
      }
      g->response_code = 200;
      auto audioData = response.audio_content();
      if (g->flushed) return;
      bool fireEvent = false;
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) g->circularBuffer;
      if (g->file) {
        fwrite(audioData.data(), 1, audioData.size(), g->file);
      }

      /**
       * this sort of reinterpretation can be dangerous as a general rule, but in this case we know that the data
       * is 16-bit PCM, so it's safe to do this and its much faster than copying the data byte by byte
       */
      const uint16_t* begin = reinterpret_cast<const uint16_t*>(audioData.data());
      const uint16_t* end = reinterpret_cast<const uint16_t*>(audioData.data() + audioData.size());

      /* lock as briefly as possible */
      switch_mutex_lock(g->mutex);
      if (cBuffer->capacity() - cBuffer->size() < audioData.size()) {
        cBuffer->set_capacity(cBuffer->size() + std::max( audioData.size(), (size_t)BUFFER_SIZE));
      }
      cBuffer->insert(cBuffer->end(), begin, end);
      switch_mutex_unlock(g->mutex);

      if (0 == g->reads++) {
        fireEvent = true;
      }

      if (fireEvent && g->session_id) {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto startTime = *static_cast<std::chrono::time_point<std::chrono::high_resolution_clock>*>(g->startTime);
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        auto time_to_first_byte_ms = std::to_string(duration.count());
        switch_core_session_t* session = switch_core_session_locate(g->session_id);
        if (session) {
          switch_channel_t *channel = switch_core_session_get_channel(session);
          switch_core_session_rwunlock(session);
          if (channel) {
            switch_event_t *event;
            if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_START) == SWITCH_STATUS_SUCCESS) {
              switch_channel_event_set_data(channel, event);
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_time_to_first_byte_ms", time_to_first_byte_ms.c_str());
              if (g->cache_filename) {
                switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", g->cache_filename);
              }
              switch_event_fire(&event);
            } else {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_google_tts start_synthesis: failed to create event\n");
            }
          }else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_google_tts start_synthesis: channel not found\n");
          }
        }
      }
    } catch (const std::exception& e) {
        g->response_code = 500;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_google_tts: Exception in start_synthesis %s\n",  e.what());
    }
    g->draining = 1;
}

extern "C" {
  switch_status_t google_speech_load() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "google_speech_loading..\n");

    /* create temp folder for cache files */
    const char* baseDir = std::getenv("JAMBONZ_TMP_CACHE_FOLDER");
    if (!baseDir) {
      baseDir = "/tmp/";
    }
    if (strcmp(baseDir, "/") == 0) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create folder %s\n", baseDir);
      return SWITCH_STATUS_FALSE;
    }

    fullDirPath = std::string(baseDir) + "jambonz-tts-cache-files";

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

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "google_speech_loaded..\n");

    return SWITCH_STATUS_SUCCESS;
  }

    switch_status_t google_speech_open(google_t* google) {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t google_speech_feed_tts(google_t* g, char* text, switch_speech_flag_t *flags) {
    const int MAX_CHARS = 20;
    char tempText[MAX_CHARS + 4]; // +4 for the ellipsis and null terminator

    if (strlen(text) > MAX_CHARS) {
        strncpy(tempText, text, MAX_CHARS);
        strcpy(tempText + MAX_CHARS, "...");
    } else {
        strcpy(tempText, text);
    }

    /* open cache file */
    if (g->cache_audio && fullDirPath.length() > 0) {
      switch_uuid_t uuid;
      char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
      char outfile[512] = "";
      int fd;

      switch_uuid_get(&uuid);
      switch_uuid_format(uuid_str, &uuid);

      switch_snprintf(outfile, sizeof(outfile), "%s%s%s.r8", fullDirPath.c_str(), SWITCH_PATH_SEPARATOR, uuid_str);
      g->cache_filename = strdup(outfile);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "writing audio cache file to %s\n", g->cache_filename);

      mode_t oldMask = umask(0);
      fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
      umask(oldMask);
      if (fd == -1 ) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
      }
      else {
        g->file = fdopen(fd, "wb");
        if (!g->file) {
          close(fd);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
        }
      }
    }

    if (!g->credential) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "google_speech_feed_tts: no credential provided\n");
      return SWITCH_STATUS_FALSE;
    }

    if (!g->language) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "google_speech_feed_tts: no language provided\n");
      return SWITCH_STATUS_FALSE;
    }

    if (!g->voice_name) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "google_speech_feed_tts: no voice_name provided\n");
      return SWITCH_STATUS_FALSE;
    }

    if (g->rate != 8000 /*Hz*/) {
      int err;
      g->resampler = speex_resampler_init(1, 8000, g->rate, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }

    std::chrono::time_point<std::chrono::high_resolution_clock>* ptr = new std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::high_resolution_clock::now());
    g->startTime = ptr;

    g->circularBuffer = (void *) new CircularBuffer_t(BUFFER_SIZE);

    std::thread(start_synthesis, text, g).detach();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "google_speech_feed_tts sent synthesize request\n");
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t google_speech_read_tts(google_t* g, void *data, size_t *datalen, switch_speech_flag_t *flags) {
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) g->circularBuffer;
    std::vector<uint16_t> pcm_data;

    if (g->response_code > 0 && g->response_code != 200) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "google_speech_read_tts, returning failure\n") ;
      return SWITCH_STATUS_FALSE;
    }
    if (g->flushed) {
      return SWITCH_STATUS_BREAK;
    }
    switch_mutex_lock(g->mutex);
    size_t bufSize = cBuffer->size();
    if (cBuffer->empty()) {
      switch_mutex_unlock(g->mutex);
      if (g->draining) {
        return SWITCH_STATUS_BREAK;
      }
      /* no audio available yet so send silence */
      memset(data, 255, *datalen);
      return SWITCH_STATUS_SUCCESS;
    }
    // google returned 8000hz 16 bit data, we have to take enough data based on call sample rate.
    size_t size = std::min((*datalen/(2 * g->rate / 8000)), bufSize);
    pcm_data.insert(pcm_data.end(), cBuffer->begin(), cBuffer->begin() + size);
    cBuffer->erase(cBuffer->begin(), cBuffer->begin() + size);
    switch_mutex_unlock(g->mutex);

    size_t data_size = pcm_data.size();

    if (g->resampler) {
      std::vector<int16_t> in(pcm_data.begin(), pcm_data.end());

      std::vector<int16_t> out((*datalen));
      spx_uint32_t in_len = data_size;
      spx_uint32_t out_len = out.size();

      speex_resampler_process_interleaved_int(g->resampler, in.data(), &in_len, out.data(), &out_len);

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

  switch_status_t google_speech_flush_tts(google_t* g) {
    bool download_complete = g->response_code == 200;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "google_speech_flush_tts, download complete? %s\n", download_complete ? "yes" : "no") ;

    CircularBuffer_t *cBuffer = (CircularBuffer_t *) g->circularBuffer;
    delete cBuffer;
    g->circularBuffer = nullptr ;
    delete static_cast<std::chrono::time_point<std::chrono::high_resolution_clock>*>(g->startTime);
    g->startTime = nullptr;

    g->flushed = 1;
    if (!download_complete) {
      if (g->file) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "closing audio cache file %s because download was interrupted\n", g->cache_filename);
        if (fclose(g->file) != 0) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error closing audio cache file\n");
        }
        g->file = nullptr ;
      }

      if (g->cache_filename) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing audio cache file %s because download was interrupted\n", g->cache_filename);
        if (unlink(g->cache_filename) != 0) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "cleanupConn: error removing audio cache file %s: %d:%s\n", 
            g->cache_filename, errno, strerror(errno));
        }
        free(g->cache_filename);
        g->cache_filename = nullptr ;
      }
    }
    if (g->session_id) {
      switch_core_session_t* session = switch_core_session_locate(g->session_id);
      if (session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);

        /* unlock as quickly as possible */
        switch_core_session_rwunlock(session);
        if (channel) {
          switch_event_t *event;
          if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_STOP) == SWITCH_STATUS_SUCCESS) {
            switch_channel_event_set_data(channel, event);
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_google_response_code", std::to_string(g->response_code).c_str());
            if (g->cache_filename && download_complete) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", g->cache_filename);
            }
            if (!download_complete && g->err_msg) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_error", g->err_msg);
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

  switch_status_t google_speech_close(google_t* g) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "google_speech_close\n") ;
    if (g->resampler) {
      speex_resampler_destroy(g->resampler);
    }

    g->resampler = NULL;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t google_speech_unload() {
    return SWITCH_STATUS_SUCCESS;
  }

} 