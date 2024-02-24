#include "mod_azure_tts.h"
#include <switch.h>
#include <speechapi_cxx.h>

#include <cstdlib>
#include <string>

#define BUFFER_SIZE 8129
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

    if (!a->region) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "azure_speech_feed_tts: no region provided\n");
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
      if (samples_per_second != 8000 /*Hz*/) {
        a->resampler = speex_resampler_init(1, samples_per_second, 8000, SWITCH_RESAMPLE_QUALITY, &err);
        if (0 != err) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
          return SWITCH_STATUS_FALSE;
        }
      }
    }

    auto speechConfig = nullptr != a->endpoint ? 
			(nullptr != a->api_key ?
				SpeechConfig::FromEndpoint(a->endpoint, a->api_key) :
				SpeechConfig::FromEndpoint(a->endpoint)) :
			SpeechConfig::FromSubscription(a->api_key, a->region);

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

    auto result = std::strncmp(text, "<speak", 6) == 0 ? speechSynthesizer->SpeakSsmlAsync(text).get() : speechSynthesizer->SpeakTextAsync(text).get();

    if (result->Reason == ResultReason::SynthesizingAudioCompleted) {
      a->response_code = 200;
      auto audioDataStream = AudioDataStream::FromResult(result);
      a->audioStream = (void *) new std::shared_ptr<AudioDataStream>(audioDataStream);;
    } else if (result->Reason == ResultReason::Canceled) {
      auto cancellation = SpeechSynthesisCancellationDetails::FromResult(result);
      a->response_code = static_cast<long int>(cancellation->ErrorCode);
      a->err_msg = strdup(cancellation->ErrorDetails.c_str());
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error synthsize tex %d with error string: %s.\n", static_cast<int>(cancellation->ErrorCode), cancellation->ErrorDetails);
      return SWITCH_STATUS_FALSE;
    }
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t azure_speech_read_tts(azure_t* a, void *data, size_t *datalen, switch_speech_flag_t *flags) {
    if (a->response_code > 0 && a->response_code != 200) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "azure_speech_read_tts, returning failure\n") ;  
      return SWITCH_STATUS_FALSE;
    }

    if (a->flushed) {
      return SWITCH_STATUS_BREAK;
    }

    if (!a->audioStream) {
      /* no audio available yet so send silence */
      memset(data, 255, *datalen);
      return SWITCH_STATUS_SUCCESS;
    }

    auto audioStream = *(static_cast<std::shared_ptr<AudioDataStream>*>(a->audioStream));
    std::vector<uint8_t> audioBuffer(*datalen);
    uint32_t bytesRead = audioStream->ReadData(audioBuffer.data(), *datalen);
    // write to cache file *.r8
    if (a->file) {
      fwrite(audioBuffer.data(), sizeof(uint8_t), bytesRead, a->file);
    }
    // There is no more to read, finished
    if (bytesRead == 0) {
        a->flushed = 1;
        if (a->file) {
          if (fclose(a->file) != 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "azure_speech_read_tts: error closing audio cache file\n");
          }
          a->file = nullptr ;
        }
        return SWITCH_STATUS_BREAK;
    }

    if (a->resampler) {
        int16_t in[bytesRead/2];
        std::memcpy(in, audioBuffer.data(), bytesRead);

        int16_t out[bytesRead/2];
        spx_uint32_t in_len = bytesRead / 2;
        spx_uint32_t out_len = bytesRead / 2;

        // Resample the audio
        speex_resampler_process_interleaved_int(a->resampler, in, &in_len, out, &out_len);
        std::memcpy(data, out, out_len * 2);
        *datalen = out_len * 2;
    } else {
        std::memcpy(data, audioBuffer.data(), bytesRead);
        *datalen = bytesRead;
    }

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t azure_speech_flush_tts(azure_t* a) {
    bool download_complete = a->response_code == 200;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "azure_speech_flush_tts, download complete? %s\n", download_complete ? "yes" : "no") ;  
    a->flushed = 1;
    if (!download_complete) {
      if (a->file) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "closing audio cache file %s because download was interrupted\n", a->cache_filename);
        if (fclose(a->file) != 0) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error closing audio cache file\n");
        }
        a->file = nullptr ;
      }

      if (a->cache_filename) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing audio cache file %s because download was interrupted\n", a->cache_filename);
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
    if (a->audioStream) {
      auto ptr = static_cast<std::shared_ptr<AudioDataStream>*>(a->audioStream);
      delete ptr;
      a->audioStream = NULL; 
    }

    a->resampler = NULL;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t azure_speech_unload() {
    return SWITCH_STATUS_SUCCESS;
  }

}