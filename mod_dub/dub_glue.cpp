#include "mod_dub.h"
#include "audio_downloader.h"
#include "tts_vendor_parser.h"
#include "file_loader.h"

#include <string>
#include <queue>

#include <switch.h>

#include <curl/curl.h>

#include <boost/circular_buffer.hpp>

typedef boost::circular_buffer<int16_t> CircularBuffer_t;
#define INIT_BUFFER_SIZE (80000)

extern "C" {

  void init_dub_track(dub_track_t *track, char* trackName, int sampleRate) {
    track->state = DUB_TRACK_STATE_READY;
    track->trackName = strdup(trackName);
    track->sampleRate = sampleRate;
    track->circularBuffer = new CircularBuffer_t(INIT_BUFFER_SIZE);
    track->req_queue = new request_queue_t;
  }

  switch_status_t silence_dub_track(dub_track_t *track) {
    assert(track);
    switch (track->generator) {
      case DUB_GENERATOR_TYPE_HTTP:
        stop_audio_download(track->generatorId);
        break;
      case DUB_GENERATOR_TYPE_FILE:
        stop_file_load(track->generatorId);
        break;
      case DUB_GENERATOR_TYPE_TTS:
        //TODO
        break;
    }
    CircularBuffer_t* buffer = reinterpret_cast<CircularBuffer_t*>(track->circularBuffer);
    buffer->clear();
    track->state = DUB_TRACK_STATE_READY;
    track->generator = DUB_GENERATOR_TYPE_UNKNOWN;
    track->generatorId = 0;

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t remove_dub_track(dub_track_t *track) {
    assert(track);
    switch (track->generator) {
      case DUB_GENERATOR_TYPE_HTTP:
        stop_audio_download(track->generatorId);
        break;
      case DUB_GENERATOR_TYPE_FILE:
        stop_file_load(track->generatorId);
        break;
      case DUB_GENERATOR_TYPE_TTS:
        //TODO
        break;
    }
    CircularBuffer_t* buffer = reinterpret_cast<CircularBuffer_t*>(track->circularBuffer);
    if (buffer) {
      delete buffer;
    }
    auto req_queue = static_cast<request_queue_t*>(track->req_queue);
    if (req_queue) {
      delete req_queue;
      track->req_queue = NULL;
    }
    if (track->trackName) {
      free(track->trackName);
    }
    memset(track, 0, sizeof(dub_track_t));

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t play_dub_track(dub_track_t *track, switch_mutex_t *mutex, char* url, int loop, int gain) {
    bool isHttp = strncmp(url, "http", 4) == 0;
    auto req_queue = static_cast<request_queue_t*>(track->req_queue);
    request_t payload;
    payload.url = url;
    if (track->state != DUB_TRACK_STATE_READY) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "play_dub_track: Audio is still playing, Put command into a queue\n");
      req_queue->push(payload);
      return SWITCH_STATUS_SUCCESS;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "play_dub_track: starting %s download: %s\n", (isHttp ? "HTTP" : "file"), url);

    int id = isHttp ?
      start_audio_download(&payload, track->sampleRate, loop, gain, mutex, (CircularBuffer_t*) track->circularBuffer, req_queue, &track->generator, &track->generatorId) :
      start_file_load(url, track->sampleRate, loop, gain, mutex, (CircularBuffer_t*) track->circularBuffer, req_queue, &track->generator, &track->generatorId);

    if (id == INVALID_DOWNLOAD_ID) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "play_dub_track: failed to start audio download\n");
      return SWITCH_STATUS_FALSE;
    }
    track->state = DUB_TRACK_STATE_ACTIVE;
    track->generatorId = id;
    track->generator = isHttp ? DUB_GENERATOR_TYPE_HTTP : DUB_GENERATOR_TYPE_FILE;
    track->gain = gain;

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t say_dub_track(dub_track_t *track, switch_mutex_t *mutex, char* text, int gain) {

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "say_dub_track: starting TTS\n");
    auto req_queue = static_cast<request_queue_t*>(track->req_queue);
    request_t payload;
    if (tts_vendor_parse_text(text, payload) != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "say_dub_track: failed to parse text\n");
      return SWITCH_STATUS_FALSE;
    }

    if (track->state != DUB_TRACK_STATE_READY) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "say_dub_track: TTS is still playing, Put command into a queue\n");
      req_queue->push(payload);
      return SWITCH_STATUS_SUCCESS;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "say_dub_track: starting HTTP download: %s\n", payload.url.c_str());
    int id = start_audio_download(&payload, track->sampleRate, 0/*loop*/,
      gain, mutex, (CircularBuffer_t*) track->circularBuffer, req_queue, &track->generator, &track->generatorId);
    
    track->state = DUB_TRACK_STATE_ACTIVE;
    track->generatorId = id;
    track->generator = DUB_GENERATOR_TYPE_HTTP;
    track->gain = gain;
    return SWITCH_STATUS_SUCCESS;
  }


  /* module load and unload */
  switch_status_t dub_init() {
    switch_status_t status;
    status = init_audio_downloader();
    if (status == SWITCH_STATUS_SUCCESS) {
      status = init_file_loader();
    }
    return status;
  }

  switch_status_t dub_cleanup() {
    switch_status_t status;
    status = deinit_audio_downloader();
    if (status == SWITCH_STATUS_SUCCESS) {
      status = deinit_file_loader();
    }
    return status;
  }

  switch_status_t dub_session_cleanup(switch_core_session_t *session, int channelIsClosing, switch_media_bug_t *bug) {
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (bug) {
      struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
      switch_mutex_lock(cb->mutex);

      if (!switch_channel_get_private(channel, MY_BUG_NAME)) {
        // race condition
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached (race).\n", switch_channel_get_name(channel));
        switch_mutex_unlock(cb->mutex);
        return SWITCH_STATUS_FALSE;
      }
      switch_channel_set_private(channel, MY_BUG_NAME, NULL);

      for (int i = 0; i < MAX_DUB_TRACKS; i++) {
        dub_track_t* track = &cb->tracks[i];
        if (track->state != DUB_TRACK_STATE_INACTIVE) {
    			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "dub_session_cleanup: cleared track %d:%s\n", i, track->trackName);
          remove_dub_track(track);
        }
      }

      if (!channelIsClosing) {
        switch_core_media_bug_remove(session, &bug);
      }

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "dub_session_cleanup: removed bug and cleared tracks\n");
			switch_mutex_unlock(cb->mutex);
    }
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s dub_session_cleanup: Bug is not attached.\n", switch_channel_get_name(channel));
    return SWITCH_STATUS_FALSE;
  }

  switch_bool_t dub_speech_frame(switch_media_bug_t *bug, void* user_data) {
    switch_core_session_t *session = switch_core_media_bug_get_session(bug);
    struct cap_cb *cb = (struct cap_cb *) user_data;

    if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {

      /* check if any tracks are actively pushing audio */
      int trackCount = 0;
      for (int i = 0; i < MAX_DUB_TRACKS; i++) {
        if (cb->tracks[i].state == DUB_TRACK_STATE_ACTIVE) trackCount++;
      }
      if (trackCount == 0 && cb->gain == 0) {
        switch_mutex_unlock(cb->mutex);
        return SWITCH_TRUE;
      }

      switch_frame_t* rframe = switch_core_media_bug_get_write_replace_frame(bug);
      int16_t *fp = reinterpret_cast<int16_t*>(rframe->data);

      rframe->channels = 1;
      rframe->datalen = rframe->samples * rframe->channels * sizeof(int16_t);

      /* apply gain to audio in main channel if requested*/
      if (cb->gain != 0) {
        switch_change_sln_volume_granular(fp, rframe->samples, cb->gain);
      }

      /* now mux in the data from tracks */
      for (int i = 0; i < rframe->samples; i++) {
        int16_t input = fp[i];
        int16_t value = input;
        for (int j = 0; j < MAX_DUB_TRACKS; j++) {
          dub_track_t* track = &cb->tracks[j];
          if (track->state == DUB_TRACK_STATE_ACTIVE) {
            CircularBuffer_t* buffer = reinterpret_cast<CircularBuffer_t*>(track->circularBuffer);
            if (buffer && !buffer->empty()) {
              int16_t sample = buffer->front();
              buffer->pop_front();
              value += sample;
            }
          }
        }
        switch_normalize_to_16bit(value);
        fp[i] = (int16_t) value;
      }
      switch_core_media_bug_set_write_replace_frame(bug, rframe);
      switch_mutex_unlock(cb->mutex);
    }
    return SWITCH_TRUE;
  }
}
