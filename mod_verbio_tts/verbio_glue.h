#ifndef __VERBIO_GLUE_H__
#define __VERBIO_GLUE_H__

switch_status_t verbio_speech_load();
switch_status_t verbio_speech_open(verbio_t* verbio);
switch_status_t verbio_speech_feed_tts(verbio_t* verbio, char* text, switch_speech_flag_t *flags);
switch_status_t verbio_speech_read_tts(verbio_t* verbio, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t verbio_speech_flush_tts(verbio_t* verbio);
switch_status_t verbio_speech_close(verbio_t* verbio);
switch_status_t verbio_speech_unload();

#endif