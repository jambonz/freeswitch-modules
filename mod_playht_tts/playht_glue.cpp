#include "mod_playht_tts.h"
#include <switch.h>
#include <switch_json.h>
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

#include "mpg123.h"

#define BUFFER_GROW_SIZE (8192)
#define MP3_DCACHE 8192 * 2

typedef boost::circular_buffer<uint16_t> CircularBuffer_t;
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
  playht_t* playht;
  char* body;
  struct curl_slist *hdr_list;
  GlobalInfo_t *global;
  mpg123_handle *mh;
  char error[CURL_ERROR_SIZE];
  FILE* file;
  std::chrono::time_point<std::chrono::high_resolution_clock> startTime;
  bool flushed;
} ConnInfo_t;


static boost::object_pool<ConnInfo_t> pool ;
static std::map<curl_socket_t, boost::asio::ip::tcp::socket *> socket_map;
static boost::asio::io_service io_service;
static boost::asio::deadline_timer timer(io_service);
static std::string fullDirPath;
static std::thread worker_thread;

std::string secondsToMillisecondsString(double seconds) {
    // Convert to milliseconds
    double milliseconds = seconds * 1000.0;

    // Truncate to remove fractional part
    long milliseconds_long = static_cast<long>(milliseconds);

    // Convert to string
    return std::to_string(milliseconds_long);
}

static std::string getEnvVar(const std::string& varName) {
    const char* val = std::getenv(varName.c_str());
    return val ? std::string(val) : "";
}

static long getConnectionTimeout() {
    std::string connectTimeoutStr = getEnvVar("PLAYHT_TTS_CURL_CONNECT_TIMEOUT");

    if (connectTimeoutStr.empty()) {
        connectTimeoutStr = getEnvVar("TTS_CURL_CONNECT_TIMEOUT");
    }

    long connectTimeout = 20L;

    if (!connectTimeoutStr.empty()) {
        connectTimeout = std::stol(connectTimeoutStr);
    }

    return connectTimeout;
}

static CURL* createEasyHandle(void) {
  CURL* easy = curl_easy_init();
  if(!easy) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "curl_easy_init() failed!\n");
    return nullptr ;
  }

  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_USERAGENT, "jambonz/0.8.5");

  // set connect timeout to 3 seconds and total timeout to 109 seconds
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  //For long text, PlayHT took more than 20 seconds to complete the download.
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, getConnectionTimeout());

  return easy ;
}

static void cleanupConn(ConnInfo_t *conn) {
  auto p = conn->playht;

  if (conn->mh) {
    mpg123_close(conn->mh);
    mpg123_delete(conn->mh);
  }

  if( conn->hdr_list ) {
    curl_slist_free_all(conn->hdr_list);
    conn->hdr_list = nullptr ;
  }
  curl_easy_cleanup(conn->easy);

  if (conn->file) {
    if (fclose(conn->file) != 0) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "cleanupConn: error closing audio cache file\n");
    }
    conn->file = nullptr ;
  }

  p->conn = nullptr ;
  p->draining = 1;

  memset(conn, 0, sizeof(ConnInfo_t));
  pool.destroy(conn) ;
}

/* Check for completed transfers, and remove their easy handles */
void check_multi_info(GlobalInfo_t *g) {
  CURLMsg *msg;
  int msgs_left;
  ConnInfo_t *conn;
  CURL *easy;
  CURLcode res;

  while((msg = curl_multi_info_read(g->multi, &msgs_left))) {
    if(msg->msg == CURLMSG_DONE) {
      long response_code;
      double namelookup=0, connect=0, total=0 ;
      char *ct = NULL ;

      easy = msg->easy_handle;
      res = msg->data.result;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &conn);
      curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code);
      curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &ct);

      curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME, &namelookup);
      curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &connect);
      curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &total);

      auto p = conn->playht;
      p->response_code = response_code;
      if (ct) p->ct = strdup(ct);

      std::string name_lookup_ms = secondsToMillisecondsString(namelookup);
      std::string connect_ms = secondsToMillisecondsString(connect);
      std::string final_response_time_ms = secondsToMillisecondsString(total);

      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
        "mod_playht_tts: response: %ld, content-type %s,"
        "dns(ms): %"  CURL_FORMAT_CURL_OFF_T ".%06ld, "
        "connect(ms): %"  CURL_FORMAT_CURL_OFF_T ".%06ld, "
        "total(ms): %"  CURL_FORMAT_CURL_OFF_T ".%06ld\n",
        response_code, ct,
        (long)(namelookup), (long)(fmod(namelookup, 1.0) * 1000000),
        (long)(connect), (long)(fmod(connect, 1.0) * 1000000),
        (long)(total), (long)(fmod(total, 1.0) * 1000000));

      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "name lookup time: %s\n", name_lookup_ms.c_str());
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "connect time: %s\n", connect_ms.c_str());
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "final response time: %s\n", final_response_time_ms.c_str());

      p->name_lookup_time_ms = strdup(name_lookup_ms.c_str());
      p->connect_time_ms = strdup(connect_ms.c_str());
      p->final_response_time_ms = strdup(final_response_time_ms.c_str());

      curl_multi_remove_handle(g->multi, easy);
      cleanupConn(conn);
    }
  }
}

