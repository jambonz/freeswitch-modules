#include "audio_downloader.h"

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

#include <map>


#include <mpg123.h>

#include <curl/curl.h>

#define BUFFER_GROW_SIZE (80000)
#define BUFFER_THROTTLE_LOW (40000)
#define BUFFER_THROTTLE_HIGH (160000)

static uint16_t currDownloadId = 0;

typedef struct
{
  CURLM *multi;
  int still_running;
} GlobalInfo_t;
static GlobalInfo_t global;

typedef enum
{
  STATUS_NONE = 0,
  STATUS_FAILED,
  STATUS_DOWNLOAD_IN_PROGRESS,
  STATUS_DOWNLOAD_PAUSED,
  STATUS_DOWNLOAD_COMPLETE,
  STATUS_AWAITING_RESTART,
  STATUS_STOPPING,
  STATUS_STOPPED
} Status_t;

static const char* status2String(Status_t status)
{
  static const char* statusStrings[] = {
    "STATUS_NONE",
    "STATUS_FAILED",
    "STATUS_DOWNLOAD_IN_PROGRESS",
    "STATUS_DOWNLOAD_PAUSED",
    "STATUS_DOWNLOAD_COMPLETE",
    "STATUS_AWAITING_RESTART",
    "STATUS_STOPPING",
    "STATUS_STOPPED"
  };

  if (status >= 0 && status < sizeof(statusStrings) / sizeof(statusStrings[0]))
  {
    return statusStrings[status];
  }
  else
  {
    return "UNKNOWN_STATUS";
  }
}

typedef struct
{
  GlobalInfo_t *global;
  CURL *easy;
  switch_mutex_t* mutex;
  CircularBuffer_t* buffer;
  mpg123_handle *mh;
  char error[CURL_ERROR_SIZE]; // curl error buffer
  char *err_msg; // http server error message
  HttpPayload_t payload;
  char* body;
  std::queue<HttpPayload_t>* cmdQueue;
  struct curl_slist *hdr_list;
  bool loop;
  int rate;
  boost::asio::deadline_timer *timer;
  Status_t status;
  downloadId_t id;
  int response_code;
  int gain;
} ConnInfo_t;

typedef std::map<int32_t, ConnInfo_t *> Id2ConnMap_t;
static Id2ConnMap_t id2ConnMap;

static boost::object_pool<ConnInfo_t> pool ;
static std::map<curl_socket_t, boost::asio::ip::tcp::socket *> socket_map;
static boost::asio::io_service io_service;
static boost::asio::deadline_timer timer(io_service);
static std::string fullDirPath;
static std::thread worker_thread;

/* forward declarations */
static ConnInfo_t* createDownloader(HttpPayload_t* payload, int rate, int loop, int gain, mpg123_handle *mhm, switch_mutex_t *mutex, CircularBuffer_t *buffer, HttpPayloadQueue_t*cmdQueue);
static CURL* createEasyHandle(void);
static void destroyConnection(ConnInfo_t *conn);
static void check_multi_info(GlobalInfo_t *g) ;
static int mcode_test(const char *where, CURLMcode code);
static void event_cb(GlobalInfo_t *g, curl_socket_t s, int action, const boost::system::error_code & error, int *fdp);
static void setsock(int *fdp, curl_socket_t s, CURL *e, int act, int oldact, GlobalInfo_t *g);
static void addsock(curl_socket_t s, CURL *easy, int action, GlobalInfo_t *g);
static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp);
static void threadFunc();
static void timer_cb(const boost::system::error_code & error, GlobalInfo_t *g);
static int multi_timer_cb(CURLM *multi, long timeout_ms, GlobalInfo_t *g);
static std::vector<int16_t> convert_mp3_to_linear(ConnInfo_t *conn, int8_t *data, size_t len);
static void throttling_cb(const boost::system::error_code& error, ConnInfo_t* conn) ;
static void restart_cb(const boost::system::error_code& error, ConnInfo_t* conn);
static size_t write_cb(void *ptr, size_t size, size_t nmemb, ConnInfo_t *conn);
static bool parseHeader(const std::string& str, std::string& header, std::string& value) ;
static int extract_response_code(const std::string& input) ;
static size_t header_callback(char *buffer, size_t size, size_t nitems, ConnInfo_t *conn);
static curl_socket_t opensocket(void *clientp, curlsocktype purpose, struct curl_sockaddr *address);
static int close_socket(void *clientp, curl_socket_t item);

