#include "mod_deepgram_tts.h"
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

#define BUFFER_GROW_SIZE (8192)

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
  deepgram_t* deepgram;
  char* body;
  struct curl_slist *hdr_list;
  GlobalInfo_t *global;
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
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);

  return easy ;    
}

static void cleanupConn(ConnInfo_t *conn) {
  auto d = conn->deepgram;

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

  d->conn = nullptr ;
  d->draining = 1;

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

      auto d = conn->deepgram;
      d->response_code = response_code;
      if (ct) d->ct = strdup(ct);

      std::string name_lookup_ms = secondsToMillisecondsString(namelookup);
      std::string connect_ms = secondsToMillisecondsString(connect);
      std::string final_response_time_ms = secondsToMillisecondsString(total);

      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
        "mod_deepgram_tts: response: %ld, content-type %s,"
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

      d->name_lookup_time_ms = strdup(name_lookup_ms.c_str());
      d->connect_time_ms = strdup(connect_ms.c_str());
      d->final_response_time_ms = strdup(final_response_time_ms.c_str());

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
  
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_deepgram_tts threadFunc - starting\n");

  for(;;) {
      
    try {
      io_service.run() ;
      break ;
    }
    catch( std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_deepgram_tts threadFunc - Error: %s\n", e.what());
    }
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_deepgram_tts threadFunc - ending\n");
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
  size_t numSamples = bytes_received / sizeof(int16_t);
  int16_t* inputData = reinterpret_cast<int16_t*>(ptr);
  auto d = conn->deepgram;
  CircularBuffer_t *cBuffer = (CircularBuffer_t *) d->circularBuffer;
  std::vector<uint16_t> pcm_data;
  
  if (conn->flushed) {
    /* this will abort the transfer */
    return 0;
  }
  {
    switch_mutex_lock(d->mutex);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "write_cb: received data, response %ld\n", 
      d->response_code);

    if (d->response_code > 0 && d->response_code != 200) {
      std::string body((char *) ptr, bytes_received);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: received body %s\n", body.c_str());
      d->err_msg = strdup(body.c_str());
      switch_mutex_unlock(d->mutex);
      return 0;
    }

    /* cache file will stay in the mp3 format for size (smaller) and simplicity */
    if (conn->file) fwrite(data, sizeof(uint8_t), bytes_received, conn->file);

    pcm_data.assign(inputData, inputData + numSamples);
    size_t bytesResampled = pcm_data.size() * sizeof(uint16_t);

    // Resize the buffer if necessary
    if (cBuffer->capacity() - cBuffer->size() < (bytesResampled / sizeof(uint16_t))) {
      //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "write_cb growing buffer\n"); 

      //TODO: if buffer exceeds some max size, return CURL_WRITEFUNC_ERROR to abort the transfer
      cBuffer->set_capacity(cBuffer->size() + std::max((bytesResampled / sizeof(uint16_t)), (size_t)BUFFER_GROW_SIZE));
    }
    
    /* Push the data into the buffer */
    cBuffer->insert(cBuffer->end(), pcm_data.data(), pcm_data.data() + pcm_data.size());

    if (0 == d->reads++) {
      fireEvent = true;
    }
    switch_mutex_unlock(d->mutex);
  }
  if (fireEvent && d->session_id) {
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - conn->startTime);
    auto time_to_first_byte_ms = std::to_string(duration.count());
    switch_core_session_t* session = switch_core_session_locate(d->session_id);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      if (channel) {
        switch_event_t *event;
        if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_START) == SWITCH_STATUS_SUCCESS) {
          switch_channel_event_set_data(channel, event);

          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "write_cb: firing playback-started\n");

          switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
          if (d->reported_latency) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_deepgram_reported_latency_ms", d->reported_latency);
          }
          if (d->request_id) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_deepgram_request_id", d->request_id);
          }
          if (d->name_lookup_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_deepgram_name_lookup_time_ms", d->name_lookup_time_ms);
          }
          if (d->connect_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_deepgram_connect_time_ms", d->connect_time_ms);
          }
          if (d->final_response_time_ms) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_deepgram_final_response_time_ms", d->final_response_time_ms);
          }
          if (d->voice_name) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_deepgram_voice_name", d->voice_name);
          }
          if (d->model_id) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_deepgram_model_id", d->model_id);
          }
          if (d->cache_filename) {
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", d->cache_filename);
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
      switch_core_session_rwunlock(session);
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: session %s not found\n", d->session_id);
    }
  }
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