int mcode_test(const char *where, CURLMcode code) {
  if(CURLM_OK != code) {
    const char *s;
    switch(code) {
    case CURLM_CALL_MULTI_PERFORM:
      s = "CURLM_CALL_MULTI_PERFORM";
      break;
    case CURLM_BAD_HANDLE:
      s = "CURLM_BAD_HANDLE";
      break;
    case CURLM_BAD_EASY_HANDLE:
      s = "CURLM_BAD_EASY_HANDLE";
      break;
    case CURLM_OUT_OF_MEMORY:
      s = "CURLM_OUT_OF_MEMORY";
      break;
    case CURLM_INTERNAL_ERROR:
      s = "CURLM_INTERNAL_ERROR";
      break;
    case CURLM_UNKNOWN_OPTION:
      s = "CURLM_UNKNOWN_OPTION";
      break;
    case CURLM_LAST:
      s = "CURLM_LAST";
      break;
    default:
      s = "CURLM_unknown";
      break;
    case CURLM_BAD_SOCKET:
      s = "CURLM_BAD_SOCKET";
      break;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mcode_test ERROR: %s returns %s:%d\n", where, s, code);

    return -1;
  }
  return 0 ;
}

static void remsock(int *f, GlobalInfo_t *g) {
  if(f) {
    free(f);
    f = NULL;
  }
}

/* Called by asio when there is an action on a socket */
static void event_cb(GlobalInfo_t *g, curl_socket_t s, int action, const boost::system::error_code & error, int *fdp) {
  int f = *fdp;

  //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "event_cb socket %#X has action %d\n", s, action) ;

  // Socket already POOL REMOVED.
  if (f == CURL_POLL_REMOVE) {
    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "event_cb socket %#X removed\n", s);
    remsock(fdp, g);
    return;
  }

  if(socket_map.find(s) == socket_map.end()) {
    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "event_cb: socket  %#X already closed\n, s");
    return;
  }

  /* make sure the event matches what are wanted */
  if(f == action || f == CURL_POLL_INOUT) {
    if(error) {
      action = CURL_CSELECT_ERR;
    }
    CURLMcode rc = curl_multi_socket_action(g->multi, s, action, &g->still_running);

    mcode_test("event_cb: curl_multi_socket_action", rc);
    check_multi_info(g);

    if(g->still_running <= 0) {
      timer.cancel();
    }

    /* keep on watching.
      * the socket may have been closed and/or fdp may have been changed
      * in curl_multi_socket_action(), so check them both */
    if(!error && socket_map.find(s) != socket_map.end() &&
        (f == action || f == CURL_POLL_INOUT)) {
      boost::asio::ip::tcp::socket *tcp_socket = socket_map.find(s)->second;

      if(action == CURL_POLL_IN) {
        tcp_socket->async_read_some(boost::asio::null_buffers(),
                                    boost::bind(&event_cb, g, s,
                                                action, boost::placeholders::_1, fdp));
      }
      if(action == CURL_POLL_OUT) {
        tcp_socket->async_write_some(boost::asio::null_buffers(),
                                      boost::bind(&event_cb, g, s,
                                                  action, boost::placeholders::_1, fdp));
      }
    }
  }
}

