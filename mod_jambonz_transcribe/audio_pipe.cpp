#include "audio_pipe.hpp"

#include <cassert>
#include <iostream>

/* discard incoming text messages over the socket that are longer than this */
#define MAX_RECV_BUF_SIZE (65 * 1024 * 10)
#define RECV_BUF_REALLOC_SIZE (8 * 1024)
#define MAX_API_KEY_LEN (8192)
using namespace jambonz;

namespace {
  static const char *requestedTcpKeepaliveSecs = std::getenv("MOD_AUDIO_FORK_TCP_KEEPALIVE_SECS");
  static int nTcpKeepaliveSecs = requestedTcpKeepaliveSecs ? ::atoi(requestedTcpKeepaliveSecs) : 55;
}

int AudioPipe::lws_callback(struct lws *wsi, 
  enum lws_callback_reasons reason,
  void *user, void *in, size_t len) {

  struct AudioPipe::lws_per_vhost_data *vhd = 
    (struct AudioPipe::lws_per_vhost_data *) lws_protocol_vh_priv_get(lws_get_vhost(wsi), lws_get_protocol(wsi));

  struct lws_vhost* vhost = lws_get_vhost(wsi);
  AudioPipe ** ppAp = (AudioPipe **) user;

  switch (reason) {
    case LWS_CALLBACK_PROTOCOL_INIT:
      vhd = (struct AudioPipe::lws_per_vhost_data *) lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi), lws_get_protocol(wsi), sizeof(struct AudioPipe::lws_per_vhost_data));
      vhd->context = lws_get_context(wsi);
      vhd->protocol = lws_get_protocol(wsi);
      vhd->vhost = lws_get_vhost(wsi);
      break;

    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
      {
        AudioPipe* ap = findPendingConnect(wsi);
        if (ap) {
          std::string apiKey = ap->getApiKey();
          unsigned char **p = (unsigned char **)in, *end = (*p) + len;
          char b[MAX_API_KEY_LEN];
          memset(b, 0, sizeof(b));
          strcpy(b,"Bearer ");
          strcpy(b + 7, apiKey.c_str());

          if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION, (unsigned char *)b, strlen(b), p, end)) return -1;
        }
      }
      break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      processPendingConnects(vhd);
      processPendingDisconnects(vhd);
      processPendingWrites();
      break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      {
        AudioPipe* ap = findAndRemovePendingConnect(wsi);
        int rc = lws_http_client_http_response(wsi);
        lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR: %s, response status %d\n", in ? (char *)in : "(null)", rc); 
        if (ap) {
          ap->m_state = LWS_CLIENT_FAILED;
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECT_FAIL, (char *) in, ap->isFinished());
        }
        else {
          lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR unable to find wsi %p..\n", wsi); 
        }
      }      
      break;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      {
        AudioPipe* ap = findAndRemovePendingConnect(wsi);
        if (ap) {
          *ppAp = ap;
          ap->m_vhd = vhd;
          ap->m_state = LWS_CLIENT_CONNECTED;
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECT_SUCCESS, NULL,  ap->isFinished());
        }
        else {
          lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_ESTABLISHED %s unable to find wsi %p..\n", ap->m_uuid.c_str(), wsi); 
        }
      }      
      break;
    case LWS_CALLBACK_CLIENT_CLOSED:
      {
        AudioPipe* ap = *ppAp;
        if (!ap) {
          lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CLOSED %s unable to find wsi %p..\n", ap->m_uuid.c_str(), wsi); 
          return 0;
        }
        if (ap->m_state == LWS_CLIENT_DISCONNECTING) {
          // closed by us

          lwsl_debug("%s socket closed by us\n", ap->m_uuid.c_str());
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECTION_CLOSED_GRACEFULLY, NULL,  ap->isFinished());
        }
        else if (ap->m_state == LWS_CLIENT_CONNECTED) {
          // closed by far end
          lwsl_info("%s socket closed by far end\n", ap->m_uuid.c_str());
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::CONNECTION_DROPPED, NULL,  ap->isFinished());
        }
        ap->m_state = LWS_CLIENT_DISCONNECTED;
        ap->setClosed();
    
        //NB: after receiving any of the events above, any holder of a 
        //pointer or reference to this object must treat is as no longer valid

        //*ppAp = NULL;
        //delete ap;
      }
      break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
      {
        AudioPipe* ap = *ppAp;
        if (!ap) {
          lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE %s unable to find wsi %p..\n", ap->m_uuid.c_str(), wsi); 
          return 0;
        }

        if (lws_frame_is_binary(wsi)) {
          lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE received binary frame, discarding.\n");
          return 0;
        }

        if (lws_is_first_fragment(wsi)) {
          // allocate a buffer for the entire chunk of memory needed
          assert(nullptr == ap->m_recv_buf);
          ap->m_recv_buf_len = len + lws_remaining_packet_payload(wsi);
          ap->m_recv_buf = (uint8_t*) malloc(ap->m_recv_buf_len);
          ap->m_recv_buf_ptr = ap->m_recv_buf;
        }

        size_t write_offset = ap->m_recv_buf_ptr - ap->m_recv_buf;
        size_t remaining_space = ap->m_recv_buf_len - write_offset;
        if (remaining_space < len) {
          lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE buffer realloc needed.\n");
          size_t newlen = ap->m_recv_buf_len + RECV_BUF_REALLOC_SIZE;
          if (newlen > MAX_RECV_BUF_SIZE) {
            free(ap->m_recv_buf);
            ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
            ap->m_recv_buf_len = 0;
            lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE max buffer exceeded, truncating message.\n");
          }
          else {
            ap->m_recv_buf = (uint8_t*) realloc(ap->m_recv_buf, newlen);
            if (nullptr != ap->m_recv_buf) {
              ap->m_recv_buf_len = newlen;
              ap->m_recv_buf_ptr = ap->m_recv_buf + write_offset;
            }
          }
        }

        if (nullptr != ap->m_recv_buf) {
          if (len > 0) {
            memcpy(ap->m_recv_buf_ptr, in, len);
            ap->m_recv_buf_ptr += len;
          }
          if (lws_is_final_fragment(wsi)) {
            if (nullptr != ap->m_recv_buf) {
              std::string msg((char *)ap->m_recv_buf, ap->m_recv_buf_ptr - ap->m_recv_buf);
              ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::MESSAGE, msg.c_str(),  ap->isFinished());
              if (nullptr != ap->m_recv_buf) free(ap->m_recv_buf);
            }
            ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
            ap->m_recv_buf_len = 0;
          }
        }
      }
      break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
      {
        AudioPipe* ap = *ppAp;
        if (!ap) {
          lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE %s unable to find wsi %p..\n", ap->m_uuid.c_str(), wsi); 
          return 0;
        }

        // check for text frames to send
        {
          std::lock_guard<std::mutex> lk(ap->m_text_mutex);
          if (ap->m_metadata.length() > 0) {
            uint8_t buf[ap->m_metadata.length() + LWS_PRE];
            memcpy(buf + LWS_PRE, ap->m_metadata.c_str(), ap->m_metadata.length());
            int n = ap->m_metadata.length();
            int m = lws_write(wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
            ap->m_metadata.clear();
            if (m < n) {
              return -1;
            }

            // there may be audio data, but only one write per writeable event
            // get it next time
            lws_callback_on_writable(wsi);

            return 0;
          }
        }

        if (ap->m_state == LWS_CLIENT_DISCONNECTING) {
          lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
          return -1;
        }

        // check for audio packets
        {
          std::lock_guard<std::mutex> lk(ap->m_audio_mutex);
          if (ap->m_audio_buffer_write_offset > LWS_PRE) {
            size_t datalen = ap->m_audio_buffer_write_offset - LWS_PRE;
            int sent = lws_write(wsi, (unsigned char *) ap->m_audio_buffer + LWS_PRE, datalen, LWS_WRITE_BINARY);
            if (sent < datalen) {
              lwsl_err("AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE %s attemped to send %lu only sent %d wsi %p..\n", 
                ap->m_uuid.c_str(), datalen, sent, wsi); 
            }
            ap->m_audio_buffer_write_offset = LWS_PRE;
          }
        }

        return 0;
      }
      break;

    default:
      break;
  }
  return lws_callback_http_dummy(wsi, reason, user, in, len);
}


