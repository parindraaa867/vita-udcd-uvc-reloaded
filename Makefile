TARGET	= udcd_uvc
OBJS	= src/main.o
LIBS	= -lSceSysmemForDriver_stub -lSceThreadmgrForDriver_stub \
	-lSceCpuForDriver_stub -lSceUdcdForDriver_stub \
	-lSceDisplayForDriver_stub -lSceIftuForDriver_stub \
	-lSceCtrlForDriver_stub \
	-lSceKernelSuspendForDriver_stub -lSceModulemgrForDriver_stub \
	-lSceIofilemgrForDriver_stub \
	-lSceOledForDriver_stub_weak -lSceLcdForDriver_stub_weak \
	-ltaihenForKernel_stub -lSceSysclibForDriver_stub

# OLED (PCH-1000) and LCD (PCH-2000) display libs are linked via *weak* stubs,
# so the single binary loads on every model and picks the right one at runtime.

ifeq ($(DEBUG), 1)
	OBJS	+= debug/log.o debug/draw.o debug/console.o debug/font_data.o
	CFLAGS	+= -DDEBUG -Idebug
endif

# Parallel convert/send with double-buffering (default on). Set PARALLEL=0 to
# fall back to a single buffer (halves the framebuffer memory).
ifeq ($(PARALLEL), 0)
	CFLAGS	+= -DENCODE_SEND_PARALLELIZE=0
endif

# Allocate the frame buffer from CDRAM instead of (phycont) main RAM.
ifeq ($(USE_CDRAM), 1)
	CFLAGS	+= -DUSE_CDRAM
endif

PREFIX	= arm-vita-eabi
CC	= $(PREFIX)-gcc
CFLAGS	+= -Wl,-q -Wall -O3 -fomit-frame-pointer -nostartfiles -mcpu=cortex-a9 -mthumb-interwork -Iinclude
DEPS	= $(OBJS:.o=.d)

all: $(TARGET).skprx

%.skprx: %.velf
	vita-make-fself -c $< $@

%.velf: %.elf
	vita-elf-create -e $(TARGET).yml $< $@

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

.PHONY: clean send

clean:
	@rm -rf $(TARGET).skprx $(TARGET).velf $(TARGET).elf $(OBJS) $(DEPS)

send: $(TARGET).skprx
	curl -T $(TARGET).skprx ftp://$(PSVITAIP):1337/ux0:/data/tai/kplugin.skprx
	@echo "Sent."

launch: send
	echo "load_skprx ux0:/data/tai/kplugin.skprx" | nc $(PSVITAIP) 1338
	@echo "Launched!"

taisend: $(TARGET).skprx
	curl -T $(TARGET).skprx ftp://$(PSVITAIP):1337/ux0:/tai/$(TARGET).skprx
	@echo "Sent."

-include $(DEPS)