/* socket functions */
static void setsock(int *fdp, curl_socket_t s, CURL *e, int act, int oldact, GlobalInfo_t *g) {
  std::map<curl_socket_t, boost::asio::ip::tcp::socket *>::iterator it = socket_map.find(s);

  if(it == socket_map.end()) {
    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "setsock: socket  %#X not found\n, s");
    return;
  }

  boost::asio::ip::tcp::socket * tcp_socket = it->second;

  *fdp = act;

  if(act == CURL_POLL_IN) {
    if(oldact != CURL_POLL_IN && oldact != CURL_POLL_INOUT) {
      tcp_socket->async_read_some(boost::asio::null_buffers(),
                                  boost::bind(&event_cb, g, s,
                                  CURL_POLL_IN, boost::placeholders::_1, fdp));
    }
  }
  else if(act == CURL_POLL_OUT) {
    if(oldact != CURL_POLL_OUT && oldact != CURL_POLL_INOUT) {
      tcp_socket->async_write_some(boost::asio::null_buffers(),
                                    boost::bind(&event_cb, g, s,
                                    CURL_POLL_OUT, boost::placeholders::_1, fdp));
    }
  }
  else if(act == CURL_POLL_INOUT) {
    if(oldact != CURL_POLL_IN && oldact != CURL_POLL_INOUT) {
      tcp_socket->async_read_some(boost::asio::null_buffers(),
                                  boost::bind(&event_cb, g, s,
                                  CURL_POLL_IN, boost::placeholders::_1, fdp));
    }
    if(oldact != CURL_POLL_OUT && oldact != CURL_POLL_INOUT) {
      tcp_socket->async_write_some(boost::asio::null_buffers(),
                                    boost::bind(&event_cb, g, s,
                                    CURL_POLL_OUT, boost::placeholders::_1, fdp));
    }
  }
}

static void addsock(curl_socket_t s, CURL *easy, int action, GlobalInfo_t *g) {
  /* fdp is used to store current action */
  int *fdp = (int *) calloc(sizeof(int), 1);

  setsock(fdp, s, easy, action, 0, g);
  curl_multi_assign(g->multi, s, fdp);
}

static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp) {
  GlobalInfo_t *g = &global;

  int *actionp = (int *) sockp;
  static const char *whatstr[] = { "none", "IN", "OUT", "INOUT", "REMOVE"};

  if(what == CURL_POLL_REMOVE) {
    *actionp = what;
  }
  else {
    if(!actionp) {
      addsock(s, e, what, g);
    }
    else {
      setsock(actionp, s, e, what, *actionp, g);
    }
  }
  return 0;
}

static void threadFunc() {
  /* to make sure the event loop doesn't terminate when there is no work to do */
  io_service.reset() ;
  boost::asio::io_service::work work(io_service);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_playht_tts threadFunc - starting\n");

  for(;;) {

    try {
      io_service.run() ;
      break ;
    }
    catch( std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_playht_tts threadFunc - Error: %s\n", e.what());
    }
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_playht_tts threadFunc - ending\n");
}


/* Called by asio when our timeout expires */
static void timer_cb(const boost::system::error_code & error, GlobalInfo_t *g)
{
  //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "timer_cb\n");

  if(!error) {
    CURLMcode rc = curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->still_running);
    mcode_test("timer_cb: curl_multi_socket_action", rc);
    check_multi_info(g);
  }
}

int multi_timer_cb(CURLM *multi, long timeout_ms, GlobalInfo_t *g) {

  /* cancel running timer */
  timer.cancel();

  if(timeout_ms >= 0) {
    // from libcurl 7.88.1-10+deb12u4 does not allow call curl_multi_socket_action or curl_multi_perform in curl_multi callback directly
    timer.expires_from_now(boost::posix_time::millisec(timeout_ms ? timeout_ms : 1));
    timer.async_wait(boost::bind(&timer_cb, boost::placeholders::_1, g));
  }

  return 0;
}

