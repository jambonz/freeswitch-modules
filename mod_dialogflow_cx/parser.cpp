#include "parser.h"
#include <switch.h>

template <typename T> cJSON* GRPCParser::parseCollection(const RepeatedPtrField<T> coll) {
	cJSON* json = cJSON_CreateArray();
	typename RepeatedPtrField<T>::const_iterator it = coll.begin();
	for (; it != coll.end(); it++) {
		cJSON_AddItemToArray(json, parse(*it));
	}
	return json;
}

cJSON* GRPCParser::parse(const StreamingDetectIntentResponse& response) {
	cJSON * json = cJSON_CreateObject();

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_INFO, "GStrGRPCParser - parsing StreamingDetectIntentResponse\n");

	// recognition_result
	if (response.has_recognition_result()) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_INFO, "GStrGRPCParser - adding recognition result\n");
        cJSON_AddItemToObject(json, "recognition_result", parse(response.recognition_result()));
	}

  // detect_intent_response
  if (response.has_detect_intent_response()) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_INFO, "GStrGRPCParser - adding detect intent response\n");
    cJSON_AddItemToObject(json, "detect_intent_response", parse(response.detect_intent_response()));
  }

	return json;
}

cJSON* GRPCParser::parse(const OutputAudioEncoding& o) {
	return cJSON_CreateString(OutputAudioEncoding_Name(o).c_str());
}

cJSON* GRPCParser::parse(const SynthesizeSpeechConfig& o) {
	cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "speaking_rate", cJSON_CreateNumber(o.speaking_rate()));
  cJSON_AddItemToObject(json, "pitch", cJSON_CreateNumber(o.pitch()));
  cJSON_AddItemToObject(json, "volume_gain_db", cJSON_CreateNumber(o.volume_gain_db()));

  return json;
}
cJSON* GRPCParser::parse(const VoiceSelectionParams& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "name", cJSON_CreateString(o.name().c_str()));
  cJSON_AddItemToObject(json, "ssml_gender", cJSON_CreateString(SsmlVoiceGender_Name(o.ssml_gender()).c_str()));

  return json;
}

cJSON* GRPCParser::parse(const OutputAudioConfig& o) {
	cJSON * json = cJSON_CreateObject();

	cJSON_AddItemToObject(json, "audio_encoding", parse(o.audio_encoding()));
	cJSON_AddItemToObject(json, "sample_rate_hertz", cJSON_CreateNumber(o.sample_rate_hertz()));
	cJSON_AddItemToObject(json, "synthesize_speech_config", parse(o.synthesize_speech_config()));

	return json;
}

cJSON* GRPCParser::parse(const google::rpc::Status& o) {
	cJSON * json = cJSON_CreateObject();

	cJSON_AddItemToObject(json, "code", cJSON_CreateNumber(o.code()));
	cJSON_AddItemToObject(json, "message", cJSON_CreateString(o.message().c_str()));

	return json;
}

cJSON* GRPCParser::parse(const Value& value) {
	cJSON* json = NULL;

	switch (value.kind_case()) {
		case Value::KindCase::kNullValue:
			json = cJSON_CreateNull();
			break;

		case Value::KindCase::kNumberValue:
			json = cJSON_CreateNumber(value.number_value());
			break;

		case Value::KindCase::kStringValue:
			json = cJSON_CreateString(value.string_value().c_str());
			break;

		case Value::KindCase::kBoolValue:
			json = cJSON_CreateBool(value.bool_value());
			break;

		case Value::KindCase::kStructValue:
			json = parse(value.struct_value());
			break;

		case Value::KindCase::kListValue:
			{
				const ListValue& list = value.list_value();
				json = cJSON_CreateArray();
				for (int i = 0; i < list.values_size(); i++) {
					const Value& val = list.values(i);
					cJSON_AddItemToArray(json, parse(val));
				}
			} 
			break;
	}

	return json;
}

cJSON* GRPCParser::parse(const Struct& rpcStruct) {
	cJSON* json = cJSON_CreateObject();

	for (StructIterator_t it = rpcStruct.fields().begin(); it != rpcStruct.fields().end(); it++) {
		const std::string& key = it->first;
		const Value& value = it->second;
		cJSON_AddItemToObject(json, key.c_str(), parse(value));
	}
	return json;
}

cJSON* GRPCParser::parse(const Intent_Parameter& param) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "id", cJSON_CreateString(param.id().c_str()));
  cJSON_AddItemToObject(json, "entity_type", cJSON_CreateString(param.entity_type().c_str()));
  cJSON_AddItemToObject(json, "is_list", cJSON_CreateBool(param.is_list()));
  cJSON_AddItemToObject(json, "redact", cJSON_CreateBool(param.redact()));

  return json;
}

