#include "mod_verbio_tts.h"
#include "verbio_glue.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_verbio_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_verbio_tts_shutdown);
SWITCH_MODULE_DEFINITION(mod_verbio_tts, mod_verbio_tts_load, mod_verbio_tts_shutdown, NULL);

static void clearverbio(verbio_t* v, int freeAll) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "clearverbio\n");
  if (v->access_token) free(v->access_token);

  if (v->ct) free(v->ct);
  if (v->err_msg) free(v->err_msg);
  if (v->name_lookup_time_ms) free(v->name_lookup_time_ms);
  if (v->connect_time_ms) free(v->connect_time_ms);
  if (v->final_response_time_ms) free(v->final_response_time_ms);
  if (v->cache_filename) free(v->cache_filename);
  

  v->access_token = NULL;
  v->ct = NULL;
  v->err_msg = NULL;
  v->name_lookup_time_ms = NULL;
  v->connect_time_ms = NULL;
  v->final_response_time_ms = NULL;
  v->cache_filename = NULL;

  if (freeAll) {
    if (v->voice_name) free(v->voice_name);
    if (v->session_id) free(v->session_id);
    v->voice_name = NULL;
    v->session_id = NULL;
  }
}

static verbio_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  verbio_t *v = (verbio_t *) sh->private_info;  
  if (!v) {
    v = switch_core_alloc(sh->memory_pool, sizeof(*v));
  	sh->private_info = v;
    memset(v, 0, sizeof(*v));
    switch_mutex_init(&v->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "allocated verbio_t\n");
  }
  return v;
}

switch_status_t v_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{
  verbio_t *v = createOrRetrievePrivateData(sh);
  v->voice_name = strdup(voice_name);
  v->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "v_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
  return verbio_speech_open(v);
}

static switch_status_t v_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
  verbio_t *v = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "v_speech_close\n");

  switch_mutex_destroy(v->mutex);

  rc = verbio_speech_close(v);
  clearverbio(v, 1);
  return rc;
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t v_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  verbio_t *v = createOrRetrievePrivateData(sh);
  v->draining = 0;
  v->reads = 0;
  v->response_code = 0;
  v->err_msg = NULL;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "v_speech_feed_tts\n");

  return verbio_speech_feed_tts(v, text, flags);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t v_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
  verbio_t *v = createOrRetrievePrivateData(sh);
  return verbio_speech_read_tts(v, data, datalen, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void v_speech_flush_tts(switch_speech_handle_t *sh)
{
  verbio_t *v = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "v_speech_flush_tts\n");
  verbio_speech_flush_tts(v);

  clearverbio(v, 0);
}

static void v_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  verbio_t *v = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "v_text_param_tts: %s=%s\n", param, val);
  if (0 == strcmp(param, "access_token")) {
    if (v->access_token) free(v->access_token);
    v->access_token = strdup(val);
  } else if (0 == strcmp(param, "voice")) {
    if (v->voice_name) free(v->voice_name);
    v->voice_name = strdup(val);
  } else if (0 == strcmp(param, "session-uuid")) {
    if (v->session_id) free(v->session_id);
    v->session_id = strdup(val);
  } else if (0 == strcmp(param, "write_cache_file") && switch_true(val)) {
    v->cache_audio = 1;
  }
}

static void v_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
}
static void v_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
}

SWITCH_MODULE_LOAD_FUNCTION(mod_verbio_tts_load)
{
  switch_speech_interface_t *speech_interface;

  *module_interface = switch_loadable_module_create_module_interface(pool, modname);
  speech_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
  speech_interface->interface_name = "verbio";
  speech_interface->speech_open = v_speech_open;
  speech_interface->speech_close = v_speech_close;
  speech_interface->speech_feed_tts = v_speech_feed_tts;
  speech_interface->speech_read_tts = v_speech_read_tts;
	speech_interface->speech_flush_tts = v_speech_flush_tts;
	speech_interface->speech_text_param_tts = v_text_param_tts;
	speech_interface->speech_numeric_param_tts = v_numeric_param_tts;
	speech_interface->speech_float_param_tts = v_float_param_tts;
  return verbio_speech_load();
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_verbio_tts_shutdown)
{
  return verbio_speech_unload();
}