static std::vector<uint16_t> convert_mp3_to_linear(ConnInfo_t *conn, uint8_t *data, size_t len) {
  std::vector<uint16_t> linear_data;
  int eof = 0;
  int mp3err = 0;
  unsigned char decode_buf[MP3_DCACHE];

  if(mpg123_feed(conn->mh, data, len) == MPG123_OK) {
    while(!eof) {
      size_t usedlen = 0;
      off_t frame_offset;
      unsigned char* audio;

      int decode_status = mpg123_decode_frame(conn->mh, &frame_offset, &audio, &usedlen);

      switch(decode_status) {
        case MPG123_NEW_FORMAT:
          continue;

        case MPG123_OK:
          for(size_t i = 0; i < usedlen; i += 2) {
            uint16_t value = reinterpret_cast<uint16_t*>(audio)[i / 2];
            linear_data.push_back(value);
          }
          break;

        case MPG123_DONE:
        case MPG123_NEED_MORE:
          eof = 1;
          break;

        case MPG123_ERR:
        default:
          if(++mp3err >= 5) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Decoder Error!\n");
            eof = 1;
          }
      }

      if (eof)
        break;

      mp3err = 0;
    }
  }

  return linear_data;
}
/* CURLOPT_WRITEFUNCTION */
static size_t write_cb(void *ptr, size_t size, size_t nmemb, ConnInfo_t *conn) {
  bool fireEvent = false;
  uint8_t *data = (uint8_t *) ptr;
  size_t bytes_received = size * nmemb;
  auto p = conn->playht;
  CircularBuffer_t *cBuffer = (CircularBuffer_t *) p->circularBuffer;
  std::vector<uint16_t> pcm_data;

  if (conn->flushed || cBuffer == nullptr) {
    /* this will abort the transfer */
    return 0;
  }
  {
    switch_mutex_lock(p->mutex);

    if (p->response_code > 0 && p->response_code != 200) {
      std::string body((char *) ptr, bytes_received);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: received body %s\n", body.c_str());
      p->err_msg = strdup(body.c_str());
      switch_mutex_unlock(p->mutex);
      return 0;
    }

    pcm_data = convert_mp3_to_linear(conn, data, bytes_received);
    size_t bytesResampled = pcm_data.size() * sizeof(uint16_t);

    /* cache same data to avoid streaming and cached audio quality is different*/
    if (conn->file) fwrite(pcm_data.data(), sizeof(uint8_t), bytesResampled, conn->file);

    // Resize the buffer if necessary
    if (cBuffer->capacity() - cBuffer->size() < (bytesResampled / sizeof(uint16_t))) {
      //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "write_cb growing buffer\n");

      //TODO: if buffer exceeds some max size, return CURL_WRITEFUNC_ERROR to abort the transfer
      cBuffer->set_capacity(cBuffer->size() + std::max((bytesResampled / sizeof(uint16_t)), (size_t)BUFFER_GROW_SIZE));
    }

    /* Push the data into the buffer */
    cBuffer->insert(cBuffer->end(), pcm_data.data(), pcm_data.data() + pcm_data.size());

    if (0 == p->reads++) {
      fireEvent = true;
    }
    switch_mutex_unlock(p->mutex);
  }
  if (fireEvent && p->session_id) {
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - conn->startTime);
    auto time_to_first_byte_ms = std::to_string(duration.count());
    switch_core_session_t* session = switch_core_session_locate(p->session_id);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_core_session_rwunlock(session);
      if (channel) {
        switch_event_t *event;
        if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_START) == SWITCH_STATUS_SUCCESS) {
          switch_channel_event_set_data(channel, event);

          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "write_cb: firing playback-started\n");

          switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
          if (p->request_id) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_playht_request_id", p->request_id);
          }
          if (p->name_lookup_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_playht_name_lookup_time_ms", p->name_lookup_time_ms);
          }
          if (p->connect_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_playht_connect_time_ms", p->connect_time_ms);
          }
          if (p->final_response_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_playht_final_response_time_ms", p->final_response_time_ms);
          }
          if (p->voice_name) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_playht_voice_name", p->voice_name);
          }
          if (p->cache_filename) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", p->cache_filename);
          }

          switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_time_to_first_byte_ms", time_to_first_byte_ms.c_str());
          switch_event_fire(&event);
          p->playback_start_sent = 1;
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: failed to create event\n");
        }
      }
      else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: channel not found\n");
      }
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: session %s not found\n", p->session_id);
    }
  }
  return bytes_received;
}

static size_t url_callback(void *ptr, size_t size, size_t nitems, void *userdata) {
  std::string *buffer = static_cast<std::string*>(userdata);
  size_t bytes_received = size * nitems;
  buffer->append(static_cast<char*>(ptr), bytes_received);
  return bytes_received;
}

static bool parseHeader(const std::string& str, std::string& header, std::string& value) {
    std::vector<std::string> parts;
    boost::split(parts, str, boost::is_any_of(":"), boost::token_compress_on);

    if (parts.size() != 2)
        return false;

    header = boost::trim_copy(parts[0]);
    value = boost::trim_copy(parts[1]);
    return true;
}