struct lws_context *AudioPipe::context = nullptr;
std::thread AudioPipe::serviceThread;
std::mutex AudioPipe::mutex_connects;
std::mutex AudioPipe::mutex_disconnects;
std::mutex AudioPipe::mutex_writes;
std::list<AudioPipe*> AudioPipe::pendingConnects;
std::list<AudioPipe*> AudioPipe::pendingDisconnects;
std::list<AudioPipe*> AudioPipe::pendingWrites;
AudioPipe::log_emit_function AudioPipe::logger;
std::mutex AudioPipe::mapMutex;
bool AudioPipe::stopFlag;

void AudioPipe::processPendingConnects(lws_per_vhost_data *vhd) {
  std::list<AudioPipe*> connects;
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    for (auto it = pendingConnects.begin(); it != pendingConnects.end(); ++it) {
      if ((*it)->m_state == LWS_CLIENT_IDLE) {
        connects.push_back(*it);
        (*it)->m_state = LWS_CLIENT_CONNECTING;
      }
    }
  }
  for (auto it = connects.begin(); it != connects.end(); ++it) {
    AudioPipe* ap = *it;
    ap->connect_client(vhd);   
  }
}

void AudioPipe::processPendingDisconnects(lws_per_vhost_data *vhd) {
  std::list<AudioPipe*> disconnects;
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    for (auto it = pendingDisconnects.begin(); it != pendingDisconnects.end(); ++it) {
      if ((*it)->m_state == LWS_CLIENT_DISCONNECTING) disconnects.push_back(*it);
    }
    pendingDisconnects.clear();
  }
  for (auto it = disconnects.begin(); it != disconnects.end(); ++it) {
    AudioPipe* ap = *it;
    lws_callback_on_writable(ap->m_wsi); 
  }
}

