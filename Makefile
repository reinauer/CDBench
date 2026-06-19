ifeq ($(origin CC),default)
CC = m68k-amigaos-gcc
endif
BUILD_DIR ?= build
TARGET ?= $(BUILD_DIR)/CDBench

AMIGA_CPUFLAGS ?= -m68000 -mtune=68020-60 -msoft-float
AMIGA_SYSFLAGS ?= -noixemul
AMIGA_DEFS ?= -DAMIGA

CFLAGS ?= -O2 -g
WARNFLAGS ?= -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS ?=
LDFLAGS ?= $(AMIGA_SYSFLAGS)
LIBS ?= -lamiga -lgcc

SRCS = src/cdbench.c
OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean fixture-iso

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(AMIGA_CPUFLAGS) $(AMIGA_SYSFLAGS) $(AMIGA_DEFS) \
		$(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(AMIGA_CPUFLAGS) $(AMIGA_SYSFLAGS) $(AMIGA_DEFS) \
		$(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -MMD -MP -c $< -o $@

fixture-iso:
	tools/make-fixture-iso.sh $(BUILD_DIR)/fixtures

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
