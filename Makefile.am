bin_PROGRAMS = audio_scheduler

config_schema.o: config_schema.xsd
	$(LD) -r -b binary -o $@ $<

cfg_handler.o: config_schema.o

audio_scheduler_SOURCES = cfg_handler.c pls_handler.c meta_handler.c \
			  media_loader.c gst_player.c utils.c scheduler.c main.c
audio_scheduler_LDADD = config_schema.o $(GStreamer_LIBS) $(LibXML2_LIBS) $(AVFORMAT_LIBS) $(AVCODEC_LIBS) $(AVUTIL_LIBS)
audio_scheduler_CFLAGS = ${CFLAGS} ${GStreamer_CFLAGS} ${LibXML2_CFLAGS} $(AVFORMAT_CFLAGS) $(AVCODEC_CFLAGS) $(AVUTIL_CFLAGS) \
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