void AudioPipe::processPendingWrites() {
  std::list<AudioPipe*> writes;
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    for (auto it = pendingWrites.begin(); it != pendingWrites.end(); ++it) {
       if ((*it)->m_state == LWS_CLIENT_CONNECTED) writes.push_back(*it);
    }  
    pendingWrites.clear();
  }
  for (auto it = writes.begin(); it != writes.end(); ++it) {
    AudioPipe* ap = *it;
    lws_callback_on_writable(ap->m_wsi);
  }
}

AudioPipe* AudioPipe::findAndRemovePendingConnect(struct lws *wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);
  std::list<AudioPipe* > toRemove;

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap; ++it) {
    int state = (*it)->m_state;

    if ((*it)->m_wsi == nullptr)
      toRemove.push_back(*it);

    if ((state == LWS_CLIENT_CONNECTING) &&
      (*it)->m_wsi == wsi) ap = *it;
  }

  for (auto it = toRemove.begin(); it != toRemove.end(); ++it)
    pendingConnects.remove(*it);

  if (ap) {
    pendingConnects.remove(ap);
  }

  return ap;
}

AudioPipe* AudioPipe::findPendingConnect(struct lws *wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap; ++it) {
    int state = (*it)->m_state;
    if ((state == LWS_CLIENT_CONNECTING) &&
      (*it)->m_wsi == wsi) ap = *it;
  }
  return ap;
}

void AudioPipe::addPendingConnect(AudioPipe* ap) {
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    pendingConnects.push_back(ap);
    lwsl_debug("%s after adding connect there are %lu pending connects\n", 
      ap->m_uuid.c_str(), pendingConnects.size());
  }
  lws_cancel_service(context);
}
void AudioPipe::addPendingDisconnect(AudioPipe* ap) {
  ap->m_state = LWS_CLIENT_DISCONNECTING;
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    pendingDisconnects.push_back(ap);
    lwsl_debug("%s after adding disconnect there are %lu pending disconnects\n", 
      ap->m_uuid.c_str(), pendingDisconnects.size());
  }
  lws_cancel_service(ap->m_vhd->context);
}
void AudioPipe::addPendingWrite(AudioPipe* ap) {
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    pendingWrites.push_back(ap);
  }
  lws_cancel_service(ap->m_vhd->context);
}

bool AudioPipe::lws_service_thread() {
  std::thread::id this_id = std::this_thread::get_id();
  struct lws_context_creation_info info;

  const struct lws_protocols protocols[] = {
    {
      "",
      AudioPipe::lws_callback,
      sizeof(void *),
      1024,
    },
    { NULL, NULL, 0, 0 }
  };

  uint16_t secs_sinceq_valid_ping = UINT16_MAX;
  uint16_t secs_since_valid_hangup = UINT16_MAX;

  char* wsVar = std::getenv("WS_PING_INTERVAL");
  if (wsVar != nullptr) {
    secs_sinceq_valid_ping = std::atoi(wsVar);
  }

  wsVar = std::getenv("WS_NO_PONG_HANGUP_INTERVAL");
  if (wsVar != nullptr) {
    secs_since_valid_hangup = std::atoi(wsVar);
  }

  const lws_retry_bo_t retry = {
    nullptr,                  // retry_ms_table
    0,                        // retry_ms_table_count
    0,                        // conceal_count
    secs_sinceq_valid_ping,   // secs_sinceq_valid_ping
    secs_since_valid_hangup,  // secs_since_valid_hangup
    0                         // jitter_percent
  };

  memset(&info, 0, sizeof info); 
  info.port = CONTEXT_PORT_NO_LISTEN; 
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  info.protocols = protocols;
  info.ka_time = nTcpKeepaliveSecs;                    // tcp keep-alive timer
  info.ka_probes = 4;                   // number of times to try ka before closing connection
  info.ka_interval = 5;                 // time between ka's
  info.timeout_secs = 10;                // doc says timeout for "various processes involving network roundtrips"
  info.keepalive_timeout = 5;           // seconds to allow remote client to hold on to an idle HTTP/1.1 connection 
  info.timeout_secs_ah_idle = 10;       // secs to allow a client to hold an ah without using it
  info.retry_and_idle_policy = &retry;

  lwsl_notice("AudioPipe::lws_service_thread creating context\n");

  context = lws_create_context(&info);
  if (!context) {
    lwsl_err("AudioPipe::lws_service_thread failed creating context\n"); 
    return false;
  }

  int n;
  do {
    n = lws_service(context, 0);
  } while (n >= 0 && !stopFlag);

  lwsl_notice("AudioPipe::lws_service_thread ending\n"); 
  lws_context_destroy(context);

  return true;
}

