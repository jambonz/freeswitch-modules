include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_verbio_transcribe

mod_LTLIBRARIES = mod_verbio_transcribe.la
mod_verbio_transcribe_la_SOURCES  = mod_verbio_transcribe.c verbio_glue.cpp
mod_verbio_transcribe_la_CFLAGS   = $(AM_CFLAGS)
mod_verbio_transcribe_la_CXXFLAGS = -I $(top_srcdir)/libs/verbio-asr-grpc-api/stubs $(AM_CXXFLAGS) -std=c++17

mod_verbio_transcribe_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_verbio_transcribe_la_LDFLAGS  = -avoid-version -module -no-undefined -shared -lstdc++ -lboost_system -lboost_thread
