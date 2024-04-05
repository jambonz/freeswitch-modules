#include "track.h"
#include "ap_file.h"
#include "ap_http.h"
#include "switch.h"

#define INIT_BUFFER_SIZE (80000)

Track::Track(const std::string& trackName, int sampleRate) : _trackName(trackName), _sampleRate(sampleRate), 
  _buffer(INIT_BUFFER_SIZE), _stopping(false)
{
}

Track::~Track() {
  removeAllAudio();
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Track::~Track: track %s\n", _trackName.c_str());
}

/**
 * @brief called when an audio producer has finished retrieving the audio.
 * If we have another producer queued, then start it.
 * 
 * @param hasError 
 * @param errMsg 
 */
void Track::onPlayDone(bool hasError, const std::string& errMsg) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Track::onPlayDone %s\n", _trackName.c_str());

  if (!_stopping) {
    std::shared_ptr<AudioProducer> apDone, apNext;  // to retain a ref so the ap is not destroyed while under lock below -- avoid deadlock
  
    if (hasError) switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "onPlayDone: error: %s\n", errMsg.c_str());
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (!_apQueue.empty()) {
        apDone = _apQueue.front();
        _apQueue.pop();
      }
       if (!_apQueue.empty()) apNext = _apQueue.front();      
    }
    if (apNext) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onPlayDone: starting queued audio on track %s\n", _trackName.c_str());
      apNext->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    }
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "onPlayDone: track %s stopping\n", _trackName.c_str());
  }
}

void Track::queueFileAudio(const std::string& path, int gain, bool loop) {
  bool startIt = false;
  if (_stopping) return;

  auto ap = std::make_shared<AudioProducerFile>(_mutex, _buffer, _sampleRate);
  ap->queueFileAudio(path, gain, loop);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _apQueue.push(ap);
    startIt = _apQueue.size() == 1;
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
    "queueFileAudio: queue file %s on track %s\n", _trackName.c_str(), path.c_str());

  if (startIt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
      "queueFileAudio: track %s starting queued file as none are in progress\n", _trackName.c_str());
    try {
      ap->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    } catch (std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, 
        "queueFileAudio: track %s error starting %s: %s\n", _trackName.c_str(), path.c_str(), e.what());
      onPlayDone(true, e.what());
    }
  }
}

void Track::queueHttpGetAudio(const std::string& url, int gain, bool loop) {
  bool startIt = false;
  if (_stopping) return;
  auto ap = std::make_shared<AudioProducerHttp>(_mutex, _buffer, _sampleRate);
  ap->queueHttpGetAudio(url, gain, loop);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _apQueue.push(ap);
    startIt = _apQueue.size() == 1;
  }

  if (startIt) {
    try {
      ap->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    } catch (std::exception& e) {
      onPlayDone(true, e.what());
    }
  }
}

void Track::queueHttpPostAudio(const std::string& url, int gain, bool loop) {
  bool startIt = false;
  if (_stopping) return;
  auto ap = std::make_shared<AudioProducerHttp>(_mutex, _buffer, _sampleRate);
  ap->queueHttpPostAudio(url, gain, loop);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _apQueue.push(ap);
    startIt = _apQueue.size() == 1;
  }

  if (startIt) {
    try {
      ap->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    } catch (std::exception& e) {
      onPlayDone(true, e.what());
    }
  }
}

void Track::queueHttpPostAudio(const std::string& url, const std::string& body, std::vector<std::string>& headers, const std::string& proxy, int gain, bool loop) {
  bool startIt = false;
  if (_stopping) return;
  auto ap = std::make_shared<AudioProducerHttp>(_mutex, _buffer, _sampleRate);
    ap->queueHttpPostAudio(url, body, headers, proxy, gain, loop);
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _apQueue.push(ap);
    startIt = _apQueue.size() == 1;
  }

  if (startIt) {
    try {
      ap->start(std::bind(&Track::onPlayDone, this, std::placeholders::_1, std::placeholders::_2));
    } catch (std::exception& e) {
      onPlayDone(true, e.what());
    }
  }
}

 bool Track::hasAudio() {
  if (_stopping) return false;
  std::lock_guard<std::mutex> lock(_mutex); 
  return hasAudio_NoLock();
}

void Track::removeAllAudio() {
  _stopping = true;
  std::queue<std::shared_ptr<AudioProducer>> apQueueCopy;
  {
    std::lock_guard<std::mutex> lock(_mutex); 
    apQueueCopy = _apQueue;
    _apQueue = std::queue<std::shared_ptr<AudioProducer>>();
  }
  
  while (!apQueueCopy.empty()) {
    auto ap = apQueueCopy.front();
    apQueueCopy.pop();
    ap->stop();
  }
}