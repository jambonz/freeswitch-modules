include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_elevenlabs_tts

mod_LTLIBRARIES = mod_elevenlabs_tts.la
mod_elevenlabs_tts_la_SOURCES  = mod_elevenlabs_tts.c elevenlabs_glue.cpp
mod_elevenlabs_tts_la_CFLAGS   = $(AM_CFLAGS)
mod_elevenlabs_tts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_elevenlabs_tts_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs boost` -lstdc++
