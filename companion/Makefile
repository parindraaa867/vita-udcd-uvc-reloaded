TARGET		= udcd_uvc_config
TITLE		= udcd_uvc config
TITLEID		= UDCD00001

OBJS		= main.o

PREFIX		= arm-vita-eabi
CC		= $(PREFIX)-gcc
CFLAGS		= -Wl,-q -Wall -O2 -mcpu=cortex-a9 -mthumb -fno-lto
ASFLAGS		= $(CFLAGS)

# GPU UI via vita2d (same stack as EQVita), system PGF font, touch input.
LIBS		= -lvita2d \
		  -lSceGxm_stub -lSceCtrl_stub -lSceTouch_stub -lSceDisplay_stub \
		  -lSceCommonDialog_stub -lScePgf_stub -lSceSysmodule_stub \
		  -lSceLibKernel_stub -lSceIofilemgr_stub -lSceAppMgr_stub \
		  -lfreetype -lpng16 -ljpeg -lz -lm

all: $(TARGET).vpk

assets:
	python3 gen_assets.py

$(TARGET).vpk: eboot.bin
	vita-mksfoex -s TITLE_ID="$(TITLEID)" "$(TITLE)" param.sfo
	vita-pack-vpk -s param.sfo -b eboot.bin \
		-a sce_sys/icon0.png=sce_sys/icon0.png \
		-a sce_sys/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png \
		-a sce_sys/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png \
		-a sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml \
		$@

eboot.bin: $(TARGET).velf
	vita-make-fself -s $< eboot.bin

$(TARGET).velf: $(TARGET).elf
	vita-elf-create $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(TARGET).vpk $(TARGET).velf $(TARGET).elf $(OBJS) \
		eboot.bin param.sfo

.PHONY: all clean
