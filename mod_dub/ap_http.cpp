#include <functional>
#include <mpg123.h>
#include "switch.h"
#include "ap_http.h"
#include "mpg_decode.h"

#include <boost/asio/ssl.hpp>
#include <boost/pool/object_pool.hpp>
#include <boost/bind/bind.hpp>
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/assign/list_of.hpp>

#define BUFFER_GROW_SIZE (80000)
#define BUFFER_THROTTLE_LOW (40000)
#define BUFFER_THROTTLE_HIGH (160000)

bool AudioProducerHttp::initialized = false;
boost::asio::io_service AudioProducerHttp::io_service;
std::thread AudioProducerHttp::worker_thread;
GlobalInfo_t AudioProducerHttp::global;
std::map<curl_socket_t, boost::asio::ip::tcp::socket *> AudioProducerHttp::socket_map;
boost::asio::deadline_timer AudioProducerHttp::multi_timer(AudioProducerHttp::io_service);

void AudioProducerHttp::threadFunc() {      
  /* to make sure the event loop doesn't terminate when there is no work to do */
  io_service.reset() ;
  boost::asio::io_service::work work(io_service);
  
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ap_http threadFunc - starting\n");

  for(;;) {
      
    try {
      io_service.run() ;
      break ;
    }
    catch( std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "ap_http threadFunc - Error: %s\n", e.what());
    }
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ap_http threadFunc - ending\n");
}

