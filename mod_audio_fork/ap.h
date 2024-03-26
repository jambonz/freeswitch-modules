#ifndef __AP_H__
#define __AP_H__

#include <mutex>
#include <functional>
#include "common.h"

class AudioProducer {
public:
  AudioProducer(
    std::mutex& mutex,
    CircularBuffer_t& circularBuffer,
    int sampleRate
  ) : _mutex(mutex), _buffer(circularBuffer), _sampleRate(sampleRate), _notified(false), _loop(false), _gain(0) {}
  virtual ~AudioProducer() {}

  virtual void notifyDone(bool error, const std::string& errorMsg) {
    if (!_notified) {
      _notified = true;
      if (_callback) _callback(error, errorMsg);
    }
  }
  virtual void start(std::function<void(bool, const std::string&)> callback) = 0;
  virtual void stop() = 0;

  bool isLoopedAudio() const { return _loop; }

protected:
  std::mutex& _mutex;
  CircularBuffer_t& _buffer;
  int _sampleRate;
  int _gain;
  bool _loop;
  std::function<void(bool, const std::string&)> _callback;
  bool _notified;
};




#endif
