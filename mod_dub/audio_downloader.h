#ifndef __AUDIO_DOWNLOADER_H__
#define __AUDIO_DOWNLOADER_H__

#include <switch.h>
#include "common.h"

extern "C" {

switch_status_t init_audio_downloader();
switch_status_t deinit_audio_downloader();

int start_audio_download(const char* url, const char* body, std::vector<std::string> headers, int rate, int loop, int gain, switch_mutex_t* mutex, CircularBuffer_t* buffer);
switch_status_t stop_audio_download(int id);

}

#endif

