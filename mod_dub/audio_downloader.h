#ifndef __AUDIO_DOWNLOADER_H__
#define __AUDIO_DOWNLOADER_H__

#include <switch.h>
#include "common.h"
#include "mod_dub.h"
#include <queue>

extern "C" {

switch_status_t init_audio_downloader();
switch_status_t deinit_audio_downloader();

int start_audio_download(request_t* payload, int rate,
  int loop, int gain, switch_mutex_t* mutex, CircularBuffer_t* buffer, request_queue_t* req_queue, dub_generator_t* generator, int* generator_id);
switch_status_t stop_audio_download(int id);

}

#endif