/* apis */
extern "C" {

  switch_status_t init_audio_downloader() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "init_audio_downloader loading..\n");
    memset(&global, 0, sizeof(GlobalInfo_t));
    global.multi = curl_multi_init();

    if (!global.multi) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "init_audio_downloader curl_multi_init() failed, exiting!\n");
      return SWITCH_STATUS_FALSE;
    }

    curl_multi_setopt(global.multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
    curl_multi_setopt(global.multi, CURLMOPT_SOCKETDATA, &global);
    curl_multi_setopt(global.multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
    curl_multi_setopt(global.multi, CURLMOPT_TIMERDATA, &global);
    curl_multi_setopt(global.multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    if (mpg123_init() != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "init_audio_downloader: failed to initiate MPG123");
      return SWITCH_STATUS_FALSE;
    }

    /* start worker thread */
    std::thread t(threadFunc) ;
    worker_thread.swap( t ) ;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "init_audio_downloader: loaded\n");

    return SWITCH_STATUS_SUCCESS;

  }

  switch_status_t deinit_audio_downloader() {
    /* stop the ASIO IO service */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deinit_audio_downloader: stopping io service\n");
    io_service.stop();

    /* Join the worker thread */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deinit_audio_downloader: wait for worker thread to complete\n");
    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    /* cleanup curl multi handle*/
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deinit_audio_downloader: release curl multi\n");
    curl_multi_cleanup(global.multi);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deinit_audio_downloader: completed\n");

    mpg123_exit();

    return SWITCH_STATUS_SUCCESS;
  }

  downloadId_t start_audio_download(HttpPayload_t* payload, int rate,
    int loop, int gain, switch_mutex_t* mutex, CircularBuffer_t* buffer, HttpPayloadQueue_t* cmdQueue) {
    int mhError = 0;

    /* allocate handle for mpeg decoding */
    mpg123_handle *mh = mpg123_new("auto", &mhError);
    if (!mh) {
      const char *mhErr = mpg123_plain_strerror(mhError);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error allocating mpg123 handle! %s\n", switch_str_nil(mhErr));
      return INVALID_DOWNLOAD_ID;
    }

    if (mpg123_open_feed(mh) != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error mpg123_open_feed!\n");
      return INVALID_DOWNLOAD_ID;
    }

    if (mpg123_format_all(mh) != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error mpg123_format_all!\n");
      return INVALID_DOWNLOAD_ID;
    }

    if (mpg123_param(mh, MPG123_FORCE_RATE, rate, 0) != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error forcing resample to 8k!\n");
      return INVALID_DOWNLOAD_ID;
    }

    if (mpg123_param(mh, MPG123_FLAGS, MPG123_MONO_MIX, 0) != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error forcing single channel!\n");
      return INVALID_DOWNLOAD_ID;
    }

    ConnInfo_t* conn = createDownloader(payload, rate, loop, gain, mh, mutex, buffer, cmdQueue);
    if (!conn) {
      return INVALID_DOWNLOAD_ID;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
      "start_audio_download: starting download %d\n", conn->id);


    return conn->id;
  }

  switch_status_t stop_audio_download(int id) {
    auto it = id2ConnMap.find(id);
    if (it == id2ConnMap.end()) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "stop_audio_download: id %d has already completed\n", id);
      return SWITCH_STATUS_FALSE;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
      "stop_audio_download: stopping download %d, status %s\n", id, status2String(it->second->status));

    ConnInfo_t *conn = it->second;
    auto status = conn->status;

    /* past this point I shall not access either the mutex or the buffer provided */
    conn->mutex = nullptr;
    conn->buffer = nullptr;
    /* if download is in progress set status to cancel it during next call back */
    if (status == Status_t::STATUS_DOWNLOAD_PAUSED) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "stop_audio_download: resuming download %d so we can cancel it\n", id);
      conn->status = Status_t::STATUS_STOPPING;
      curl_easy_pause(conn->easy, CURLPAUSE_CONT);
    }
    if (status != Status_t::STATUS_DOWNLOAD_IN_PROGRESS) {
      destroyConnection(conn);
    }
    conn->status = Status_t::STATUS_STOPPING;
    return SWITCH_STATUS_SUCCESS;
  }
}

