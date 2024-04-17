#include "mod_google_tts.h"
#include "google_glue.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_google_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_google_tts_shutdown);
SWITCH_MODULE_DEFINITION(mod_google_tts, mod_google_tts_load, mod_google_tts_shutdown, NULL);

static void clearGoogle(google_t* g, int freeAll) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "clearGoogle\n");

  if (g->cache_filename) free(g->cache_filename);
  if (g->credential) free(g->credential);
  if (g->model) free(g->model);
  if (g->reported_usage) free(g->reported_usage);
  if (g->gender) free(g->gender);
  if (g->language) free(g->language);
  if (g->err_msg) free(g->err_msg);
  
  g->cache_filename = NULL;
  g->credential = NULL;
  g->model = NULL;
  g->reported_usage = NULL;
  g->gender = NULL;
  g->language = NULL;
  g->err_msg = NULL;


  if (freeAll) {
    if (g->voice_name) free(g->voice_name);
    if (g->session_id) free(g->session_id);
    g->voice_name = NULL;
    g->session_id = NULL;
  }

}

static google_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  google_t *g = (google_t *) sh->private_info;  
  if (!g) {
    g = switch_core_alloc(sh->memory_pool, sizeof(*g));
  	sh->private_info = g;
    memset(g, 0, sizeof(*g));
    switch_mutex_init(&g->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "allocated google_t\n");
  }
  return g;
}

switch_status_t g_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{
  google_t *g = createOrRetrievePrivateData(sh);
  g->voice_name = strdup(voice_name);
  g->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "g_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
  return google_speech_open(g);
}

static switch_status_t g_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
  google_t *g = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "g_speech_close\n");

  switch_mutex_destroy(g->mutex);

  rc = google_speech_close(g);
  clearGoogle(g, 1);
  return rc;
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t g_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  google_t *g = createOrRetrievePrivateData(sh);
  g->draining = 0;
  g->reads = 0;
  g->flushed = 0;

  return google_speech_feed_tts(g, text, flags);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t g_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
  google_t *g = createOrRetrievePrivateData(sh);
  return google_speech_read_tts(g, data, datalen, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void g_speech_flush_tts(switch_speech_handle_t *sh)
{
  google_t *g = createOrRetrievePrivateData(sh);
  google_speech_flush_tts(g);

  clearGoogle(g, 0);
}

static void g_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  google_t *g = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "g_text_param_tts: %s=%s\n", param, val);
  if (0 == strcmp(param, "credential")) {
    if (g->credential) free(g->credential);
    g->credential = strdup(val);
  } else if (0 == strcmp(param, "voice")) {
    if (g->voice_name) free(g->voice_name);
    g->voice_name = strdup(val);
  } else if (0 == strcmp(param, "model")) {
    if (g->model) free(g->model);
    g->model = strdup(val);
  } else if (0 == strcmp(param, "reported_usage")) {
    if (g->reported_usage) free(g->reported_usage);
    g->reported_usage = strdup(val);
  } else if (0 == strcmp(param, "language")) {
    if (g->language) free(g->language);
    g->language = strdup(val);
  } else if (0 == strcmp(param, "session-uuid")) {
    if (g->session_id) free(g->session_id);
    g->session_id = strdup(val);
  } else if (0 == strcmp(param, "write_cache_file") && switch_true(val)) {
    g->cache_audio = 1;
  }
}

static void g_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
}
static void g_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
}

SWITCH_MODULE_LOAD_FUNCTION(mod_google_tts_load)
{
  switch_speech_interface_t *speech_interface;

  *module_interface = switch_loadable_module_create_module_interface(pool, modname);
  speech_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
  speech_interface->interface_name = "google";
  speech_interface->speech_open = g_speech_open;
  speech_interface->speech_close = g_speech_close;
  speech_interface->speech_feed_tts = g_speech_feed_tts;
  speech_interface->speech_read_tts = g_speech_read_tts;
	speech_interface->speech_flush_tts = g_speech_flush_tts;
	speech_interface->speech_text_param_tts = g_text_param_tts;
	speech_interface->speech_numeric_param_tts = g_numeric_param_tts;
	speech_interface->speech_float_param_tts = g_float_param_tts;
  return google_speech_load();
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_google_tts_shutdown)
{
  return google_speech_unload();
}