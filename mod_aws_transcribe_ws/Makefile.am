include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_aws_transcribe_ws

mod_LTLIBRARIES = mod_aws_transcribe_ws.la
mod_aws_transcribe_ws_la_SOURCES  = mod_aws_transcribe_ws.c aws_transcribe_glue.cpp transcribe_manager.cpp audio_pipe.cpp
mod_aws_transcribe_ws_la_CFLAGS   = $(AM_CFLAGS)
mod_aws_transcribe_ws_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++11 -I${switch_srcdir}/libs/aws-sdk-cpp/aws-cpp-sdk-core/include -I${switch_srcdir}/libs/aws-sdk-cpp/aws-cpp-sdk-transcribestreaming/include -I${switch_srcdir}/libs/aws-sdk-cpp/build/.deps/install/include

mod_aws_transcribe_ws_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_aws_transcribe_ws_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs libwebsockets` 