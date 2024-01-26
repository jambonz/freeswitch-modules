/* 
 *
 * mod_elevenlabs_tts.c -- Google GRPC-based text to speech
 *
 */
#include "mod_elevenlabs_tts.h"
#include "elevenlabs_glue.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_elevenlabs_tts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_elevenlabs_tts_shutdown);
SWITCH_MODULE_DEFINITION(mod_elevenlabs_tts, mod_elevenlabs_tts_load, mod_elevenlabs_tts_shutdown, NULL);

static void clearElevenlabs(elevenlabs_t* el, int freeAll) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "clearElevenlabs\n");
  if (el->api_key) free(el->api_key);
  if (el->model_id) free(el->model_id);
  if (el->similarity_boost) free(el->similarity_boost);
  if (el->stability) free(el->stability);
  if (el->style) free(el->style);
  if (el->use_speaker_boost) free(el->use_speaker_boost);
  if (el->optimize_streaming_latency) free(el->optimize_streaming_latency);
  if (el->ct) free(el->ct);
  if (el->reported_latency) free(el->reported_latency);
  if (el->request_id) free(el->request_id);
  if (el->history_item_id) free(el->history_item_id);
  if (el->err_msg) free(el->err_msg);
  if (el->name_lookup_time_ms) free(el->name_lookup_time_ms);
  if (el->connect_time_ms) free(el->connect_time_ms);
  if (el->final_response_time_ms) free(el->final_response_time_ms);
  if (el->cache_filename) free(el->cache_filename);
  
  el->api_key = NULL;
  el->model_id = NULL;
  el->similarity_boost = NULL;
  el->stability = NULL;
  el->style = NULL;
  el->use_speaker_boost = NULL;
  el->optimize_streaming_latency = NULL;
  el->ct = NULL;
  el->reported_latency = NULL;
  el->request_id = NULL;
  el->history_item_id = NULL;
  el->err_msg = NULL;
  el->name_lookup_time_ms = NULL;
  el->connect_time_ms = NULL;
  el->final_response_time_ms = NULL;
  el->cache_filename = NULL;

  el->file = NULL;

  if (freeAll) {
    if (el->voice_name) free(el->voice_name);
    if (el->session_id) free(el->session_id);
    el->voice_name = NULL;
    el->session_id = NULL;
  }
}
static elevenlabs_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  elevenlabs_t *el = (elevenlabs_t *) sh->private_info;  
  if (!el) {
    el = switch_core_alloc(sh->memory_pool, sizeof(*el));
  	sh->private_info = el;
    memset(el, 0, sizeof(*el));
    switch_mutex_init(&el->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "allocated elevenlabs_t\n");
  }
  return el;
}

/**
 * this is called when FreeSWITCH loads the module
 * if the module is retrieved from cache this will not be called
 * therefore, probably best not to connect to elevenlabs yet
 * Note: if cached, the voice will be provide as a param
 */
static switch_status_t ell_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{

  elevenlabs_t *el = createOrRetrievePrivateData(sh);
  el->voice_name = strdup(voice_name);
  el->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
	return elevenlabs_speech_open(el);
}

static switch_status_t ell_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
	elevenlabs_t *el = (elevenlabs_t *) sh->private_info;
	assert(el != NULL);
  
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_speech_close\n");

  switch_mutex_destroy(el->mutex);

	rc = elevenlabs_speech_close(el);
  clearElevenlabs(el, 1);
  return rc;  
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t ell_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  elevenlabs_t *el = createOrRetrievePrivateData(sh);
  el->draining = 0;
  el->reads = 0;
  
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_speech_feed_tts\n");
  
	return elevenlabs_speech_feed_tts(el, text, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void ell_speech_flush_tts(switch_speech_handle_t *sh)
{
	elevenlabs_t *el = (elevenlabs_t *) sh->private_info;
	//assert(el != NULL);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_speech_flush_tts\n");
  elevenlabs_speech_flush_tts(el);

  clearElevenlabs(el, 0);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t ell_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
	elevenlabs_t *el = (elevenlabs_t *) sh->private_info;
  return elevenlabs_speech_read_tts(el, data, datalen, flags);
}

static void ell_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  elevenlabs_t *el = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_text_param_tts: %s=%s\n", param, val);

  if (0 == strcmp(param, "voice")) {
    if (el->voice_name) free(el->voice_name);
    el->voice_name = strdup(val);
  }
  else if (0 == strcmp(param, "api_key")) {
    if (el->api_key) free(el->api_key);
    el->api_key = strdup(val);
  }
  else if (0 == strcmp(param, "model_id")) {
    if (el->model_id) free(el->model_id);
    el->model_id = strdup(val);
  }
  else if (0 == strcmp(param, "similarity_boost")) {
    if (el->similarity_boost) free(el->similarity_boost);
    el->similarity_boost = strdup(val);
  }
  else if (0 == strcmp(param, "stability")) {
    if (el->stability) free(el->stability);
    el->stability = strdup(val);
  }
  else if (0 == strcmp(param, "style")) {
    if (el->style) free(el->style);
    el->style = strdup(val);
  }
  else if (0 == strcmp(param, "use_speaker_boost")) {
    if (el->use_speaker_boost) free(el->use_speaker_boost);
    el->use_speaker_boost = strdup(val);
  }
  else if (0 == strcmp(param, "optimize_streaming_latency")) {
    if (el->optimize_streaming_latency) free(el->optimize_streaming_latency);
    el->optimize_streaming_latency = strdup(val);
  }
  else if (0 == strcmp(param, "write_cache_file") && switch_true(val)) {
    el->cache_audio = 1;
  }
  else if (0 == strcmp(param, "session-uuid")) {
    if (el->session_id) free(el->session_id);
    el->session_id = strdup(val);
  }
}

static void ell_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{
}

static void ell_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{
}

SWITCH_MODULE_LOAD_FUNCTION(mod_elevenlabs_tts_load)
{
	switch_speech_interface_t *speech_interface;

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	speech_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
	speech_interface->interface_name = "elevenlabs";
	speech_interface->speech_open = ell_speech_open;
	speech_interface->speech_close = ell_speech_close;
	speech_interface->speech_feed_tts = ell_speech_feed_tts;
	speech_interface->speech_read_tts = ell_speech_read_tts;
	speech_interface->speech_flush_tts = ell_speech_flush_tts;
	speech_interface->speech_text_param_tts = ell_text_param_tts;
	speech_interface->speech_numeric_param_tts = ell_numeric_param_tts;
	speech_interface->speech_float_param_tts = ell_float_param_tts;

	return elevenlabs_speech_load();
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_elevenlabs_tts_shutdown)
{
  return elevenlabs_speech_unload();
}
