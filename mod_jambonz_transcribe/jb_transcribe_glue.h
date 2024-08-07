#ifndef __JB_GLUE_H__
#define __JB_GLUE_H__

int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags);

switch_status_t jb_transcribe_init();
switch_status_t jb_transcribe_cleanup();
switch_status_t jb_transcribe_session_init(switch_core_session_t *session, responseHandler_t responseHandler, 
		uint32_t samples_per_second, int desiredSampling, uint32_t channels, char* lang, int interim, char* bugname, void **ppUserData);
switch_status_t jb_transcribe_session_stop(switch_core_session_t *session, int channelIsClosing, char* bugname);
switch_bool_t jb_transcribe_frame(switch_core_session_t *session, switch_media_bug_t *bug);

#endif