void AudioProducerHttp::_init() {
  if (!initialized) {
    initialized = true;
    if (mpg123_init() != MPG123_OK) {
      throw std::runtime_error("AudioProducerFile::AudioProducerFile: failed to initiate MPG123");
      return ;
    }

    memset(&global, 0, sizeof(GlobalInfo_t));
    global.multi = curl_multi_init();
    if (!global.multi) {
      throw std::runtime_error("AudioProducerHttp::_init: failed to initiate CURL multi");
      return ;
    }

    curl_multi_setopt(global.multi, CURLMOPT_SOCKETFUNCTION, &AudioProducerHttp::sock_cb);
    curl_multi_setopt(global.multi, CURLMOPT_SOCKETDATA, &global);
    curl_multi_setopt(global.multi, CURLMOPT_TIMERFUNCTION, &AudioProducerHttp::multi_timer_cb);
    curl_multi_setopt(global.multi, CURLMOPT_TIMERDATA, &global);
    curl_multi_setopt(global.multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    /* start worker thread */
    std::thread t(threadFunc) ;
    worker_thread.swap( t ) ;
  }
}

void AudioProducerHttp::_deinit() {
  if (initialized) {
    initialized = false;
    io_service.stop();
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
    /* cleanup curl multi handle*/
    curl_multi_cleanup(global.multi);

    mpg123_exit();
  }
}

AudioProducerHttp::AudioProducerHttp(
    std::mutex& mutex,
    CircularBuffer_t& circularBuffer,
    int sampleRate
) : AudioProducer(mutex, circularBuffer, sampleRate), _status(Status_t::STATUS_NONE), _mh(nullptr), _easy(nullptr), 
    _error{0}, _response_code(0), _timer(io_service) {

  AudioProducerHttp::_init();
}

AudioProducerHttp::~AudioProducerHttp() {
  reset();
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AudioProducerFile::~AudioProducerFile %p\n", (void *)this);
}

void AudioProducerHttp::start(std::function<void(bool, const std::string&)> callback) {
  int mhError = 0;

  _callback = callback;
  memset(_error, 0, sizeof(_error));

  /* allocate handle for mpeg decoding */
  _mh = mpg123_new("auto", &mhError);
  if (!_mh) {
    const char *mhErr = mpg123_plain_strerror(mhError);
    throw std::runtime_error("Error allocating mpg123 handle! " + std::string(mhErr));
  }

  if (mpg123_open_feed(_mh) != MPG123_OK) throw std::runtime_error("Error mpg123_open_feed!\n");
  if (mpg123_format_all(_mh) != MPG123_OK) throw std::runtime_error("Error mpg123_format_all!\n");
  if (mpg123_param(_mh, MPG123_FORCE_RATE, _sampleRate, 0) != MPG123_OK) throw std::runtime_error("Error forcing resample to 8k!\n");
  if (mpg123_param(_mh, MPG123_FLAGS, MPG123_MONO_MIX, 0) != MPG123_OK) throw std::runtime_error("Error forcing single channel!\n");

  _easy = createEasyHandle();
  if (!_easy) throw std::runtime_error("Error creating easy handle!\n");

  curl_easy_setopt(_easy, CURLOPT_WRITEFUNCTION, &AudioProducerHttp::static_write_cb);
  curl_easy_setopt(_easy, CURLOPT_URL, _url.c_str());
  curl_easy_setopt(_easy, CURLOPT_HTTPGET, _method == HttpMethod_t::HTTP_METHOD_GET ? 1L : 0L);
  curl_easy_setopt(_easy, CURLOPT_WRITEDATA, this);
  curl_easy_setopt(_easy, CURLOPT_ERRORBUFFER, _error);
  curl_easy_setopt(_easy, CURLOPT_PRIVATE, this);
  curl_easy_setopt(_easy, CURLOPT_VERBOSE, 0L);
  curl_easy_setopt(_easy, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(_easy, CURLOPT_HEADERFUNCTION, &AudioProducerHttp::static_header_callback);
  curl_easy_setopt(_easy, CURLOPT_HEADERDATA, this);
  
  /* call this function to get a socket */
  curl_easy_setopt(_easy, CURLOPT_OPENSOCKETFUNCTION, &AudioProducerHttp::open_socket);

  /* call this function to close a socket */
  curl_easy_setopt(_easy, CURLOPT_CLOSESOCKETFUNCTION, &AudioProducerHttp::close_socket);

  // libcurl adding random byte to the response body that creates white noise to audio file
  // https://github.com/curl/curl/issues/10525
  const bool disable_http_2 = switch_true(std::getenv("DISABLE_HTTP2_FOR_TTS_STREAMING"));
  curl_easy_setopt(_easy, CURLOPT_HTTP_VERSION, disable_http_2 ? CURL_HTTP_VERSION_1_1 : CURL_HTTP_VERSION_2_0);

  /* keep the speed down so we don't have to buffer large amounts*/
  curl_easy_setopt(_easy, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)31415);
  /*Add request body*/
  if (!_body.empty()) curl_easy_setopt(_easy, CURLOPT_POSTFIELDS, _body.c_str());
  /*Add request proxy*/
  if (!_proxy.empty()) curl_easy_setopt(_easy, CURLOPT_PROXY, _proxy.c_str());

  /*Add request headers*/
  struct curl_slist *hdr_list = nullptr;
  for(const auto& header : _headers) {
    hdr_list = curl_slist_append(hdr_list, header.c_str());
  }
  if (hdr_list) curl_easy_setopt(_easy, CURLOPT_HTTPHEADER, hdr_list);

  _status = Status_t::STATUS_AWAITING_RESTART;

  _timer.expires_from_now(boost::posix_time::millisec(1));
  _timer.async_wait(boost::bind(&AudioProducerHttp::addCurlHandle, this, boost::placeholders::_1));
}
void AudioProducerHttp::queueHttpGetAudio(const std::string& url, int gain, bool loop) {
  _method = HttpMethod_t::HTTP_METHOD_GET;
  _url = url;
  _gain = gain;
  _loop = loop;
}
void AudioProducerHttp::queueHttpPostAudio(const std::string& url, int gain, bool loop) {
  _method = HttpMethod_t::HTTP_METHOD_POST;
  _url = url;
  _gain = gain;
  _loop = loop;
}
void AudioProducerHttp::queueHttpPostAudio(const std::string& url, const std::string& body, std::vector<std::string>& headers, const std::string& proxy, int gain, bool loop) {
  _method = HttpMethod_t::HTTP_METHOD_POST;
  _url = url;
  _body = body;
  _headers = headers;
  _proxy = proxy;
  _gain = gain;
  _loop = loop;
}

void AudioProducerHttp::addCurlHandle(const boost::system::error_code& error) {
  if (_status == Status_t::STATUS_AWAITING_RESTART) {
    auto rc = curl_multi_add_handle(global.multi, _easy);
    if (mcode_test("new_conn: curl_multi_add_handle", rc) < 0) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AudioProducerHttp::addCurlHandle: Error adding easy handle to multi handle\n");
    }
    else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AudioProducerHttp::addCurlHandle retrieving from %s\n", _url.c_str());
    }
  }
}

