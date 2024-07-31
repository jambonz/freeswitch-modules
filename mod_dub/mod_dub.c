/* 
 *
 * mod_dub.c
 *
 */
#include "mod_dub.h"
#include <stdlib.h>
#include <switch.h>
#include <switch_curl.h>
#include "dub_glue.h"
#include <string.h>

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dub_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_dub_load);

SWITCH_MODULE_DEFINITION(mod_dub, mod_dub_load, mod_dub_shutdown, NULL);

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
  switch_bool_t ret = SWITCH_TRUE;

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_INIT.\n");
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			dub_session_cleanup(session, 1, bug);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
		}
		break;
	
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		ret = dub_speech_frame(bug, user_data);
		break;

	default:
		break;
	}

	return ret;
}

static switch_status_t dub_set_gain(switch_core_session_t *session, int gain) {
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug = NULL;
  struct cap_cb *cb = NULL;

  if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

  if (!(bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME))) {
    cb =(struct cap_cb *) switch_core_session_alloc(session, sizeof(struct cap_cb));
    memset(cb, 0, sizeof(struct cap_cb));
    switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));    
  }
  else {
    cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
  }
  cb->gain = gain;

  /* check again under lock */
  switch_mutex_lock(cb->mutex);
  if (!(bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "dub_set_gain: adding bug so we can set gain on main channel\n");
    if (switch_core_media_bug_add(session, MY_BUG_NAME, NULL, capture_callback, (void *) cb, 0, SMBF_WRITE_REPLACE, &bug) != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_set_gain: error adding bug!\n");
      switch_mutex_unlock(cb->mutex);
      return SWITCH_STATUS_FALSE;
    }
    switch_channel_set_private(channel, MY_BUG_NAME, bug);
  }
  else {
    cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
    cb->gain = gain;
  }
  switch_mutex_unlock(cb->mutex);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "dub_set_gain: setting gain to %d\n", gain);
  return SWITCH_STATUS_SUCCESS;
}


static switch_status_t dub_add_track(switch_core_session_t *session, char* trackName) {
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug = NULL;
  struct cap_cb *cb = NULL;
  int samples_per_second;

  switch_codec_implementation_t write_impl = { 0 };

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

  if (!(bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME))) {
    /* allocate per-session memory and use track 0 */
    cb =(struct cap_cb *) switch_core_session_alloc(session, sizeof(struct cap_cb));
    memset(cb, 0, sizeof(struct cap_cb));
    switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));    
  }
  else {
    /* retrieve the bug and search for an empty track */
    cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
  }

  switch_core_session_get_write_impl(session, &write_impl);
	samples_per_second = !strcasecmp(write_impl.iananame, "g722") ? write_impl.actual_samples_per_second : write_impl.samples_per_second;

  switch_mutex_lock(cb->mutex);
  if (add_track(cb, trackName, samples_per_second) != SWITCH_STATUS_SUCCESS) {
    switch_mutex_unlock(cb->mutex);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_add_track: error adding track %s\n", trackName);
    return SWITCH_STATUS_FALSE;
  }

  if (!bug) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "dub_add_track: adding bug for track %s\n", trackName);
    if (switch_core_media_bug_add(session, MY_BUG_NAME, NULL, capture_callback, (void *) cb, 0, SMBF_WRITE_REPLACE, &bug) != SWITCH_STATUS_SUCCESS) {
      switch_mutex_unlock(cb->mutex);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_add_track: error adding bug!\n");
      return SWITCH_STATUS_FALSE;
    }
    switch_channel_set_private(channel, MY_BUG_NAME, bug);
  }
  switch_mutex_unlock(cb->mutex);

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t dub_remove_track(switch_core_session_t *session, char* trackName) {
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
  switch_status_t status = SWITCH_STATUS_FALSE;
  
  if (bug) {
    struct cap_cb *cb =(struct cap_cb *) switch_core_media_bug_get_user_data(bug);

    switch_mutex_lock(cb->mutex);
    status = remove_dub_track(cb, trackName);
    switch_mutex_unlock(cb->mutex);
    if (status != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_remove_track: error removing track %s\n", trackName);
      return SWITCH_STATUS_FALSE;
    }
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_remove_track: bug not found\n");
  }

  return status;
}

static switch_status_t dub_silence_track(switch_core_session_t *session, char* trackName) {
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
  switch_status_t status = SWITCH_STATUS_FALSE;

  // DH: I found it is much simpler to implement silence as a sequence of remove and add operations  
  if (bug) {
    struct cap_cb *cb =(struct cap_cb *) switch_core_media_bug_get_user_data(bug);
    status = remove_dub_track(cb, trackName);
    if (status != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_silence_track: error finding track %s\n", trackName);
      return SWITCH_STATUS_FALSE;
    }
    status = dub_add_track(session, trackName);
  }
  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_silence_track: bug not found\n");
  }

  return status;
}

