TARGET = KodiRemote
OBJS = main.o config.o sys.o net.o ui.o menus.o setup.o direct.o lib.o

CFLAGS = -O2 -G0 -Wall
# ARK_EXTRAS=1 adds the music-note button (fullscreen) and a screen that goes black when idle, through ARK-4's SystemControl and
# kubridge (user-mode EBOOT, kernel calls by name). Off by default.
ifdef ARK_EXTRAS
CFLAGS += -DARK_EXTRAS
endif
ifdef APP_BUILD
CFLAGS += -DAPP_BUILD=\"$(APP_BUILD)\"
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

ifdef ARK_EXTRAS
KLIBS_LAST = -lpspkubridge -lpspsystemctrl_user
endif
LIBS = -lintrafont -lpspgu -lpspgum -lpspge -lpspdisplay -lpspnet_apctl -lpspnet_inet -lpspnet -lpspnet_resolver -lpsputility -lpsppower -lpspwlan -lm $(KLIBS_LAST)

BUILD_PRX = 1
PSP_FW_VERSION = 661

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Kodi Remote
PSP_EBOOT_ICON = ICON0.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
