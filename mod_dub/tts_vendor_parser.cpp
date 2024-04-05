#include "tts_vendor_parser.h"

#include <sstream>
#include <switch_json.h>
#include <map>

switch_status_t azure_parse_text(const std::map<std::string, std::string>& params, const std::string& text, 
  std::string& url, std::string& body, std::vector<std::string>& headers, std::string& proxy) {

  std::string api_key;
  std::string voice_name;
  std::string language;
  std::string region;
  std::string endpoint;
  std::string endpointId;
  std::string http_proxy_ip;
  std::string http_proxy_port;

  for (const auto& pair : params) {
    if (pair.first == "api_key") {
      api_key = pair.second;
    } else if (pair.first == "voice") {
      voice_name = pair.second;
    } else if (pair.first == "language") {
      language = pair.second;
    } else if (pair.first == "region") {
      region = pair.second;
    } else if (pair.first == "endpoint") {
      endpoint = pair.second;
    } else if (pair.first == "endpointId") {
      endpointId = pair.second;
    } else if (pair.first == "http_proxy_ip") {
      http_proxy_ip = pair.second;
    } else if (pair.first == "http_proxy_port") {
      http_proxy_port = pair.second;
    }
  }
  bool isSSML = strncmp(text.c_str(), "<speak", 6) == 0;

  if (region.empty()) {
    region = "westus";
  }
    /* format url*/
  std::ostringstream url_stream;
  if (!endpoint.empty()) {
    url_stream << endpoint;
  } else {
    url_stream << "https://" << region << ".tts.speech.microsoft.com/cognitiveservices/v1";
  }
  url = url_stream.str();

  // Body
  body = text;

  // Create headers
  if (!api_key.empty()) {
    headers.push_back("Ocp-Apim-Subscription-Key: " + api_key);
  }
  headers.push_back("Content-Type: " + isSSML ? "application/ssml+xml" : "text/plain");
  headers.push_back("X-Microsoft-OutputFormat: audio-8khz-128kbitrate-mono-mp3");

  // Proxy
  std::ostringstream proxy_stream;
  if (!http_proxy_ip.empty()) {
    proxy_stream << "http://" << http_proxy_ip;
    if (!http_proxy_port.empty()) {
      proxy_stream << ":" << http_proxy_port;
    }
  }
  proxy = proxy_stream.str();

  return SWITCH_STATUS_SUCCESS;
}

switch_status_t deepgram_parse_text(const std::map<std::string, std::string>& params, const std::string& text, 
  std::string& url, std::string& body, std::vector<std::string>& headers) {

  std::string api_key;
  std::string voice_name;

  for (const auto& pair : params) {
    if (pair.first == "api_key") {
      api_key = pair.second;
    } else if (pair.first == "voice") {
      voice_name = pair.second;
    }
  }

  if (api_key.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "elevenlabs_parse_text: no api_key provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (voice_name.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "elevenlabs_parse_text: no voice_name provided\n");
    return SWITCH_STATUS_FALSE;
  }

  /* format url*/
  std::ostringstream url_stream;
  url_stream << "https://api.deepgram.com/v1/speak?model=" << voice_name << "&encoding=mp3";
  url = url_stream.str();

  /* create the JSON body */
  cJSON * jResult = cJSON_CreateObject();
  cJSON_AddStringToObject(jResult, "text", text.c_str());

  char* _body = cJSON_PrintUnformatted(jResult);
  body = _body;

  cJSON_Delete(jResult);
  free(_body);

  // Create headers
  headers.push_back("Authorization: Token " + api_key);
  headers.push_back("Content-Type: application/json");

  return SWITCH_STATUS_SUCCESS;
}

switch_status_t elevenlabs_parse_text(const std::map<std::string, std::string>& params, const std::string& text, 
  std::string& url, std::string& body, std::vector<std::string>& headers) {

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
  }
  char* _body = cJSON_PrintUnformatted(jResult);
  body = _body;

  cJSON_Delete(jResult);
  free(_body);

  // Create headers
  headers.push_back("xi-api-key: " + api_key);
  headers.push_back("Content-Type: application/json");

  return SWITCH_STATUS_SUCCESS;
}

switch_status_t tts_vendor_parse_text(const std::string& say, std::string& url, std::string& body, std::vector<std::string>& headers, std::string& proxy) {
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
    return elevenlabs_parse_text(params, text, url, body, headers);
  } else if (params["vendor"] == "deepgram") {
    return deepgram_parse_text(params, text, url, body, headers);
  } else if (params["vendor"] == "microsoft") {
    return azure_parse_text(params, text, url, body, headers, proxy);
  } else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "tts_vendor_parse_text: There is no available parser for vendor %s\n", params["vendor"]);
    return SWITCH_STATUS_FALSE;
  }
}