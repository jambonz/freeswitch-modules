#include "tts_vendor_parser.h"

#include <sstream>
#include <switch_json.h>

switch_status_t tts_vendor_parse_text(const std::string& text, std::string& url, std::string& body, std::vector<std::string>& headers) {
  if (text.find("vendor=elevenlabs") != std::string::npos) {
    return elevenlabs_parse_text(text, url, body, headers);
  } else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "tts_vendor_parse_text: There is no available parser for text\n");
    return SWITCH_STATUS_FALSE;
  }
}

switch_status_t elevenlabs_parse_text(const std::string& text, std::string& url, std::string& body, std::vector<std::string>& headers) {
  size_t start = text.find("{") + 1;
  size_t end = text.find("}");

  std::string api_key;
  std::string voice_name;
  std::string model_id;
  std::string similarity_boost;
  std::string stability;
  std::string style;
  std::string use_speaker_boost;
  std::string optimize_streaming_latency;

  body = text.substr(end + 1);


  std::string params_string = text.substr(start, end - start);
  std::istringstream ss(params_string);

  while (ss.good()) {
    std::string substr;
    getline(ss, substr, ',');
    substr.erase(0, substr.find_first_not_of(' '));

    size_t equal_pos = substr.find("=");
    std::string key = substr.substr(0, equal_pos);
    std::string value = substr.substr(equal_pos + 1, substr.size());

    if (key == "api_key") {
      api_key = value;
    } else if (key == "voice_name") {
      voice_name = value;
    } else if (key == "model_id") {
      model_id = value;
    } else if (key == "similarity_boost") {
      similarity_boost = value;
    } else if (key == "stability") {
      stability = value;
    } else if (key == "style") {
      style = value;
    } else if (key == "use_speaker_boost") {
      use_speaker_boost = value;
    } else if (key == "modeloptimize_streaming_latency_id") {
      optimize_streaming_latency = value;
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
  url = url_stream.str();

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

    body = cJSON_PrintUnformatted(jResult);

    // Create headers
    std::ostringstream api_key_stream;
    api_key_stream << "xi-api-key: " << api_key;
    headers.push_back("Content-Type: application/json");
    headers.push_back(api_key_stream.str());
  }
  return SWITCH_STATUS_SUCCESS;
}