static int extract_response_code(const std::string& input) {
  std::size_t space_pos = input.find(' ');
  if (space_pos == std::string::npos) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid HTTP response format %s\n", input.c_str());
    return 0;
  }

  std::size_t code_start_pos = space_pos + 1;
  std::size_t code_end_pos = input.find(' ', code_start_pos);
  if (code_end_pos == std::string::npos) {
    code_end_pos = input.length();
  }

  std::string code_str = input.substr(code_start_pos, code_end_pos - code_start_pos);
  int response_code = std::stoi(code_str);
  return response_code;
}

static size_t header_callback(char *buffer, size_t size, size_t nitems, ConnInfo_t *conn) {
  size_t bytes_received = size * nitems;
  const std::string prefix = "HTTP/";
  playht_t* p = conn->playht;
  std::string header, value;
  std::string input(buffer, bytes_received);
  if (parseHeader(input, header, value)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "recv header: %s with value %s\n", header.c_str(), value.c_str());
    if (0 == header.compare("x-job-ids")) p->request_id = strdup(value.c_str());
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "header_callback: %s\n", input.c_str());
    if (input.rfind(prefix, 0) == 0) {
      try {
        p->response_code = extract_response_code(input);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "header_callback: parsed response code: %ld\n", p->response_code);
      } catch (const std::invalid_argument& e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "header_callback: invalid response code %s\n", input.substr(prefix.length()).c_str());
      }
    }
  }
  return bytes_received;
}

/* CURLOPT_OPENSOCKETFUNCTION */
static curl_socket_t opensocket(void *clientp, curlsocktype purpose, struct curl_sockaddr *address) {
  curl_socket_t sockfd = CURL_SOCKET_BAD;

  //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "opensocket: %d\n", purpose);
  /* restrict to IPv4 */
  if(purpose == CURLSOCKTYPE_IPCXN && address->family == AF_INET) {
    /* create a tcp socket object */
    boost::asio::ip::tcp::socket *tcp_socket = new boost::asio::ip::tcp::socket(io_service);

    /* open it and get the native handle*/
    boost::system::error_code ec;
    tcp_socket->open(boost::asio::ip::tcp::v4(), ec);

    if(ec) {
      /* An error occurred */
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't open socket [%ld][%s]\n", ec, ec.message().c_str());
    }
    else {
      sockfd = tcp_socket->native_handle();

      /* save it for monitoring */
      socket_map.insert(std::pair<curl_socket_t, boost::asio::ip::tcp::socket *>(sockfd, tcp_socket));
    }
  }
  return sockfd;
}

/* CURLOPT_CLOSESOCKETFUNCTION */
static int close_socket(void *clientp, curl_socket_t item) {
  //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "close_socket : %#X\n", item);

  std::map<curl_socket_t, boost::asio::ip::tcp::socket *>::iterator it = socket_map.find(item);
  if(it != socket_map.end()) {
    delete it->second;
    socket_map.erase(it);
  }
  return 0;
}


