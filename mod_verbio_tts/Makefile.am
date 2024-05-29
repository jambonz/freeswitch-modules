include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_verbio_tts

mod_LTLIBRARIES = mod_verbio_tts.la
mod_verbio_tts_la_SOURCES  = mod_verbio_tts.c verbio_glue.cpp
mod_verbio_tts_la_CFLAGS   = $(AM_CFLAGS)
mod_verbio_tts_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++17

mod_verbio_tts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_verbio_tts_la_LDFLAGS  = -avoid-version -module -no-undefined -shared -lstdc++ -lboost_system -lboost_thread
