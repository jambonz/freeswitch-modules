include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_deepgram_tts

mod_LTLIBRARIES = mod_deepgram_tts.la
mod_deepgram_tts_la_SOURCES  = mod_deepgram_tts.c deepgram_glue.cpp
mod_deepgram_tts_la_CFLAGS   = $(AM_CFLAGS)
mod_deepgram_tts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_deepgram_tts_la_LDFLAGS  = -avoid-version -module -no-undefined -shared -lstdc++ -lboost_system -lboost_thread
