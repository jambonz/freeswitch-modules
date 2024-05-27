include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_vad_detect

mod_LTLIBRARIES = mod_vad_detect.la
mod_vad_detect_la_SOURCES  = mod_vad_detect.c 
mod_vad_detect_la_CFLAGS   = $(AM_CFLAGS)
mod_vad_detect_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++11
mod_vad_detect_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_vad_detect_la_LDFLAGS  = -avoid-version -module -no-undefined
