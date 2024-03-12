#ifndef __DUB_GLUE_H__
#define __DUB_GLUE_H__

switch_status_t dub_init();
switch_status_t dub_cleanup();

void init_dub_track(dub_track_t *track, char* trackName, int sampleRate);
switch_status_t silence_dub_track(dub_track_t *track);
switch_status_t remove_dub_track(dub_track_t *track);
switch_status_t play_dub_track(dub_track_t *track, switch_mutex_t *mutex, char* url, int loop, int gain);
switch_status_t say_dub_track(dub_track_t *track, switch_mutex_t *mutex, char* text, int gain);

switch_status_t dub_session_cleanup(switch_core_session_t *session, int channelIsClosing, switch_media_bug_t *bug);
switch_bool_t dub_speech_frame(switch_media_bug_t *bug, void* user_data);

#endif