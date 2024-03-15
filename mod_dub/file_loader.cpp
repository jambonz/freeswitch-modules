#include "file_loader.h"

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

#define INIT_BUFFER_SIZE (80000)
#define BUFFER_GROW_SIZE (80000)
#define BUFFER_THROTTLE_LOW (40000)
#define BUFFER_THROTTLE_HIGH (160000)

static uint16_t currDownloadId = 0;

typedef enum
{
  STATUS_NONE = 0,
  STATUS_FAILED,
  STATUS_FILE_IN_PROGRESS,
  STATUS_FILE_PAUSED,
  STATUS_FILE_COMPLETE,
  STATUS_AWAITING_RESTART,
  STATUS_STOPPING,
  STATUS_STOPPED
} Status_t;

typedef enum {
  FILE_TYPE_MP3 = 0,
  FILE_TYPE_R8
} FileType_t;

static const char* status2String(Status_t status)
{
  static const char* statusStrings[] = {
    "STATUS_NONE",
    "STATUS_FAILED",
    "STATUS_FILE_IN_PROGRESS",
    "STATUS_FILE_PAUSED",
    "STATUS_FILE_COMPLETE",
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
  switch_mutex_t* mutex;
  CircularBuffer_t* buffer;
  mpg123_handle *mh;
  FILE* fp;
  char* path;
  bool loop;
  int rate;
  boost::asio::deadline_timer *timer;
  Status_t status;
  FileType_t type;
  downloadId_t id;
  int gain;
  dub_track_t* track;
} FileInfo_t;

typedef std::map<int32_t, FileInfo_t *> Id2FileMap_t;
static Id2FileMap_t id2FileMap;

static boost::object_pool<FileInfo_t> pool ;
static boost::asio::io_service io_service;
static std::thread worker_thread;


/* forward declarations */
static FileInfo_t* createFileLoader(const char *path, int rate, int loop, int gain, mpg123_handle *mhm, switch_mutex_t *mutex, CircularBuffer_t *buffer, dub_track_t* track);
static void destroyFileInfo(FileInfo_t *finfo);
static void threadFunc();
static std::vector<int16_t> convert_mp3_to_linear(FileInfo_t *file, int8_t *data, size_t len);
static void read_cb(const boost::system::error_code& error, FileInfo_t* finfo) ;
static void restart_cb(const boost::system::error_code& error, FileInfo_t* finfo) ;

/* apis */
extern "C" {

  switch_status_t init_file_loader() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "init_file_loader loading..\n");

    if (mpg123_init() != MPG123_OK) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "init_file_loader: failed to initiate MPG123");
      return SWITCH_STATUS_FALSE;
    }

    /* start worker thread */
    std::thread t(threadFunc) ;
    worker_thread.swap( t ) ;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "init_file_loader: loaded\n");

    return SWITCH_STATUS_SUCCESS;

  }

  switch_status_t deinit_file_loader() {
    /* stop the ASIO IO service */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deinit_file_loader: stopping io service\n");
    io_service.stop();

    /* Join the worker thread */
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "deinit_file_loader: wait for worker thread to complete\n");
    if (worker_thread.joinable()) {
        worker_thread.join();
    }

    mpg123_exit();

    return SWITCH_STATUS_SUCCESS;
  }

  downloadId_t start_file_load(const char* path, int rate, int loop, int gain, switch_mutex_t* mutex, CircularBuffer_t* buffer, dub_track_t* track) {
    int mhError = 0;

    /* we only handle mp3 or r8 files atm */
    const char *ext = strrchr(path, '.');
    if (!ext) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "start_file_load: file %s has no extension\n", path);
      return INVALID_DOWNLOAD_ID;
    }
    ext++;
    if (0 != strcmp(ext, "mp3") && 0 != strcmp(ext, "r8")) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "start_file_load: file %s has unsupported extension %s\n", path, ext);
      return INVALID_DOWNLOAD_ID;
    }

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

    FileInfo_t* finfo = createFileLoader(path, rate, loop, gain, mh, mutex, buffer, track);
    if (!finfo) {
      return INVALID_DOWNLOAD_ID;
    }

    /* do the initial read in the worker thread so we don't block here */
    finfo->timer->expires_from_now(boost::posix_time::millisec(1));
    finfo->timer->async_wait(boost::bind(&read_cb, boost::placeholders::_1, finfo));

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
      "start_file_load: starting load %d\n", finfo->id);

    return finfo->id;
  }

  switch_status_t stop_file_load(int id) {
    auto it = id2FileMap.find(id);
    if (it == id2FileMap.end()) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "stop_file_load: id %d has already completed\n", id);
      return SWITCH_STATUS_FALSE;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
      "stop_audio_download: stopping download %d, status %s\n", id, status2String(it->second->status));

    FileInfo_t *finfo = it->second;
    auto status = finfo->status;

    /* past this point I shall not access either the mutex or the buffer provided */
    finfo->mutex = nullptr;
    finfo->buffer = nullptr;

    destroyFileInfo(finfo);

    finfo->status = Status_t::STATUS_STOPPED;
    return SWITCH_STATUS_SUCCESS;
  }
}

