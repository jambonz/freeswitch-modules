#ifndef __AZURE_TTS_GLUE_H__
#define __AZURE_TTS_GLUE_H__

switch_status_t azure_speech_load();
switch_status_t azure_speech_open(azure_t* azure);
switch_status_t azure_speech_feed_tts(azure_t* azure, char* text, switch_speech_flag_t *flags);
switch_status_t azure_speech_read_tts(azure_t* azure, void *data, size_t *datalen, switch_speech_flag_t *flags);
switch_status_t azure_speech_flush_tts(azure_t* azure);
switch_status_t azure_speech_close(azure_t* azure);
switch_status_t azure_speech_unload();

#endif