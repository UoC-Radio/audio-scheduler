bin_PROGRAMS = audio_scheduler

config_schema.o: config_schema.xsd
	$(LD) -r -b binary -o $@ $<

cfg_handler.o: config_schema.o

# Keep these out until we fix/update the gstreamer player backend
#audio_scheduler_SOURCES = cfg_handler.c pls_handler.c meta_handler.c \
#			  media_loader.c gst_player.c utils.c scheduler.c main.c
#audio_scheduler_LDADD = config_schema.o $(GStreamer_LIBS) $(LibXML2_LIBS)
#audio_scheduler_CFLAGS = ${CFLAGS} ${GStreamer_CFLAGS} ${LibXML2_CFLAGS} \
#	-Wall -fms-extensions

audio_scheduler_SOURCES = utils.c cfg_handler.c pls_handler.c media_loader.c scheduler.c meta_handler.c fsp_player.c sig_dispatcher.c main.c
audio_scheduler_LDADD = config_schema.o
audio_scheduler_LDFLAGS = $(LibXML2_LIBS) $(AVFORMAT_LIBS) $(AVCODEC_LIBS) $(AVUTIL_LIBS) $(SWRESAMPLE_LIBS) $(JACK_LIBS) $(PIPEWIRE_LIBS) -lm -pthread
audio_scheduler_CFLAGS = $(CFLAGS) $(LibXML2_CFLAGS) $(AVFORMAT_CFLAGS) $(AVCODEC_CFLAGS) $(AVUTIL_CFLAGS) $(SWRESAMPLE_CFLAGS) $(JACK_CFLAGS) $(PIPEWIRE_CFLAGS) \
			 -Wall -fms-extensions

#Also clean up after autoconf
distclean-local:
	-rm -rf autom4te.cache
	-rm -rf build-aux
	-rm aclocal.m4
	-rm configure
	-rm *.in
	-rm *~

#Do nothing
test:
