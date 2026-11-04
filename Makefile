ifeq ($(origin CC),default)
CC = m68k-amigaos-gcc
endif
CC_TARGET := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq ($(findstring aros,$(CC_TARGET)),)
TARGET_OS ?= aros
else ifneq ($(findstring ppc-amigaos,$(CC_TARGET)),)
TARGET_OS ?= os4
else
TARGET_OS ?= os3
endif

ifeq ($(TARGET_OS),aros)
BUILD_DIR ?= build/aros
AMIGA_CPUFLAGS ?= $(if $(findstring x86_64,$(CC_TARGET)),-mno-red-zone,)
AMIGA_SYSFLAGS ?=
AMIGA_DEFS ?= -DAMIGA
LIBS ?= -lamiga -larossupport -lgcc
else ifeq ($(TARGET_OS),os4)
BUILD_DIR ?= build/os4
AMIGA_CPUFLAGS ?= -mcpu=powerpc
AMIGA_SYSFLAGS ?= -mcrt=newlib
AMIGA_DEFS ?= -DAMIGA -D__USE_INLINE__ -D__USE_BASETYPE__
LIBS ?= -lauto -lgcc
else ifeq ($(TARGET_OS),os3)
BUILD_DIR ?= build
AMIGA_CPUFLAGS ?= -m68000 -mtune=68020-60 -msoft-float
AMIGA_SYSFLAGS ?= -noixemul
AMIGA_DEFS ?= -DAMIGA
LIBS ?= -lamiga -lgcc
else
$(error Unsupported TARGET_OS: $(TARGET_OS))
endif

TARGET ?= $(BUILD_DIR)/CDBench

CFLAGS ?= -O2 -g
WARNFLAGS ?= -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS ?=
LDFLAGS ?= $(AMIGA_SYSFLAGS)

SRCS = src/cdbench.c
OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean fixture-iso

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(AMIGA_CPUFLAGS) $(AMIGA_SYSFLAGS) $(AMIGA_DEFS) \
		$(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD_DIR)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(AMIGA_CPUFLAGS) $(AMIGA_SYSFLAGS) $(AMIGA_DEFS) \
		$(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -MMD -MP -c $< -o $@

fixture-iso:
	tools/make-fixture-iso.sh $(BUILD_DIR)/fixtures

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
