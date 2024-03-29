#include "switch.h"
#include "ap_file.h"
#include "mpg_decode.h"

#define INIT_BUFFER_SIZE (80000)
#define BUFFER_GROW_SIZE (80000)
#define BUFFER_THROTTLE_LOW (40000)
#define BUFFER_THROTTLE_HIGH (160000)

bool AudioProducerFile::initialized = false;
boost::asio::io_service AudioProducerFile::io_service;
std::thread AudioProducerFile::worker_thread;

void AudioProducerFile::threadFunc() {      
  io_service.reset() ;
  boost::asio::io_service::work work(io_service);
  
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "file_loader threadFunc - starting\n");

  for(;;) {
      
    try {
      io_service.run() ;
      break ;
    }
    catch( std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "file_loader threadFunc - Error: %s\n", e.what());
    }
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "file_loader threadFunc - ending\n");
}

void AudioProducerFile::_init() {
  if (!initialized) {
    initialized = true;
    if (mpg123_init() != MPG123_OK) {
      throw std::runtime_error("AudioProducerFile::AudioProducerFile: failed to initiate MPG123");
      return ;
    }

    /* start worker thread */
    std::thread t(threadFunc) ;
    worker_thread.swap( t ) ;
  }
}

void AudioProducerFile::_deinit() {
  if (initialized) {
    initialized = false;
    io_service.stop();
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
    mpg123_exit();
  }
}
AudioProducerFile::AudioProducerFile(
  std::mutex& mutex,
  CircularBuffer_t& circularBuffer,
  int sampleRate
) : AudioProducer(mutex, circularBuffer, sampleRate), _timer(io_service), _mh(nullptr), _fp(nullptr) {

  AudioProducerFile::_init();
}

AudioProducerFile::~AudioProducerFile() {
  reset();
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AudioProducerFile::~AudioProducerFile %p\n", (void *)this);
}

void AudioProducerFile::queueFileAudio(const std::string& path, int gain, bool loop) {
    _path = path;
    _gain = gain;
    _loop = loop;

    /* we only handle mp3 or r8 files atm */
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) {
      throw std::runtime_error("file " + path + " has no extension");
    }
    auto filetype = path.substr(pos + 1);
    if (0 == filetype.compare("mp3")) _type = FILE_TYPE_MP3;
    else if (0 == filetype.compare("r8")) _type = FILE_TYPE_R8;
    else throw std::runtime_error("file " + path + " has unsupported extension " + filetype);
}

void AudioProducerFile::start(std::function<void(bool, const std::string&)> callback) {
    int mhError = 0;

    _callback = callback;

    /* allocate handle for mpeg decoding */
    _mh = mpg123_new("auto", &mhError);
    if (!_mh) {
      const char *mhErr = mpg123_plain_strerror(mhError);
      throw std::runtime_error("Error allocating mpg123 handle! " + std::string(mhErr));
    }

    if (mpg123_open_feed(_mh) != MPG123_OK) throw std::runtime_error("Error mpg123_open_feed!");
    if (mpg123_format_all(_mh) != MPG123_OK) throw std::runtime_error("Error mpg123_format_all!");
    if (mpg123_param(_mh, MPG123_FORCE_RATE, _sampleRate, 0) != MPG123_OK) throw std::runtime_error("Error forcing resample to 8k!");
    if (mpg123_param(_mh, MPG123_FLAGS, MPG123_MONO_MIX, 0) != MPG123_OK) throw std::runtime_error("Error forcing single channel!");
    
    _fp = fopen(_path.c_str(), "rb");
    if (!_fp) throw std::runtime_error("Error opening file " + _path);

    _status = Status_t::STATUS_AWAITING_RESTART;

    /* do the initial read in the worker thread so we don't block here */
    _timer.expires_from_now(boost::posix_time::millisec(1));
    _timer.async_wait(boost::bind(&AudioProducerFile::read_cb, this, boost::placeholders::_1));
}

void AudioProducerFile::read_cb(const boost::system::error_code& error) {
  if (_status == Status_t::STATUS_STOPPED) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: session gone\n");
    return;
  }
  if (_status == Status_t::STATUS_AWAITING_RESTART) {
    _status = Status_t::STATUS_IN_PROGRESS;
  }
  if (!error) {
    size_t size = _buffer.size();
    if (size < BUFFER_THROTTLE_LOW) {
      std::vector<int16_t> pcm_data;
      int8_t buf[INIT_BUFFER_SIZE];

      size_t bytesRead = ::fread(buf, sizeof(int8_t), INIT_BUFFER_SIZE, _fp);
      if (bytesRead <= 0) {
        if (::feof(_fp)) switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %p eof\n", (void *) this);
        else if (::ferror(_fp)) switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "read_cb: %p error reading file\n", (void *) this);
        else switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "read_cb: %p unknown error reading file\n", (void *) this);
        _status = Status_t::STATUS_COMPLETE;
        return;
      }

      if (_type == FileType_t::FILE_TYPE_MP3) pcm_data = convert_mp3_to_linear(_mh, _gain, buf, bytesRead);
      else pcm_data = std::vector<int16_t>(reinterpret_cast<int16_t*>(buf), reinterpret_cast<int16_t*>(buf) + bytesRead / 2);

      {
        std::lock_guard<std::mutex> lock(_mutex); 

        // Resize the buffer if necessary
        size_t capacity = _buffer.capacity();
        if (capacity - size < pcm_data.size()) {
          //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "write_cb %p growing buffer, size now %ld\n", (void *) this, size); 
          _buffer.set_capacity(size + std::max(pcm_data.size(), (size_t)BUFFER_GROW_SIZE));
        }
        
        /* Push the data into the buffer */
        _buffer.insert(_buffer.end(), pcm_data.data(), pcm_data.data() + pcm_data.size());
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %p wrote data, buffer size is now %ld\n", (void *) this, _buffer.size());        
      }

      if (bytesRead < INIT_BUFFER_SIZE) {
        _status = Status_t::STATUS_COMPLETE;
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u reached end of file, status is %s\n", (void *) this, status2String(_status));
      }
    }

    if (_status == Status_t::STATUS_COMPLETE) {
      cleanup(Status_t::STATUS_COMPLETE);
    }
    else {
      // read more in 2 seconds
      _timer.expires_from_now(boost::posix_time::millisec(2000));
      _timer.async_wait(boost::bind(&AudioProducerFile::read_cb, this, boost::placeholders::_1));
    }
  } else {
    cleanup(Status_t::STATUS_FAILED, error.message());
  }
}

void AudioProducerFile::stop() {
  cleanup(Status_t::STATUS_STOPPED, "");
}

void AudioProducerFile::reset() {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_fp) {
      fclose(_fp);
      _fp = nullptr;
    }
    if (_mh) {
      mpg123_close(_mh);
      mpg123_delete(_mh);
      _mh = nullptr;
    }
  }
  _timer.cancel();
  _status = Status_t::STATUS_NONE;
}

void AudioProducerFile::cleanup(Status_t status, std::string errMsg) {
  reset();
  _status = status;
  notifyDone(status != Status_t::STATUS_COMPLETE, errMsg);
}