CURL* AudioProducerHttp::createEasyHandle(void) {
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

size_t AudioProducerHttp::static_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  return static_cast<AudioProducerHttp*>(userdata)->write_cb(ptr, size, nmemb);
}

size_t AudioProducerHttp::write_cb(void *ptr, size_t size, size_t nmemb) {
  int8_t *data = (int8_t *) ptr;
  size_t bytes_received = size * nmemb;
  std::vector<int16_t> pcm_data;
  if (_status == Status_t::STATUS_STOPPING || _status == Status_t::STATUS_STOPPED) {
    _timer.cancel();
    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
    //  "AudioProducerHttp::write_cb: aborting transfer, status %s, mutex %p, buffer %p\n", status2String(_status));
    /* this will abort the transfer */
    return 0;
  }
  if (_response_code > 0 && _response_code != 200) {
    std::string body((char *) ptr, bytes_received);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "AudioProducerHttp::write_cb: received body %s\n", body.c_str());
    _err_msg = body;
    _status = Status_t::STATUS_FAILED;
    return 0;
  }

  /* throttle after reaching high water mark */
  size_t bufSize = _buffer.size();
  if (bufSize > BUFFER_THROTTLE_HIGH) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AudioProducerHttp::write_cb: throttling download, buffer size is %ld\n", _buffer.size());
    
    // check back in 2 seconds
    _timer.expires_from_now(boost::posix_time::millisec(2000));
    _timer.async_wait(boost::bind(&AudioProducerHttp::throttling_cb, this, boost::placeholders::_1));

    _status = Status_t::STATUS_DOWNLOAD_PAUSED;
    return CURL_WRITEFUNC_PAUSE;
  }

  pcm_data = convert_mp3_to_linear(_mh, _gain, data, bytes_received);
  size_t samples = pcm_data.size();
  std::lock_guard<std::mutex> lock(_mutex); 

  // Resize the buffer if necessary
  if (_buffer.capacity() - bufSize < samples) {
    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AudioProducerHttp::write_cb growing buffer, size now %ld\n", _buffer.size()); 

    //TODO: if buffer exceeds some max size, return CURL_WRITEFUNC_ERROR to abort the transfer
    _buffer.set_capacity(_buffer.size() + std::max(samples, (size_t)BUFFER_GROW_SIZE));
  }
  //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AudioProducerHttp::write_cb: writing %ld samples to buffer\n", pcm_data.size());
  
  /* Push the data into the buffer */
  _buffer.insert(_buffer.end(), pcm_data.data(), pcm_data.data() + pcm_data.size());
  return bytes_received;

}

size_t AudioProducerHttp::static_header_callback(char *buffer, size_t size, size_t nitems, void* userdata) {
  return static_cast<AudioProducerHttp*>(userdata)->header_callback(buffer, size, nitems);
}

