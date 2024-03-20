#include "track.h"
#include "ap_file.h"
#include "ap_http.h"
#include "switch.h"

#define INIT_BUFFER_SIZE (80000)

Track::Track(const std::string& trackName, int sampleRate) : _trackName(trackName), _sampleRate(sampleRate), 
  _buffer(INIT_BUFFER_SIZE)
{

}

Track::~Track() {
  removeAllAudio();
}

/**
 * @brief called when an audio producer has finished retrieving the audio.
 * If we have another producer queued, then start it.
 * 
 * @param hasError 
 * @param errMsg 
 */
void Track::onPlayDone(bool hasError, const std::string& errMsg) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onPlayDone: error? %s %s\n", (hasError ? "yes" : "no"), errMsg.c_str());
  std::lock_guard<std::mutex> lock(_mutex);
  _apQueue.pop();
  if (!_apQueue.empty()) {
    _apQueue.front()->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
  }
}

void Track::queueFileAudio(const std::string& path, int gain, bool loop) {
  bool startIt = false;

  auto ap = std::make_shared<AudioProducerFile>(_mutex, _buffer, _sampleRate);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _apQueue.push(ap);
    startIt = _apQueue.size() == 1;
  }

  if (startIt) {
    ap->queueFileAudio(path, gain, loop);
    try {
      ap->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    } catch (std::exception& e) {
      onPlayDone(true, e.what());
    }
  }
}

void Track::queueHttpGetAudio(const std::string& url, int gain, bool loop) {
  bool startIt = false;
  auto ap = std::make_shared<AudioProducerHttp>(_mutex, _buffer, _sampleRate);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _apQueue.push(ap);
    startIt = _apQueue.size() == 1;
  }

  if (startIt) {
    ap->queueHttpGetAudio(url, gain, loop);
    try {
      ap->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    } catch (std::exception& e) {
      onPlayDone(true, e.what());
    }
  }
}

void Track::queueHttpPostAudio(const std::string& url, int gain, bool loop) {
  bool startIt = false;
  auto ap = std::make_shared<AudioProducerHttp>(_mutex, _buffer, _sampleRate);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _apQueue.push(ap);
    startIt = _apQueue.size() == 1;
  }

  if (startIt) {
    ap->queueHttpPostAudio(url, gain, loop);
    try {
      ap->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    } catch (std::exception& e) {
      onPlayDone(true, e.what());
    }
  }
}

void Track::queueHttpPostAudio(const std::string& url, const std::string& body, std::vector<std::string>& headers, int gain, bool loop) {
  bool startIt = false;
  auto ap = std::make_shared<AudioProducerHttp>(_mutex, _buffer, _sampleRate);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _apQueue.push(ap);
    startIt = _apQueue.size() == 1;
  }

  if (startIt) {
    ap->queueHttpPostAudio(url, body, headers, gain, loop);
    try {
      ap->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    } catch (std::exception& e) {
      onPlayDone(true, e.what());
    }
  }
}

bool Track::hasAudio() {
  std::lock_guard<std::mutex> lock(_mutex); 
  return hasAudio_NoLock();
}
bool Track::hasAudio_NoLock() const {
  return !_buffer.empty();
}

void Track::removeAllAudio() {
  std::lock_guard<std::mutex> lock(_mutex); 
  
  /* clear the queue */
  while (!_apQueue.empty()) {
    auto ap = _apQueue.front();
    _apQueue.pop();
    ap->stop();
  }
}