#include "tts_vendor_parser.h"

#include <sstream>
#include <switch_json.h>
#include <map>


switch_status_t custom_vendor_parse_text(const std::map<std::string, std::string>& params, const std::string& text, 
  std::string& url, std::string& body, std::vector<std::string>& headers) {
  std::string auth_token;
  std::string voice_name;
  std::string custom_tts_url;
  std::string language;

  for (const auto& pair : params) {
    if (pair.first == "auth_token") {
      auth_token = pair.second;
    } else if (pair.first == "voice") {
      voice_name = pair.second;
    } else if (pair.first == "custom_tts_url") {
      custom_tts_url = pair.second;
    } else if (pair.first == "language") {
      language = pair.second;
    }
  }

  if (custom_tts_url.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "custom_vendor_parse_text: no custom_tts_url provided\n");
    return SWITCH_STATUS_FALSE;
  }

  url = custom_tts_url;

  /* create the JSON body */
  cJSON * jResult = cJSON_CreateObject();
  cJSON_AddStringToObject(jResult, "text", text.c_str());
  cJSON_AddStringToObject(jResult, "type", text.substr(0, 6) == "<speak" ? "ssml" : "text");
  cJSON_AddNumberToObject(jResult, "samplingRate", 8000);
  if (!voice_name.empty()) {
    cJSON_AddStringToObject(jResult, "voice", voice_name.c_str());
  }
  if (!language.empty()) {
    cJSON_AddStringToObject(jResult, "language", language.c_str());
  }
  char* _body = cJSON_PrintUnformatted(jResult);
  body = _body;

  cJSON_Delete(jResult);
  free(_body);

  // Create headers
  if (!auth_token.empty()) {
    headers.push_back("Authorization: Bearer " + auth_token);
  }
  headers.push_back("Accept: audio/mp3");
  headers.push_back("Content-Type: application/json");

  return SWITCH_STATUS_SUCCESS;
}

switch_status_t rimelabs_parse_text(const std::map<std::string, std::string>& params, const std::string& text, 
  std::string& url, std::string& body, std::vector<std::string>& headers) {
  std::string api_key;
  std::string voice_name;
  std::string model_id;
  std::string speed_alpha;
  std::string reduce_latency;

  for (const auto& pair : params) {
    if (pair.first == "api_key") {
      api_key = pair.second;
    } else if (pair.first == "voice") {
      voice_name = pair.second;
    } else if (pair.first == "model_id") {
      model_id = pair.second;
    } else if (pair.first == "speed_alpha") {
      speed_alpha = pair.second;
    }  else if (pair.first == "reduce_latency") {
      reduce_latency = pair.second;
    }
  }

  if (api_key.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "rimelabs_parse_text: no api_key provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (voice_name.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "rimelabs_parse_text: no voice_name provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (model_id.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "rimelabs_parse_text: no model_id provided\n");
    return SWITCH_STATUS_FALSE;
  }

  url = "https://users.rime.ai/v1/rime-tts";

  /* create the JSON body */
  cJSON * jResult = cJSON_CreateObject();
  cJSON_AddStringToObject(jResult, "text", text.c_str());
  cJSON_AddNumberToObject(jResult, "samplingRate", 8000);
  if (!voice_name.empty()) {
    cJSON_AddStringToObject(jResult, "speaker", voice_name.c_str());
  }
  if (!model_id.empty()) {
    cJSON_AddStringToObject(jResult, "modelId", model_id.c_str());
  }
  if (!speed_alpha.empty()) {
    cJSON_AddNumberToObject(jResult, "speedAlpha", std::strtof(speed_alpha.c_str(), nullptr));
  }
  if (!reduce_latency.empty()) {
    cJSON_AddBoolToObject(jResult, "reduceLatency", !strcmp(reduce_latency.c_str(), "true") ? 1 : 0);
  }
  char* _body = cJSON_PrintUnformatted(jResult);
  body = _body;

  cJSON_Delete(jResult);
  free(_body);

  // Create headers
  headers.push_back("Authorization: Bearer " + api_key);
  headers.push_back("Accept: audio/mp3");
  headers.push_back("Content-Type: application/json");

  return SWITCH_STATUS_SUCCESS;
}

