#ifndef __AP_HTTP_H__
#define __AP_HTTP_H__

#include "ap.h"
#include <curl/curl.h>
#include <mpg123.h>
#include <boost/asio.hpp>


typedef struct
{
  CURLM *multi;
  int still_running;
} GlobalInfo_t;

class AudioProducerHttp : public AudioProducer {
public:

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

  typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST
  } HttpMethod_t;

  static const char* status2String(Status_t status) {
    static const char* statusStrings[] = {
      "STATUS_NONE",
      "STATUS_FAILED",
      "STATUS_DOWNLOAD_IN_PROGRESS",
      "STATUS_DOWNLOAD_PAUSED",
      "STATUS_DOWNLOAD_COMPLETE",
      "STATUS_AWAITING_RESTART",
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
  AudioProducerHttp(
    std::mutex& mutex,
    CircularBuffer_t& circularBuffer,
    int sampleRate
  );
  virtual ~AudioProducerHttp();

  virtual void start(std::function<void(bool, const std::string&)> callback);
  void addCurlHandle(const boost::system::error_code& error);
  virtual void stop();
  void cleanup(Status_t status, int response_code);
  void reset();

  void queueHttpGetAudio(const std::string& url, int gain = 0, bool loop = false);
  void queueHttpPostAudio(const std::string& url, int gain = 0, bool loop = false);
  void queueHttpPostAudio(const std::string& url, const std::string& body, std::vector<std::string>& headers, const std::string& proxy, int gain = 0, bool loop = false);

  Status_t getStatus() const { return _status; }
  void setStatus(Status_t status) { _status = status; }

  boost::asio::deadline_timer& getTimer() { return _timer; }
  
  static bool initialized;
  static std::thread worker_thread;
  static boost::asio::io_service io_service;
  static void threadFunc();
  static std::map<curl_socket_t, boost::asio::ip::tcp::socket *> socket_map;
  static boost::asio::deadline_timer multi_timer;


  static GlobalInfo_t global;

  static int sock_cb(CURL *e, curl_socket_t s, int what, void *cbp, void *sockp);
  static int multi_timer_cb(CURLM *multi, long timeout_ms, GlobalInfo_t *g);
  static void timer_cb(const boost::system::error_code & error, GlobalInfo_t *g);
  static void addsock(curl_socket_t s, CURL *easy, int action, GlobalInfo_t *g);
  static void setsock(int *fdp, curl_socket_t s, CURL *e, int act, int oldact, GlobalInfo_t *g);
  static int mcode_test(const char *where, CURLMcode code);
  static void check_multi_info(GlobalInfo_t *g);
  static void event_cb(GlobalInfo_t *g, curl_socket_t s, int action, const boost::system::error_code & error, int *fdp);
  static void remsock(int *f, GlobalInfo_t *g);
  
  static size_t static_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);
  size_t write_cb(void *ptr, size_t size, size_t nmemb);

  static size_t static_header_callback(char *buffer, size_t size, size_t nitems, void* userdata);
  size_t header_callback(char *buffer, size_t size, size_t nitems);

  void throttling_cb(const boost::system::error_code& error);

  static void static_restart_cb(const boost::system::error_code& error, void* userdata);
  void restart_cb(const boost::system::error_code& error);

  static int close_socket(void *clientp, curl_socket_t item);
  static curl_socket_t open_socket(void *clientp, curlsocktype purpose, struct curl_sockaddr *address);

private:

  static void _init();
  static void _deinit();

  CURL* createEasyHandle();
  bool parseHeader(const std::string& str, std::string& header, std::string& value);
  int extract_response_code(const std::string& input);

  HttpMethod_t _method;
  std::string _url;
  std::string _body;
  std::string _proxy;
  std::vector<std::string> _headers;
  Status_t _status;
  mpg123_handle *_mh;
  CURL *_easy;
  char _error[CURL_ERROR_SIZE]; // curl error buffer
  std::string _err_msg;
  int _response_code;
  boost::asio::deadline_timer _timer;
};

#endif