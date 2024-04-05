#include "mod_dub.h"
#include "tts_vendor_parser.h"
#include "track.h"
#include "vector_math.h"
#include <string>
#include <queue>

#include <switch.h>

#include <curl/curl.h>

#include <boost/circular_buffer.hpp>

typedef boost::circular_buffer<int16_t> CircularBuffer_t;
#define INIT_BUFFER_SIZE (80000)

extern "C" {

  Track* find_track_by_name(void** tracks, const std::string& trackName) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "find_track_by_name: searching for %s\n", trackName.c_str());

    for (int i = 0; i < MAX_DUB_TRACKS; i++) {
      Track* track = static_cast<Track*>(tracks[i]);
      std::string name = track ? track->getTrackName() : "null";
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "find_track_by_name: offset %d: %s\n", i, name.c_str());
      if (track && 0 == track->getTrackName().compare(trackName)) {
        return track;
      }
    }
    return nullptr;
  }

  switch_status_t add_track(struct cap_cb* cb, char* trackName, int sampleRate) {
    Track* existingTrack = find_track_by_name(cb->tracks, trackName);
    if (existingTrack) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "add_track: track %s already exists\n", trackName);
      return SWITCH_STATUS_FALSE;
    }

    for (int i = 0; i < MAX_DUB_TRACKS; i++) {
      if (!cb->tracks[i]) {
        cb->tracks[i] = new Track(trackName, sampleRate);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "add_track: added track %s at offset %d\n", trackName, i);
        return SWITCH_STATUS_SUCCESS;
      }
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "add_track: no room for track %s\n", trackName);
    return SWITCH_STATUS_FALSE;
  }

  switch_status_t silence_dub_track(struct cap_cb* cb, char* trackName) {
    Track* track = find_track_by_name(cb->tracks, trackName);
    if (track) {
      track->removeAllAudio();
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "silence_dub_track: silenced track %s\n", trackName);
      return SWITCH_STATUS_SUCCESS;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "silence_dub_track: track %s not found\n", trackName);
    return SWITCH_STATUS_FALSE;
  }

  switch_status_t remove_dub_track(struct cap_cb* cb, char* trackName) {
    for (int i = 0; i < MAX_DUB_TRACKS; i++) {
      Track* track = static_cast<Track*>(cb->tracks[i]);
      if (track && track->getTrackName() == trackName) {
        track->removeAllAudio();
        delete track;
        cb->tracks[i] = nullptr;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "remove_dub_track: removed track %s\n", trackName);
        return SWITCH_STATUS_SUCCESS;
      }
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "remove_dub_track: track %s not found\n", trackName);
    return SWITCH_STATUS_FALSE;
  }

  switch_status_t play_dub_track(struct cap_cb* cb, char* trackName, char* url, int loop, int gain) {
    bool isHttp = strncmp(url, "http", 4) == 0;
    Track* track = find_track_by_name(cb->tracks, trackName);

    if (!track) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "play_dub_track: track %s not found\n", trackName);
      return SWITCH_STATUS_FALSE;
    }

    if (isHttp) {
      track->queueHttpGetAudio(url, gain, loop);
    }
    else {
      track->queueFileAudio(url, gain, loop);
    }

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t say_dub_track(struct cap_cb* cb, char* trackName, char* text, int gain) {
    std::vector<std::string> headers;
    std::string url, body, proxy;
    Track* track = find_track_by_name(cb->tracks, trackName);

    if (!track) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "play_dub_track: track %s not found\n", trackName);
      return SWITCH_STATUS_FALSE;
    }
    if (tts_vendor_parse_text(text, url, body, headers, proxy) != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "say_dub_track: failed to parse text\n");
      return SWITCH_STATUS_FALSE;
    }
    track->queueHttpPostAudio(url, body, headers, proxy, gain);
    return SWITCH_STATUS_SUCCESS;
  }


  /* module load and unload */
  switch_status_t dub_init() {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t dub_cleanup() {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t dub_session_cleanup(switch_core_session_t *session, int channelIsClosing, switch_media_bug_t *bug) {
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (bug) {
      struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
      switch_mutex_lock(cb->mutex);

      if (!switch_channel_get_private(channel, MY_BUG_NAME)) {
        // race condition
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s Bug is not attached (race).\n", switch_channel_get_name(channel));
        switch_mutex_unlock(cb->mutex);
        return SWITCH_STATUS_FALSE;
      }
      switch_channel_set_private(channel, MY_BUG_NAME, NULL);

      for (int i = 0; i < MAX_DUB_TRACKS; i++) {
        Track* track = static_cast<Track*>(cb->tracks[i]);
        if (track) {
          track->removeAllAudio();
          delete track;
          cb->tracks[i] = nullptr;
        }
      }

      if (!channelIsClosing) {
        switch_core_media_bug_remove(session, &bug);
      }

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dub_session_cleanup: removed bug and cleared tracks\n");
			switch_mutex_unlock(cb->mutex);
    }
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s dub_session_cleanup: Bug is not attached.\n", switch_channel_get_name(channel));
    return SWITCH_STATUS_FALSE;
  }

  switch_bool_t dub_speech_frame(switch_media_bug_t *bug, void* user_data) {
    switch_core_session_t *session = switch_core_media_bug_get_session(bug);
    struct cap_cb *cb = (struct cap_cb *) user_data;

    if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {

      /* check if any tracks have audio to contribute */
      std::vector<Track*> activeTracks;
      activeTracks.reserve(MAX_DUB_TRACKS);
      for (int i = 0; i < MAX_DUB_TRACKS; i++) {
        if (cb->tracks[i]) {
          auto track = static_cast<Track*>(cb->tracks[i]);
          if (track->hasAudio_NoLock()) activeTracks.push_back(static_cast<Track*>(cb->tracks[i]));
        }
      }

      if (activeTracks.size() == 0 && cb->gain == 0) {
        switch_mutex_unlock(cb->mutex);
        return SWITCH_TRUE;
      }

      switch_frame_t* rframe = switch_core_media_bug_get_write_replace_frame(bug);
      int16_t *fp = reinterpret_cast<int16_t*>(rframe->data);

      rframe->channels = 1;
      rframe->datalen = rframe->samples * sizeof(int16_t);

      /* apply gain to audio in main channel if requested*/
      if (cb->gain != 0) {
        vector_change_sln_volume_granular(fp, rframe->samples, cb->gain);
      }

      /* now mux in the data from tracks */
      for (auto track : activeTracks) {
        int16_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        memset(data, 0, sizeof(data));
        auto samples = track->retrieveAndClearAudio(data, rframe->samples);
        if (samples > 0) {
          vector_add(fp, data, rframe->samples);
        }
      }
      vector_normalize(fp, rframe->samples);

      switch_core_media_bug_set_write_replace_frame(bug, rframe);
      switch_mutex_unlock(cb->mutex);
    }
    return SWITCH_TRUE;
  }
}
