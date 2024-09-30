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

#include "base64.hpp"
#include "parser.hpp"
#include "mod_audio_fork.h"
#include "audio_pipe.hpp"
#include "vector_math.h"

#include <boost/circular_buffer.hpp>


typedef boost::circular_buffer<uint16_t> CircularBuffer_t;

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320 /*which means each 20ms frame as 320 bytes at 8 khz (1 channel only)*/
#define BUFFER_GROW_SIZE (16384)
#define AUDIO_MARKER 0xFFFF
#define MAX_MARKS (30)

namespace {
  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = std::max(1, std::min(requestedBufferSecs ? ::atoi(requestedBufferSecs) : 2, 5));
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static const char* mySubProtocolName = std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") ?
    std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") : "audio.drachtio.org";
  static unsigned int nServiceThreads = std::max(1, std::min(requestedNumServiceThreads ? ::atoi(requestedNumServiceThreads) : 1, 5));
  static unsigned int idxCallCount = 0;
  static uint32_t playCount = 0;

  static bool markCountExceeded(private_t* tech_pvt) {
    if (nullptr != tech_pvt->pVecMarksInUse) {
      std::deque<std::string>* pVecMarksInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
      std::deque<std::string>* pVecMarksInInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
      return pVecMarksInUse->size()+ pVecMarksInInventory->size() >= MAX_MARKS;
    }
    return false;
  }

  switch_status_t processIncomingBinary(private_t* tech_pvt, switch_core_session_t* session, const char* message, size_t dataLength) {
    std::vector<uint8_t> data;

    // Prepend the set-aside byte if there is one
    if (tech_pvt->has_set_aside_byte) {
        data.push_back(tech_pvt->set_aside_byte);
        tech_pvt->has_set_aside_byte = false;
    }

    // Append the new incoming message
    data.insert(data.end(), message, message + dataLength);

    // Check if the total data length is now odd
    if (data.size() % 2 != 0) {
        // Set aside the last byte
        tech_pvt->set_aside_byte = data.back();
        tech_pvt->has_set_aside_byte = true;
        data.pop_back(); // Remove the last byte from the data vector
    }

    // Convert the data to 16-bit elements
    const uint16_t* data_uint16 = reinterpret_cast<const uint16_t*>(data.data());
    size_t numSamples = data.size() / sizeof(uint16_t);

    // Access the prebuffer
    CircularBuffer_t* cBuffer = static_cast<CircularBuffer_t*>(tech_pvt->streamingPreBuffer);

    int numMarkers = 0;
    std::deque<std::string>* pVecMarksInInventory = nullptr;
    std::deque<std::string>* pVecMarksInUse = nullptr;
    if (nullptr != tech_pvt->pVecMarksInInventory) {
      pVecMarksInInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
      pVecMarksInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
      numMarkers = pVecMarksInInventory->size();

      // move inventory to in-use
      pVecMarksInUse->insert(pVecMarksInUse->end(), pVecMarksInInventory->begin(), pVecMarksInInventory->end());
      pVecMarksInInventory->clear();
    }
  
    // Ensure the prebuffer has enough capacity
    if (cBuffer->capacity() - cBuffer->size() < numSamples + numMarkers) {
        size_t newCapacity = cBuffer->size() + std::max(numSamples + numMarkers, (size_t)BUFFER_GROW_SIZE);
        cBuffer->set_capacity(newCapacity);
    }

    // prepend any markers
    while (numMarkers-- > 0) {
      cBuffer->push_back(AUDIO_MARKER);
    }

    // Append the data to the prebuffer
    cBuffer->insert(cBuffer->end(), data_uint16, data_uint16 + numSamples);
    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Appended %zu 16-bit samples to the prebuffer.\n", numSamples);

    // if we haven't reached threshold amount of prebuffered data, return
    if (cBuffer->size() < tech_pvt->streamingPreBufSize) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Prebuffered data is below threshold %u, returning.\n", tech_pvt->streamingPreBufSize);
        return SWITCH_STATUS_SUCCESS;
    }

    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Prebuffered data samples %u is above threshold %u, prepare to playout.\n", cBuffer->size(), tech_pvt->streamingPreBufSize);

