/* 
 *
 * mod_verbio_transcribe.c -- Freeswitch module for using verbio streaming transcribe api
 *
 */
#include "mod_verbio_transcribe.h"
#include "verbio_glue.h"

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_verbio_transcribe_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_verbio_transcribe_load);

SWITCH_MODULE_DEFINITION(mod_verbio_transcribe, mod_verbio_transcribe_load, mod_verbio_transcribe_shutdown, NULL);

static switch_status_t do_stop(switch_core_session_t *session, char* bugname);

static void responseHandler(switch_core_session_t* session, const char* eventName, const char * json, const char* bugname, int finished) {
  switch_event_t *event;
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "responseHandler event %s, body %s.\n", eventName, json);
  switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
  switch_channel_event_set_data(channel, event);
  switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "verbio");
  switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-session-finished", finished ? "true" : "false");
  if (finished) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "responseHandler returning event %s, from finished recognition session\n", eventName);
  }
  if (json) switch_event_add_body(event, "%s", json);
  if (bugname) switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "media-bugname", bugname);
  switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
  switch_core_session_t *session = switch_core_media_bug_get_session(bug);

  switch (type) {
  case SWITCH_ABC_TYPE_INIT:
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_INIT.\n");
    break;

  case SWITCH_ABC_TYPE_CLOSE:
    {
      struct cap_cb* cb = (struct cap_cb*) switch_core_media_bug_get_user_data(bug);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_CLOSE.\n");

      verbio_speech_session_cleanup(session, 1, cb->bugname);
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
    }
    break;
  
  case SWITCH_ABC_TYPE_READ:
    return verbio_speech_frame(bug, user_data);
    break;

  case SWITCH_ABC_TYPE_WRITE:
  default:
    break;
  }

  return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, char* lang, int interim, char* bugname)
{
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug;
  switch_status_t status;
  void *pUserData;

  if (switch_channel_get_private(channel, bugname)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Verbio removing bug from previous transcribe\n");
    do_stop(session, bugname);
  }

  if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
    return SWITCH_STATUS_FALSE;
  }

  if (SWITCH_STATUS_FALSE == verbio_speech_session_init(session, responseHandler, 
    flags & SMBF_STEREO ? 2 : 1/*channels*/,lang, interim, bugname, &pUserData)) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing verbio speech session.\n");
    return SWITCH_STATUS_FALSE;
  }
  if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
    return status;
  }
  switch_channel_set_private(channel, bugname, bug);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "added media bug for verbio transcribe\n");

  return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session, char* bugname)
{
  switch_status_t status = SWITCH_STATUS_SUCCESS;

  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug = switch_channel_get_private(channel, bugname);

  if (bug) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "do_stop: Received user command command to stop transcribe.\n");
    status = verbio_speech_session_cleanup(session, 0, bugname);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "do_stop: stopped transcribe.\n");
  }

  return status;
}

#define TRANSCRIBE_API_SYNTAX "<uuid> [start|stop] lang-code [interim] [stereo|mono] [bugname]"
SWITCH_STANDARD_API(verbio_transcribe_function)
{
  char *mycmd = NULL, *argv[6] = { 0 };
  int argc = 0;
  switch_status_t status = SWITCH_STATUS_FALSE;
  switch_media_bug_flag_t flags = SMBF_READ_STREAM /* | SMBF_WRITE_STREAM | SMBF_READ_PING */;

  if (!zstr(cmd) && (mycmd = strdup(cmd))) {
    argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
  }


  if (zstr(cmd) || zstr(argv[0])) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
    stream->write_function(stream, "-USAGE: %s\n", TRANSCRIBE_API_SYNTAX);
    goto done;
  } else {
    switch_core_session_t *lsession = NULL;

    if ((lsession = switch_core_session_locate(argv[0]))) {
      if (!strcasecmp(argv[1], "stop")) {
        char *bugname = argc > 2 ? argv[2] : MY_BUG_NAME;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Verbio stop transcribing %s\n", bugname);
        status = do_stop(lsession, bugname);
      } else if (!strcasecmp(argv[1], "start")) {
        char* lang = argv[2];
        int interim = argc > 3 && !strcmp(argv[3], "interim");
				char *bugname = argc > 5 ? argv[5] : MY_BUG_NAME;
				if (argc > 4 && !strcmp(argv[4], "stereo")) {
          flags |= SMBF_WRITE_STREAM ;
          flags |= SMBF_STEREO;
				}
    		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "start transcribing %s %s %s\n", 
          lang, interim ? "interim": "complete", bugname);
        status = start_capture(lsession, flags, lang, interim, bugname);
      }
      switch_core_session_rwunlock(lsession);
    }
  }

  if (status == SWITCH_STATUS_SUCCESS) {
    stream->write_function(stream, "+OK Success\n");
  } else {
    stream->write_function(stream, "-ERR Operation Failed\n");
  }

  done:

  switch_safe_free(mycmd);
  return SWITCH_STATUS_SUCCESS;
}


SWITCH_MODULE_LOAD_FUNCTION(mod_verbio_transcribe_load)
{
  switch_api_interface_t *api_interface;

  /* create/register custom event message type */
  if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_RESULTS) != SWITCH_STATUS_SUCCESS) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_RESULTS);
    return SWITCH_STATUS_TERM;
  }

  /* connect my internal structure to the blank pointer passed to me */
  *module_interface = switch_loadable_module_create_module_interface(pool, modname);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "verbio Speech Transcription API loading..\n");

  if (SWITCH_STATUS_FALSE == verbio_speech_init()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed initializing verbio speech interface\n");
  }

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "verbio Speech Transcription API successfully loaded\n");

  SWITCH_ADD_API(api_interface, "uuid_verbio_transcribe", "verbio Speech Transcription API", verbio_transcribe_function, TRANSCRIBE_API_SYNTAX);
  switch_console_set_complete("add uuid_verbio_transcribe start lang-code [interim|final] [stereo|mono] [bugname]");
  switch_console_set_complete("add uuid_verbio_transcribe stop ");

  /* indicate that the module should continue to be loaded */
  return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_verbio_transcribe_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_verbio_transcribe_shutdown)
{
  verbio_speech_cleanup();
  switch_event_free_subclass(TRANSCRIBE_EVENT_RESULTS);
  return SWITCH_STATUS_SUCCESS;
}