switch_status_t whisper_parse_text(const std::map<std::string, std::string>& params, const std::string& text, 
  std::string& url, std::string& body, std::vector<std::string>& headers) {
  std::string api_key;
  std::string voice_name;
  std::string model_id;
  std::string speed;

  for (const auto& pair : params) {
    if (pair.first == "api_key") {
      api_key = pair.second;
    } else if (pair.first == "voice") {
      voice_name = pair.second;
    } else if (pair.first == "model_id") {
      model_id = pair.second;
    } else if (pair.first == "speed") {
      speed = pair.second;
    }
  }

  if (api_key.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "whisper_parse_text: no api_key provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (model_id.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "whisper_parse_text: no model_id provided\n");
    return SWITCH_STATUS_FALSE;
  }

  url = "https://api.openai.com/v1/audio/speech";

  /* create the JSON body */
  cJSON * jResult = cJSON_CreateObject();
  cJSON_AddStringToObject(jResult, "model", model_id.c_str());
  cJSON_AddStringToObject(jResult, "input", text.c_str());
  cJSON_AddStringToObject(jResult, "voice", voice_name.c_str());
  cJSON_AddStringToObject(jResult, "response_format", "mp3");
  if (!speed.empty()) {
    cJSON_AddStringToObject(jResult, "speed", speed.c_str());
  }
  char* _body = cJSON_PrintUnformatted(jResult);
  body = _body;

  cJSON_Delete(jResult);
  free(_body);

  // Create headers
  headers.push_back("Authorization: Bearer " + api_key);
  headers.push_back("Content-Type: application/json");

  return SWITCH_STATUS_SUCCESS;
}

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

  if (language.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "azure_parse_text: no language provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (voice_name.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "azure_parse_text: no voice_name provided\n");
    return SWITCH_STATUS_FALSE;
  }

  if (region.empty()) {
    region = "westus";
  }
    /* format url*/
  url = !endpoint.empty() ? endpoint : "https://" + region + ".tts.speech.microsoft.com/cognitiveservices/v1";

  // Body  
  if (strncmp(text.c_str(), "<speak", 6) == 0) {
    body = text;
  } else {
    std::ostringstream body_stream;
    body_stream << "<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xmlns:mstts=\"https://www.w3.org/2001/mstts\" xml:lang=\"" << language << "\">";
    body_stream << "<voice name=\"" << voice_name << "\">";
    body_stream << text;
    body_stream << "</voice>";
    body_stream << "</speak>";
    body = body_stream.str();
  }

  // Create headers
  if (!api_key.empty()) {
    headers.push_back("Ocp-Apim-Subscription-Key: " + api_key);
  }
  if (!endpointId.empty()) {
    headers.push_back("X-Microsoft-EndpointId: " + endpointId);
  }
  headers.push_back("Content-Type: application/ssml+xml");
  headers.push_back("X-Microsoft-OutputFormat: audio-16khz-32kbitrate-mono-mp3");

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
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "deepgram_parse_text: no api_key provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (voice_name.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "deepgram_parse_text: no voice_name provided\n");
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

