#include "mod_azure_tts.h"
#include <switch.h>
#include <speechapi_cxx.h>

#include <boost/circular_buffer.hpp>

#include <cstdlib>
#include <string>
#include <chrono>

#define BUFFER_SIZE 8129

typedef boost::circular_buffer<uint16_t> CircularBuffer_t;

using namespace Microsoft::CognitiveServices::Speech;

static std::string fullDirPath;

extern "C" {
  switch_status_t azure_speech_load() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "azure_speech_loading..\n");

    /* create temp folder for cache files */
    const char* baseDir = std::getenv("JAMBONZ_TMP_CACHE_FOLDER");
    if (!baseDir) {
      baseDir = "/var/";
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

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "azure_speech_loaded..\n");

    return SWITCH_STATUS_SUCCESS;
  }

    switch_status_t azure_speech_open(azure_t* azure) {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t azure_speech_feed_tts(azure_t* a, char* text, switch_speech_flag_t *flags) {
    const int MAX_CHARS = 20;
    char tempText[MAX_CHARS + 4]; // +4 for the ellipsis and null terminator

    if (strlen(text) > MAX_CHARS) {
        strncpy(tempText, text, MAX_CHARS);
        strcpy(tempText + MAX_CHARS, "...");
    } else {
        strcpy(tempText, text);
    }

    /* open cache file */
    if (a->cache_audio && fullDirPath.length() > 0) {
      switch_uuid_t uuid;
      char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
      char outfile[512] = "";
      int fd;

      switch_uuid_get(&uuid);
      switch_uuid_format(uuid_str, &uuid);

      switch_snprintf(outfile, sizeof(outfile), "%s%s%s.r8", fullDirPath.c_str(), SWITCH_PATH_SEPARATOR, uuid_str);
      a->cache_filename = strdup(outfile);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "writing audio cache file to %s\n", a->cache_filename);

      mode_t oldMask = umask(0);
      fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
      umask(oldMask);
      if (fd == -1 ) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
      }
      else {
        a->file = fdopen(fd, "wb");
        if (!a->file) {
          close(fd);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
        }
      }
    }

    if (!a->api_key) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "azure_speech_feed_tts: no api_key provided\n");
      return SWITCH_STATUS_FALSE;
    }

    if (!a->language) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "azure_speech_feed_tts: no language provided\n");
      return SWITCH_STATUS_FALSE;
    }

    if (a->session_id) {
      int err;
      switch_codec_implementation_t read_impl;
      switch_core_session_t *psession = switch_core_session_locate(a->session_id);
      switch_core_session_get_read_impl(psession, &read_impl);
      uint32_t samples_per_second = !strcasecmp(read_impl.iananame, "g722") ? read_impl.actual_samples_per_second : read_impl.samples_per_second;
      a->samples_rate = samples_per_second;
      if (samples_per_second != 8000 /*Hz*/) {
        a->resampler = speex_resampler_init(1, 8000, samples_per_second, SWITCH_RESAMPLE_QUALITY, &err);
        if (0 != err) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
          return SWITCH_STATUS_FALSE;
        }
      }
    }

    std::chrono::time_point<std::chrono::high_resolution_clock>* ptr = new std::chrono::time_point<std::chrono::high_resolution_clock>(std::chrono::high_resolution_clock::now());
    a->startTime = ptr;

    a->circularBuffer = (void *) new CircularBuffer_t(BUFFER_SIZE);

    auto speechConfig = nullptr != a->endpoint ? 
			(nullptr != a->api_key ?
				SpeechConfig::FromEndpoint(a->endpoint, a->api_key) :
				SpeechConfig::FromEndpoint(a->endpoint)) :
			SpeechConfig::FromSubscription(a->api_key, a->region ? a->region : "");

    speechConfig->SetSpeechSynthesisOutputFormat(SpeechSynthesisOutputFormat::Raw8Khz16BitMonoPcm);
    speechConfig->SetSpeechSynthesisLanguage(a->language);
    speechConfig->SetSpeechSynthesisVoiceName(a->voice_name);
    if (a->http_proxy_ip) {
      uint32_t port = a->http_proxy_port && a->http_proxy_port[0] != '\0' ? static_cast<uint32_t>(std::stoul(a->http_proxy_port)) : 80;
      speechConfig->SetProxy(a->http_proxy_ip, port);
    }

    if (nullptr != a->endpointId) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "azure_speech_feed_tts setting endpoint id: %s\n", a->endpointId);
			speechConfig->SetEndpointId(a->endpointId);
		}

    auto speechSynthesizer = SpeechSynthesizer::FromConfig(speechConfig);

    speechSynthesizer->SynthesisStarted += [a](const SpeechSynthesisEventArgs& e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "azure_speech_feed_tts SynthesisStarted\n");
        a->response_code = 200;
    };

    speechSynthesizer->Synthesizing += [a](const SpeechSynthesisEventArgs& e) {
      bool fireEvent = false;
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) a->circularBuffer;
      std::vector<uint16_t> pcm_data;

      //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Synthesizing: received data\n");

      if (a->flushed) {
        return;
      }
      {
        auto audioData = e.Result->GetAudioData();
        for (size_t i = 0; i < audioData->size(); i += sizeof(int16_t)) {
            int16_t value = static_cast<int16_t>((*audioData)[i]) | (static_cast<int16_t>((*audioData)[i + 1]) << 8);
            pcm_data.push_back(value);
        }

        /* and write to the file */
        size_t bytesResampled = pcm_data.size() * sizeof(uint16_t);
        if (a->file) fwrite(pcm_data.data(), sizeof(uint16_t), pcm_data.size(), a->file);

        switch_mutex_lock(a->mutex);

        // Resize the buffer if necessary
        if (cBuffer->capacity() - cBuffer->size() < (bytesResampled / sizeof(uint16_t))) {

          //TODO: if buffer exceeds some max size, return CURL_WRITEFUNC_ERROR to abort the transfer
          cBuffer->set_capacity(cBuffer->size() + std::max((bytesResampled / sizeof(uint16_t)), (size_t)BUFFER_SIZE));
        }

        /* Push the data into the buffer */
        cBuffer->insert(cBuffer->end(), pcm_data.data(), pcm_data.data() + pcm_data.size());

        switch_mutex_unlock(a->mutex);
      }

      if (0 == a->reads++) {
        fireEvent = true;
      }

      if (fireEvent && a->session_id) {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto startTime = *static_cast<std::chrono::time_point<std::chrono::high_resolution_clock>*>(a->startTime);
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        auto time_to_first_byte_ms = std::to_string(duration.count());
        switch_core_session_t* session = switch_core_session_locate(a->session_id);
        if (session) {
          switch_channel_t *channel = switch_core_session_get_channel(session);
          if (channel) {
            switch_event_t *event;
            if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_START) == SWITCH_STATUS_SUCCESS) {
              switch_channel_event_set_data(channel, event);
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_time_to_first_byte_ms", time_to_first_byte_ms.c_str());
              if (a->cache_filename) {
                switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", a->cache_filename);
              }
              switch_event_fire(&event);
            } else {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "speechSynthesizer->Synthesizing: failed to create event\n");
            }
          }else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "speechSynthesizer->Synthesizing: channel not found\n");
          }
          switch_core_session_rwunlock(session);
        }
      }
    };

    speechSynthesizer->SynthesisCompleted += [a](const SpeechSynthesisEventArgs& e) {
       switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "azure_speech_feed_tts SynthesisCompleted\n");
       a->draining = 1;
    };

    speechSynthesizer->SynthesisCanceled += [a](const SpeechSynthesisEventArgs& e) {
      if (e.Result->Reason == ResultReason::Canceled) {
        auto cancellation = SpeechSynthesisCancellationDetails::FromResult(e.Result);
        a->response_code = static_cast<long int>(cancellation->ErrorCode);
        a->err_msg = strdup(cancellation->ErrorDetails.c_str());
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error synthsize tex %d with error string: %s.\n", static_cast<int>(cancellation->ErrorCode), cancellation->ErrorDetails.c_str());
      }
    };
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "azure_speech_feed_tts before sending synthesize request\n");
    if (std::strncmp(text, "<speak", 6) == 0) {
       std::shared_ptr< SpeechSynthesisResult > result = speechSynthesizer->SpeakSsmlAsync(text).get();
    } else {
      std::shared_ptr< SpeechSynthesisResult > result = speechSynthesizer->SpeakTextAsync(text).get();
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "azure_speech_feed_tts sent synthesize request\n");
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t azure_speech_read_tts(azure_t* a, void *data, size_t *datalen, switch_speech_flag_t *flags) {
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) a->circularBuffer;
    std::vector<uint16_t> pcm_data;

    if (a->response_code > 0 && a->response_code != 200) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "azure_speech_read_tts, returning failure\n") ;
      return SWITCH_STATUS_FALSE;
    }
    if (a->flushed) {
      return SWITCH_STATUS_BREAK;
    }
    switch_mutex_lock(a->mutex);
    size_t bufSize = cBuffer->size();
    if (cBuffer->empty()) {
      switch_mutex_unlock(a->mutex);
      if (a->draining) {
        return SWITCH_STATUS_BREAK;
      }
      /* no audio available yet so send silence */
      memset(data, 255, *datalen);
      return SWITCH_STATUS_SUCCESS;
    }
    // azure returned 8000hz 16 bit data, we have to take enough data based on call sample rate.
    size_t size = a->samples_rate ?
      std::min((*datalen/(2 * a->samples_rate / 8000)), bufSize) :
      std::min((*datalen/2), bufSize);
    pcm_data.insert(pcm_data.end(), cBuffer->begin(), cBuffer->begin() + size);
    cBuffer->erase(cBuffer->begin(), cBuffer->begin() + size);
    switch_mutex_unlock(a->mutex);

    size_t data_size = pcm_data.size();

    if (a->resampler) {
        std::vector<int16_t> in(pcm_data.begin(), pcm_data.end());

        std::vector<int16_t> out((*datalen));
        spx_uint32_t in_len = data_size;
        spx_uint32_t out_len = out.size();

        speex_resampler_process_interleaved_int(a->resampler, in.data(), &in_len, out.data(), &out_len);

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

  switch_status_t azure_speech_flush_tts(azure_t* a) {
    bool download_complete = a->response_code == 200;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "azure_speech_flush_tts, download complete? %s\n", download_complete ? "yes" : "no") ;

    CircularBuffer_t *cBuffer = (CircularBuffer_t *) a->circularBuffer;
    delete cBuffer;
    a->circularBuffer = nullptr ;
    delete static_cast<std::chrono::time_point<std::chrono::high_resolution_clock>*>(a->startTime);
    a->startTime = nullptr;

    a->flushed = 1;
    if (!download_complete) {
      if (a->file) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "closing audio cache file %s because download was interrupted\n", a->cache_filename);
        if (fclose(a->file) != 0) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error closing audio cache file\n");
        }
        a->file = nullptr ;
      }

      if (a->cache_filename) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing audio cache file %s because download was interrupted\n", a->cache_filename);
        if (unlink(a->cache_filename) != 0) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "cleanupConn: error removing audio cache file %s: %d:%s\n", 
            a->cache_filename, errno, strerror(errno));
        }
        free(a->cache_filename);
        a->cache_filename = nullptr ;
      }
    }
    if (a->session_id) {
      switch_core_session_t* session = switch_core_session_locate(a->session_id);
      if (session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (channel) {
          switch_event_t *event;
          if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_STOP) == SWITCH_STATUS_SUCCESS) {
            switch_channel_event_set_data(channel, event);
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_azure_response_code", std::to_string(a->response_code).c_str());
            if (a->cache_filename && download_complete) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", a->cache_filename);
            }
            if (!download_complete && a->err_msg) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_error", a->err_msg);
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
        switch_core_session_rwunlock(session);
      }
    }

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t azure_speech_close(azure_t* a) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "azure_speech_close\n") ;
    if (a->resampler) {
      speex_resampler_destroy(a->resampler);
    }

    a->resampler = NULL;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t azure_speech_unload() {
    return SWITCH_STATUS_SUCCESS;
  }

} 