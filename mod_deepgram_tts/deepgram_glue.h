#ifndef __DEEPGRAM_GLUE_H__
#define __DEEPGRAM_GLUE_H__

switch_status_t deepgram_speech_load();
switch_status_t deepgram_speech_open(deepgram_t* deepgram);
switch_status_t deepgram_speech_feed_tts(deepgram_t* deepgram, char* text, switch_speech_flag_t *flags);
switch_status_t deepgram_speech_read_tts(deepgram_t* deepgram, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t deepgram_speech_flush_tts(deepgram_t* deepgram);
switch_status_t deepgram_speech_close(deepgram_t* deepgram);
switch_status_t deepgram_speech_unload();

#endif