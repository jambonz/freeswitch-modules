include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_whisper_tts

mod_LTLIBRARIES = mod_whisper_tts.la
mod_whisper_tts_la_SOURCES  = mod_whisper_tts.c whisper_glue.cpp
mod_whisper_tts_la_CFLAGS   = $(AM_CFLAGS)
mod_whisper_tts_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_whisper_tts_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs boost` -lstdc++
