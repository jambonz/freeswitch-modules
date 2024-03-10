include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_dub

mod_LTLIBRARIES = mod_dub.la
mod_dub_la_SOURCES  = file_loader.cpp audio_downloader.cpp mod_dub.c dub_glue.cpp tts_vendor_parser.cpp
mod_dub_la_CFLAGS   = $(AM_CFLAGS)
mod_dub_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++17

mod_dub_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_dub_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs boost` -lstdc++ -lmpg123
