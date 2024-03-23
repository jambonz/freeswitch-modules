#ifndef __AP_FILE_H__
#define __AP_FILE_H__

#include <thread>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>

#include <mpg123.h>

#include "ap.h"

class AudioProducerFile : public AudioProducer {
public:

  typedef enum
  {
    STATUS_NONE = 0,
    STATUS_FAILED,
    STATUS_IN_PROGRESS,
    STATUS_PAUSED,
    STATUS_COMPLETE,
    STATUS_AWAITING_RESTART,
    STATUS_STOPPED
  } Status_t;

  typedef enum {
    FILE_TYPE_MP3 = 0,
    FILE_TYPE_R8
  } FileType_t;

  const char* status2String(Status_t status)
  {
    static const char* statusStrings[] = {
      "STATUS_NONE",
      "STATUS_FAILED",
      "STATUS_IN_PROGRESS",
      "STATUS_PAUSED",
      "STATUS_COMPLETE",
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

  AudioProducerFile(
    std::mutex& mutex,
    CircularBuffer_t& circularBuffer,
    int sampleRate
  );
  virtual ~AudioProducerFile();
  virtual void start(std::function<void(bool, const std::string&)> callback);
  virtual void stop();
  void cleanup(Status_t status, std::string errMsg = "");
  void reset();

  void queueFileAudio(const std::string& path, int gain = 0, bool loop = false);

  void read_cb(const boost::system::error_code& error);

  static bool initialized;
  static std::thread worker_thread;
  static boost::asio::io_service io_service;
  static void threadFunc();

private:

  static void _init();
  static void _deinit();

  void stop_file_load();

  std::string _path;
  Status_t _status;
  FileType_t _type;
  mpg123_handle *_mh;
  boost::asio::deadline_timer _timer;
  FILE* _fp;

};

#endif