void AudioPipe::initialize(int loglevel, log_emit_function logger) {

  //lws_set_log_level(loglevel, logger);

  lwsl_notice("AudioPipe::initialize starting\n"); 
  std::lock_guard<std::mutex> lock(mapMutex);
  stopFlag = false;
  serviceThread = std::thread(&AudioPipe::lws_service_thread);
}

bool AudioPipe::deinitialize() {
  lwsl_notice("AudioPipe::deinitialize\n"); 
  std::lock_guard<std::mutex> lock(mapMutex);
  stopFlag = true;
  if (serviceThread.joinable()) {
    serviceThread.join();
  }
  return true;
}


// instance members
AudioPipe::AudioPipe(const char* uuid, const char* bugname, const char* host, unsigned int port, const char* path,
  int sslFlags, size_t bufLen, size_t minFreespace, const char* apiKey, notifyHandler_t callback) :
  m_uuid(uuid), m_bugname(bugname), m_host(host), m_port(port), m_path(path), m_sslFlags(sslFlags), m_finished(false),
  m_audio_buffer_min_freespace(minFreespace), m_audio_buffer_max_len(bufLen), m_gracefulShutdown(false),
  m_audio_buffer_write_offset(LWS_PRE), m_recv_buf(nullptr), m_recv_buf_ptr(nullptr), 
  m_state(LWS_CLIENT_IDLE), m_wsi(nullptr), m_vhd(nullptr), m_apiKey(apiKey), m_callback(callback) {

  m_audio_buffer = new uint8_t[m_audio_buffer_max_len];
}
AudioPipe::~AudioPipe() {
  if (m_audio_buffer) delete [] m_audio_buffer;
  if (m_recv_buf) delete [] m_recv_buf;
}

void AudioPipe::connect(void) {
  addPendingConnect(this);
}

bool AudioPipe::connect_client(struct lws_per_vhost_data *vhd) {
  assert(m_audio_buffer != nullptr);
  assert(m_vhd == nullptr);
  struct lws_client_connect_info i;

  memset(&i, 0, sizeof(i));
  i.context = vhd->context;
  i.port = m_port;
  i.address = m_host.c_str();
  i.path = m_path.c_str();
  i.host = i.address;
  i.origin = i.address;
  i.ssl_connection = m_sslFlags;
  i.pwsi = &(m_wsi);

  m_state = LWS_CLIENT_CONNECTING;
  m_vhd = vhd;

  m_wsi = lws_client_connect_via_info(&i);
  lwsl_debug("%s attempting connection, wsi is %p\n", m_uuid.c_str(), m_wsi);

  return nullptr != m_wsi;
}

void AudioPipe::bufferForSending(const char* text) {
  if (m_state != LWS_CLIENT_CONNECTED) return;
  {
    std::lock_guard<std::mutex> lk(m_text_mutex);
    m_metadata.append(text);
  }
  addPendingWrite(this);
}

void AudioPipe::unlockAudioBuffer() {
  if (m_audio_buffer_write_offset > LWS_PRE) addPendingWrite(this);
  m_audio_mutex.unlock();
}

void AudioPipe::close() {
  if (m_state != LWS_CLIENT_CONNECTED) return;
  addPendingDisconnect(this);
}

void AudioPipe::finish() {
  if (m_finished || m_state != LWS_CLIENT_CONNECTED) return;
  m_finished = true;
  bufferForSending("{\"type\": \"stop\"}");
}

void AudioPipe::waitForClose() {
  std::shared_future<void> sf(m_promise.get_future());
  sf.wait();
  return;
}
