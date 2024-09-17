#include "mod_verbio_tts.h"
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

#include <speex/speex_resampler.h>

#define BUFFER_GROW_SIZE (80000)

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
  verbio_t* verbio;
  char* body;
  struct curl_slist *hdr_list;
  GlobalInfo_t *global;
  char error[CURL_ERROR_SIZE];
  FILE* file;
  std::chrono::time_point<std::chrono::high_resolution_clock> startTime;
  bool flushed;

  bool has_last_byte;
  uint8_t last_byte;
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
    std::string connectTimeoutStr = getEnvVar("VERBIO_TTS_CURL_CONNECT_TIMEOUT");

    if (connectTimeoutStr.empty()) {
        connectTimeoutStr = getEnvVar("TTS_CURL_CONNECT_TIMEOUT");
    }

    long connectTimeout = 10L;

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
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, getConnectionTimeout());

  return easy ;
}

static void cleanupConn(ConnInfo_t *conn) {
  auto v = conn->verbio;

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

  v->conn = nullptr ;
  v->draining = 1;

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

      auto v = conn->verbio;
      v->response_code = response_code;
      if (ct) v->ct = strdup(ct);

      std::string name_lookup_ms = secondsToMillisecondsString(namelookup);
      std::string connect_ms = secondsToMillisecondsString(connect);
      std::string final_response_time_ms = secondsToMillisecondsString(total);

      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
        "mod_verbio_tts: response: %ld, content-type %s,"
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

      v->name_lookup_time_ms = strdup(name_lookup_ms.c_str());
      v->connect_time_ms = strdup(connect_ms.c_str());
      v->final_response_time_ms = strdup(final_response_time_ms.c_str());

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
  
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_verbio_tts threadFunc - starting\n");

  for(;;) {
      
    try {
      io_service.run() ;
      break ;
    }
    catch( std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_verbio_tts threadFunc - Error: %s\n", e.what());
    }
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_verbio_tts threadFunc - ending\n");
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

/* CURLOPT_WRITEFUNCTION */
static size_t write_cb(void *ptr, size_t size, size_t nmemb, ConnInfo_t *conn) {
  bool fireEvent = false;
  uint8_t *data = (uint8_t *) ptr;
  size_t bytes_received = size * nmemb;
  size_t total_bytes_to_process;
  auto v = conn->verbio;
  CircularBuffer_t *cBuffer = (CircularBuffer_t *) v->circularBuffer;
  
  if (conn->flushed || cBuffer == nullptr) {
    /* this will abort the transfer */
    return 0;
  }
  // Buffer to hold combined data if there is unprocessed byte from the last call.
  std::unique_ptr<uint8_t[]> combinedData;

  if (conn->has_last_byte) {
    conn->has_last_byte = false;  // We'll handle the last_byte now, so toggle the flag off

    // Allocate memory for the new data array
    combinedData.reset(new uint8_t[bytes_received + 1]);

    // Prepend the last byte from previous call
    combinedData[0] = conn->last_byte;

    // Copy the new data following the prepended byte
    memcpy(combinedData.get() + 1, data, bytes_received);

    // Point our data pointer to the new array
    data = combinedData.get();

    total_bytes_to_process = bytes_received + 1;
  } else {
    total_bytes_to_process = bytes_received;
  }

  // If we now have an odd total, save the last byte for next time
  if ((total_bytes_to_process % sizeof(int16_t)) != 0) {
    conn->last_byte = data[total_bytes_to_process - 1];
    conn->has_last_byte = true;
    total_bytes_to_process--;
  }

  int16_t* inputData = reinterpret_cast<int16_t*>(data);
  if (0 == v->reads++) {
    fireEvent = true;
    // Verbio return PCM linear16 WAV file which contains 44 bytes headers, remove that.
    inputData += 22;
    total_bytes_to_process -= 44;
  }
  size_t numSamples = total_bytes_to_process / sizeof(int16_t);
  {
    switch_mutex_lock(v->mutex);

    if (v->response_code > 0 && v->response_code != 200) {
      std::string body((char *) ptr, bytes_received);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: received body %s\n", body.c_str());
      v->err_msg = strdup(body.c_str());
      switch_mutex_unlock(v->mutex);
      return 0;
    }

    /* cache file will stay in the mp3 format for size (smaller) and simplicity */
    if (conn->file) fwrite(inputData, sizeof(int16_t), numSamples, conn->file);

    // Resize the buffer if necessary
    if (cBuffer->capacity() - cBuffer->size() < numSamples) {
      //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "write_cb growing buffer\n"); 

      //TODO: if buffer exceeds some max size, return CURL_WRITEFUNC_ERROR to abort the transfer
      cBuffer->set_capacity(cBuffer->size() + std::max(numSamples, (size_t)BUFFER_GROW_SIZE));
    }
    
    /* Push the data into the buffer */
    cBuffer->insert(cBuffer->end(), inputData, inputData + numSamples);

    switch_mutex_unlock(v->mutex);
  }
  if (fireEvent && v->session_id) {
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - conn->startTime);
    auto time_to_first_byte_ms = std::to_string(duration.count());
    switch_core_session_t* session = switch_core_session_locate(v->session_id);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_core_session_rwunlock(session);
      if (channel) {
        switch_event_t *event;
        if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_START) == SWITCH_STATUS_SUCCESS) {
          switch_channel_event_set_data(channel, event);

          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "write_cb: firing playback-started\n");

          switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
          if (v->name_lookup_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_verbio_name_lookup_time_ms", v->name_lookup_time_ms);
          }
          if (v->connect_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_verbio_connect_time_ms", v->connect_time_ms);
          }
          if (v->final_response_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_verbio_final_response_time_ms", v->final_response_time_ms);
          }
          if (v->voice_name) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_verbio_voice_name", v->voice_name);
          }
          if (v->cache_filename) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", v->cache_filename);
          }

          switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_time_to_first_byte_ms", time_to_first_byte_ms.c_str());
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
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: session %s not found\n", v->session_id);
    }
  }
  return size*nmemb;
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
  verbio_t* v = conn->verbio;
  std::string header, value;
  std::string input(buffer, bytes_received);
  if (parseHeader(input, header, value)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "recv header: %s with value %s\n", header.c_str(), value.c_str());
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "header_callback: %s\n", input.c_str());
    if (input.rfind(prefix, 0) == 0) {
      try {
        v->response_code = extract_response_code(input);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "header_callback: parsed response code: %ld\n", v->response_code);
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
  switch_status_t verbio_speech_load() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_loading..\n");
    memset(&global, 0, sizeof(GlobalInfo_t));
    global.multi = curl_multi_init();

     if (!global.multi) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "verbio_speech_load curl_multi_init() failed, exiting!\n");
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

    /* start worker thread that handles transfers*/
    std::thread t(threadFunc) ;
    worker_thread.swap( t ) ;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_loaded..\n");


    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_unload() {
    /* stop the ASIO IO service */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "verbio_speech_unload: stopping io service\n");
    io_service.stop();

    /* Join the worker thread */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "verbio_speech_unload: wait for worker thread to complete\n");
    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    /* cleanup curl multi handle*/
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "verbio_speech_unload: release curl multi\n");
    curl_multi_cleanup(global.multi);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "verbio_speech_unload: completed\n");

		return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_open(verbio_t* verbio) {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_feed_tts(verbio_t* v, char* text, switch_speech_flag_t *flags) {
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
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "verbio_speech_feed_tts: no access_token provided\n");
      return SWITCH_STATUS_FALSE;
    }
    /* format url*/
    std::string url = "https://us.rest.speechcenter.verbio.com/api/v1/synthesize";

    /* create the JSON body */
    cJSON * jResult = cJSON_CreateObject();
    cJSON_AddStringToObject(jResult, "voice_id", v->voice_name);
    cJSON_AddStringToObject(jResult, "output_sample_rate", "8k");
    cJSON_AddStringToObject(jResult, "output_encoding", "pcm16");
    cJSON_AddStringToObject(jResult, "text", text);
    char *json = cJSON_PrintUnformatted(jResult);

    cJSON_Delete(jResult);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_feed_tts: [%s] [%s]\n", url.c_str(), tempText);

    ConnInfo_t *conn = pool.malloc() ;

    CURL* easy = createEasyHandle();
    v->conn = (void *) conn ;
    conn->verbio = v;
    conn->easy = easy;
    conn->global = &global;
    conn->hdr_list = NULL ;
    conn->file = v->file;
    conn->body = json;
    conn->flushed = false;
    conn->has_last_byte = false;
    conn->last_byte = 0;


    v->circularBuffer = (void *) new CircularBuffer_t(BUFFER_GROW_SIZE);
    // Always use verbio at rate 8000 for helping cache audio from jambonz.
    if (v->rate != 8000) {
      int err;
      v->resampler = speex_resampler_init(1, 8000, v->rate, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }

    std::ostringstream api_key_stream;
    api_key_stream << "Authorization: Bearer " << v->access_token;

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

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "verbio_speech_feed_tts: called curl_multi_add_handle\n");


    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_read_tts(verbio_t* v, void *data, size_t *datalen, switch_speech_flag_t *flags) {
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) v->circularBuffer;
    std::vector<uint16_t> pcm_data;

    {
      switch_mutex_lock(v->mutex);
      ConnInfo_t *conn = (ConnInfo_t *) v->conn;
      if (v->response_code > 0 && v->response_code != 200) {
        switch_mutex_unlock(v->mutex);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_read_tts, returning failure\n") ;  
        return SWITCH_STATUS_FALSE;
      }
      if (conn && conn->flushed) {
        switch_mutex_unlock(v->mutex);
        return SWITCH_STATUS_BREAK;
      }
      if (cBuffer->empty()) {
        if (v->draining) {
          switch_mutex_unlock(v->mutex);
          return SWITCH_STATUS_BREAK;
        }
        /* no audio available yet so send silence */
        memset(data, 255, *datalen);
        switch_mutex_unlock(v->mutex);
        return SWITCH_STATUS_SUCCESS;
      }
      size_t size = std::min((*datalen/(2 * v->rate / 8000)), cBuffer->size());
      pcm_data.insert(pcm_data.end(), cBuffer->begin(), cBuffer->begin() + size);
      cBuffer->erase(cBuffer->begin(), cBuffer->begin() + size);
      switch_mutex_unlock(v->mutex);
    }

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
      memcpy(data, pcm_data.data(), pcm_data.size() * sizeof(uint16_t));
      *datalen = pcm_data.size() * sizeof(uint16_t);
    }

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t verbio_speech_flush_tts(verbio_t* v) {
    bool download_complete = v->response_code == 200;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_flush_tts, download complete? %s\n", download_complete ? "yes" : "no") ;  

    ConnInfo_t *conn = (ConnInfo_t *) v->conn;
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) v->circularBuffer;
    delete cBuffer;
    v->circularBuffer = nullptr ;

    // destroy resampler
    if (v->resampler) {
      speex_resampler_destroy(v->resampler);
      v->resampler = NULL;
    }

    if (conn) {
      conn->flushed = true;
      if (!download_complete) {
        if (conn->file) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "closing audio cache file %s because download was interrupted\n", v->cache_filename);
          if (fclose(conn->file) != 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error closing audio cache file\n");
          }
          conn->file = nullptr ;
        }

        if (v->cache_filename) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing audio cache file %s because download was interrupted\n", v->cache_filename);
          if (unlink(v->cache_filename) != 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "cleanupConn: error removing audio cache file %s: %d:%s\n", 
              v->cache_filename, errno, strerror(errno));
          }
          free(v->cache_filename);
          v->cache_filename = nullptr ;
        }
      }
    }
    if (v->session_id) {
      switch_core_session_t* session = switch_core_session_locate(v->session_id);
      if (session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        switch_core_session_rwunlock(session);
        if (channel) {
          switch_event_t *event;
          if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_STOP) == SWITCH_STATUS_SUCCESS) {
            switch_channel_event_set_data(channel, event);
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_verbio_response_code", std::to_string(v->response_code).c_str());
            if (v->cache_filename && v->response_code == 200) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", v->cache_filename);
            }
            if (v->response_code != 200 && v->err_msg) {
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

	switch_status_t verbio_speech_close(verbio_t* w) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "verbio_speech_close\n") ;
		return SWITCH_STATUS_SUCCESS;
	}
}
