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

  if (freeAll) {
    if (el->voice_name) free(el->voice_name);
    if (el->session_id) free(el->session_id);
    el->voice_name = NULL;
    el->session_id = NULL;
  }
}
static elevenlabs_t * createOrRetrievePrivateData(switch_speech_handle_t *sh) {
  elevenlabs_t *elevenlabs = (elevenlabs_t *) sh->private_info;  
  if (!elevenlabs) {
    elevenlabs = switch_core_alloc(sh->memory_pool, sizeof(*elevenlabs));
  	sh->private_info = elevenlabs;
    memset(elevenlabs, 0, sizeof(*elevenlabs));
    switch_mutex_init(&elevenlabs->mutex, SWITCH_MUTEX_NESTED, sh->memory_pool);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "allocated elevenlabs_t\n");
  }
  return elevenlabs;
}

/**
 * this is called when FreeSWITCH loads the module
 * if the module is retrieved from cache this will not be called
 * therefore, probably best not to connect to elevenlabs yet
 * Note: if cached, the voice will be provide as a param
 */
static switch_status_t ell_speech_open(switch_speech_handle_t *sh, const char *voice_name, int rate, int channels, switch_speech_flag_t *flags)
{

  elevenlabs_t *elevenlabs = createOrRetrievePrivateData(sh);
  elevenlabs->voice_name = strdup(voice_name);
  elevenlabs->rate = rate;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_speech_open voice: %s, rate %d, channels %d\n", voice_name, rate, channels);
	return elevenlabs_speech_open(elevenlabs);
}

static switch_status_t ell_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
  switch_status_t rc;
	elevenlabs_t *elevenlabs = (elevenlabs_t *) sh->private_info;
	//assert(elevenlabs != NULL);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_speech_close\n");

  switch_mutex_destroy(elevenlabs->mutex);

	rc = elevenlabs_speech_close(elevenlabs);
  clearElevenlabs(elevenlabs, 1);
  return rc;  
}

/**
 * Freeswitch will call this function to feed us text to speak
 */
static switch_status_t ell_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
  char* apiKey = "a079b159403b8c7de3a022a46e272c07";
  switch_uuid_t uuid;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	//char outfile[512] = "";
  elevenlabs_t *elevenlabs = createOrRetrievePrivateData(sh);
  elevenlabs->draining = 0;
  elevenlabs->reads = 0;
  
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_speech_feed_tts\n");
  
	/* Construct temporary file name with a new UUID */
	switch_uuid_get(&uuid);
	switch_uuid_format(uuid_str, &uuid);
	//switch_snprintf(outfile, sizeof(outfile), "%s%s%s.tmp.r8", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, uuid_str);
  //elevenlabs->file = fopen(outfile, "wb");


	return elevenlabs_speech_feed_tts(elevenlabs, apiKey, text, flags);
}

/**
 * This is called at the end, not sure exactly what we need to do here..
 */
static void ell_speech_flush_tts(switch_speech_handle_t *sh)
{
	elevenlabs_t *elevenlabs = (elevenlabs_t *) sh->private_info;
	//assert(elevenlabs != NULL);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_speech_flush_tts\n");
  elevenlabs_speech_flush_tts(elevenlabs);

  clearElevenlabs(elevenlabs, 0);
}

/**
 * Freeswitch calls periodically to get some rendered audio in L16 format. We can provide up to 8k of audio at a time.
 */
static switch_status_t ell_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
	elevenlabs_t *elevenlabs = (elevenlabs_t *) sh->private_info;
  return elevenlabs_speech_read_tts(elevenlabs, data, datalen, flags);
}

static void ell_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{
  elevenlabs_t *elevenlabs = createOrRetrievePrivateData(sh);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "ell_text_param_tts: %s=%s\n", param, val);

  if (0 == strcmp(param, "voice")) {
    if (elevenlabs->voice_name) free(elevenlabs->voice_name);
    elevenlabs->voice_name = strdup(val);
  }
  else if (0 == strcmp(param, "api_key")) {
    if (elevenlabs->api_key) free(elevenlabs->api_key);
    elevenlabs->api_key = strdup(val);
  }
  else if (0 == strcmp(param, "model_id")) {
    if (elevenlabs->model_id) free(elevenlabs->model_id);
    elevenlabs->model_id = strdup(val);
  }
  else if (0 == strcmp(param, "similarity_boost")) {
    if (elevenlabs->similarity_boost) free(elevenlabs->similarity_boost);
    elevenlabs->similarity_boost = strdup(val);
  }
  else if (0 == strcmp(param, "stability")) {
    if (elevenlabs->stability) free(elevenlabs->stability);
    elevenlabs->stability = strdup(val);
  }
  else if (0 == strcmp(param, "style")) {
    if (elevenlabs->style) free(elevenlabs->style);
    elevenlabs->style = strdup(val);
  }
  else if (0 == strcmp(param, "use_speaker_boost")) {
    if (elevenlabs->use_speaker_boost) free(elevenlabs->use_speaker_boost);
    elevenlabs->use_speaker_boost = strdup(val);
  }
  else if (0 == strcmp(param, "optimize_streaming_latency")) {
    if (elevenlabs->optimize_streaming_latency) free(elevenlabs->optimize_streaming_latency);
    elevenlabs->optimize_streaming_latency = strdup(val);
  }
  else if (0 == strcmp(param, "session-uuid")) {
    if (elevenlabs->session_id) free(elevenlabs->session_id);
    elevenlabs->session_id = strdup(val);
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
