
#ifndef MOD_AUDIO_MPG_DECODE_H
#define MOD_AUDIO_MPG_DECODE_H

#include <vector>
#include <mpg123.h>
#include "switch.h"

std::vector<int16_t> convert_mp3_to_linear(mpg123_handle *mh, int gain, int8_t *data, size_t len);

#endif
