
#include "mpg_decode.h"
#include "vector_math.h"

std::vector<int16_t> convert_mp3_to_linear(mpg123_handle *mh, int gain, int8_t *data, size_t len) {
  std::vector<int16_t> linear_data;
  int eof = 0;
  int mp3err = 0;

  if(mpg123_feed(mh, (const unsigned char*) data, len) == MPG123_OK) {
    while(!eof) {
      size_t usedlen = 0;
      off_t frame_offset;
      unsigned char* audio;

      int decode_status = mpg123_decode_frame(mh, &frame_offset, &audio, &usedlen);

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

    if (gain != 0) {
      vector_change_sln_volume_granular(linear_data.data(), linear_data.size(), gain);
    }
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error feeding data to mpg123\n");
  }


  return linear_data;
}
