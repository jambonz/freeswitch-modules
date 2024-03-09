#ifndef __MOD_DUB_TTS_VENDOR_PARSER_H__
#define __MOD_DUB_TTS_VENDOR_PARSER_H__

#include <string>
#include <vector>
#include <switch.h>

switch_status_t tts_vendor_parse_text(const std::string& text, std::string& url, std::string& body, std::vector<std::string>& headers);
switch_status_t elevenlabs_parse_text(const std::string& text, std::string& url, std::string& body, std::vector<std::string>& headers);

#endif