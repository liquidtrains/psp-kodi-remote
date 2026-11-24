TARGET = KodiRemote
OBJS = main.o config.o sys.o net.o ui.o menus.o setup.o direct.o

CFLAGS = -O2 -G0 -Wall
ifdef APP_BUILD
CFLAGS += -DAPP_BUILD=\"$(APP_BUILD)\"
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lintrafont -lpspgu -lpspgum -lpspge -lpspdisplay -lpspnet_apctl -lpspnet_inet -lpspnet -lpspnet_resolver -lpsputility -lpsppower -lpspwlan -lm

BUILD_PRX = 1
PSP_FW_VERSION = 661

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Kodi Remote
PSP_EBOOT_ICON = ICON0.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