size_t AudioProducerHttp::header_callback(char *buffer, size_t size, size_t nitems) {
  size_t bytes_received = size * nitems;
  const std::string prefix = "HTTP/";
  std::string header, value;
  std::string input(buffer, bytes_received);
  if (parseHeader(input, header, value)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "header_callback: %s with value %s\n", header.c_str(), value.c_str());
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "header_callback: %s\n", input.c_str());
    if (input.rfind(prefix, 0) == 0) {
      try {
        _response_code = extract_response_code(input);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "header_callback: parsed response code: %ld\n", _response_code);
      } catch (const std::invalid_argument& e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "header_callback: invalid response code %s\n", input.substr(prefix.length()).c_str());
      }
    }
  }
  return bytes_received;
}

void AudioProducerHttp::throttling_cb(const boost::system::error_code& error) {
  if (_status == Status_t::STATUS_STOPPING) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: session gone, resume download so we can complete\n");
    curl_easy_pause(_easy, CURLPAUSE_CONT);
    return;
  }
  //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: status is %s\n", status2String(_status));

  if (!error) {
    auto size = _buffer.size();

    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: size is now %ld\n", size);
    if (size < BUFFER_THROTTLE_LOW) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: resuming download\n");
      curl_easy_pause(_easy, CURLPAUSE_CONT);
     return;
    }

    // check back in 2 seconds
    _timer.expires_from_now(boost::posix_time::millisec(2000));
    _timer.async_wait(boost::bind(&AudioProducerHttp::throttling_cb, this, boost::placeholders::_1));

  } else if (125 == error.value()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "throttling_cb: timer canceled\n");
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "throttling_cb: error (%d): %s\n", error.value(), error.message().c_str());
  }
}

