#ifndef __GOOGLE_TTS_GLUE_H__
#define __GOOGLE_TTS_GLUE_H__

switch_status_t google_speech_load();
switch_status_t google_speech_open(google_t* google);
switch_status_t google_speech_feed_tts(google_t* google, char* text, switch_speech_flag_t *flags);
switch_status_t google_speech_read_tts(google_t* google, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t google_speech_flush_tts(google_t* google);
switch_status_t google_speech_close(google_t* google);
switch_status_t google_speech_unload();

#endif