    // after initial pre-buffering, rachet down the threshold to 40ms
    tech_pvt->streamingPreBufSize = 320 * tech_pvt->downscale_factor * 2;

    // Check for downsampling factor
    size_t downsample_factor = tech_pvt->downscale_factor;

    // Calculate the number of samples that can be evenly divided by the downsample factor
    size_t numCompleteSamples = (cBuffer->size() / downsample_factor) * downsample_factor;

    // Handle leftover samples
    std::vector<uint16_t> leftoverSamples;
    size_t numLeftoverSamples = cBuffer->size() - numCompleteSamples;
    if (numLeftoverSamples > 0) {
        leftoverSamples.assign(cBuffer->end() - numLeftoverSamples, cBuffer->end());
        cBuffer->resize(numCompleteSamples);
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Temporarily removing %u leftover samples due to downsampling.\n", numLeftoverSamples);
    }

    // resample if necessary
    std::vector<int16_t> out;
    try {
      if (tech_pvt->bidirectional_audio_resampler) {
        // Improvement: Use assign to convert circular buffer to vector for resampling
        std::vector<int16_t> in;
        in.assign(cBuffer->begin(), cBuffer->end()); 
        out.resize(in.size() * 6); // max upsampling would be from 8k -> 48k

        spx_uint32_t in_len = in.size();
        spx_uint32_t out_len = out.size();

        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Resampling %u samples into a buffer that can hold %u samples\n", in.size(), out_len);

        speex_resampler_process_interleaved_int(tech_pvt->bidirectional_audio_resampler, in.data(), &in_len, out.data(), &out_len);

        // Resize the output buffer to match the output length from resampler
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Resizing output buffer from %u to %u samples\n", in.size(), out_len);

        out.resize(out_len);
      }
      else {
        // If no resampling is needed, copy the data from the prebuffer to the output buffer
        out.assign(cBuffer->begin(), cBuffer->end());
      }
    } catch (const std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error resampling incoming binary message: %s\n", e.what());
      return SWITCH_STATUS_FALSE;
    } catch (...) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error resampling incoming binary message\n");
      return SWITCH_STATUS_FALSE;
    }

    if (nullptr != tech_pvt->mutex && switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      CircularBuffer_t *playoutBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;

      try {
        // Resize the buffer if necessary
        if (playoutBuffer->capacity() - playoutBuffer->size() < out.size()) {
          size_t newCapacity = playoutBuffer->size() + std::max(out.size(), (size_t)BUFFER_GROW_SIZE);
          playoutBuffer->set_capacity(newCapacity);
          //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Resized playout buffer to new capacity: %zu\n", newCapacity);
        }
        // Push the data into the buffer.
        playoutBuffer->insert(playoutBuffer->end(), out.begin(), out.end());
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Appended %zu 16-bit samples to the playout buffer.\n", out.size());
      } catch (const std::exception& e) {
        switch_mutex_unlock(tech_pvt->mutex);
        cBuffer->clear();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error processing incoming binary message: %s\n", e.what());
        return SWITCH_STATUS_FALSE;
      } catch (...) {
        switch_mutex_unlock(tech_pvt->mutex);
        cBuffer->clear();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error processing incoming binary message\n");
        return SWITCH_STATUS_FALSE;
      }
      switch_mutex_unlock(tech_pvt->mutex);
      cBuffer->clear();

      // Put the leftover samples back in the prebuffer for the next time
      if (!leftoverSamples.empty()) {
          cBuffer->insert(cBuffer->end(), leftoverSamples.begin(), leftoverSamples.end());
          //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Put back %u leftover samples into the prebuffer.\n", leftoverSamples.size());
      }
      return SWITCH_STATUS_SUCCESS;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Failed to get mutext (temp)\n");

    return SWITCH_STATUS_SUCCESS;
  }

  void processIncomingMessage(private_t* tech_pvt, switch_core_session_t* session, const char* message) {
    std::string msg = message;
    std::string type;
    cJSON* json = parse_json(session, msg, type) ;
    if (json) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - received %s message %s\n", tech_pvt->id, type.c_str(), message);
      cJSON* jsonData = cJSON_GetObjectItem(json, "data");
      if (0 == type.compare("playAudio") &&
        // playAudio is enabled and there is no bidirectional audio from stream is enabled.
        tech_pvt->bidirectional_audio_enable &&
        !tech_pvt->bidirectional_audio_stream) {
        if (jsonData) {
          // dont send actual audio bytes in event message
          cJSON* jsonFile = NULL;
          cJSON* jsonAudio = cJSON_DetachItemFromObject(jsonData, "audioContent");
          int validAudio = (jsonAudio && NULL != jsonAudio->valuestring);

          const char* szAudioContentType = cJSON_GetObjectCstr(jsonData, "audioContentType");
          char fileType[6];
          int sampleRate = 16000;
          if (0 == strcmp(szAudioContentType, "raw")) {
            cJSON* jsonSR = cJSON_GetObjectItem(jsonData, "sampleRate");
            sampleRate = jsonSR && jsonSR->valueint ? jsonSR->valueint : 0;

            switch(sampleRate) {
              case 8000:
                strcpy(fileType, ".r8");
                break;
              case 16000:
                strcpy(fileType, ".r16");
                break;
              case 24000:
                strcpy(fileType, ".r24");
                break;
              case 32000:
                strcpy(fileType, ".r32");
                break;
              case 48000:
                strcpy(fileType, ".r48");
                break;
              case 64000:
                strcpy(fileType, ".r64");
                break;
              default:
                strcpy(fileType, ".r16");
                break;
            }
          }
          else if (0 == strcmp(szAudioContentType, "wave") || 0 == strcmp(szAudioContentType, "wav")) {
            strcpy(fileType, ".wav");
          }
          else {
            validAudio = 0;
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - unsupported audioContentType: %s\n", tech_pvt->id, szAudioContentType);
          }

          if (validAudio) {
            char szFilePath[256];

            std::string rawAudio = drachtio::base64_decode(jsonAudio->valuestring);
            switch_snprintf(szFilePath, 256, "%s%s%s_%d.tmp%s", SWITCH_GLOBAL_dirs.temp_dir, 
              SWITCH_PATH_SEPARATOR, tech_pvt->sessionId, playCount++, fileType);
            std::ofstream f(szFilePath, std::ofstream::binary);
            f << rawAudio;
            f.close();

            // add the file to the list of files played for this session, we'll delete when session closes
            struct playout* playout = (struct playout *) malloc(sizeof(struct playout));
            playout->file = (char *) malloc(strlen(szFilePath) + 1);
            strcpy(playout->file, szFilePath);
            playout->next = tech_pvt->playout;
            tech_pvt->playout = playout;

            jsonFile = cJSON_CreateString(szFilePath);
            cJSON_AddItemToObject(jsonData, "file", jsonFile);
          }

          char* jsonString = cJSON_PrintUnformatted(jsonData);
          tech_pvt->responseHandler(session, EVENT_PLAY_AUDIO, jsonString);
          free(jsonString);
          if (jsonAudio) cJSON_Delete(jsonAudio);
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%u) processIncomingMessage - missing data payload in playAudio request\n", tech_pvt->id); 
        }
      }
      else if (0 == type.compare("killAudio")) {
        tech_pvt->responseHandler(session, EVENT_KILL_AUDIO, NULL);

        // kill any current playback on the channel
        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_channel_set_flag_value(channel, CF_BREAK, 2);

        // this will dump buffered incoming audio
        tech_pvt->clear_bidirectional_audio_buffer = true;
      }
      else if (0 == type.compare("mark")) {
        cJSON* data = cJSON_GetObjectItem(json, "data");
        if (data) {
          cJSON* name = cJSON_GetObjectItem(data, "name");
          if (cJSON_IsString(name) && name->valuestring) {
            if (markCountExceeded(tech_pvt)) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "(%u) processIncomingMessage - mark count exceeded, discarding mark %s\n", tech_pvt->id, cJSON_GetStringValue(name));
            }
            else {
              if (nullptr == tech_pvt->pVecMarksInInventory) {
                tech_pvt->pVecMarksInInventory = static_cast<void *>(new std::deque<std::string>());
                tech_pvt->pVecMarksInUse = static_cast<void *>(new std::deque<std::string>());
                tech_pvt->pVecMarksCleared = static_cast<void *>(new std::deque<std::string>());
              }
              std::deque<std::string>* pVec = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
              pVec->push_back(name->valuestring);
            }
          }
        }
      }
      else if (0 == type.compare("clearMarks")) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - received clearMarks\n", tech_pvt->id);
        if (nullptr != tech_pvt->pVecMarksInInventory) {
          std::deque<std::string>* pVecMarksInInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
          std::deque<std::string>* pVecMarksInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
          std::deque<std::string>* pVecMarksCleared = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksCleared);
          pVecMarksCleared->insert(pVecMarksCleared->end(), pVecMarksInUse->begin(), pVecMarksInUse->end());
          pVecMarksCleared->insert(pVecMarksCleared->end(), pVecMarksInInventory->begin(), pVecMarksInInventory->end());
          pVecMarksInInventory->clear();
          pVecMarksInUse->clear();
        }
      }
      else if (0 == type.compare("transcription")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_TRANSCRIPTION, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("transfer")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_TRANSFER, jsonString);
        free(jsonString);                
      }
      else if (0 == type.compare("disconnect")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_DISCONNECT, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("error")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_ERROR, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("json")) {
        char* jsonString = cJSON_PrintUnformatted(json);
        tech_pvt->responseHandler(session, EVENT_JSON, jsonString);
        free(jsonString);
      }
      else if (0 == type.compare("transcription")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_TRANSCRIPTION, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("transfer")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_TRANSFER, jsonString);
        free(jsonString);                
      }
      else if (0 == type.compare("disconnect")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_DISCONNECT, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("error")) {
        char* jsonString = cJSON_PrintUnformatted(jsonData);
        tech_pvt->responseHandler(session, EVENT_ERROR, jsonString);
        free(jsonString);        
      }
      else if (0 == type.compare("json")) {
        char* jsonString = cJSON_PrintUnformatted(json);
        tech_pvt->responseHandler(session, EVENT_JSON, jsonString);
        free(jsonString);
      }
      else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%u) processIncomingMessage - unsupported msg type %s\n", tech_pvt->id, type.c_str());  
      }
      cJSON_Delete(json);
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) processIncomingMessage - could not parse message: %s\n", tech_pvt->id, message);
    }
  }

  static void eventCallback(const char* sessionId, const char* bugname, drachtio::AudioPipe::NotifyEvent_t event, const char* message, const char* binary, size_t len) {
    switch_core_session_t* session = switch_core_session_locate(sessionId);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (tech_pvt) {
          switch (event) {
            case drachtio::AudioPipe::CONNECT_SUCCESS:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "connection successful\n");
              tech_pvt->responseHandler(session, EVENT_CONNECT_SUCCESS, NULL);
              if (strlen(tech_pvt->initialMetadata) > 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "sending initial metadata %s\n", tech_pvt->initialMetadata);
                drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
                pAudioPipe->bufferForSending(tech_pvt->initialMetadata);
              }
            break;
            case drachtio::AudioPipe::CONNECT_FAIL:
            {
              // first thing: we can no longer access the AudioPipe
              std::stringstream json;
              json << "{\"reason\":\"" << message << "\"}";
              tech_pvt->pAudioPipe = nullptr;
              tech_pvt->responseHandler(session, EVENT_CONNECT_FAIL, (char *) json.str().c_str());
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection failed: %s\n", message);
            }
            break;
            case drachtio::AudioPipe::CONNECTION_DROPPED:
              // first thing: we can no longer access the AudioPipe
              tech_pvt->pAudioPipe = nullptr;
              tech_pvt->responseHandler(session, EVENT_DISCONNECT, NULL);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection dropped from far end\n");
            break;
            case drachtio::AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
              // first thing: we can no longer access the AudioPipe
              tech_pvt->pAudioPipe = nullptr;
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection closed gracefully\n");
            break;
            case drachtio::AudioPipe::MESSAGE:
              processIncomingMessage(tech_pvt, session, message);
            break;
            case drachtio::AudioPipe::BINARY:
            processIncomingBinary(tech_pvt, session, binary, len);
            break;
          }
        }
      }
      switch_core_session_rwunlock(session);
    }
  }
  switch_status_t fork_data_init(private_t *tech_pvt, switch_core_session_t *session, char * host, 
    unsigned int port, char* path, int sslFlags, int sampling, int desiredSampling, int channels, 
    char *bugname, char* metadata, int bidirectional_audio_enable,
    int bidirectional_audio_stream, int bidirectional_audio_sample_rate, responseHandler_t responseHandler) {

    const char* username = nullptr;
    const char* password = nullptr;
    int err;
    int bidirectional_audio_stream_enable = bidirectional_audio_enable + bidirectional_audio_stream;
    switch_codec_implementation_t read_impl;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_core_session_get_read_impl(session, &read_impl);
  
    if (username = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_USERNAME")) {
      password = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_PASSWORD");
    }

    memset(tech_pvt, 0, sizeof(private_t));
  
    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
    strncpy(tech_pvt->host, host, MAX_WS_URL_LEN);
    tech_pvt->port = port;
    strncpy(tech_pvt->path, path, MAX_PATH_LEN);    
    tech_pvt->sampling = desiredSampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->playout = NULL;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    tech_pvt->buffer_overrun_notified = 0;
    tech_pvt->audio_paused = 0;
    tech_pvt->graceful_shutdown = 0;
    tech_pvt->streamingPlayoutBuffer = (void *) new CircularBuffer_t(8192);
    tech_pvt->bidirectional_audio_enable = bidirectional_audio_enable;
    tech_pvt->bidirectional_audio_stream = bidirectional_audio_stream;
    tech_pvt->bidirectional_audio_sample_rate = bidirectional_audio_sample_rate;
    tech_pvt->clear_bidirectional_audio_buffer = false;
    tech_pvt->has_set_aside_byte = 0;
    tech_pvt->downscale_factor = 1;
    if (bidirectional_audio_sample_rate > sampling) {
      tech_pvt->downscale_factor = bidirectional_audio_sample_rate / sampling;
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "downscale_factor is %d\n", tech_pvt->downscale_factor);
    }
    tech_pvt->streamingPreBufSize = 320 * tech_pvt->downscale_factor * 4; // min 80ms prebuffer
    tech_pvt->streamingPreBuffer = (void *) new CircularBuffer_t(8192);
    tech_pvt->pVecMarksInInventory = nullptr;
    tech_pvt->pVecMarksInUse = nullptr;
    tech_pvt->pVecMarksCleared = nullptr;

    strncpy(tech_pvt->bugname, bugname, MAX_BUG_LEN);
    if (metadata) strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN);
    
    size_t buflen = LWS_PRE + (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);

    drachtio::AudioPipe* ap = new drachtio::AudioPipe(tech_pvt->sessionId, host, port, path, sslFlags, 
      buflen, read_impl.decoded_bytes_per_packet, username, password, bugname, bidirectional_audio_stream_enable, eventCallback);
    if (!ap) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error allocating AudioPipe\n");
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->pAudioPipe = static_cast<void *>(ap);

    switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

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

    if (bidirectional_audio_sample_rate && sampling != bidirectional_audio_sample_rate) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) bidirectional audio resampling from %u to %u, channels %d\n", tech_pvt->id, bidirectional_audio_sample_rate, sampling, channels);
      tech_pvt->bidirectional_audio_resampler = speex_resampler_init(1, bidirectional_audio_sample_rate, sampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing bidirectional audio resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_data_init\n", tech_pvt->id);

    return SWITCH_STATUS_SUCCESS;
  }

  void destroy_tech_pvt(private_t* tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    if (tech_pvt->resampler) {
      speex_resampler_destroy(tech_pvt->resampler);
      tech_pvt->resampler = nullptr;
    }
    if (tech_pvt->bidirectional_audio_resampler) {
      speex_resampler_destroy(tech_pvt->bidirectional_audio_resampler);
      tech_pvt->bidirectional_audio_resampler = nullptr;
    }
    if (tech_pvt->mutex) {
      switch_mutex_destroy(tech_pvt->mutex);
      tech_pvt->mutex = nullptr;
    }
    if (tech_pvt->streamingPlayoutBuffer) {
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;
      delete cBuffer;
      tech_pvt->streamingPlayoutBuffer = nullptr;
    }
    if (tech_pvt->streamingPreBuffer) {
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPreBuffer;
      delete cBuffer;
      tech_pvt->streamingPreBuffer = nullptr;
    }

    if (tech_pvt->pVecMarksInInventory) {
      delete static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
      tech_pvt->pVecMarksInInventory = nullptr;
      delete static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
      tech_pvt->pVecMarksInUse = nullptr;
      delete static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksCleared);
      tech_pvt->pVecMarksCleared = nullptr;
    }
  }

  static void send_mark_event(private_t* tech_pvt, const char* name, int cleared = false) {
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    std::ostringstream json;
    json << "{\"type\": \"mark\", \"data\": {\"name\":\"" << name << "\", ";
    if (cleared) json << "\"event\": \"cleared\"}}";
    else json << "\"event\": \"playout\"}}";

    if (pAudioPipe) {
      std::string str = json.str();
      pAudioPipe->bufferForSending(str.c_str());
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) send_mark_event: %s\n", tech_pvt->id, str.c_str());
    }
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
	  switch_log_printf(SWITCH_CHANNEL_LOG, llevel, "%s\n", line);
  }
}

