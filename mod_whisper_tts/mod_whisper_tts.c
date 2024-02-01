#include "mod_whisper_tts.h"
#include "whisper_glue.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_whisper_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_whisper_tts_shutdown);
SWITCH_MODULE_DEFINITION(mod_whisper_tts_load, mod_whisper_tts_load, mod_whisper_tts_shutdown, NULL);

static void clearWhisper(whisper_t* w, int freeAll) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "clearWhisper\n");
  if (w->api_key) free(w->api_key);
  if (w->model_id) free(w->model_id);
  if (w->speed) free(w->speed);

  w->api_key = NULL;
  w->model_id = NULL;
  w->speed = NULL;

  if (freeAll) {
    if (w->voice_name) free(w->voice_name);
    w->voice_name = NULL;
  }
}

static whisper_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  whisper_t *w = (whisper_t *) sh->private_info;  
  if (!w) {
    w = switch_core_alloc(sh->memory_pool, sizeof(*w));
  	sh->private_info = w;
    memset(w, 0, sizeof(*w));
    switch_mutex_init(&w->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "allocated whisper_t\n");
  }
  return w;
}

switch_status_t w_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{
  whisper_t *w = createOrRetrievePrivateData(sh);
  w->voice_name = strdup(voice_name);
  w->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "w_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
  return whisper_speech_open(w);
}

static switch_status_t w_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
  whisper_t *w = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "w_speech_close\n");

  switch_mutex_destroy(w->mutex);

  rc = whisper_speech_close(w);
  clearWhisper(w, 1);
  return rc;
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t w_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  whisper_t *w = createOrRetrievePrivateData(sh);
  w->draining = 0;
  w->reads = 0;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "w_speech_feed_tts\n");

  return whisper_speech_feed_tts(w, text, flags);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t w_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
  whisper_t *w = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "w_speech_read_tts\n");
  return whisper_speech_read_tts(w, data, datalen, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void w_speech_flush_tts(switch_speech_handle_t *sh)
{
  whisper_t *w = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "w_speech_flush_tts\n");
  whisper_speech_flush_tts(w);

  clearWhisper(w, 0);
}

static void w_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  whisper_t *w = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "w_text_param_tts: %s=%s\n", param, val);
  if (0 == strcmp(param, "api_key")) {
    if (w->api_key) free(w->api_key);
    w->api_key = strdup(val);
  } else if (0 == strcmp(param, "voice")) {
    if (w->voice_name) free(w->voice_name);
    w->voice_name = strdup(val);
  } else if (0 == strcmp(param, "model_id")) {
    if (w->model_id) free(w->model_id);
    w->model_id = strdup(val);
  } else if (0 == strcmp(param, "speed")) {
    if (w->speed) free(w->speed);
    w->speed = strdup(val);
  } else if (0 == strcmp(param, "session-uuid")) {
    if (w->session_id) free(w->session_id);
    w->session_id = strdup(val);
  }
}
static void w_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
}
static void w_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
}

SWITCH_MODULE_LOAD_FUNCTION(mod_whisper_tts_load)
{
  switch_speech_interface_t *speech_interface;

  *module_interface = switch_loadable_module_create_module_interface(pool, modname);
  speech_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
  speech_interface->interface_name = "whisper";
  speech_interface->speech_open = w_speech_open;
  speech_interface->speech_close = w_speech_close;
  speech_interface->speech_feed_tts = w_speech_feed_tts;
  speech_interface->speech_read_tts = w_speech_read_tts;
	speech_interface->speech_flush_tts = w_speech_flush_tts;
	speech_interface->speech_text_param_tts = w_text_param_tts;
	speech_interface->speech_numeric_param_tts = w_numeric_param_tts;
	speech_interface->speech_float_param_tts = w_float_param_tts;
  return whisper_speech_load();
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_whisper_tts_shutdown)
{
  return whisper_speech_unload();
}