/* internal */
FileInfo_t* createFileLoader(const char *path, int rate, int loop, int gain, mpg123_handle *mh, switch_mutex_t *mutex, CircularBuffer_t *buffer, dub_track_t* track) {
  FileInfo_t *finfo = pool.malloc() ;
  const char *ext = strrchr(path, '.');

  memset(finfo, 0, sizeof(FileInfo_t));
  finfo->mutex = mutex;
  finfo->buffer = buffer;
  finfo->mh = mh;
  finfo->loop = loop;
  finfo->gain = gain;
  finfo->rate = rate;
  finfo->path = strdup(path);
  finfo->status = Status_t::STATUS_NONE; 
  finfo->timer = new boost::asio::deadline_timer(io_service);
  finfo->track = track;

  if (0 == strcmp(ext, "mp3")) finfo->type = FileType_t::FILE_TYPE_MP3;
  else if (0 == strcmp(ext, "r8")) finfo->type = FileType_t::FILE_TYPE_R8;

  downloadId_t id = ++currDownloadId;
  if (id == 0) id++;

  id2FileMap[id] = finfo;
  finfo->id = id;

  finfo->status = Status_t::STATUS_AWAITING_RESTART;

  finfo->fp = fopen(finfo->path, "rb");
  if (finfo->fp == NULL) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "createFileLoader: failed to open file %s\n", finfo->path);
    destroyFileInfo(finfo);
    return nullptr;
  }

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
    "createFileLoader: launched request, loop %s, gain %d\n", (finfo->loop ? "yes": "no"), finfo->gain);
  return finfo;
}

void destroyFileInfo(FileInfo_t *finfo) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "destroyFileInfo\n");

  /* clear asio resources and free resources */
  if (finfo->timer) {
    finfo->timer->cancel();
    delete finfo->timer;
  }

  /* free mp3 decoder */
  if (finfo->mh) {
    mpg123_close(finfo->mh);
    mpg123_delete(finfo->mh);
  }

  if (finfo->path) {
    free(finfo->path);
  }

  if (finfo->mutex) switch_mutex_lock(finfo->mutex);
  id2FileMap.erase(finfo->id);
  if (finfo->mutex) switch_mutex_unlock(finfo->mutex);

  memset(finfo, 0, sizeof(FileInfo_t));
  pool.destroy(finfo) ;
}

void threadFunc() {      
  /* to make sure the event loop doesn't terminate when there is no work to do */
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

std::vector<int16_t> convert_mp3_to_linear(FileInfo_t *finfo, int8_t *data, size_t len) {
  std::vector<int16_t> linear_data;
  int eof = 0;
  int mp3err = 0;

  if(mpg123_feed(finfo->mh, (const unsigned char*) data, len) == MPG123_OK) {
    while(!eof) {
      size_t usedlen = 0;
      off_t frame_offset;
      unsigned char* audio;

      int decode_status = mpg123_decode_frame(finfo->mh, &frame_offset, &audio, &usedlen);

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

    if (finfo->gain != 0) {
      switch_change_sln_volume_granular(linear_data.data(), linear_data.size(), finfo->gain);
    }
  }

  return linear_data;
}

static void restart_cb(const boost::system::error_code& error, FileInfo_t* finfo) {
  if (finfo->status == Status_t::STATUS_STOPPING) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "restart_cb: %u session gone\n", finfo->id);
    return;
  }

  auto cmdQueue = static_cast<std::queue<HttpPayload_t>*> (finfo->track->cmdQueue);
  auto rate = finfo->rate;
  auto loop = finfo->loop;
  auto gain = finfo->gain;
  auto mutex = finfo->mutex;
  auto buffer = finfo->buffer;
  auto oldId = finfo->id;
  auto track = finfo->track;

  if (loop) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u looping\n", finfo->id);
    ::fseek(finfo->fp, 0, SEEK_SET);
    finfo->status = Status_t::STATUS_AWAITING_RESTART;
    finfo->timer->expires_from_now(boost::posix_time::millisec(1));
    finfo->timer->async_wait(boost::bind(&read_cb, boost::placeholders::_1, finfo));
    return;
  }

  stop_file_load(oldId);

  if (!cmdQueue->empty()) {
    HttpPayload_t payload = cmdQueue->front();
    cmdQueue->pop();

    bool isHttp = strncmp(payload.url.c_str(), "http", 4) == 0;
    bool isSay = strncmp(payload.url.c_str(), "say:", 4) == 0;
    if (isHttp || isSay) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Running to next command, but it's on http, terminate myself and start audio downloader\n");
      track->generatorId = start_audio_download(&payload, rate, loop, gain, mutex, (CircularBuffer_t*) track->circularBuffer, track);
      track->generator = DUB_GENERATOR_TYPE_HTTP;
    } else {
      track->generatorId = start_file_load(payload.url.c_str(), rate, loop, gain, mutex, (CircularBuffer_t*) track->circularBuffer, track);
    }
  }
}