/* internal */
ConnInfo_t* createDownloader(HttpPayload_t* payload, int rate, int loop, int gain, mpg123_handle *mh, switch_mutex_t *mutex, CircularBuffer_t *buffer, HttpPayloadQueue_t* cmdQueue) {
  ConnInfo_t *conn = pool.malloc() ;
  CURL* easy = createEasyHandle();

  if (!easy || !conn) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "createDownloader: failed to allocate memory\n");
    return nullptr;
  }

  memset(conn, 0, sizeof(ConnInfo_t));
  conn->easy = easy;
  conn->mutex = mutex;
  conn->buffer = buffer;
  conn->mh = mh;
  conn->loop = loop;
  conn->gain = gain;
  conn->rate = rate;
  conn->hdr_list = NULL;
  conn->global = &global;
  conn->status = Status_t::STATUS_NONE; 
  conn->timer = new boost::asio::deadline_timer(io_service);
  conn->cmdQueue = cmdQueue;
  if (!payload->body.empty())
    conn->payload.body = payload->body;
  conn->payload.headers = payload->headers;
  conn->payload.url = payload->url;

  downloadId_t id = ++currDownloadId;
  if (id == 0) id++;

  id2ConnMap[id] = conn;
  conn->id = id;

  curl_easy_setopt(easy, CURLOPT_URL, payload->url.c_str());
  curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
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

  curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);

  /* keep the speed down so we don't have to buffer large amounts*/
  curl_easy_setopt(easy, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)31415);
  /*Add request body*/
  if (!conn->payload.body.empty()) {
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, conn->payload.body.c_str());
  }
  /*Add request headers*/
  for(const auto& header : conn->payload.headers) {
    conn->hdr_list = curl_slist_append(conn->hdr_list, header.c_str());
  }
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, conn->hdr_list);

  auto rc = curl_multi_add_handle(global.multi, conn->easy);
  if (mcode_test("new_conn: curl_multi_add_handle", rc) < 0) {
    return nullptr;
  }
  conn->status = Status_t::STATUS_DOWNLOAD_IN_PROGRESS;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "createDownloader: launched request, gain %d\n", conn->gain);
  return conn;
}

void destroyConnection(ConnInfo_t *conn) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "destroyConnection\n");

  /* clean up the curl handle*/
  curl_multi_remove_handle(conn->global, conn->easy);
  curl_easy_cleanup(conn->easy);
  if( conn->hdr_list ) {
    curl_slist_free_all(conn->hdr_list);
    conn->hdr_list = nullptr ;
  }

  /* clear asio resources and free resources */
  if (conn->timer) {
    conn->timer->cancel();
    delete conn->timer;
  }
  if (conn->body) {
    free(conn->body);
    conn->body = nullptr;
  }
  if (conn->err_msg) {
    free(conn->err_msg);
  }

  /* free mp3 decoder */
  if (conn->mh) {
    mpg123_close(conn->mh);
    mpg123_delete(conn->mh);
  }

  if (conn->mutex) switch_mutex_lock(conn->mutex);
  id2ConnMap.erase(conn->id);
  if (conn->mutex) switch_mutex_unlock(conn->mutex);

  memset(conn, 0, sizeof(ConnInfo_t));
  pool.destroy(conn) ;
}

CURL* createEasyHandle(void) {
  CURL* easy = curl_easy_init();
  if(!easy) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "curl_easy_init() failed!\n");
    return nullptr ;
  }  

  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_USERAGENT, "jambonz/0.8.5");

  // set connect timeout to 3 seconds and no total timeout as files could be large
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 0L); // no timeout

  return easy ;    
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

      downloadId_t id = conn->id;
      auto mutex = conn->mutex;
      auto buffer = conn->buffer;
      auto rate = conn->rate;
      auto loop = conn->loop;
      auto gain = conn->gain;
      auto oldId = conn->id;
      auto cmdQueue = conn->cmdQueue;
      bool restart = conn->loop && conn->status != Status_t::STATUS_STOPPING && response_code == 200;
      if (!restart && !cmdQueue->empty() && conn->status != Status_t::STATUS_STOPPING) {
        conn->payload = cmdQueue->front();
        cmdQueue->pop();
        restart = true;
      }

      conn->response_code = response_code;
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "curl done, response code %d, status %s\n", response_code, status2String(conn->status));
      conn->status = Status_t::STATUS_DOWNLOAD_COMPLETE;

      curl_multi_remove_handle(g->multi, easy);

      if (restart) {
        conn->status = Status_t::STATUS_AWAITING_RESTART;
        conn->timer->expires_from_now(boost::posix_time::millisec(1000));
        conn->timer->async_wait(boost::bind(&restart_cb, boost::placeholders::_1, conn));
      }
      else {
        destroyConnection(conn);
      }
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

