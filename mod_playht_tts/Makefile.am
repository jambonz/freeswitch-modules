include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_playht_tts

mod_LTLIBRARIES = mod_playht_tts.la
mod_playht_tts_la_SOURCES  = mod_playht_tts.c playht_glue.cpp
mod_playht_tts_la_CFLAGS   = $(AM_CFLAGS)
mod_playht_tts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_playht_tts_la_LDFLAGS  = -avoid-version -module -no-undefined -shared -lstdc++ -lboost_system -lboost_thread -lmpg123