static size_t header_callback(char *buffer, size_t size, size_t nitems, ConnInfo_t *conn) {
  size_t bytes_received = size * nitems;
  const std::string prefix = "HTTP/2 ";
  deepgram_t* d = conn->deepgram;
  std::string header, value;
  std::string input(buffer, bytes_received);
  if (parseHeader(input, header, value)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "recv header: %s with value %s\n", header.c_str(), value.c_str());
    if (0 == header.compare("openai-processing-ms")) d->reported_latency = strdup(value.c_str());
    else if (0 == header.compare("x-request-id")) d->request_id = strdup(value.c_str());
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "recv header: %s\n", input.c_str());
    if (input.rfind(prefix, 0) == 0) {
      try {
        d->response_code = std::stoi(input.substr(prefix.length()));
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parsed response code: %ld\n", d->response_code);
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
  switch_status_t deepgram_speech_load() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "deepgram_speech_loading..\n");
    memset(&global, 0, sizeof(GlobalInfo_t));
    global.multi = curl_multi_init();

     if (!global.multi) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "deepgram_speech_load curl_multi_init() failed, exiting!\n");
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

    /* start worker thread that handles transfers*/
    std::thread t(threadFunc) ;
    worker_thread.swap( t ) ;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "deepgram_speech_loaded..\n");


    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t deepgram_speech_unload() {
    /* stop the ASIO IO service */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deepgram_speech_unload: stopping io service\n");
    io_service.stop();

    /* Join the worker thread */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deepgram_speech_unload: wait for worker thread to complete\n");
    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    /* cleanup curl multi handle*/
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deepgram_speech_unload: release curl multi\n");
    curl_multi_cleanup(global.multi);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deepgram_speech_unload: completed\n");

		return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t deepgram_speech_open(deepgram_t* deepgram) {
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t deepgram_speech_feed_tts(deepgram_t* d, char* text, switch_speech_flag_t *flags) {
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
    if (d->cache_audio && fullDirPath.length() > 0) {
      switch_uuid_t uuid;
      char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
      char outfile[512] = "";
      int fd;

      switch_uuid_get(&uuid);
      switch_uuid_format(uuid_str, &uuid);

      switch_snprintf(outfile, sizeof(outfile), "%s%s%s.mp3", fullDirPath.c_str(), SWITCH_PATH_SEPARATOR, uuid_str);
      d->cache_filename = strdup(outfile);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "writing audio cache file to %s\n", d->cache_filename);

      mode_t oldMask = umask(0);
      fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
      umask(oldMask);
      if (fd == -1 ) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
      }
      else {
        d->file = fdopen(fd, "wb");
        if (!d->file) {
          close(fd);
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error opening cache file %s: %s\n", outfile, strerror(errno));
        }
      }
    }

    if (!d->api_key) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "deepgram_speech_feed_tts: no api_key provided\n");
      return SWITCH_STATUS_FALSE;
    }
    if (!d->model_id) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "deepgram_speech_feed_tts: no model_id provided\n");
      return SWITCH_STATUS_FALSE;
    }
    // sample rate
    uint32_t samples_per_second = 8000;
    if (d->session_id) {
      int err;
      switch_codec_implementation_t read_impl;
      switch_core_session_t *psession = switch_core_session_locate(d->session_id);
      switch_core_session_get_read_impl(psession, &read_impl);
      samples_per_second = !strcasecmp(read_impl.iananame, "g722") ? read_impl.actual_samples_per_second : read_impl.samples_per_second;
    }
    /* format url*/
    std::string url;
    std::ostringstream url_stream;
    url_stream << "https://api.deepgram.com/v1/speak?model=" << d->voice_name << "&encoding=linear16&sample_rate=" << samples_per_second;
    url = url_stream.str();

    /* create the JSON body */
    cJSON * jResult = cJSON_CreateObject();
    cJSON_AddStringToObject(jResult, "text", text);
    char *json = cJSON_PrintUnformatted(jResult);

    cJSON_Delete(jResult);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "deepgram_speech_feed_tts: [%s] [%s]\n", url.c_str(), tempText);

    ConnInfo_t *conn = pool.malloc() ;

    CURL* easy = createEasyHandle();
    d->conn = (void *) conn ;
    conn->deepgram = d;
    conn->easy = easy;
    conn->global = &global;
    conn->hdr_list = NULL ;
    conn->file = d->file;
    conn->body = json;
    conn->flushed = false;
    

    d->circularBuffer = (void *) new CircularBuffer_t(8192);

    std::ostringstream api_key_stream;
    api_key_stream << "Authorization: Token " << d->api_key;

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

    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);

    rc = curl_multi_add_handle(global.multi, conn->easy);
    mcode_test("new_conn: curl_multi_add_handle", rc);

    /* start a timer to measure the duration until we receive first byte of audio */
    conn->startTime = std::chrono::high_resolution_clock::now();

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "deepgram_speech_feed_tts: called curl_multi_add_handle\n");


    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t deepgram_speech_read_tts(deepgram_t* d, void *data, size_t *datalen, switch_speech_flag_t *flags) {
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) d->circularBuffer;
    std::vector<uint16_t> pcm_data;

    {
      switch_mutex_lock(d->mutex);
      ConnInfo_t *conn = (ConnInfo_t *) d->conn;
      if (d->response_code > 0 && d->response_code != 200) {
        switch_mutex_unlock(d->mutex);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "deepgram_speech_read_tts, returning failure\n") ;  
        return SWITCH_STATUS_FALSE;
      }
      if (conn && conn->flushed) {
        switch_mutex_unlock(d->mutex);
        return SWITCH_STATUS_BREAK;
      }
      if (cBuffer->empty()) {
        if (d->draining) {
          switch_mutex_unlock(d->mutex);
          return SWITCH_STATUS_BREAK;
        }
        /* no audio available yet so send silence */
        memset(data, 255, *datalen);
        switch_mutex_unlock(d->mutex);
        return SWITCH_STATUS_SUCCESS;
      }
      size_t size = std::min((*datalen/2), cBuffer->size());
      pcm_data.insert(pcm_data.end(), cBuffer->begin(), cBuffer->begin() + size);
      cBuffer->erase(cBuffer->begin(), cBuffer->begin() + size);
      switch_mutex_unlock(d->mutex);
    }

    memcpy(data, pcm_data.data(), pcm_data.size() * sizeof(uint16_t));
    *datalen = pcm_data.size() * sizeof(uint16_t);

    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t deepgram_speech_flush_tts(deepgram_t* d) {
    bool download_complete = d->response_code == 200;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "deepgram_speech_flush_tts, download complete? %s\n", download_complete ? "yes" : "no") ;  

    ConnInfo_t *conn = (ConnInfo_t *) d->conn;
    CircularBuffer_t *cBuffer = (CircularBuffer_t *) d->circularBuffer;
    delete cBuffer;
    d->circularBuffer = nullptr ;

    if (conn) {
      conn->flushed = true;
      if (!download_complete) {
        if (conn->file) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "closing audio cache file %s because download was interrupted\n", d->cache_filename);
          if (fclose(conn->file) != 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "error closing audio cache file\n");
          }
          conn->file = nullptr ;
        }

        if (d->cache_filename) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing audio cache file %s because download was interrupted\n", d->cache_filename);
          if (unlink(d->cache_filename) != 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "cleanupConn: error removing audio cache file %s: %d:%s\n", 
              d->cache_filename, errno, strerror(errno));
          }
          free(d->cache_filename);
          d->cache_filename = nullptr ;
        }
      }
    }
    if (d->session_id) {
      switch_core_session_t* session = switch_core_session_locate(d->session_id);
      if (session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (channel) {
          switch_event_t *event;
          if (switch_event_create(&event, SWITCH_EVENT_PLAYBACK_STOP) == SWITCH_STATUS_SUCCESS) {
            switch_channel_event_set_data(channel, event);
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Playback-File-Type", "tts_stream");
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_deepgram_response_code", std::to_string(d->response_code).c_str());
            if (d->cache_filename && d->response_code == 200) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_cache_filename", d->cache_filename);
            }
            if (d->response_code != 200 && d->err_msg) {
              switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "variable_tts_error", d->err_msg);
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

	switch_status_t deepgram_speech_close(deepgram_t* w) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "deepgram_speech_close\n") ;
		return SWITCH_STATUS_SUCCESS;
	}
}