void remsock(int *f, GlobalInfo_t *g) {
  if(f) {
    free(f);
    f = NULL;
  }
}

/* Called by asio when there is an action on a socket */
void event_cb(GlobalInfo_t *g, curl_socket_t s, int action, const boost::system::error_code & error, int *fdp) {
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
void setsock(int *fdp, curl_socket_t s, CURL *e, int act, int oldact, GlobalInfo_t *g) {
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

void addsock(curl_socket_t s, CURL *easy, int action, GlobalInfo_t *g) {
  /* fdp is used to store current action */
  int *fdp = (int *) calloc(sizeof(int), 1);

  setsock(fdp, s, easy, action, 0, g);
  curl_multi_assign(g->multi, s, fdp);
}

int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp) {
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

void threadFunc() {      
  /* to make sure the event loop doesn't terminate when there is no work to do */
  io_service.reset() ;
  boost::asio::io_service::work work(io_service);
  
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dub threadFunc - starting\n");

  for(;;) {
      
    try {
      io_service.run() ;
      break ;
    }
    catch( std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_dub threadFunc - Error: %s\n", e.what());
    }
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dub threadFunc - ending\n");
}


/* Called by asio when our timeout expires */
void timer_cb(const boost::system::error_code & error, GlobalInfo_t *g)
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

std::vector<int16_t> convert_mp3_to_linear(ConnInfo_t *conn, int8_t *data, size_t len) {
  std::vector<int16_t> linear_data;
  int eof = 0;
  int mp3err = 0;

  if(mpg123_feed(conn->mh, (const unsigned char*) data, len) == MPG123_OK) {
    while(!eof) {
      size_t usedlen = 0;
      off_t frame_offset;
      unsigned char* audio;

      int decode_status = mpg123_decode_frame(conn->mh, &frame_offset, &audio, &usedlen);

      switch(decode_status) {
        case MPG123_NEW_FORMAT:
          continue;

        case MPG123_OK:
          {
            size_t samples = usedlen / sizeof(int16_t);
            linear_data.insert(linear_data.end(), reinterpret_cast<int16_t*>(audio), reinterpret_cast<int16_t*>(audio) + samples);
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

    if (conn->gain != 0) {
      switch_change_sln_volume_granular(linear_data.data(), linear_data.size(), conn->gain);
    }
  }

  return linear_data;
}

void restart_cb(const boost::system::error_code& error, ConnInfo_t* conn) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "restart_cb status is %s\n", status2String(conn->status));
  if (conn->status == Status_t::STATUS_AWAITING_RESTART) {
    auto payload = conn->payload;
    auto rate = conn->rate;
    auto loop = conn->loop;
    auto gain = conn->gain;
    auto mutex = conn->mutex;
    auto buffer = conn->buffer;
    auto oldId = conn->id;
    auto cmdQueue = conn->cmdQueue;

    destroyConnection(conn);

    downloadId_t id = start_audio_download(&payload, rate, loop, gain, mutex, buffer, cmdQueue);

    /* re-use id since caller is tracking that id */
    auto * newConnection = id2ConnMap[id];
    id2ConnMap[oldId] = newConnection;
    id2ConnMap.erase(id);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "restarted looped download\n");
  }
}

void throttling_cb(const boost::system::error_code& error, ConnInfo_t* conn) {
  if (conn->status == Status_t::STATUS_STOPPING || !conn->mutex || !conn->buffer) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: session gone, resume download so we can complete\n");
    curl_easy_pause(conn->easy, CURLPAUSE_CONT);
    return;
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: status is %s\n", status2String(conn->status));

  switch_mutex_lock(conn->mutex);
  if (!error) {
    auto size = conn->buffer->size();

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: size is now %ld\n", size);
    if (size < BUFFER_THROTTLE_LOW) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: resuming download\n");
      curl_easy_pause(conn->easy, CURLPAUSE_CONT);
      switch_mutex_unlock(conn->mutex);
     return;
    }

    // check back in 2 seconds
    conn->timer->expires_from_now(boost::posix_time::millisec(2000));
    conn->timer->async_wait(boost::bind(&throttling_cb, boost::placeholders::_1, conn));

  } else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: error (%d): %s\n", error.value(), error.message().c_str());

    // Handle any errors
  }
  switch_mutex_unlock(conn->mutex);
}


/* CURLOPT_WRITEFUNCTION - here is where we receive the data */
size_t write_cb(void *ptr, size_t size, size_t nmemb, ConnInfo_t *conn) {
  int8_t *data = (int8_t *) ptr;
  size_t bytes_received = size * nmemb;
  std::vector<int16_t> pcm_data;
  if (conn->status == Status_t::STATUS_STOPPING || conn->status == Status_t::STATUS_STOPPED || !conn->mutex || !conn->buffer) {
    if (conn->timer) conn->timer->cancel();
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
      "write_cb: aborting transfer, status %s, mutex %p, buffer %p\n", status2String(conn->status), conn->mutex, conn->buffer);
    /* this will abort the transfer */
    return 0;
  }
  {
    switch_mutex_lock(conn->mutex);

    if (conn->response_code > 0 && conn->response_code != 200) {
      std::string body((char *) ptr, bytes_received);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "write_cb: received body %s\n", body.c_str());
      conn->err_msg = strdup(body.c_str());
      conn->status = Status_t::STATUS_FAILED;
      switch_mutex_unlock(conn->mutex);
      return 0;
    }

    /* throttle after reaching high water mark */
    if (conn->buffer->size() > BUFFER_THROTTLE_HIGH) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "write_cb: throttling download, buffer size is %ld\n", conn->buffer->size());
      
      // check back in 2 seconds
      conn->timer->expires_from_now(boost::posix_time::millisec(2000));
      conn->timer->async_wait(boost::bind(&throttling_cb, boost::placeholders::_1, conn));

      conn->status = Status_t::STATUS_DOWNLOAD_PAUSED;
      switch_mutex_unlock(conn->mutex);
      return CURL_WRITEFUNC_PAUSE;
    }

    pcm_data = convert_mp3_to_linear(conn, data, bytes_received);
    size_t bytesResampled = pcm_data.size() * sizeof(int16_t);

    // Resize the buffer if necessary
    if (conn->buffer->capacity() - conn->buffer->size() < (bytesResampled / sizeof(int16_t))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "write_cb growing buffer, size now %ld\n", conn->buffer->size()); 

      //TODO: if buffer exceeds some max size, return CURL_WRITEFUNC_ERROR to abort the transfer
      conn->buffer->set_capacity(conn->buffer->size() + std::max((bytesResampled / sizeof(int16_t)), (size_t)BUFFER_GROW_SIZE));
    }
    
    /* Push the data into the buffer */
    conn->buffer->insert(conn->buffer->end(), pcm_data.data(), pcm_data.data() + pcm_data.size());
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "write_cb: wrote data, buffer size is now %ld\n", conn->buffer->size());

    switch_mutex_unlock(conn->mutex);
  }
  return bytes_received;
}

