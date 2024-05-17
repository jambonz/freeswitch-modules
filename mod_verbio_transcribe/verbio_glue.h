#ifndef __VERBIO_GLUE_H__
#define __VERBIO_GLUE_H__

switch_status_t verbio_transcribe_init();
switch_status_t verbio_transcribe_cleanup();
switch_status_t verbio_transcribe_session_init(switch_core_session_t *session, responseHandler_t responseHandler, 
		uint32_t channels,  char* bugname, void **ppUserData);
switch_status_t verbio_transcribe_session_stop(switch_core_session_t *session, int channelIsClosing, char* bugname);
switch_bool_t verbio_transcribe_frame(switch_media_bug_t *bug, void* user_data);

#endif