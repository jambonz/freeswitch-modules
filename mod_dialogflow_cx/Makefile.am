include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_dialogflow_cx

mod_LTLIBRARIES = mod_dialogflow_cx.la
mod_dialogflow_cx_la_SOURCES  = mod_dialogflow_cx.c parser.cpp google_glue.cpp
mod_dialogflow_cx_la_CFLAGS   = $(AM_CFLAGS)
mod_dialogflow_cx_la_CXXFLAGS = -I $(top_srcdir)/libs/googleapis/gens $(AM_CXXFLAGS) -std=c++17

mod_dialogflow_cx_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_dialogflow_cx_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs grpc++ grpc` 
