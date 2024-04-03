include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_audio_fork

mod_LTLIBRARIES = mod_audio_fork.la
mod_audio_fork_la_SOURCES  = mod_audio_fork.c lws_glue.cpp parser.cpp audio_pipe.cpp vector_math.cpp
mod_audio_fork_la_CFLAGS   = $(AM_CFLAGS)
mod_audio_fork_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++11

if USE_AVX2
mod_audio_fork_la_CXXFLAGS += -mavx2 -DUSE_AVX2
else
if USE_SSE2
mod_audio_fork_la_CXXFLAGS += -msse2 -DUSE_SSE2
endif
endif

mod_audio_fork_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_audio_fork_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs libwebsockets` -lstdc++ -lboost_system -lboost_thread
