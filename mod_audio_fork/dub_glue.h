#ifndef __AUDIO_FORK_DUB_GLUE_H__
#define __AUDIO_FORK_DUB_GLUE_H__

#include "mod_audio_fork.h"

switch_status_t dub_init(private_t *user_data, int sampleRate);
switch_status_t dub_cleanup();

switch_status_t play_dub(private_t *user_data, char* url, int gain, int loop);

switch_status_t dub_session_cleanup(switch_core_session_t *session, int channelIsClosing, switch_media_bug_t *bug);
switch_bool_t dub_speech_frame(switch_media_bug_t *bug, private_t * user_data);

#endif