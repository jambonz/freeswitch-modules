#include "tts_vendor_parser.h"

#include <sstream>
#include <switch_json.h>
#include <map>

switch_status_t elevenlabs_parse_text(std::map<std::string, std::string> params, std::string text,  HttpPayload_t& payload) {
  std::string api_key;
  std::string voice_name;
  std::string model_id;
  std::string similarity_boost;
  std::string stability;
  std::string style;
  std::string use_speaker_boost;
  std::string optimize_streaming_latency;

  for (const auto& pair : params) {
    if (pair.first == "api_key") {
      api_key = pair.second;
    } else if (pair.first == "voice") {
      voice_name = pair.second;
    } else if (pair.first == "model_id") {
      model_id = pair.second;
    } else if (pair.first == "similarity_boost") {
      similarity_boost = pair.second;
    } else if (pair.first == "stability") {
      stability = pair.second;
    } else if (pair.first == "style") {
      style = pair.second;
    } else if (pair.first == "use_speaker_boost") {
      use_speaker_boost = pair.second;
    } else if (pair.first == "modeloptimize_streaming_latency_id") {
      optimize_streaming_latency = pair.second;
    }
  }

  if (api_key.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "elevenlabs_parse_text: no api_key provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (model_id.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "elevenlabs_parse_text: no model_id provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (optimize_streaming_latency.empty()) {
    optimize_streaming_latency = "2";
  }

  // URL
  std::ostringstream url_stream;
  url_stream << "https://api.elevenlabs.io/v1/text-to-speech/" << voice_name << "/stream?";
  url_stream << "optimize_streaming_latency=" << optimize_streaming_latency << "&output_format=mp3_44100_128";
  payload.url = url_stream.str();

  /* create the JSON body */
  cJSON * jResult = cJSON_CreateObject();
  cJSON_AddStringToObject(jResult, "model_id", model_id.c_str());
  cJSON_AddStringToObject(jResult, "text", text.c_str());
  if (!similarity_boost.empty() || !style.empty() || !use_speaker_boost.empty() || !stability.empty()) {
    cJSON * jVoiceSettings = cJSON_CreateObject();
    cJSON_AddItemToObject(jResult, "voice_settings", jVoiceSettings);
    if (!similarity_boost.empty()) {
      cJSON_AddStringToObject(jVoiceSettings, "similarity_boost", similarity_boost.c_str());
    }
    if (!style.empty()) {
      cJSON_AddStringToObject(jVoiceSettings, "style", style.c_str());
    }
    if (!use_speaker_boost.empty()) {
      cJSON_AddStringToObject(jVoiceSettings, "use_speaker_boost", use_speaker_boost.c_str());
    }
    if (!stability.empty()) {
      cJSON_AddStringToObject(jVoiceSettings, "stability", stability.c_str());
    }
  }
  char* body = cJSON_PrintUnformatted(jResult);
  payload.body = body;
  free(body);

  // Create headers
  std::ostringstream api_key_stream;
  api_key_stream << "xi-api-key: " << api_key;
  payload.headers.push_back(api_key_stream.str());
  payload.headers.push_back("Content-Type: application/json");

  return SWITCH_STATUS_SUCCESS;
}

switch_status_t tts_vendor_parse_text(const std::string& say, HttpPayload_t& payload) {
  // Parse Say string
  size_t start = say.find("{") + 1;
  size_t end = say.find("}");

  std::string text = say.substr(end + 1);

  std::string params_string = say.substr(start, end - start);
  std::istringstream ss(params_string);
  std::map<std::string, std::string> params;

  while (ss.good()) {
    std::string substr;
    getline(ss, substr, ',');
    substr.erase(0, substr.find_first_not_of(' '));

    size_t equal_pos = substr.find("=");
    std::string key = substr.substr(0, equal_pos);
    std::string value = substr.substr(equal_pos + 1, substr.size());

    params[key] = value;
  }

  if (params["vendor"] == "elevenlabs") {
    return elevenlabs_parse_text(params, text, payload);
  } else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "tts_vendor_parse_text: There is no available parser for text\n");
    return SWITCH_STATUS_FALSE;
  }
}