bool parseHeader(const std::string& str, std::string& header, std::string& value) {
    std::vector<std::string> parts;
    boost::split(parts, str, boost::is_any_of(":"), boost::token_compress_on);

    if (parts.size() != 2)
        return false;

    header = boost::trim_copy(parts[0]);
    value = boost::trim_copy(parts[1]);
    return true;
}

int extract_response_code(const std::string& input) {
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

size_t header_callback(char *buffer, size_t size, size_t nitems, ConnInfo_t *conn) {
  size_t bytes_received = size * nitems;
  const std::string prefix = "HTTP/";
  std::string header, value;
  std::string input(buffer, bytes_received);
  if (parseHeader(input, header, value)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "recv header: %s with value %s\n", header.c_str(), value.c_str());
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "recv header: %s\n", input.c_str());
    if (input.rfind(prefix, 0) == 0) {
      try {
        conn->response_code = extract_response_code(input);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parsed response code: %ld\n", conn->response_code);
      } catch (const std::invalid_argument& e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "header_callback: invalid response code %s\n", input.substr(prefix.length()).c_str());
      }
    }
  }
  return bytes_received;
}

/* CURLOPT_OPENSOCKETFUNCTION */
curl_socket_t opensocket(void *clientp, curlsocktype purpose, struct curl_sockaddr *address) {
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
int close_socket(void *clientp, curl_socket_t item) {
  //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "close_socket : %#X\n", item);

  std::map<curl_socket_t, boost::asio::ip::tcp::socket *>::iterator it = socket_map.find(item);
  if(it != socket_map.end()) {
    delete it->second;
    socket_map.erase(it);
  }
  return 0;
}