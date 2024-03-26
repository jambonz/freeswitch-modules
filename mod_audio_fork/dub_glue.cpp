#include <switch.h>
#include "common.h"
#include "mod_audio_fork.h"
#include "vector_math.h"
#include "ap.h"
#include "track.h"
#include <queue>

#include <curl/curl.h>
#include <cstdlib>

#include <boost/circular_buffer.hpp>
#include <boost/thread.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/pool/object_pool.hpp>
#include <boost/bind/bind.hpp>
#include <boost/tokenizer.hpp>
#include <boost/foreach.hpp>
#include <boost/asio.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/algorithm/string.hpp>

#define BUFFER_GROW_SIZE (8192)
#define TRACK_NAME "mod_audio_fork_dub_track"

/* Global information, common to all connections */
typedef struct
{
  CURLM *multi;
  int still_running;
} GlobalInfo_t;
static GlobalInfo_t global;

/* Information associated with a specific easy handle */
typedef struct
{
  CURL *easy;
  GlobalInfo_t *global;
  char error[CURL_ERROR_SIZE];
  bool flushed;
} ConnInfo_t;

static boost::object_pool<ConnInfo_t> pool ;
static std::map<curl_socket_t, boost::asio::ip::tcp::socket *> socket_map;
static boost::asio::io_service io_service;
static boost::asio::deadline_timer timer(io_service);
static std::string fullDirPath;
static std::thread worker_thread;

extern "C" {

  /* module load and unload */
  switch_status_t dub_init(private_t *user_data, int sampleRate) {
    user_data->dub_track = new Track(TRACK_NAME, sampleRate);

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t dub_cleanup() {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t play_dub(private_t *user_data, char* url, int gain, int loop) {
    Track* track = static_cast<Track*>(user_data->dub_track);
    if (track) {
      track->queueHttpGetAudio(url, gain, loop);
    }
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t dub_session_cleanup(switch_core_session_t *session, int channelIsClosing, switch_media_bug_t *bug) {
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "dub_session_cleanup: conection already closed\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* user_data = (private_t*) switch_core_media_bug_get_user_data(bug);
    Track* track = static_cast<Track*>(user_data->dub_track);

    if (track) {
      track->removeAllAudio();
      delete track;
      user_data->dub_track = nullptr;
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "dub_session_cleanup: removed track %s\n", TRACK_NAME);
      return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }

  switch_bool_t dub_speech_frame(switch_media_bug_t *bug, private_t* user_data) {
    Track* track = static_cast<Track*>(user_data->dub_track);

    if (switch_mutex_trylock(user_data->mutex) == SWITCH_STATUS_SUCCESS) {
      if (track) {
        switch_frame_t* rframe = switch_core_media_bug_get_write_replace_frame(bug);
        int16_t *fp = reinterpret_cast<int16_t*>(rframe->data);

        rframe->channels = 1;
        rframe->datalen = rframe->samples * sizeof(int16_t);

        int16_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        memset(data, 0, sizeof(data));

        auto samples = track->retrieveAndClearAudio(data, rframe->samples);
        if (samples > 0) {
          vector_add(fp, data, rframe->samples);
        }

        vector_normalize(fp, rframe->samples);

        switch_core_media_bug_set_write_replace_frame(bug, rframe);
        switch_mutex_unlock(user_data->mutex);
      }
    }
    return SWITCH_TRUE;
  }
}