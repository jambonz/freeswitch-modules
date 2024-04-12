include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_rimelabs_tts

mod_LTLIBRARIES = mod_rimelabs_tts.la
mod_rimelabs_tts_la_SOURCES  = mod_rimelabs_tts.c rimelabs_glue.cpp
mod_rimelabs_tts_la_CFLAGS   = $(AM_CFLAGS)
mod_rimelabs_tts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_rimelabs_tts_la_LDFLAGS  = -avoid-version -module -no-undefined -shared -lstdc++ -lboost_system -lboost_thread
