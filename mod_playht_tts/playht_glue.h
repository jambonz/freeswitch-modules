#ifndef __PLAYHT_GLUE_H__
#define __PLAYHT_GLUE_H__

switch_status_t playht_speech_load();
switch_status_t playht_speech_open(playht_t* playht);
switch_status_t playht_speech_feed_tts(playht_t* playht, char* text, switch_speech_flag_t *flags);
switch_status_t playht_speech_read_tts(playht_t* playht, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t playht_speech_flush_tts(playht_t* playht);
switch_status_t playht_speech_close(playht_t* playht);
switch_status_t playht_speech_unload();

#endif