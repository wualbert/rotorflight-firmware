
# src/main/common has headers (time.h, ctype.h) that have the names of C
# library headers. With -I, they also hide the host C library headers:
# <pthread.h> then includes common/time.h and misses clockid_t. With -iquote,
# only #include "..." searches src/main/common, and <...> finds the host headers.
INCLUDE_DIRS    := $(filter-out $(SRC_DIR)/common,$(INCLUDE_DIRS)) \
                   $(ROOT)/lib/main/dyad

TARGET_FLAGS    = -D$(TARGET) -iquote $(SRC_DIR)/common

MCU_COMMON_SRC  := $(ROOT)/lib/main/dyad/dyad.c

#Flags
ARCH_FLAGS      =
DEVICE_FLAGS    =
LD_SCRIPT       = src/main/target/SITL/pg.ld
STARTUP_SRC     =

MCU_FLASH_SIZE  := 2048

ARM_SDK_PREFIX  =

# common/string_light.c: the host C library has these functions, and with
#   -iquote <ctype.h> is the host header (glibc macros, not common/ctype.h).
MCU_EXCLUDES = \
            common/string_light.c \
            drivers/adc.c \
            drivers/bus_i2c.c \
            drivers/bus_i2c_config.c \
            drivers/bus_spi.c \
            drivers/bus_spi_config.c \
            drivers/bus_spi_pinconfig.c \
            drivers/dma.c \
            drivers/pwm_output.c \
            drivers/timer.c \
            drivers/system.c \
            drivers/rcc.c \
            drivers/serial_escserial.c \
            drivers/serial_pinconfig.c \
            drivers/serial_uart.c \
            drivers/serial_uart_init.c \
            drivers/serial_uart_pinconfig.c \
            drivers/rx/rx_xn297.c \
            drivers/display_ug2864hsweg01.c \
            telemetry/crsf.c \
            telemetry/ghst.c \
            telemetry/srxl.c \
            io/displayport_oled.c

TARGET_MAP  = $(OBJECT_DIR)/$(FORKNAME)_$(TARGET).map

LD_FLAGS    := \
              -lm \
              -lpthread \
              -lc \
              -lrt \
              $(ARCH_FLAGS) \
              $(LTO_FLAGS) \
              $(DEBUG_FLAGS) \
              -Wl,-gc-sections,-Map,$(TARGET_MAP) \
              -Wl,-L$(LINKER_DIR) \
              -Wl,--cref \
              -T$(LD_SCRIPT)

ifneq ($(filter SITL_STATIC,$(OPTIONS)),)
LD_FLAGS     += \
              -static \
              -static-libgcc
endif

ifneq ($(DEBUG),GDB)
OPTIMISE_DEFAULT    := -Ofast
OPTIMISE_SPEED      := -Ofast
OPTIMISE_SIZE       := -Os

LTO_FLAGS           := $(OPTIMISATION_BASE) $(OPTIMISE_SPEED)
endif