extern "C" {
  switch_status_t playht_speech_load() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "playht_speech_loading..\n");
    memset(&global, 0, sizeof(GlobalInfo_t));
    global.multi = curl_multi_init();

     if (!global.multi) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "playht_speech_load curl_multi_init() failed, exiting!\n");
      return SWITCH_STATUS_FALSE;
    }

    curl_multi_setopt(global.multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
    curl_multi_setopt(global.multi, CURLMOPT_SOCKETDATA, &global);
    curl_multi_setopt(global.multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
    curl_multi_setopt(global.multi, CURLMOPT_TIMERDATA, &global);
    curl_multi_setopt(global.multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

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
    // init mgp123
    if (mpg123_init() != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to initiate MPG123");
      return SWITCH_STATUS_FALSE;
    }

    /* start worker thread that handles transfers*/
    std::thread t(threadFunc) ;
    worker_thread.swap( t ) ;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "playht_speech_loaded..\n");


    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t playht_speech_unload() {
    /* stop the ASIO IO service */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "playht_speech_unload: stopping io service\n");
    io_service.stop();

    /* Join the worker thread */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "playht_speech_unload: wait for worker thread to complete\n");
    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    /* cleanup curl multi handle*/
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "playht_speech_unload: release curl multi\n");
    curl_multi_cleanup(global.multi);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "playht_speech_unload: completed\n");

    mpg123_exit();

		return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t playht_speech_open(playht_t* playht) {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t playht_speech_feed_tts(playht_t* p, char* text, switch_speech_flag_t *flags) {
    CURLMcode rc;

    const int MAX_CHARS = 20;
    char tempText[MAX_CHARS + 4]; // +4 for the ellipsis and null terminator

    if (strlen(text) > MAX_CHARS) {
        strncpy(tempText, text, MAX_CHARS);
        strcpy(tempText + MAX_CHARS, "...");
    } else {
        strcpy(tempText, text);
    }

    /* open cache file */
    if (p->cache_audio && fullDirPath.length() > 0) {
      switch_uuid_t uuid;
      char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
      char outfile[512] = "";
      int fd;

      switch_uuid_get(&uuid);
      switch_uuid_format(uuid_str, &uuid);

      switch_snprintf(outfile, sizeof(outfile), "%s%s%s.mp3", fullDirPath.c_str(), SWITCH_PATH_SEPARATOR, uuid_str);
      p->cache_filename = strdup(outfile);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "writing audio cache file to %s\n", p->cache_filename);

      mode_t oldMask = umask(0);
      fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
      umask(oldMask);
      if (fd == -1 ) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
      }
      else {
        p->file = fdopen(fd, "wb");
        if (!p->file) {
          close(fd);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
        }
      }
    }

    if (!p->api_key) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "playht_speech_feed_tts: no api_key provided\n");
      return SWITCH_STATUS_FALSE;
    }
    if (!p->user_id) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "playht_speech_feed_tts: no user_id provided\n");
      return SWITCH_STATUS_FALSE;
    }

    /* format url*/
    std::string url = "https://api.play.ht/api/v2/tts/stream";
    /* for 3.0 voice engine, we must get a url from the auth api */
    if (p->voice_engine && strcmp(p->voice_engine, "Play3.0") == 0) {
      /* if the url is expired or expiring in the next 30 seconds, get a new one */
      if (p->url_expires_at_ms < (time(NULL) * 1000 + 30000)) {
        std::string auth_url = "https://api.play.ht/api/v3/auth"
        std::string readBuffer;

        CURL* easy = createEasyHandle();
        CURLcode res;

        /* set up headers */
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, ("x-user-id: " + user_id).c_str());
        headers = curl_slist_append(headers, ("authorization: Bearer " + api_key).c_str());

        /* set CURL options */
        curl_easy_setopt(easy, CURLOPT_URL, auth_url.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, url_callback);

        /* perform the request */
        res = curl_easy_perform(easy);
        if (res != CURLE_OK) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error performing Play auth request: %s\n", curl_easy_strerror(res));
          return SWITCH_STATUS_FALSE;
        } else {
          /* check HTTP response code */
          long http_code = 0;
          curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);
          if (http_code == 200) {
            /* parse JSON response */
            cJSON *json = cJSON_Parse(readBuffer.c_str());
            if (json == nullptr) {
              switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing Play auth JSON response\n");
              return SWITCH_STATUS_FALSE;
            } else {
              cJSON *expires_item = cJSON_GetObjectItemCaseSensitive(json, "expires_at_ms");
              if (expires_item && cJSON_IsNumber(expires_item)) {
                p->url_expires_at_ms = expires_item->valueint;
                cJSON_Delete(expires_item);
              } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error retrieving Play auth expiration from JSON response\n");
                return SWITCH_STATUS_FALSE;
              }
              cJSON *url_item = cJSON_GetObjectItemCaseSensitive(json, "url");
              if (url_item && cJSON_IsString(url_item)) {
                p->url = strdup(url_item->valuestring);
                cJSON_Delete(url_item);
              } else {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error retrieving Play auth url from JSON response\n");
                return SWITCH_STATUS_FALSE;
              }
              cJSON_Delete(json);
            }
          } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Play auth HTTP request failed with code %ld\n", http_code);
            return SWITCH_STATUS_FALSE;
          }
        }

        /* clean up */
        curl_slist_free_all(headers);
        curl_easy_cleanup(easy);
      }
      if (p->url) {
        url = p->url;
      } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No Play3.0 auth url available\n");
        return SWITCH_STATUS_FALSE;
      }

    }

    /* create the JSON body */
    cJSON * jResult = cJSON_CreateObject();
    cJSON_AddStringToObject(jResult, "text", text);
    cJSON_AddStringToObject(jResult, "voice", p->voice_name);
    cJSON_AddStringToObject(jResult, "output_format", "mp3");
    cJSON_AddNumberToObject(jResult, "sample_rate", 8000);
    if (p->voice_engine) {
      cJSON_AddStringToObject(jResult, "voice_engine", p->voice_engine);
    }
    if (p->quality) { // DEPRECATED, use sample rate to adjust quality
      cJSON_AddStringToObject(jResult, "quality", p->quality);
    }
    if (p->speed) {
      cJSON_AddNumberToObject(jResult, "speed", std::strtof(p->speed, nullptr));
    }
    if (p->seed) {
      cJSON_AddNumberToObject(jResult, "seed", std::strtof(p->seed, nullptr));
    }
    if (p->temperature) {
      cJSON_AddNumberToObject(jResult, "temperature", std::strtof(p->temperature, nullptr));
    }
    if (p->top_p) {
      cJSON_AddNumberToObject(jResult, "top_p", std::strtof(p->top_p, nullptr));
    }
    if (p->emotion) { // DEPRECATED
      cJSON_AddStringToObject(jResult, "emotion", p->emotion);
    }
    if (p->voice_guidance) {
      cJSON_AddNumberToObject(jResult, "voice_guidance", std::strtof(p->voice_guidance, nullptr));
    }
    if (p->style_guidance) {
      cJSON_AddNumberToObject(jResult, "style_guidance", std::strtof(p->style_guidance, nullptr));
    }
    if (p->text_guidance) {
      cJSON_AddNumberToObject(jResult, "text_guidance", std::strtof(p->text_guidance, nullptr));
    }
    if (p->repetition_penalty) {
      cJSON_AddNumberToObject(jResult, "repetition_penalty", std::strtof(p->repetition_penalty, nullptr));
    }
    if (p->language) { // Only applies to Play3.0 voice engine
      cJSON_AddStringToObject(jResult, "language", p->language);
    }
    char *json = cJSON_PrintUnformatted(jResult);

    cJSON_Delete(jResult);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "playht_speech_feed_tts: [%s] [%s]\n", url.c_str(), tempText);

    ConnInfo_t *conn = pool.malloc() ;

    // COnfigure MPG123
    int mhError = 0;
    mpg123_handle *mh = mpg123_new("auto", &mhError);
    if (!mh) {
      const char *mhErr = mpg123_plain_strerror(mhError);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error allocating mpg123 handle! %s\n", switch_str_nil(mhErr));
      return SWITCH_STATUS_FALSE;
    }

    if (mpg123_open_feed(mh) != MPG123_OK) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error mpg123_open_feed!\n");
      return SWITCH_STATUS_FALSE;
		}

    if (mpg123_format_all(mh) != MPG123_OK) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error mpg123_format_all!\n");
      return SWITCH_STATUS_FALSE;
		}

    if (mpg123_param(mh, MPG123_FLAGS, MPG123_MONO_MIX, 0) != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error forcing single channel!\n");
      return SWITCH_STATUS_FALSE;
    }

    CURL* easy = createEasyHandle();
    p->conn = (void *) conn ;
    conn->playht = p;
    conn->easy = easy;
    conn->mh = mh;
    conn->global = &global;
    conn->hdr_list = NULL ;
    conn->file = p->file;
    conn->body = json;
    conn->flushed = false;


    p->circularBuffer = (void *) new CircularBuffer_t(8192);

    if (mpg123_param(mh, MPG123_FORCE_RATE, p->rate /*Hz*/, 0) != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error mpg123_param!\n");
      return SWITCH_STATUS_FALSE;
    }

    std::ostringstream api_key_stream;
    api_key_stream << "AUTHORIZATION: " << p->api_key;
    std::ostringstream user_id_stream;
    user_id_stream << "X-USER-ID: " << p->user_id;

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, conn);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, conn->error);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, conn);
    curl_easy_setopt(easy, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, conn);

    /* call this function to get a socket */
    curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, opensocket);

    /* call this function to close a socket */
    curl_easy_setopt(easy, CURLOPT_CLOSESOCKETFUNCTION, close_socket);

    conn->hdr_list = curl_slist_append(conn->hdr_list, api_key_stream.str().c_str());
    conn->hdr_list = curl_slist_append(conn->hdr_list, user_id_stream.str().c_str());
    conn->hdr_list = curl_slist_append(conn->hdr_list, "Accept: audio/mpeg");
    conn->hdr_list = curl_slist_append(conn->hdr_list, "Content-Type: application/json");
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, conn->hdr_list);

    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, conn->body);
    //curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, body.length());

    // libcurl adding random byte to the response body that creates white noise to audio file
    // https://github.com/curl/curl/issues/10525
    const bool disable_http_2 = switch_true(std::getenv("DISABLE_HTTP2_FOR_TTS_STREAMING"));
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, disable_http_2 ? CURL_HTTP_VERSION_1_1 : CURL_HTTP_VERSION_2_0);

    rc = curl_multi_add_handle(global.multi, conn->easy);
    mcode_test("new_conn: curl_multi_add_handle", rc);

    /* start a timer to measure the duration until we receive first byte of audio */
    conn->startTime = std::chrono::high_resolution_clock::now();

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "playht_speech_feed_tts: called curl_multi_add_handle\n");


    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t playht_speech_read_tts(playht_t* p, void *data, size_t *datalen, switch_speech_flag_t *flags) {
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) p->circularBuffer;
    std::vector<uint16_t> pcm_data;

    {
      switch_mutex_lock(p->mutex);
      ConnInfo_t *conn = (ConnInfo_t *) p->conn;
      if (p->response_code > 0 && p->response_code != 200) {
        switch_mutex_unlock(p->mutex);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "playht_speech_read_tts, returning failure\n") ;
        return SWITCH_STATUS_FALSE;
      }
      if (conn && conn->flushed) {
        switch_mutex_unlock(p->mutex);
        return SWITCH_STATUS_BREAK;
      }
      if (cBuffer->empty()) {
        if (p->draining) {
          switch_mutex_unlock(p->mutex);
          return SWITCH_STATUS_BREAK;
        }
        /* no audio available yet so send silence */
        memset(data, 255, *datalen);
        switch_mutex_unlock(p->mutex);
        return SWITCH_STATUS_SUCCESS;
      }
      size_t size = std::min((*datalen/2), cBuffer->size());
      pcm_data.insert(pcm_data.end(), cBuffer->begin(), cBuffer->begin() + size);
      cBuffer->erase(cBuffer->begin(), cBuffer->begin() + size);
      switch_mutex_unlock(p->mutex);
    }

    memcpy(data, pcm_data.data(), pcm_data.size() * sizeof(uint16_t));
    *datalen = pcm_data.size() * sizeof(uint16_t);

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t playht_speech_flush_tts(playht_t* p) {
    bool download_complete = p->response_code == 200;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "playht_speech_flush_tts, download complete? %s\n", download_complete ? "yes" : "no") ;
    ConnInfo_t *conn = (ConnInfo_t *) p->conn;
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) p->circularBuffer;
    delete cBuffer;
    p->circularBuffer = nullptr ;

    if (conn) {
      conn->flushed = true;
      if (!download_complete) {
        if (conn->file) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "closing audio cache file %s because download was interrupted\n", p->cache_filename);
          if (fclose(conn->file) != 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error closing audio cache file\n");
          }
          conn->file = nullptr ;
        }
      }
    }
    // if playback event has not been sent, delete the file.
    if (p->cache_filename && !p->playback_start_sent) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing audio cache file %s because download was interrupted\n", p->cache_filename);
      if (unlink(p->cache_filename) != 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "cleanupConn: error removing audio cache file %s: %d:%s\n",
          p->cache_filename, errno, strerror(errno));
      }
      free(p->cache_filename);
      p->cache_filename = nullptr ;
    }
    if (p->session_id) {
      switch_core_session_t* session = switch_core_session_locate(p->session_id);
      if (session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_core_session_rwunlock(session);
        if (channel) {
          switch_event_t *event;
          if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_STOP) == SWITCH_STATUS_SUCCESS) {
            switch_channel_event_set_data(channel, event);
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_playht_response_code", std::to_string(p->response_code).c_str());
            if (p->cache_filename && p->response_code == 200) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", p->cache_filename);
            }
            if (p->response_code != 200 && p->err_msg) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_error", p->err_msg);
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

	switch_status_t playht_speech_close(playht_t* p) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "playht_speech_close\n") ;
		return SWITCH_STATUS_SUCCESS;
	}
}