void read_cb(const boost::system::error_code& error, FileInfo_t* finfo) {
  if (finfo->status == Status_t::STATUS_STOPPING || !finfo->mutex || !finfo->buffer) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u session gone\n", finfo->id);
    return;
  }
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u status is %s\n", finfo->id, status2String(finfo->status));
  if (finfo->status == Status_t::STATUS_AWAITING_RESTART) {
    finfo->status = Status_t::STATUS_FILE_IN_PROGRESS;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u starting initial read of file\n", finfo->id);
  }

  if (!error) {
    size_t size = 0;
    
    switch_mutex_lock(finfo->mutex);
    size = finfo->buffer->size();
    switch_mutex_unlock(finfo->mutex);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u size is now %ld\n", finfo->id, size);
    if (size < BUFFER_THROTTLE_LOW) {
      std::vector<int16_t> pcm_data;
      int8_t buf[INIT_BUFFER_SIZE];

      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u reading data\n", finfo->id);

      size_t bytesRead = ::fread(buf, sizeof(int8_t), INIT_BUFFER_SIZE, finfo->fp);
      if (bytesRead <= 0) {
        if (::feof(finfo->fp)) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u eof\n", finfo->id);
        }
        else if (::ferror(finfo->fp)) {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "read_cb: %u error reading file\n", finfo->id);
        }
        else {
          switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "read_cb: %u unknown error reading file\n", finfo->id);
        }
        finfo->status = Status_t::STATUS_FILE_COMPLETE;
        return;
      }

      if (finfo->type == FileType_t::FILE_TYPE_MP3) {
        pcm_data = convert_mp3_to_linear(finfo, buf, bytesRead);
      } else {
        pcm_data = std::vector<int16_t>(reinterpret_cast<int16_t*>(buf), reinterpret_cast<int16_t*>(buf) + bytesRead / 2);
      }

      switch_mutex_lock(finfo->mutex);

      // Resize the buffer if necessary
      if (finfo->buffer->capacity() - finfo->buffer->size() < pcm_data.size()) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "write_cb %u growing buffer, size now %ld\n", finfo->id, finfo->buffer->size()); 
        finfo->buffer->set_capacity(finfo->buffer->size() + std::max(pcm_data.size(), (size_t)BUFFER_GROW_SIZE));
      }
      
      /* Push the data into the buffer */
      finfo->buffer->insert(finfo->buffer->end(), pcm_data.data(), pcm_data.data() + pcm_data.size());
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u wrote data, buffer size is now %ld\n", finfo->id, finfo->buffer->size());

      switch_mutex_unlock(finfo->mutex);

      if (bytesRead < INIT_BUFFER_SIZE) {
        finfo->status = Status_t::STATUS_FILE_COMPLETE;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "read_cb: %u reached end of file, status is %s\n", finfo->id, status2String(finfo->status));
      }
    }

    finfo->timer->expires_from_now(boost::posix_time::millisec(2000));
    finfo->timer->async_wait(boost::bind( finfo->status != Status_t::STATUS_FILE_COMPLETE ? &read_cb : &restart_cb, boost::placeholders::_1, finfo));
  } else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "read_cb: %u error (%d): %s\n", finfo->id, error.value(), error.message().c_str());

    // Handle any errors
  }
}