extern "C" {
  int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags) {
    int i = 0, offset;
    char server[MAX_WS_URL_LEN + MAX_PATH_LEN];
    char *saveptr;
    int flags = LCCSCF_USE_SSL;
    
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_SELFSIGNED"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - allowing self-signed certs\n");
      flags |= LCCSCF_ALLOW_SELFSIGNED;
    }
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_SKIP_SERVER_CERT_HOSTNAME_CHECK"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - skipping hostname check\n");
      flags |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    }
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_EXPIRED"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - allowing expired certs\n");
      flags |= LCCSCF_ALLOW_EXPIRED;
    }

    // get the scheme
    strncpy(server, szServerUri, MAX_WS_URL_LEN + MAX_PATH_LEN);
    if (0 == strncmp(server, "https://", 8) || 0 == strncmp(server, "HTTPS://", 8)) {
      *pSslFlags = flags;
      offset = 8;
      *pPort = 443;
    }
    else if (0 == strncmp(server, "wss://", 6) || 0 == strncmp(server, "WSS://", 6)) {
      *pSslFlags = flags;
      offset = 6;
      *pPort = 443;
    }
    else if (0 == strncmp(server, "http://", 7) || 0 == strncmp(server, "HTTP://", 7)) {
      offset = 7;
      *pSslFlags = 0;
      *pPort = 80;
    }
    else if (0 == strncmp(server, "ws://", 5) || 0 == strncmp(server, "WS://", 5)) {
      offset = 5;
      *pSslFlags = 0;
      *pPort = 80;
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - error parsing uri %s: invalid scheme\n", szServerUri);;
      return 0;
    }

    std::string strHost(server + offset);
    //- `([^/:]+)` captures the hostname/IP address, match any character except in the set
    //- `:?([0-9]*)?` optionally captures a colon and the port number, if it's present.
    //- `(/.*)` captures everything else (the path).
    std::regex re("([^/:]+):?([0-9]*)?(/.*)?$");
    std::smatch matches;
    if(std::regex_search(strHost, matches, re)) {
      /*
      for (int i = 0; i < matches.length(); i++) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - %d: %s\n", i, matches[i].str().c_str());
      }
      */
      strncpy(host, matches[1].str().c_str(), MAX_WS_URL_LEN);
      if (matches[2].str().length() > 0) {
        *pPort = atoi(matches[2].str().c_str());
      }
      if (matches[3].str().length() > 0) {
        strncpy(path, matches[3].str().c_str(), MAX_PATH_LEN);
      }
      else {
        strcpy(path, "/");
      }
    } else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - invalid format %s\n", strHost.c_str());
      return 0;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - host %s, path %s\n", host, path);

    return 1;
  }

  switch_status_t fork_init() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: audio buffer (in secs):    %d secs\n", nAudioBufferSecs);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: sub-protocol:              %s\n", mySubProtocolName);
 
    //int logs = LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_PARSER | LLL_HEADER | LLL_EXT | LLL_CLIENT  | LLL_LATENCY | LLL_DEBUG ;
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE;
    drachtio::AudioPipe::initialize(mySubProtocolName, logs, lws_logger);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork successfully initialized\n");
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_cleanup() {
    bool cleanup = false;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork unloading..\n");

    cleanup = drachtio::AudioPipe::deinitialize();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork unloaded status %d\n", cleanup);
    if (cleanup == true) {
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }

  switch_status_t fork_session_init(switch_core_session_t *session, 
    responseHandler_t responseHandler,
    uint32_t samples_per_second, 
    char *host,
    unsigned int port,
    char *path,
    int sampling,
    int sslFlags,
    int channels,
    char *bugname,
    char* metadata,
    int bidirectional_audio_enable,
    int bidirectional_audio_stream,
    int bidirectional_audio_sample_rate,
    void **ppUserData
    )
  {
    int err;

    // allocate per-session data structure
    private_t* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
      return SWITCH_STATUS_FALSE;
    }

    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, host, port, path, sslFlags, samples_per_second, sampling, channels, 
      bugname, metadata, bidirectional_audio_enable, bidirectional_audio_stream, bidirectional_audio_sample_rate, responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;
    return SWITCH_STATUS_SUCCESS;
  }

   switch_status_t fork_session_connect(void **ppUserData) {
    private_t *tech_pvt = static_cast<private_t *>(*ppUserData);
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe*>(tech_pvt->pAudioPipe);
    pAudioPipe->connect();
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_cleanup(switch_core_session_t *session, char *bugname, char* text, int channelIsClosing) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "fork_session_cleanup: no bug %s - websocket conection already closed\n", bugname);
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    uint32_t id = tech_pvt->id;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_session_cleanup\n", id);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
      
    switch_mutex_lock(tech_pvt->mutex);

    // get the bug again, now that we are under lock
    {
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        switch_channel_set_private(channel, bugname, NULL);
        if (!channelIsClosing) {
          switch_core_media_bug_remove(session, &bug);
        }
      }
    }

    // delete any temp files
    struct playout* playout = tech_pvt->playout;
    while (playout) {
      std::remove(playout->file);
      free(playout->file);
      struct playout *tmp = playout;
      playout = playout->next;
      free(tmp);
    }

    if (pAudioPipe && text) pAudioPipe->bufferForSending(text);
    if (pAudioPipe) pAudioPipe->close();

    destroy_tech_pvt(tech_pvt);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%u) fork_session_cleanup: connection closed\n", id);
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_send_text(switch_core_session_t *session, char *bugname, char* text) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_send_text failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
  
    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe && text) pAudioPipe->bufferForSending(text);

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_pauseresume(switch_core_session_t *session, char *bugname, int pause) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_pauseresume failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
  
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_core_media_bug_flush(bug);
    tech_pvt->audio_paused = pause;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_graceful_shutdown(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_graceful_shutdown failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
  
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    tech_pvt->graceful_shutdown = 1;

    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe) pAudioPipe->do_graceful_shutdown();

    return SWITCH_STATUS_SUCCESS;
  }

  switch_bool_t fork_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    size_t inuse = 0;
    bool dirty = false;
    char *p = (char *) "{\"msg\": \"buffer overrun\"}";

    if (!tech_pvt || tech_pvt->audio_paused || tech_pvt->graceful_shutdown) return SWITCH_TRUE;
    
    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      if (!tech_pvt->pAudioPipe) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }
      drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
      if (pAudioPipe->getLwsState() != drachtio::AudioPipe::LWS_CLIENT_CONNECTED) {
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
              tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
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
                tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
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

  switch_bool_t dub_speech_frame(switch_media_bug_t *bug, private_t* tech_pvt) {
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;
    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {

      // if flag was set to clear the buffer, do so and clear the flag
      if (tech_pvt->clear_bidirectional_audio_buffer) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) dub_speech_frame - clearing buffer\n", tech_pvt->id); 
        cBuffer->clear();
        tech_pvt->clear_bidirectional_audio_buffer = false;

        // send "mark" event for any queued markers
        if (nullptr != tech_pvt->pVecMarksInInventory) {
          std::deque<std::string>* pVecMarksInInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
          std::deque<std::string>* pVecMarksInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
          if (pVecMarksInInventory->size() + pVecMarksInUse->size() > 0) {
            std::deque<std::string> vec = *pVecMarksInUse;
            vec.insert(vec.end(), pVecMarksInInventory->begin(), pVecMarksInInventory->end());
            for (auto it = vec.begin(); it != vec.end(); ++it) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "(%u) dub_speech_frame - Marker %s cleared\n",
                  tech_pvt->id, it->c_str());
              send_mark_event(tech_pvt, it->c_str(), true);
            }

            // put the "in-use" ones into the "cleared" queue so we dont notify again when they eventually come through
            std::deque<std::string>* pVecMarksCleared = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksCleared);
            pVecMarksCleared->insert(pVecMarksCleared->end(), pVecMarksInUse->begin(), pVecMarksInUse->end());

            pVecMarksInUse->clear();
            pVecMarksInInventory->clear();
          }
        }
      }
      else {
        switch_frame_t* rframe = switch_core_media_bug_get_write_replace_frame(bug);
        int16_t *fp = reinterpret_cast<int16_t*>(rframe->data);

        rframe->channels = 1;
        rframe->datalen = rframe->samples * sizeof(int16_t);

        int16_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        memset(data, 0, sizeof(data));

        int samplesToCopy = std::min(static_cast<int>(cBuffer->size()), static_cast<int>(rframe->samples));

        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) dub_speech_frame - samples to copy %u\n", tech_pvt->id, samplesToCopy); 

        bool hasMarkers = false;
        std::deque<std::string>* pVecInventory = nullptr;
        std::deque<std::string>* pVecInUse = nullptr;
        std::deque<std::string>* pVecCleared = nullptr;
        if (tech_pvt->pVecMarksInInventory && tech_pvt->pVecMarksInUse && tech_pvt->pVecMarksCleared) {
          pVecInventory = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInInventory);
          pVecInUse = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksInUse);
          pVecCleared = static_cast<std::deque<std::string>*>(tech_pvt->pVecMarksCleared);
          hasMarkers = pVecInUse->size() + pVecCleared->size() >  0;
        }

        if (hasMarkers) {
          /* discard markers and send notifications */
          auto bufferIter = cBuffer->begin();
          auto dataIter = data;
          for (int i = 0; i < samplesToCopy; ++i) {
            if (*bufferIter == AUDIO_MARKER) {
              // Marker detected, discard it and send a notice unless it was previously cleared
              auto * pVec = pVecCleared->size() > 0 ? pVecCleared : pVecInUse;
              if (!pVec->empty()) {
                auto name = pVec->front();
                pVec->pop_front();

                if (pVec == pVecInUse) {
                  send_mark_event(tech_pvt, name.c_str());
                  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) dub_speech_frame - Marker %s detected in playout\n",
                    tech_pvt->id, name.c_str());
                }
                else {
                  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) dub_speech_frame - Marker %s detected in playout but previously cleared\n",
                    tech_pvt->id, name.c_str());
                }
              }
            } else {
              // Copy valid audio samplewhat 
              *dataIter = *bufferIter;
              ++dataIter;
            }
            ++bufferIter;
          }

          // Remove the processed samples (including discarded markers) from the buffer
          cBuffer->erase(cBuffer->begin(), cBuffer->begin() + samplesToCopy);

          // Adjust the number of samples copied to the output frame
          int validSamplesCopied = std::distance(data, dataIter);

          if (validSamplesCopied > 0) {
            vector_add(fp, data, validSamplesCopied);
          }
        }
        else {
          std::copy_n(cBuffer->begin(), samplesToCopy, data);
          cBuffer->erase(cBuffer->begin(), cBuffer->begin() + samplesToCopy);

          if (samplesToCopy > 0) {
            vector_add(fp, data, rframe->samples);
          } else if (pVecInventory && pVecInventory->size()) {
            // no bidirectional audio to dub but still have some mark in inventory, send them now
            auto name = pVecInventory->front();
            pVecInventory->pop_front();

            send_mark_event(tech_pvt, name.c_str());
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%u) dub_speech_frame - Marker %s detected in inventory\n",
              tech_pvt->id, name.c_str());
          }
        }
        vector_normalize(fp, rframe->samples);

        switch_core_media_bug_set_write_replace_frame(bug, rframe);
      }
      switch_mutex_unlock(tech_pvt->mutex);
    }
    return SWITCH_TRUE;
  }

  switch_status_t fork_session_stop_play(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_stop_play failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;

    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      if (cBuffer != nullptr) {
        cBuffer->clear();
      }
      switch_mutex_unlock(tech_pvt->mutex);
    }
    return SWITCH_STATUS_SUCCESS;
  }

}