static switch_status_t dub_play_on_track(switch_core_session_t *session, char* trackName, char* url, int loop, int gain) {
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
  switch_status_t status = SWITCH_STATUS_FALSE;
  
  if (bug) {
    struct cap_cb *cb =(struct cap_cb *) switch_core_media_bug_get_user_data(bug);
    status = play_dub_track(cb, trackName, url, loop, gain);
    if (status != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_play_on_track: error playing %s on track %s\n", url, trackName);
      return SWITCH_STATUS_FALSE;
    }
  }
  return status;
}

static switch_status_t dub_say_on_track(switch_core_session_t *session, char* trackName, char* text, int gain) {
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
  switch_status_t status = SWITCH_STATUS_FALSE;
  
  if (bug) {
    struct cap_cb *cb =(struct cap_cb *) switch_core_media_bug_get_user_data(bug);
    status = say_dub_track(cb, trackName, text, gain);
    if (status != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "dub_say_on_track: error saying %s on track %s\n", text, trackName);
      return SWITCH_STATUS_FALSE;
    }
  }
  return status;
}

#define DUB_API_SYNTAX "<uuid> [addTrack|removeTrack|silenceTrack|playOnTrack|sayOnTrack|setGain] track [url|text|gain] [gain] [loop]"
#define MAX_PARAMS 6
SWITCH_STANDARD_API(dub_function)
{
	char *mycmd = NULL, *argv[MAX_PARAMS] = { 0 };
	int argc = 0;
  int error_written = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "dub_function: %s\n", mycmd);
    argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

  if (zstr(cmd) || argc < 3 || zstr(argv[0]) || zstr(argv[1]) || zstr(argv[2])) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error with command %s.\n", cmd);
  }
  else {
    switch_core_session_t *session = NULL;
		if ((session = switch_core_session_locate(argv[0]))) {
      char* action = argv[1];
      char* track = argv[2];

      if (0 == strcmp(action, "setGain")) {
        int gain = atoi(track);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "setGain %d\n", gain);
        status = dub_set_gain(session, gain);
      }
      else if (0 == strcmp(action, "addTrack")) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "addTrack %s\n", track);
        status = dub_add_track(session, track);
      }
      else if (0 == strcmp(action, "removeTrack")) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "removeTrack %s\n", track);
        status = dub_remove_track(session, track);
      }
      else if (0 == strcmp(action, "silenceTrack")) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "silenceTrack %s\n", track);
        status = dub_silence_track(session, track);
      }
      else if (0 == strcmp(action, "playOnTrack")) {
        if (argc < 4) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "playOnTrack requires a url\n");
          stream->write_function(stream, "-USAGE: %s\n", DUB_API_SYNTAX);
          error_written = 1;
        }
        else {
          char* url = argv[3];
          int loop = argc > 4 && 0 == strcmp(argv[4], "loop");
          int gain = argc > 5 ? atoi(argv[5]) : 0;
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, 
            "playOnTrack %s, %s gain %d\n", url, loop ? "loop" : "once", gain);
          status = dub_play_on_track(session, track, url, loop, gain);
        }
      }
      else if (0 == strcmp(action, "sayOnTrack")) {
        if (argc < 4) {
          stream->write_function(stream, "-USAGE: %s\n", DUB_API_SYNTAX);
          error_written = 1;
        }
        else {
          char* text = argv[3];
          int gain = argc > 5 ? atoi(argv[5]) : 0;
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "sayOnTrack %s gain %d\n", text, gain);
          status = dub_say_on_track(session, track, text, gain);
        }
      }
      else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
        stream->write_function(stream, "-USAGE: %s\n", DUB_API_SYNTAX);
          error_written = 1;
      }
      switch_core_session_rwunlock(session);
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error locating session for %s.\n", cmd);
      stream->write_function(stream, "-ERR Invalid session\n");
      error_written = 1;
    }
  }

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK Success\n");
	} else if (!error_written){
		stream->write_function(stream, "-ERR Operation Failed\n");
	}

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_dub_load)
{
	switch_api_interface_t *api_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	dub_init();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_dub loaded\n");

	SWITCH_ADD_API(api_interface, "uuid_dub", "dub mp3 track over channel audio", dub_function, DUB_API_SYNTAX);
	switch_console_set_complete("add uuid_dub addTrack <trackname>");
	switch_console_set_complete("add uuid_dub removeTrack <trackname>");
	switch_console_set_complete("add uuid_dub silenceTrack <trackname>");
	switch_console_set_complete("add uuid_dub playOnTrack <trackname> <url|file> [loop|once] [gain]");
	switch_console_set_complete("add uuid_dub setGain <gain>");
	switch_console_set_complete("add uuid_dub stop ");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_dub_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_dub_shutdown)
{
	dub_cleanup();
	return SWITCH_STATUS_SUCCESS;
}

