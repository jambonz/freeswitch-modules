#include "mod_vad_detect.h"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_detect_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_vad_detect_load);
SWITCH_MODULE_DEFINITION(mod_vad_detect, mod_vad_detect_load, mod_vad_detect_shutdown, NULL);

static void responseHandler(switch_core_session_t* session, switch_vad_state_t state, const char* bugname) {
  switch_event_t *event;
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "responseHandler event %s, detect %s.\n", VAD_EVENT_DETECTION,
    state == SWITCH_VAD_STATE_START_TALKING ? "start_talking" : "stop_talking");
  switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, VAD_EVENT_DETECTION);
  switch_channel_event_set_data(channel, event);
  switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "detected-event", state == SWITCH_VAD_STATE_START_TALKING ? "start_talking" : "stop_talking");
  if (bugname) switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "media-bugname", bugname);
  switch_event_fire(&event);
}

static void cleanVadDetect(private_t* u) {
  if (u) {
    if (u->vad) {
      switch_vad_destroy(&u->vad);
      u->vad = NULL;
    }
    if (u->bugname) free(u->bugname);
    if (u->strategy) free(u->strategy);
    if (u->sessionId) free(u->sessionId);
  }
}

static switch_status_t do_stop(switch_core_session_t *session, char* bugname) {
  switch_status_t status = SWITCH_STATUS_SUCCESS;

  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug = switch_channel_get_private(channel, bugname);
  if (bug) {
    switch_channel_set_private(channel, bugname, NULL);
    switch_core_media_bug_remove(session, &bug);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "do_stop: stopped vad detection.\n");
  }

  return status;
}

static void *SWITCH_THREAD_FUNC stop_thread(switch_thread_t *thread, void *obj) {
  private_t* u = (private_t*) obj;
  switch_core_session_t* session = switch_core_session_locate(u->sessionId);
  do_stop(session, u->bugname);
  switch_core_session_rwunlock(session);
  return NULL;
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
  switch_core_session_t *session = switch_core_media_bug_get_session(bug);
  switch_memory_pool_t *pool = switch_core_session_get_pool(session);
  private_t* userData = (private_t*) user_data;

  switch (type) {
  case SWITCH_ABC_TYPE_INIT:
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_INIT.\n");
    break;

  case SWITCH_ABC_TYPE_CLOSE:
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_CLOSE.\n");
    cleanVadDetect(userData);
    break;
  
  case SWITCH_ABC_TYPE_READ:
    {
      uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
      switch_threadattr_t *thd_attr = NULL;
      switch_frame_t frame;
      memset(&frame, 0, sizeof(frame));
      frame.data = data;
      frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
      if (userData->stopping) {
        return SWITCH_TRUE;
      }
      while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
        if (frame.datalen) {
          switch_vad_state_t state = switch_vad_process(userData->vad, (int16_t*) frame.data, frame.samples);
          switch (state)
          {
            case SWITCH_VAD_STATE_START_TALKING:
            case SWITCH_VAD_STATE_STOP_TALKING:
              responseHandler(session, state, userData->bugname);
              if (!strcasecmp(userData->strategy, "one-shot")) {
                userData->stopping = 1;
                // create the stop thread
                switch_threadattr_create(&thd_attr, pool);
                switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
                switch_thread_create(&userData->thread, thd_attr, stop_thread, userData, pool);
              }
            break;

            case SWITCH_VAD_STATE_TALKING:
            case SWITCH_VAD_STATE_NONE:
            default:
              break;
          }
        }
      }
    }
    break;

  case SWITCH_ABC_TYPE_WRITE:
  default:
    break;
  }

  return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, 
  char* action, int mode, uint32_t silence_ms, uint32_t voice_ms, char* bugname)
{
  switch_channel_t *channel = switch_core_session_get_channel(session);
  switch_media_bug_t *bug;
  switch_status_t status = SWITCH_STATUS_SUCCESS; 
  private_t* userData = (private_t *) switch_core_session_alloc(session, sizeof(*userData));
  switch_codec_implementation_t read_impl = { 0 };
  uint32_t samples_per_second;

  if (switch_channel_get_private(channel, bugname)) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "removing bug from previous vad detection\n");
    do_stop(session, bugname);
  }
  

  if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
    return SWITCH_STATUS_FALSE;
  }
  switch_core_session_get_read_impl(session, &read_impl);
  samples_per_second = !strcasecmp(read_impl.iananame, "g722") ? read_impl.actual_samples_per_second : read_impl.samples_per_second;

  userData->stopping = 0;
  userData->vad = switch_vad_init(samples_per_second, 1);
  if (userData->vad) {
    userData->bugname = strdup(bugname);
    userData->strategy = strdup(action);
    userData->sessionId = strdup(switch_core_session_get_uuid(session));
    switch_vad_set_mode(userData->vad, mode);
    switch_vad_set_param(userData->vad, "silence_ms", silence_ms);
    switch_vad_set_param(userData->vad, "voice_ms", voice_ms);
    // switch_vad_set_param(userData->vad, "debug", 1);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "configured vad mode %d, silence_ms %d, voice_ms %d\n", 
      mode, silence_ms, voice_ms);
    if ((status = switch_core_media_bug_add(session, bugname, NULL, capture_callback, userData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to initiate vad resource\n");
      return status;
    }
    switch_channel_set_private(channel, bugname, bug);
  }
  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Successfully initiated vad resource\n");
  return SWITCH_STATUS_SUCCESS;
}