cJSON* GRPCParser::parse(const Intent& o) {
    cJSON * json = cJSON_CreateObject();

	cJSON_AddItemToObject(json, "name", cJSON_CreateString(o.name().c_str()));
	cJSON_AddItemToObject(json, "display_name", cJSON_CreateString(o.display_name().c_str()));

  cJSON* params = cJSON_CreateArray();
  for (int i = 0; i < o.parameters_size(); i++) {
    cJSON_AddItemToArray(params, parse(o.parameters(i)));
  }
  cJSON_AddItemToObject(json, "parameters", params);
	cJSON_AddItemToObject(json, "priority", cJSON_CreateNumber(o.priority()));
	cJSON_AddItemToObject(json, "is_fallback", cJSON_CreateBool(o.is_fallback()));
	return json;
}

cJSON* GRPCParser::parse(const Match& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "resolved_input", cJSON_CreateString(o.resolved_input().c_str()));
  cJSON_AddItemToObject(json, "event", cJSON_CreateString(o.event().c_str()));
  if (o.has_intent()) cJSON_AddItemToObject(json, "intent", parse(o.intent()));
  cJSON_AddItemToObject(json, "parameters", parse(o.parameters()));
  cJSON_AddItemToObject(json, "match_type", cJSON_CreateString(Match_MatchType_Name(o.match_type()).c_str()));
  cJSON_AddItemToObject(json, "confidence", cJSON_CreateNumber(o.confidence()));

  return json;
}

cJSON* GRPCParser::parse(const AdvancedSettings_SpeechSettings& o) {
  cJSON * json = cJSON_CreateObject();

  if (o.has_no_speech_timeout()) {
    double total_seconds = o.no_speech_timeout().seconds() + o.no_speech_timeout().nanos() / 1e9;
    cJSON_AddItemToObject(json, "no_speech_timeout", cJSON_CreateNumber(total_seconds));
  }
  cJSON_AddItemToObject(json, "endpointer_sensitivity", cJSON_CreateNumber(o.endpointer_sensitivity()));
  cJSON_AddItemToObject(json, "use_timeout_based_endpointing", cJSON_CreateBool(o.use_timeout_based_endpointing()));

  return json;
}

cJSON* GRPCParser::parse(const AdvancedSettings_DtmfSettings& o) {
  cJSON * json = cJSON_CreateObject();

  double interdigit_timeout_duration = o.interdigit_timeout_duration().seconds() + o.interdigit_timeout_duration().nanos() / 1e9;
  double endpointing_timeout_duration = o.endpointing_timeout_duration().seconds() + o.endpointing_timeout_duration().nanos() / 1e9;

  cJSON_AddItemToObject(json, "interdigit_timeout_duration", cJSON_CreateNumber(interdigit_timeout_duration));
  cJSON_AddItemToObject(json, "endpointing_timeout_duration", cJSON_CreateNumber(endpointing_timeout_duration));
  cJSON_AddItemToObject(json, "enabled", cJSON_CreateBool(o.enabled()));
  cJSON_AddItemToObject(json, "max_digits", cJSON_CreateNumber(o.max_digits()));
  
  return json;

}
cJSON* GRPCParser::parse(const AdvancedSettings_LoggingSettings& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "enable_stackdriver_logging", cJSON_CreateBool(o.enable_stackdriver_logging()));
  cJSON_AddItemToObject(json, "enable_interaction_logging", cJSON_CreateBool(o.enable_interaction_logging()));
  cJSON_AddItemToObject(json, "enable_consent_based_redaction", cJSON_CreateBool(o.enable_consent_based_redaction()));

  return json;
}

cJSON* GRPCParser::parse(const GcsDestination& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "uri", cJSON_CreateString(o.uri().c_str()));

  return json;
}

cJSON* GRPCParser::parse(const AdvancedSettings& o) {
  cJSON * json = cJSON_CreateObject();

  if (o.has_audio_export_gcs_destination()) cJSON_AddItemToObject(json, "audio_export_gcs_destination", parse(o.audio_export_gcs_destination()));  
  if (o.has_speech_settings()) cJSON_AddItemToObject(json, "speech_settings", parse(o.speech_settings()));
  if (o.has_dtmf_settings()) cJSON_AddItemToObject(json, "dtmf_settings", parse(o.dtmf_settings()));
  if (o.has_logging_settings()) cJSON_AddItemToObject(json, "logging_settings", parse(o.logging_settings()));

  return json;
}


cJSON* GRPCParser::parse(const ResponseMessage_Text& o) {
  cJSON* t = cJSON_CreateArray();
  for (int i = 0; i < o.text_size(); i++) {
    cJSON * json = cJSON_CreateObject();
    cJSON_AddItemToObject(json, "text", cJSON_CreateString(o.text(i).c_str()));
    cJSON_AddItemToObject(json, "allow_playback_interruption", cJSON_CreateBool(o.allow_playback_interruption()));
    cJSON_AddItemToArray(t, json);
  }
  return t;
}

