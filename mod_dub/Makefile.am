include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_dub

mod_LTLIBRARIES = mod_dub.la
mod_dub_la_SOURCES  = ap_file.cpp ap_http.cpp track.cpp dub_glue.cpp tts_vendor_parser.cpp mod_dub.c vector_math.cpp mpg_decode.cpp
mod_dub_la_CFLAGS   = $(AM_CFLAGS)
mod_dub_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++17

if USE_AVX2
mod_dub_la_CXXFLAGS += -mavx2 -DUSE_AVX2
else
if USE_SSE2
mod_dub_la_CXXFLAGS += -msse2 -DUSE_SSE2
endif
endif

mod_dub_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_dub_la_LDFLAGS  = -avoid-version -module -no-undefined -lstdc++ -lmpg123 -lboost_system -lboost_thread
