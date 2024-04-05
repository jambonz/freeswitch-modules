#ifndef  __TRACK_H__
#define __TRACK_H__

#include <mutex>
#include <queue>
#include "common.h"
#include "ap.h"

class Track {
public:
  Track(const std::string& trackName, int sampleRate);
  ~Track();

  /* audio production methods */
  void queueHttpGetAudio(const std::string& url, int gain = 0, bool loop = false);
  void queueHttpPostAudio(const std::string& url, int gain = 0, bool loop = false);
  void queueHttpPostAudio(const std::string& url, const std::string& body, std::vector<std::string>& headers, const std::string& proxy, int gain = 0, bool loop = false);
  void queueFileAudio(const std::string& path, int gain = 0, bool loop = false);
  void removeAllAudio();

  void onPlayDone(bool hasError, const std::string& errMsg);

  std::string& getTrackName() { return _trackName; }

  /* audio playout methods */
  bool hasAudio();
  inline bool hasAudio_NoLock() const {
    return !_stopping && !_buffer.empty();
  }


  int retrieveAndClearAudio(int16_t* buf, int desiredSamples) {
    std::lock_guard<std::mutex> lock(_mutex);
    int samplesToCopy = std::min(static_cast<int>(_buffer.size()), desiredSamples);
    std::copy_n(_buffer.begin(), samplesToCopy, buf);
    _buffer.erase(_buffer.begin(), _buffer.begin() + samplesToCopy);
    return samplesToCopy;
  }

private:
  std::string _trackName;
  int _sampleRate;
  std::mutex _mutex;
  CircularBuffer_t _buffer;
  std::queue<std::shared_ptr<AudioProducer>> _apQueue;
  bool _stopping;
};




#endif