cJSON* GRPCParser::parse(const ResponseMessage_ConversationSuccess& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "metadata", parse(o.metadata()));

  return json;
}

cJSON* GRPCParser::parse(const ResponseMessage_OutputAudioText& o) {
  cJSON * json = cJSON_CreateObject();

  if (o.has_text()) cJSON_AddItemToObject(json, "text", cJSON_CreateString(o.text().c_str()));
  if (o.has_ssml()) cJSON_AddItemToObject(json, "ssml", cJSON_CreateString(o.ssml().c_str()));

  return json;
}

cJSON* GRPCParser::parse(const ResponseMessage_LiveAgentHandoff& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "metadata", parse(o.metadata()));

  return json;
}

cJSON* GRPCParser::parse(const ResponseMessage_EndInteraction& o) {
  cJSON * json = cJSON_CreateObject();

  // TODOL: need to research this more
  return json;
}

cJSON* GRPCParser::parse(const ResponseMessage_PlayAudio& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "audio_uri", cJSON_CreateString(o.audio_uri().c_str()));
  cJSON_AddItemToObject(json, "allow_playback_interruption", cJSON_CreateBool(o.allow_playback_interruption()));

  return json;
}

cJSON* GRPCParser::parse(const ResponseMessage_MixedAudio& o) {
  cJSON * json = cJSON_CreateArray();

  for (int i = 0; i < o.segments_size(); i++) {
    cJSON * segment = cJSON_CreateObject();
    if (o.segments(i).has_audio()) cJSON_AddItemToObject(segment, "audio", cJSON_CreateString(o.segments(i).audio().c_str()));
    else if (o.segments(i).has_uri()) cJSON_AddItemToObject(segment, "uri", cJSON_CreateString(o.segments(i).uri().c_str()));
    cJSON_AddItemToObject(segment, "allow_playback_interruption", cJSON_CreateBool(o.segments(i).allow_playback_interruption()));
  }
  return json;
}

cJSON* GRPCParser::parse(const ResponseMessage_TelephonyTransferCall& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "phone_number", cJSON_CreateString(o.phone_number().c_str()));

  return json;
}


cJSON* GRPCParser::parse(const ResponseMessage& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "response_type", cJSON_CreateString(ResponseMessage_ResponseType_Name(o.response_type()).c_str()));
  cJSON_AddItemToObject(json, "text", parse(o.text()));
  cJSON_AddItemToObject(json, "payload", parse(o.payload()));
  cJSON_AddItemToObject(json, "conversation_success", parse(o.conversation_success()));
  cJSON_AddItemToObject(json, "output_audio_text", parse(o.output_audio_text()));
  cJSON_AddItemToObject(json, "live_agent_handoff", parse(o.live_agent_handoff()));
  cJSON_AddItemToObject(json, "end_interaction", parse(o.end_interaction()));
  cJSON_AddItemToObject(json, "play_audio", parse(o.play_audio()));
  cJSON_AddItemToObject(json, "mixed_audio", parse(o.mixed_audio()));
  cJSON_AddItemToObject(json, "telephony_transfer_call", parse(o.telephony_transfer_call()));

  return json;
}

