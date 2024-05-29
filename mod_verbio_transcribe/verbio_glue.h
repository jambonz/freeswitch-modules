#ifndef __VERBIO_GLUE_H__
#define __VERBIO_GLUE_H__

switch_status_t verbio_speech_init();
switch_status_t verbio_speech_cleanup();
switch_status_t verbio_speech_session_init(switch_core_session_t *session, responseHandler_t responseHandler, 
		uint32_t channels, char* lang, int interim, char* bugname, void **ppUserData);
switch_status_t verbio_speech_session_cleanup(switch_core_session_t *session, int channelIsClosing, char* bugname);
switch_bool_t verbio_speech_frame(switch_media_bug_t *bug, void* user_data);

#endif