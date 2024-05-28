#include "mod_custom_tts.h"
#include "custom_glue.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_custom_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_custom_tts_shutdown);
SWITCH_MODULE_DEFINITION(mod_custom_tts, mod_custom_tts_load, mod_custom_tts_shutdown, NULL);

static void clearCustomVendor(custom_t* c, int freeAll) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "clearCustomVendor\n");
  if (c->auth_token) free(c->auth_token);
  if (c->custom_tts_url) free(c->custom_tts_url);
  if (c->language) free(c->language);
  if (c->ct) free(c->ct);
  if (c->err_msg) free(c->err_msg);
  if (c->name_lookup_time_ms) free(c->name_lookup_time_ms);
  if (c->connect_time_ms) free(c->connect_time_ms);
  if (c->final_response_time_ms) free(c->final_response_time_ms);
  if (c->cache_filename) free(c->cache_filename);
  

  c->auth_token = NULL;
  c->custom_tts_url = NULL;
  c->language = NULL;
  c->ct = NULL;
  c->err_msg = NULL;
  c->name_lookup_time_ms = NULL;
  c->connect_time_ms = NULL;
  c->final_response_time_ms = NULL;
  c->cache_filename = NULL;

  if (freeAll) {
    if (c->voice_name) free(c->voice_name);
    if (c->session_id) free(c->session_id);
    c->voice_name = NULL;
    c->session_id = NULL;
  }
}

static custom_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  custom_t *c = (custom_t *) sh->private_info;  
  if (!c) {
    c = switch_core_alloc(sh->memory_pool, sizeof(*c));
  	sh->private_info = c;
    memset(c, 0, sizeof(*c));
    switch_mutex_init(&c->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "allocated custom_t\n");
  }
  return c;
}

switch_status_t w_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{
  custom_t *c = createOrRetrievePrivateData(sh);
  c->voice_name = strdup(voice_name);
  c->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "w_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
  return custom_speech_open(c);
}

static switch_status_t w_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
  custom_t *c = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "w_speech_close\n");

  switch_mutex_destroy(c->mutex);

  rc = custom_speech_close(c);
  clearCustomVendor(c, 1);
  return rc;
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t w_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  custom_t *c = createOrRetrievePrivateData(sh);
  c->draining = 0;
  c->reads = 0;
  c->response_code = 0;
  c->err_msg = NULL;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "w_speech_feed_tts\n");

  return custom_speech_feed_tts(c, text, flags);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t w_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
  custom_t *c = createOrRetrievePrivateData(sh);
  return custom_speech_read_tts(c, data, datalen, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void w_speech_flush_tts(switch_speech_handle_t *sh)
{
  custom_t *c = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "w_speech_flush_tts\n");
  custom_speech_flush_tts(c);

  clearCustomVendor(c, 0);
}

static void w_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  custom_t *c = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "w_text_param_tts: %s=%s\n", param, val);
  if (0 == strcmp(param, "auth_token")) {
    if (c->auth_token) free(c->auth_token);
    c->auth_token = strdup(val);
  } else if (0 == strcmp(param, "custom_tts_url")) {
    if (c->custom_tts_url) free(c->custom_tts_url);
    c->custom_tts_url = strdup(val);
  } else if (0 == strcmp(param, "language")) {
    if (c->language) free(c->language);
    c->language = strdup(val);
  } else if (0 == strcmp(param, "session-uuid")) {
    if (c->session_id) free(c->session_id);
    c->session_id = strdup(val);
  } else if (0 == strcmp(param, "write_cache_file") && switch_true(val)) {
    c->cache_audio = 1;
  }
}
static void w_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
}
static void w_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
}

SWITCH_MODULE_LOAD_FUNCTION(mod_custom_tts_load)
{
  switch_speech_interface_t *speech_interface;

  *module_interface = switch_loadable_module_create_module_interface(pool, modname);
  speech_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
  speech_interface->interface_name = "custom";
  speech_interface->speech_open = w_speech_open;
  speech_interface->speech_close = w_speech_close;
  speech_interface->speech_feed_tts = w_speech_feed_tts;
  speech_interface->speech_read_tts = w_speech_read_tts;
	speech_interface->speech_flush_tts = w_speech_flush_tts;
	speech_interface->speech_text_param_tts = w_text_param_tts;
	speech_interface->speech_numeric_param_tts = w_numeric_param_tts;
	speech_interface->speech_float_param_tts = w_float_param_tts;
  return custom_speech_load();
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_custom_tts_shutdown)
{
  return custom_speech_unload();
}