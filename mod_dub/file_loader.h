#ifndef __FILE_LOADER_H__
#define __FILE_LOADER_H__

#include <switch.h>
#include "common.h"
#include "mod_dub.h"
#include "audio_downloader.h"

extern "C" {

switch_status_t init_file_loader();
switch_status_t deinit_file_loader();

int start_file_load(const char* path, int rate, int loop, int gain, switch_mutex_t* mutex, CircularBuffer_t* buffer, dub_track_t* track);
switch_status_t stop_file_load(int id);

}

#endif