#define VAD_API_SYNTAX "<uuid> [start|stop] [one-shot|continuous] mode silence-ms voice-ms [bugname]"
SWITCH_STANDARD_API(vad_detect_function) {
  char *mycmd = NULL, *argv[7] = { 0 };
  int argc = 0;
  switch_status_t status = SWITCH_STATUS_FALSE;
  switch_media_bug_flag_t flags = SMBF_READ_STREAM /* | SMBF_WRITE_STREAM | SMBF_READ_PING */;

  if (!zstr(cmd) && (mycmd = strdup(cmd))) {
    argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
  }

  if (zstr(cmd) || 
      (!strcasecmp(argv[1], "stop") && argc < 2) ||
      (!strcasecmp(argv[1], "start") && argc < 3) ||
      zstr(argv[0])) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
    stream->write_function(stream, "-USAGE: %s\n", VAD_API_SYNTAX);
    goto done;
  } else {
    switch_core_session_t *lsession = NULL;

    if ((lsession = switch_core_session_locate(argv[0]))) {
      if (!strcasecmp(argv[1], "stop")) {
        char *bugname = argc > 2 ? argv[2] : MY_BUG_NAME;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "stop vad detection %s\n", bugname);
        status = do_stop(lsession, bugname);
      } else if (!strcasecmp(argv[1], "start")) {
        char* action = argv[2];
        int mode = atoi(argv[3]);
        uint32_t silence_ms = atoi(argv[4]);
        uint32_t voice_ms = atoi(argv[5]);
        char *bugname = argc > 6 ? argv[6] : MY_BUG_NAME;

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "start vad detect action: %s mode: %d silence_ms: %d voice_ms: %d bugname: %s.\n",
          action, mode, silence_ms, voice_ms, bugname);
        status = start_capture(lsession, flags, action, mode, silence_ms, voice_ms, bugname);
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
SWITCH_MODULE_LOAD_FUNCTION(mod_vad_detect_load)
{
  switch_api_interface_t *api_interface;

  /* create/register custom event message type */
  if (switch_event_reserve_subclass(VAD_EVENT_DETECTION) != SWITCH_STATUS_SUCCESS) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", VAD_EVENT_DETECTION);
    return SWITCH_STATUS_TERM;
  }

  /* connect my internal structure to the blank pointer passed to me */
  *module_interface = switch_loadable_module_create_module_interface(pool, modname);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VAD dectetion API loading..\n");

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VAD detection API successfully loaded\n");

  SWITCH_ADD_API(api_interface, "uuid_vad_detect", "VAD detection API", vad_detect_function, VAD_API_SYNTAX);
  switch_console_set_complete("add uuid_vad_detect start [one-shot|continuous] mode silence-ms voice-ms [bugname]");
  switch_console_set_complete("add uuid_vad_detect stop [bugname]");

  /* indicate that the module should continue to be loaded */
  return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_vad_detect_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_detect_shutdown)
{
  switch_event_free_subclass(VAD_EVENT_DETECTION);
  return SWITCH_STATUS_SUCCESS;
}