#ifndef __WHISPER_GLUE_H__
#define __WHISPER_GLUE_H__

switch_status_t whisper_speech_load();
switch_status_t whisper_speech_open(whisper_t* whisper);
switch_status_t whisper_speech_feed_tts(whisper_t* whisper, char* text, switch_speech_flag_t *flags);
switch_status_t whisper_speech_read_tts(whisper_t* whisper, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t whisper_speech_flush_tts(whisper_t* whisper);
switch_status_t whisper_speech_close(whisper_t* whisper);
switch_status_t whisper_speech_unload();

#endif