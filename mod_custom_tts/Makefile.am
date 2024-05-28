include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_custom_tts

mod_LTLIBRARIES = mod_custom_tts.la
mod_custom_tts_la_SOURCES  = mod_custom_tts.c custom_glue.cpp
mod_custom_tts_la_CFLAGS   = $(AM_CFLAGS)
mod_custom_tts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_custom_tts_la_LDFLAGS  = -avoid-version -module -no-undefined -shared -lstdc++ -lboost_system -lboost_thread -lmpg123