int AudioProducerHttp::sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp) {
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

void AudioProducerHttp::timer_cb(const boost::system::error_code & error, GlobalInfo_t *g)
{
  //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "timer_cb\n");

  if(!error) {
    CURLMcode rc = curl_multi_socket_action(g->multi, CURL_SOCKET_TIMEOUT, 0, &g->still_running);
    mcode_test("timer_cb: curl_multi_socket_action", rc);
    check_multi_info(g);
  }
}

int AudioProducerHttp::multi_timer_cb(CURLM *multi, long timeout_ms, GlobalInfo_t *g) {

  /* cancel running timer */
  multi_timer.cancel();

  if(timeout_ms >= 0) {
    // from libcurl 7.88.1-10+deb12u4 does not allow call curl_multi_socket_action or curl_multi_perform in curl_multi callback directly
    multi_timer.expires_from_now(boost::posix_time::millisec(timeout_ms ? timeout_ms : 1));
    multi_timer.async_wait(boost::bind(&timer_cb, boost::placeholders::_1, g));
  }

  return 0;
}

curl_socket_t AudioProducerHttp::open_socket(void *clientp, curlsocktype purpose, struct curl_sockaddr *address) {
  curl_socket_t sockfd = CURL_SOCKET_BAD;

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

int AudioProducerHttp::close_socket(void *clientp, curl_socket_t item) {
  std::map<curl_socket_t, boost::asio::ip::tcp::socket *>::iterator it = socket_map.find(item);
  if(it != socket_map.end()) {
    delete it->second;
    socket_map.erase(it);
  }
  return 0;
}

int AudioProducerHttp::mcode_test(const char *where, CURLMcode code) {
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

bool AudioProducerHttp::parseHeader(const std::string& str, std::string& header, std::string& value) {
  std::vector<std::string> parts;
  boost::split(parts, str, boost::is_any_of(":"), boost::token_compress_on);

  if (parts.size() != 2)
      return false;

  header = boost::trim_copy(parts[0]);
  value = boost::trim_copy(parts[1]);
  return true;
}

int AudioProducerHttp::extract_response_code(const std::string& input) {
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

void AudioProducerHttp::setsock(int *fdp, curl_socket_t s, CURL *e, int act, int oldact, GlobalInfo_t *g) {
  std::map<curl_socket_t, boost::asio::ip::tcp::socket *>::iterator it = socket_map.find(s);

  if(it == socket_map.end()) {
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

/* Called by asio when there is an action on a socket */
void AudioProducerHttp::event_cb(GlobalInfo_t *g, curl_socket_t s, int action, const boost::system::error_code & error, int *fdp) {
  int f = *fdp;


  // Socket already POOL REMOVED.
  if (f == CURL_POLL_REMOVE) {
    remsock(fdp, g);
    return;
  }

  if(socket_map.find(s) == socket_map.end()) {
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
      multi_timer.cancel();
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

void AudioProducerHttp::remsock(int *f, GlobalInfo_t *g) {
  if(f) {
    free(f);
    f = NULL;
  }
}

void AudioProducerHttp::addsock(curl_socket_t s, CURL *easy, int action, GlobalInfo_t *g) {
  /* fdp is used to store current action */
  int *fdp = (int *) calloc(sizeof(int), 1);

  setsock(fdp, s, easy, action, 0, g);
  curl_multi_assign(g->multi, s, fdp);
}

/* Check for completed transfers, and remove their easy handles */
void AudioProducerHttp::check_multi_info(GlobalInfo_t *g) {
  CURLMsg *msg;
  int msgs_left;
  AudioProducerHttp *ap;
  CURL *easy;
  CURLcode res;
  
  while((msg = curl_multi_info_read(g->multi, &msgs_left))) {
    if(msg->msg == CURLMSG_DONE) {
      long response_code;
      double namelookup=0, connect=0, total=0 ;
      char *ct = NULL ;

      easy = msg->easy_handle;
      res = msg->data.result;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ap);
      curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &response_code);
      curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &ct);

      curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME, &namelookup);
      curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &connect);
      curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &total);

      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "curl done, response code %d, status %s\n", response_code, status2String(ap->getStatus()));

      bool restart = ap->isLoopedAudio() && ap->getStatus() != Status_t::STATUS_STOPPING && response_code == 200;
      if (restart) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "restarting looped audio\n");
        ap->getTimer().expires_from_now(boost::posix_time::millisec(1000));
        ap->getTimer().async_wait(boost::bind(&AudioProducerHttp::static_restart_cb, boost::placeholders::_1, ap));
      }
      else {
        ap->cleanup(Status_t::STATUS_DOWNLOAD_COMPLETE, (int) response_code);
      }
    }
  }
}

void AudioProducerHttp::static_restart_cb(const boost::system::error_code& error, void* userdata) {
  static_cast<AudioProducerHttp*>(userdata)->restart_cb(error);
}

void AudioProducerHttp::restart_cb(const boost::system::error_code& error) {
  if (_status == Status_t::STATUS_STOPPING) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "restart_cb: session gone\n");
    return;
  }
  if (!error) {
    reset();
    start(_callback);
  }
  else if (error.value() == boost::asio::error::operation_aborted) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "restart_cb: %s, cancelling retrieve\n", error.message().c_str());
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "restart_cb: error (%d): %s\n", error.value(), error.message().c_str());
  }
}


void AudioProducerHttp::stop() {
  cleanup(Status_t::STATUS_STOPPED, 0);
  return;
}

void AudioProducerHttp::reset() {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_easy) {
      curl_multi_remove_handle(global.multi, _easy);
      curl_easy_cleanup(_easy);
      _easy = nullptr;
    }
    if (_mh) {
      mpg123_close(_mh);
      mpg123_delete(_mh);
      _mh = nullptr;
    }
  }
  _err_msg.clear();
  _response_code = 0;
  _timer.cancel();
  _status = Status_t::STATUS_NONE;
}

void AudioProducerHttp::cleanup(Status_t status, int response_code) {
  std::string errMsg = response_code > 200 ? "http response: " + std::to_string(response_code) : "";
  reset();
  _status = status;
  notifyDone(!errMsg.empty(), errMsg);
}