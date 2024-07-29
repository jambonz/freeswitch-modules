#include "mod_deepgram_tts.h"
#include "deepgram_glue.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_deepgram_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_deepgram_tts_shutdown);
SWITCH_MODULE_DEFINITION(mod_deepgram_tts, mod_deepgram_tts_load, mod_deepgram_tts_shutdown, NULL);

static void cleardeepgram(deepgram_t* d, int freeAll) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "cleardeepgram\n");
  if (d->api_key) free(d->api_key);

  if (d->request_id) free(d->request_id);
  if (d->reported_model_name) free(d->reported_model_name);
  if (d->reported_model_uuid) free(d->reported_model_uuid);
  if (d->reported_char_count) free(d->reported_char_count);
  if (d->ct) free(d->ct);
  if (d->err_msg) free(d->err_msg);
  if (d->name_lookup_time_ms) free(d->name_lookup_time_ms);
  if (d->connect_time_ms) free(d->connect_time_ms);
  if (d->final_response_time_ms) free(d->final_response_time_ms);
  if (d->cache_filename) free(d->cache_filename);
  if (d->endpoint) free(d->endpoint);
  

  d->api_key = NULL;
  d->request_id = NULL;
  d->endpoint = NULL;

  d->reported_model_name = NULL;
  d->reported_model_uuid = NULL;
  d->reported_char_count = NULL;
  d->ct = NULL;
  d->err_msg = NULL;
  d->name_lookup_time_ms = NULL;
  d->connect_time_ms = NULL;
  d->final_response_time_ms = NULL;
  d->cache_filename = NULL;

  if (freeAll) {
    if (d->voice_name) free(d->voice_name);
    if (d->session_id) free(d->session_id);
    d->voice_name = NULL;
    d->session_id = NULL;
  }
}

static deepgram_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  deepgram_t *d = (deepgram_t *) sh->private_info;  
  if (!d) {
    d = switch_core_alloc(sh->memory_pool, sizeof(*d));
  	sh->private_info = d;
    memset(d, 0, sizeof(*d));
    switch_mutex_init(&d->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "allocated deepgram_t\n");
  }
  return d;
}

switch_status_t d_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{
  deepgram_t *d = createOrRetrievePrivateData(sh);
  d->voice_name = strdup(voice_name);
  d->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "d_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
  return deepgram_speech_open(d);
}

static switch_status_t d_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
  deepgram_t *d = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "d_speech_close\n");

  switch_mutex_destroy(d->mutex);

  rc = deepgram_speech_close(d);
  cleardeepgram(d, 1);
  return rc;
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t d_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  deepgram_t *d = createOrRetrievePrivateData(sh);
  d->draining = 0;
  d->reads = 0;
  d->response_code = 0;
  d->err_msg = NULL;
  d->playback_start_sent = 0;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "d_speech_feed_tts\n");

  return deepgram_speech_feed_tts(d, text, flags);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t d_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
  deepgram_t *d = createOrRetrievePrivateData(sh);
  return deepgram_speech_read_tts(d, data, datalen, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void d_speech_flush_tts(switch_speech_handle_t *sh)
{
  deepgram_t *d = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "d_speech_flush_tts\n");
  deepgram_speech_flush_tts(d);

  cleardeepgram(d, 0);
}

static void d_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  deepgram_t *d = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "d_text_param_tts: %s=%s\n", param, val);
  if (0 == strcmp(param, "api_key")) {
    if (d->api_key) free(d->api_key);
    d->api_key = strdup(val);
  } else if (0 == strcmp(param, "endpoint")) {
    if (d->endpoint) free(d->endpoint);
    d->endpoint = strdup(val);
  } else if (0 == strcmp(param, "voice")) {
    if (d->voice_name) free(d->voice_name);
    d->voice_name = strdup(val);
  } else if (0 == strcmp(param, "session-uuid")) {
    if (d->session_id) free(d->session_id);
    d->session_id = strdup(val);
  } else if (0 == strcmp(param, "write_cache_file") && switch_true(val)) {
    d->cache_audio = 1;
  }
}

static void d_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
}
static void d_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
}

SWITCH_MODULE_LOAD_FUNCTION(mod_deepgram_tts_load)
{
  switch_speech_interface_t *speech_interface;

  *module_interface = switch_loadable_module_create_module_interface(pool, modname);
  speech_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
  speech_interface->interface_name = "deepgram";
  speech_interface->speech_open = d_speech_open;
  speech_interface->speech_close = d_speech_close;
  speech_interface->speech_feed_tts = d_speech_feed_tts;
  speech_interface->speech_read_tts = d_speech_read_tts;
	speech_interface->speech_flush_tts = d_speech_flush_tts;
	speech_interface->speech_text_param_tts = d_text_param_tts;
	speech_interface->speech_numeric_param_tts = d_numeric_param_tts;
	speech_interface->speech_float_param_tts = d_float_param_tts;
  return deepgram_speech_load();
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_deepgram_tts_shutdown)
{
  return deepgram_speech_unload();
}
