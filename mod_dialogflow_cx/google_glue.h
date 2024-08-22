#ifndef __GOOGLE_GLUE_CX_H__
#define __GOOGLE_GLUE_CX_H__

switch_status_t google_dialogflow_cx_init();
switch_status_t google_dialogflow_cx_cleanup();
switch_status_t google_dialogflow_cx_session_init(
    switch_core_session_t *session, 
		responseHandler_t responseHandler, 
		errorHandler_t errorHandler, 
		uint32_t samples_per_second, 
		char* lang,
    char* region,
		char* projectId,
    char* agentId,
    char* environmentId,
    char* event,
    char* text,
		struct cap_cb **ppUserData);
switch_status_t google_dialogflow_cx_session_stop(switch_core_session_t *session, int channelIsClosing);
switch_bool_t google_dialogflow_cx_frame(switch_media_bug_t *bug, void* user_data);

void destroyChannelUserData(struct cap_cb* cb);
#endif