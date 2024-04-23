#ifndef __CUSTOM_GLUE_H__
#define __CUSTOM_GLUE_H__

switch_status_t custom_speech_load();
switch_status_t custom_speech_open(custom_t* custom);
switch_status_t custom_speech_feed_tts(custom_t* custom, char* text, switch_speech_flag_t *flags);
switch_status_t custom_speech_read_tts(custom_t* custom, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t custom_speech_flush_tts(custom_t* custom);
switch_status_t custom_speech_close(custom_t* custom);
switch_status_t custom_speech_unload();

#endif