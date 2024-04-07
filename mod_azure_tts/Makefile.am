include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_azure_tts

mod_LTLIBRARIES = mod_azure_tts.la
mod_azure_tts_la_SOURCES  = mod_azure_tts.c azure_glue.cpp
mod_azure_tts_la_CFLAGS   = $(AM_CFLAGS)
mod_azure_tts_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++14 -I/usr/local/include/MicrosoftSpeechSDK/cxx_api -I/usr/local/include/MicrosoftSpeechSDK/c_api

mod_azure_tts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_azure_tts_la_LDFLAGS  = -avoid-version -module -no-undefined -L/usr/local/lib/MicrosoftSpeechSDK/x64 -lMicrosoft.CognitiveServices.Speech.core -shared -lstdc++ -lboost_system -lboost_thread