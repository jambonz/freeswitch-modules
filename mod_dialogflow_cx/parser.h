#ifndef __PARSER_H__
#define __PARSER_H__

#include <switch_json.h>
#include <grpc++/grpc++.h>
#include "google/cloud/dialogflow/cx/v3/session.grpc.pb.h"

using google::cloud::dialogflow::cx::v3::Sessions;
using google::cloud::dialogflow::cx::v3::StreamingDetectIntentRequest;
using google::cloud::dialogflow::cx::v3::StreamingDetectIntentResponse;
using google::cloud::dialogflow::cx::v3::DetectIntentResponse;
using google::cloud::dialogflow::cx::v3::ResponseMessage;
using google::cloud::dialogflow::cx::v3::ResponseMessage_Text;
using google::cloud::dialogflow::cx::v3::ResponseMessage_LiveAgentHandoff;
using google::cloud::dialogflow::cx::v3::ResponseMessage_ConversationSuccess;
using google::cloud::dialogflow::cx::v3::ResponseMessage_OutputAudioText;
using google::cloud::dialogflow::cx::v3::ResponseMessage_EndInteraction;
using google::cloud::dialogflow::cx::v3::ResponseMessage_PlayAudio;
using google::cloud::dialogflow::cx::v3::ResponseMessage_MixedAudio;
using google::cloud::dialogflow::cx::v3::ResponseMessage_TelephonyTransferCall;
using google::cloud::dialogflow::cx::v3::ResponseMessage_KnowledgeInfoCard;
using google::cloud::dialogflow::cx::v3::ResponseMessage_ResponseType;
using google::cloud::dialogflow::cx::v3::AudioEncoding;
using google::cloud::dialogflow::cx::v3::InputAudioConfig;
using google::cloud::dialogflow::cx::v3::OutputAudioConfig;
using google::cloud::dialogflow::cx::v3::SynthesizeSpeechConfig;
using google::cloud::dialogflow::cx::v3::VoiceSelectionParams;
using google::cloud::dialogflow::cx::v3::Intent;
using google::cloud::dialogflow::cx::v3::Intent_Parameter;
using google::cloud::dialogflow::cx::v3::QueryInput;
using google::cloud::dialogflow::cx::v3::QueryResult;
using google::cloud::dialogflow::cx::v3::Match;
using google::cloud::dialogflow::cx::v3::Match_MatchType_Name;
using google::cloud::dialogflow::cx::v3::AdvancedSettings;
using google::cloud::dialogflow::cx::v3::AdvancedSettings_SpeechSettings;
using google::cloud::dialogflow::cx::v3::AdvancedSettings_DtmfSettings;
using google::cloud::dialogflow::cx::v3::AdvancedSettings_LoggingSettings;
using google::cloud::dialogflow::cx::v3::StreamingRecognitionResult;
using google::cloud::dialogflow::cx::v3::StreamingRecognitionResult_MessageType;
using google::cloud::dialogflow::cx::v3::StreamingRecognitionResult_MessageType_Name;
using google::cloud::dialogflow::cx::v3::EventInput;
using google::cloud::dialogflow::cx::v3::OutputAudioEncoding;
using google::cloud::dialogflow::cx::v3::OutputAudioEncoding_Name;
using google::cloud::dialogflow::cx::v3::SentimentAnalysisResult;
using google::cloud::dialogflow::cx::v3::GcsDestination;
using google::cloud::dialogflow::cx::v3::DtmfInput;
using google::protobuf::RepeatedPtrField;
using google::rpc::Status;
using google::protobuf::Struct;
using google::protobuf::Value;
using google::protobuf::ListValue;

typedef google::protobuf::Map< std::string, Value >::const_iterator StructIterator_t;

class GRPCParser {
public:
    GRPCParser(switch_core_session_t *session) : m_session(session) {}
    ~GRPCParser() {}

    template <typename T> cJSON* parseCollection(const RepeatedPtrField<T> coll) ;
    
    cJSON* parse(const StreamingDetectIntentResponse& response) ;
    const std::string& parseAudio(const StreamingDetectIntentResponse& response);


    cJSON* parse(const DetectIntentResponse& o) ;
    cJSON* parse(const ResponseMessage& o) ;
    cJSON* parse(const ResponseMessage_Text& o) ;
    cJSON* parse(const ResponseMessage_ResponseType& o) ;
    cJSON* parse(const ResponseMessage_LiveAgentHandoff& o) ;
    cJSON* parse(const ResponseMessage_ConversationSuccess& o) ;
    cJSON* parse(const ResponseMessage_OutputAudioText& o) ;
    cJSON* parse(const ResponseMessage_EndInteraction& o) ;
    cJSON* parse(const ResponseMessage_PlayAudio& o) ;
    cJSON* parse(const ResponseMessage_MixedAudio& o) ;
    cJSON* parse(const ResponseMessage_TelephonyTransferCall& o) ;
    cJSON* parse(const ResponseMessage_KnowledgeInfoCard& o) ;
    cJSON* parse(const Match& o) ;
    cJSON* parse(const AdvancedSettings& o) ;
    cJSON* parse(const AdvancedSettings_SpeechSettings& o) ;
    cJSON* parse(const AdvancedSettings_DtmfSettings& o) ;
    cJSON* parse(const AdvancedSettings_LoggingSettings& o) ;
    cJSON* parse(const SynthesizeSpeechConfig& o) ;
    cJSON* parse(const OutputAudioEncoding& o) ;
    cJSON* parse(const OutputAudioConfig& o) ;
    cJSON* parse(const VoiceSelectionParams& o) ;
    cJSON* parse(const Intent& o) ;
    cJSON* parse(const Intent_Parameter& o) ;
    cJSON* parse(const QueryInput& o) ;
    cJSON* parse(const DtmfInput& o) ;
    cJSON* parse(const google::rpc::Status& o) ;
    cJSON* parse(const Value& value) ;
    cJSON* parse(const Struct& rpcStruct) ;
    cJSON* parse(const std::string& val) ;
    cJSON* parse(const SentimentAnalysisResult& o) ;
    cJSON* parse(const StreamingRecognitionResult_MessageType& o) ;
    cJSON* parse(const StreamingRecognitionResult& o) ;
    cJSON* parse(const GcsDestination& o) ;
    cJSON* parse(const QueryResult& qr);


private:
    switch_core_session_t *m_session;
} ;


#endif