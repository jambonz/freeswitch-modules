#ifndef __ELEVENLABS_GLUE_H__
#define __ELEVENLABS_GLUE_H__

switch_status_t elevenlabs_speech_load();
switch_status_t elevenlabs_speech_open(elevenlabs_t* elevenlabs);
switch_status_t elevenlabs_speech_feed_tts(elevenlabs_t* elevenlabs, char* apiKey, char* text, switch_speech_flag_t *flags);
switch_status_t elevenlabs_speech_read_tts(elevenlabs_t* elevenlabs, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t elevenlabs_speech_flush_tts(elevenlabs_t* elevenlabs);
switch_status_t elevenlabs_speech_close(elevenlabs_t* elevenlabs);
switch_status_t elevenlabs_speech_unload();

#endif