cJSON* GRPCParser::parse(const QueryResult& qr) {
    cJSON * json = cJSON_CreateObject();

    // one-of
    if (qr.has_text()) cJSON_AddItemToObject(json, "text", cJSON_CreateString(qr.text().c_str()));
    else if (qr.has_trigger_intent()) cJSON_AddItemToObject(json, "trigger_intent", cJSON_CreateString(qr.trigger_intent().c_str()));
    else if (qr.has_transcript()) cJSON_AddItemToObject(json, "transcript", cJSON_CreateString(qr.transcript().c_str()));
    else if (qr.has_trigger_event()) cJSON_AddItemToObject(json, "trigger_event", cJSON_CreateString(qr.trigger_event().c_str()));

    if (qr.has_dtmf()) cJSON_AddItemToObject(json, "dtmf", parse(qr.dtmf()));
    cJSON_AddItemToObject(json, "language_code", cJSON_CreateString(qr.language_code().c_str()));
    cJSON_AddItemToObject(json, "parameters", parse(qr.parameters()));

    cJSON* rms = cJSON_CreateArray();
    for (int i = 0; i < qr.response_messages_size(); i++) {
      cJSON_AddItemToArray(rms, parse(qr.response_messages(i)));
    }
    cJSON_AddItemToObject(json, "response_messages", rms);

    cJSON* whids = cJSON_CreateArray();
    for (int i = 0; i < qr.webhook_ids_size(); i++) {
      cJSON_AddItemToArray(whids, cJSON_CreateString(qr.webhook_ids(i).c_str()));
    }
    cJSON_AddItemToObject(json, "webhook_ids", whids);

    cJSON* whDisplayNames = cJSON_CreateArray();
    for (int i = 0; i < qr.webhook_display_names_size(); i++) {
      cJSON_AddItemToArray(whDisplayNames, cJSON_CreateString(qr.webhook_display_names(i).c_str()));
    }
    cJSON_AddItemToObject(json, "webhook_display_names", whDisplayNames);

    cJSON* whLatencies = cJSON_CreateArray();
    for (int i = 0; i < qr.webhook_latencies_size(); i++) {
      double total_seconds = qr.webhook_latencies(i).seconds() + qr.webhook_latencies(i).nanos() / 1e9;
      cJSON_AddItemToArray(whLatencies, cJSON_CreateNumber(total_seconds));
    }

    cJSON* whTags = cJSON_CreateArray();
    for (int i = 0; i < qr.webhook_tags_size(); i++) {
      cJSON_AddItemToArray(whids, cJSON_CreateString(qr.webhook_tags(i).c_str()));
    }

    cJSON* whStatuses = cJSON_CreateArray();
    for (int i = 0; i < qr.webhook_statuses_size(); i++) {
      cJSON_AddItemToArray(whStatuses, parse(qr.webhook_statuses(i)));
    }

    cJSON* whPayloads = cJSON_CreateArray();
    for (int i = 0; i < qr.webhook_payloads_size(); i++) {
      cJSON_AddItemToArray(whPayloads, parse(qr.webhook_payloads(i)));
    }

    //if (qr.has_current_page()) cJSON_AddItemToObject(json, "current_page", parse(qr.current_page());
    //if (qr.has_current_flow()) cJSON_AddItemToObject(json, "current_flow", parse(qr.current_flow());
    if (qr.has_match()) cJSON_AddItemToObject(json, "match", parse(qr.match()));
    if (qr.has_diagnostic_info()) cJSON_AddItemToObject(json, "diagnostic_info", parse(qr.diagnostic_info()));
    if (qr.has_sentiment_analysis_result()) cJSON_AddItemToObject(json, "sentiment_analysis_result", parse(qr.sentiment_analysis_result()));
    if (qr.has_advanced_settings()) cJSON_AddItemToObject(json, "advanced_settings", parse(qr.advanced_settings()));

    cJSON_AddItemToObject(json, "allow_answer_feedback", cJSON_CreateBool(qr.allow_answer_feedback()));

    // skipping DataStoreConnectionSignals for now, it doesn't seem consequential

    return json;
}
cJSON* GRPCParser::parse(const StreamingRecognitionResult_MessageType& o) {
	return cJSON_CreateString(StreamingRecognitionResult_MessageType_Name(o).c_str());
}

cJSON* GRPCParser::parse(const DtmfInput& o) {
  cJSON * json = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "digits", cJSON_CreateString(o.digits().c_str()));
  cJSON_AddItemToObject(json, "finish_digit", cJSON_CreateString(o.finish_digit().c_str()));

  return json;
}

cJSON* GRPCParser::parse(const StreamingRecognitionResult& o) {
    cJSON * json = cJSON_CreateObject();

    cJSON_AddItemToObject(json, "message_type", parse(o.message_type()));
    cJSON_AddItemToObject(json, "transcript", cJSON_CreateString(o.transcript().c_str()));
    cJSON_AddItemToObject(json, "is_final", cJSON_CreateBool(o.is_final()));
    cJSON_AddItemToObject(json, "confidence", cJSON_CreateNumber(o.confidence()));
    cJSON_AddItemToObject(json, "stability", cJSON_CreateNumber(o.stability()));
    cJSON_AddItemToObject(json, "language_code", cJSON_CreateString(o.language_code().c_str()));

    // TODO: we also have SpeechWordInfo if we want to dive into that

    return json;
}

cJSON* GRPCParser::parse(const DetectIntentResponse& o)  {
    cJSON * json = cJSON_CreateObject();

    cJSON_AddItemToObject(json, "response_id", cJSON_CreateString(o.response_id().c_str()));
    cJSON_AddItemToObject(json, "query_result", parse(o.query_result()));
    cJSON_AddItemToObject(json, "output_audio", cJSON_CreateString(o.output_audio().c_str()));
    cJSON_AddItemToObject(json, "output_audio_config", parse(o.output_audio_config()));
    cJSON_AddItemToObject(json, "response_type", cJSON_CreateString(DetectIntentResponse_ResponseType_Name(o.response_type()).c_str()));
    cJSON_AddItemToObject(json, "allow_cancellation", cJSON_CreateBool(o.allow_cancellation()));

    return json;
}
