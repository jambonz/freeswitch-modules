#ifndef __RIMELABS_GLUE_H__
#define __RIMELABS_GLUE_H__

switch_status_t rimelabs_speech_load();
switch_status_t rimelabs_speech_open(rimelabs_t* rimelabs);
switch_status_t rimelabs_speech_feed_tts(rimelabs_t* rimelabs, char* text, switch_speech_flag_t *flags);
switch_status_t rimelabs_speech_read_tts(rimelabs_t* rimelabs, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t rimelabs_speech_flush_tts(rimelabs_t* rimelabs);
switch_status_t rimelabs_speech_close(rimelabs_t* rimelabs);
switch_status_t rimelabs_speech_unload();

#endif