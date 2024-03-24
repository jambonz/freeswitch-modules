#include "mod_azure_tts.h"
#include "azure_glue.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_azure_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_azure_tts_shutdown);
SWITCH_MODULE_DEFINITION(mod_azure_tts, mod_azure_tts_load, mod_azure_tts_shutdown, NULL);

static void clearAzure(azure_t* a, int freeAll) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "clearAzure\n");

  if (a->cache_filename) free(a->cache_filename);
  if (a->api_key) free(a->api_key);
  if (a->language) free(a->language);
  if (a->region) free(a->region);
  if (a->endpoint) free(a->endpoint);
  if (a->endpointId) free(a->endpointId);
  if (a->err_msg) free(a->err_msg);
  if (a->http_proxy_ip) free(a->http_proxy_ip);
  if (a->http_proxy_port) free(a->http_proxy_port);

  
  a->cache_filename = NULL;
  a->api_key = NULL;
  a->language = NULL;
  a->region = NULL;
  a->endpoint = NULL;
  a->endpointId = NULL;
  a->err_msg = NULL;
  a->http_proxy_ip = NULL;
  a->http_proxy_port = NULL;


  if (freeAll) {
    if (a->voice_name) free(a->voice_name);
    if (a->session_id) free(a->session_id);
    a->voice_name = NULL;
    a->session_id = NULL;
  }

}

static azure_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  azure_t *a = (azure_t *) sh->private_info;  
  if (!a) {
    a = switch_core_alloc(sh->memory_pool, sizeof(*a));
  	sh->private_info = a;
    memset(a, 0, sizeof(*a));
    switch_mutex_init(&a->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "allocated azure_t\n");
  }
  return a;
}

switch_status_t a_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{
  azure_t *a = createOrRetrievePrivateData(sh);
  a->voice_name = strdup(voice_name);
  a->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "a_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
  return azure_speech_open(a);
}

static switch_status_t a_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
  azure_t *a = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "a_speech_close\n");

  switch_mutex_destroy(a->mutex);

  rc = azure_speech_close(a);
  clearAzure(a, 1);
  return rc;
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t a_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  azure_t *a = createOrRetrievePrivateData(sh);
  a->draining = 0;
  a->reads = 0;
  a->flushed = 0;
  a->samples_rate = 0;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "a_speech_feed_tts\n");

  return azure_speech_feed_tts(a, text, flags);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t a_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
  azure_t *a = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "a_speech_read_tts\n");
  return azure_speech_read_tts(a, data, datalen, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void a_speech_flush_tts(switch_speech_handle_t *sh)
{
  azure_t *a = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "w_speech_flush_tts\n");
  azure_speech_flush_tts(a);

  clearAzure(a, 0);
}

static void a_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  azure_t *a = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "a_text_param_tts: %s=%s\n", param, val);
  if (0 == strcmp(param, "api_key")) {
    if (a->api_key) free(a->api_key);
    a->api_key = strdup(val);
  } else if (0 == strcmp(param, "region")) {
    if (a->region) free(a->region);
    a->region = strdup(val);
  } else if (0 == strcmp(param, "voice")) {
    if (a->voice_name) free(a->voice_name);
    a->voice_name = strdup(val);
  } else if (0 == strcmp(param, "language")) {
    if (a->language) free(a->language);
    a->language = strdup(val);
  } else if (0 == strcmp(param, "endpoint")) {
    if (a->endpoint) free(a->endpoint);
    a->endpoint = strdup(val);
  } else if (0 == strcmp(param, "endpointId")) {
    if (a->endpointId) free(a->endpointId);
    a->endpointId = strdup(val);
  } else if (0 == strcmp(param, "http_proxy_ip")) {
    if (a->http_proxy_ip) free(a->http_proxy_ip);
    a->http_proxy_ip = strdup(val);
  } else if (0 == strcmp(param, "http_proxy_port")) {
    if (a->http_proxy_port) free(a->http_proxy_port);
    a->http_proxy_port = strdup(val);
  } else if (0 == strcmp(param, "session-uuid")) {
    if (a->session_id) free(a->session_id);
    a->session_id = strdup(val);
  } else if (0 == strcmp(param, "write_cache_file") && switch_true(val)) {
    a->cache_audio = 1;
  }
}

static void a_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
}
static void a_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
}

SWITCH_MODULE_LOAD_FUNCTION(mod_azure_tts_load)
{
  switch_speech_interface_t *speech_interface;

  *module_interface = switch_loadable_module_create_module_interface(pool, modname);
  speech_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
  speech_interface->interface_name = "microsoft";
  speech_interface->speech_open = a_speech_open;
  speech_interface->speech_close = a_speech_close;
  speech_interface->speech_feed_tts = a_speech_feed_tts;
  speech_interface->speech_read_tts = a_speech_read_tts;
	speech_interface->speech_flush_tts = a_speech_flush_tts;
	speech_interface->speech_text_param_tts = a_text_param_tts;
	speech_interface->speech_numeric_param_tts = a_numeric_param_tts;
	speech_interface->speech_float_param_tts = a_float_param_tts;
  return azure_speech_load();
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_azure_tts_shutdown)
{
  return azure_speech_unload();
}