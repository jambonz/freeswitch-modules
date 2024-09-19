#include "mod_playht_tts.h"
#include "playht_glue.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_playht_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_playht_tts_shutdown);
SWITCH_MODULE_DEFINITION(mod_playht_tts, mod_playht_tts_load, mod_playht_tts_shutdown, NULL);

static void clearPlayht(playht_t* p, int freeAll) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "clearPlayht\n");
  if (p->api_key) free(p->api_key);
  if (p->user_id) free(p->user_id);
  if (p->quality) free(p->quality);
  if (p->speed) free(p->speed);
  if (p->seed) free(p->seed);
  if (p->temperature) free(p->temperature);
  if (p->voice_engine) free(p->voice_engine);
  if (p->emotion) free(p->emotion);
  if (p->voice_guidance) free(p->voice_guidance);
  if (p->style_guidance) free(p->style_guidance);
  if (p->text_guidance) free(p->text_guidance);


  if (p->request_id) free(p->request_id);
  if (p->ct) free(p->ct);
  if (p->err_msg) free(p->err_msg);
  if (p->name_lookup_time_ms) free(p->name_lookup_time_ms);
  if (p->connect_time_ms) free(p->connect_time_ms);
  if (p->final_response_time_ms) free(p->final_response_time_ms);
  if (p->cache_filename) free(p->cache_filename);
  if (p->url) free(p->url);


  p->api_key = NULL;
  p->user_id = NULL;
  p->quality = NULL;
  p->speed = NULL;
  p->seed = NULL;
  p->temperature = NULL;
  p->voice_engine = NULL;
  p->emotion = NULL;
  p->voice_guidance = NULL;
  p->style_guidance = NULL;
  p->text_guidance = NULL;

  p->request_id = NULL;
  p->ct = NULL;
  p->err_msg = NULL;
  p->name_lookup_time_ms = NULL;
  p->connect_time_ms = NULL;
  p->final_response_time_ms = NULL;
  p->cache_filename = NULL;
  p->url = NULL;

  if (freeAll) {
    if (p->voice_name) free(p->voice_name);
    if (p->session_id) free(p->session_id);
    p->voice_name = NULL;
    p->session_id = NULL;
  }
}

static playht_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  playht_t *p = (playht_t *) sh->private_info;
  if (!p) {
    p = switch_core_alloc(sh->memory_pool, sizeof(*p));
  	sh->private_info = p;
    memset(p, 0, sizeof(*p));
    switch_mutex_init(&p->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "allocated playht_t\n");
  }
  return p;
}

switch_status_t p_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{
  playht_t *p = createOrRetrievePrivateData(sh);
  p->voice_name = strdup(voice_name);
  p->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "p_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
  return playht_speech_open(p);
}

static switch_status_t p_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
  playht_t *p = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "p_speech_close\n");

  switch_mutex_destroy(p->mutex);

  rc = playht_speech_close(p);
  clearPlayht(p, 1);
  return rc;
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t p_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  playht_t *p = createOrRetrievePrivateData(sh);
  p->draining = 0;
  p->reads = 0;
  p->response_code = 0;
  p->err_msg = NULL;
  p->playback_start_sent = 0;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "p_speech_feed_tts\n");

  return playht_speech_feed_tts(p, text, flags);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t p_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
  playht_t *p = createOrRetrievePrivateData(sh);
  return playht_speech_read_tts(p, data, datalen, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void p_speech_flush_tts(switch_speech_handle_t *sh)
{
  playht_t *p = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "p_speech_flush_tts\n");
  playht_speech_flush_tts(p);

  clearPlayht(p, 0);
}

static void p_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  playht_t *p = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "p_text_param_tts: %s=%s\n", param, val);
  if (0 == strcmp(param, "api_key")) {
    if (p->api_key) free(p->api_key);
    p->api_key = strdup(val);
  } else if (0 == strcmp(param, "user_id")) {
    if (p->user_id) free(p->user_id);
    p->user_id = strdup(val);
  } else if (0 == strcmp(param, "quality")) {
    if (p->quality) free(p->quality);
    p->quality = strdup(val);
  } else if (0 == strcmp(param, "speed")) {
    if (p->speed) free(p->speed);
    p->speed = strdup(val);
  } else if (0 == strcmp(param, "seed")) {
    if (p->seed) free(p->seed);
    p->seed = strdup(val);
  } else if (0 == strcmp(param, "temperature")) {
    if (p->temperature) free(p->temperature);
    p->temperature = strdup(val);
  } else if (0 == strcmp(param, "voice_engine")) {
    if (p->voice_engine) free(p->voice_engine);
    p->voice_engine = strdup(val);
  } else if (0 == strcmp(param, "emotion")) {
    if (p->emotion) free(p->emotion);
    p->emotion = strdup(val);
  } else if (0 == strcmp(param, "voice_guidance")) {
    if (p->voice_guidance) free(p->voice_guidance);
    p->voice_guidance = strdup(val);
  } else if (0 == strcmp(param, "style_guidance")) {
    if (p->style_guidance) free(p->style_guidance);
    p->style_guidance = strdup(val);
  } else if (0 == strcmp(param, "session-uuid")) {
    if (p->session_id) free(p->session_id);
    p->session_id = strdup(val);
  } else if (0 == strcmp(param, "write_cache_file") && switch_true(val)) {
    p->cache_audio = 1;
  }
}
static void p_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
}
static void p_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
}

SWITCH_MODULE_LOAD_FUNCTION(mod_playht_tts_load)
{
  switch_speech_interface_t *speech_interface;

  *module_interface = switch_loadable_module_create_module_interface(pool, modname);
  speech_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
  speech_interface->interface_name = "playht";
  speech_interface->speech_open = p_speech_open;
  speech_interface->speech_close = p_speech_close;
  speech_interface->speech_feed_tts = p_speech_feed_tts;
  speech_interface->speech_read_tts = p_speech_read_tts;
	speech_interface->speech_flush_tts = p_speech_flush_tts;
	speech_interface->speech_text_param_tts = p_text_param_tts;
	speech_interface->speech_numeric_param_tts = p_numeric_param_tts;
	speech_interface->speech_float_param_tts = p_float_param_tts;
  return playht_speech_load();
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_playht_tts_shutdown)
{
  return playht_speech_unload();
}