switch_status_t playht_parse_text(const std::map<std::string, std::string>& params, const std::string& text, 
  std::string& url, std::string& body, std::vector<std::string>& headers) {

  std::string api_key;
  std::string voice_name;
  std::string user_id;
  std::string quality;
  std::string speed;
  std::string seed;
  std::string temperature;
  std::string voice_engine;
  std::string emotion;
  std::string voice_guidance;
  std::string style_guidance;
  std::string text_guidance;

  for (const auto& pair : params) {
    if (pair.first == "api_key") {
      api_key = pair.second;
    } else if (pair.first == "voice") {
      voice_name = pair.second;
    } else if (pair.first == "user_id") {
      user_id = pair.second;
    } else if (pair.first == "quality") {
      quality = pair.second;
    } else if (pair.first == "speed") {
      speed = pair.second;
    } else if (pair.first == "seed") {
      seed = pair.second;
    } else if (pair.first == "temperature") {
      temperature = pair.second;
    } else if (pair.first == "voice_engine") {
      voice_engine = pair.second;
    }  else if (pair.first == "emotion") {
      emotion = pair.second;
    }  else if (pair.first == "voice_guidance") {
      voice_guidance = pair.second;
    }  else if (pair.first == "style_guidance") {
      style_guidance = pair.second;
    }  else if (pair.first == "text_guidance") {
      text_guidance = pair.second;
    }
  }

  if (api_key.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "playht_parse_text: no api_key provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (user_id.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "playht_parse_text: no user_id provided\n");
    return SWITCH_STATUS_FALSE;
  }
  if (voice_name.empty()) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "playht_parse_text: no voice_name provided\n");
    return SWITCH_STATUS_FALSE;
  }

  // URL
  url = "https://api.play.ht/api/v2/tts/stream";

  /* create the JSON body */
  cJSON * jResult = cJSON_CreateObject();
  cJSON_AddStringToObject(jResult, "text", text.c_str());
  cJSON_AddStringToObject(jResult, "voice", voice_name.c_str());
  cJSON_AddStringToObject(jResult, "output_format", "mp3");
  cJSON_AddNumberToObject(jResult, "sample_rate", 8000);
  if (!voice_engine.empty()) {
    cJSON_AddStringToObject(jResult, "voice_engine", voice_engine.c_str());
  }
  if (!quality.empty()) {
    cJSON_AddStringToObject(jResult, "quality", quality.c_str());
  }
  if (!speed.empty()) {
      double val = strtod(speed.c_str(), NULL);
      if (val != 0.0) {
        cJSON_AddNumberToObject(jResult, "speed", val);
      }
  }
  if (!seed.empty()) {
    cJSON_AddNumberToObject(jResult, "seed", atoi(seed.c_str()));
  }
  if (!temperature.empty()) {
    cJSON_AddNumberToObject(jResult, "temperature", std::strtof(temperature.c_str(), nullptr));
  }
  if (!emotion.empty()) {
    cJSON_AddStringToObject(jResult, "emotion", emotion.c_str());
  }
  if (!voice_guidance.empty()) {
    cJSON_AddNumberToObject(jResult, "voice_guidance", atoi(voice_guidance.c_str()));
  }
  if (!style_guidance.empty()) {
    cJSON_AddNumberToObject(jResult, "style_guidance", atoi(style_guidance.c_str()));
  }
  if (!text_guidance.empty()) {
    cJSON_AddNumberToObject(jResult, "text_guidance", atoi(text_guidance.c_str()));
  }
  char* _body = cJSON_PrintUnformatted(jResult);
  body = _body;

  cJSON_Delete(jResult);
  free(_body);

  // Create headers
  headers.push_back("AUTHORIZATION: " + api_key);
  headers.push_back("X-USER-ID: " + user_id);
  headers.push_back("Accept: audio/mpeg");
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
  } else if (params["vendor"] == "whisper") {
    return whisper_parse_text(params, text, url, body, headers);
  } else if (params["vendor"] == "playht") {
    return playht_parse_text(params, text, url, body, headers);
  } else if (params["vendor"] == "rimelabs") {
    return rimelabs_parse_text(params, text, url, body, headers);
  } else if (params["vendor"] == "custom") {
    return custom_vendor_parse_text(params, text, url, body, headers);
  }  else {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "tts_vendor_parse_text: There is no available parser for vendor %s\n", params["vendor"]);
    return SWITCH_STATUS_FALSE;
  }
}