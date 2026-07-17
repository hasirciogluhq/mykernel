# Usermode C++ SDK + apps (.mke)

SDK_SRCS := $(SRC)/user/sdk/syscall.cpp \
            $(SRC)/user/sdk/gfx.cpp \
            $(SRC)/user/sdk/process.cpp \
            $(SRC)/user/sdk/settings.cpp \
            $(SRC)/user/string.c
SDK_OBJS := $(BUILD)/user/sdk/syscall.o \
            $(BUILD)/user/sdk/gfx.o \
            $(BUILD)/user/sdk/process.o \
            $(BUILD)/user/sdk/settings.o \
            $(BUILD)/user/string.o

MKE_OSUI := $(USEROUT)/os-ui.mke
MKE_TERM := $(USEROUT)/terminal.mke
MKE_SETTINGS := $(USEROUT)/os-settings.mke
MKE_FILES := $(USEROUT)/files.mke
MKE_ACTIVITY := $(USEROUT)/activity-monitor.mke
MKE_IMGUI_DEMO := $(USEROUT)/imgui-demo.mke
MKES     := $(MKE_OSUI) $(MKE_TERM) $(MKE_SETTINGS) $(MKE_FILES) $(MKE_ACTIVITY) $(MKE_IMGUI_DEMO)
LOAD_OSUI := 0x02000000
LOAD_TERM := 0x02200000
LOAD_SETTINGS := 0x02400000
LOAD_FILES := 0x02600000
LOAD_ACTIVITY := 0x02800000
LOAD_IMGUI_DEMO := 0x02A00000

IMGUI_DEMO_SRCS := $(IMGUI_DEMO_DIR)/main.cpp \
                   $(IMGUI_DEMO_DIR)/imgui_impl_ugx.cpp \
                   $(IMGUI_DEMO_DIR)/support.cpp \
                   $(IMGUI_DIR)/imgui.cpp \
                   $(IMGUI_DIR)/imgui_draw.cpp \
                   $(IMGUI_DIR)/imgui_tables.cpp \
                   $(IMGUI_DIR)/imgui_widgets.cpp
IMGUI_DEMO_OBJS := $(BUILD)/apps/imgui-demo/main.o \
                   $(BUILD)/apps/imgui-demo/imgui_impl_ugx.o \
                   $(BUILD)/apps/imgui-demo/support.o \
                   $(BUILD)/apps/imgui-demo/imgui.o \
                   $(BUILD)/apps/imgui-demo/imgui_draw.o \
                   $(BUILD)/apps/imgui-demo/imgui_tables.o \
                   $(BUILD)/apps/imgui-demo/imgui_widgets.o

define pack_mke_from_elf
	@mkdir -p $(dir $(3))
	$(OBJCOPY) -O binary $(1) $(2)
	ENTRY=$$($(NM) -n $(1) | awk '/ mke_main$$/{print $$1}'); \
	EDATA=$$($(NM) -n $(1) | awk '/ _mke_edata$$/{print $$1}'); \
	END=$$($(NM) -n $(1) | awk '/ _mke_end$$/{print $$1}'); \
	ENTRY_OFF=$$((0x$$ENTRY - $(4))); \
	IMG=$$((0x$$EDATA - $(4))); \
	BSS=$$((0x$$END - 0x$$EDATA)); \
	$(PACK_MKE) $(3) $(2) $(4) $$ENTRY_OFF $$IMG $$BSS $(5)
endef

$(USEROUT)/os-ui.elf: $(BUILD)/user/apps/os-ui.o $(SDK_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_OSUI) -o $@ \
		$(BUILD)/user/apps/os-ui.o $(SDK_OBJS)

$(MKE_OSUI): $(USEROUT)/os-ui.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/os-ui.bin,$@,$(LOAD_OSUI),os-ui)

$(USEROUT)/terminal.elf: $(BUILD)/user/apps/terminal.o $(SDK_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_TERM) -o $@ \
		$(BUILD)/user/apps/terminal.o $(SDK_OBJS)

$(MKE_TERM): $(USEROUT)/terminal.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/terminal.bin,$@,$(LOAD_TERM),terminal)

$(USEROUT)/os-settings.elf: $(BUILD)/user/apps/os-settings.o $(SDK_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_SETTINGS) -o $@ \
		$(BUILD)/user/apps/os-settings.o $(SDK_OBJS)

$(MKE_SETTINGS): $(USEROUT)/os-settings.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/os-settings.bin,$@,$(LOAD_SETTINGS),os-settings)

$(USEROUT)/files.elf: $(BUILD)/user/apps/files.o $(SDK_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_FILES) -o $@ \
		$(BUILD)/user/apps/files.o $(SDK_OBJS)

$(MKE_FILES): $(USEROUT)/files.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/files.bin,$@,$(LOAD_FILES),files)

$(USEROUT)/activity-monitor.elf: $(BUILD)/user/apps/activity-monitor.o $(SDK_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_ACTIVITY) -o $@ \
		$(BUILD)/user/apps/activity-monitor.o $(SDK_OBJS)

$(MKE_ACTIVITY): $(USEROUT)/activity-monitor.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/activity-monitor.bin,$@,$(LOAD_ACTIVITY),activity-monitor)

$(BUILD)/apps/imgui-demo/%.o: $(IMGUI_DEMO_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUI_CXXFLAGS) -c $< -o $@

$(BUILD)/apps/imgui-demo/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(IMGUI_CXXFLAGS) -c $< -o $@

LIBGCC := $(shell $(CXX) -print-libgcc-file-name)

$(USEROUT)/imgui-demo.elf: $(IMGUI_DEMO_OBJS) $(SDK_OBJS) user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USERLDFLAGS) --defsym=LOAD_ADDR=$(LOAD_IMGUI_DEMO) -o $@ \
		$(IMGUI_DEMO_OBJS) $(SDK_OBJS) $(LIBGCC)

$(MKE_IMGUI_DEMO): $(USEROUT)/imgui-demo.elf $(PACK_MKE)
	$(call pack_mke_from_elf,$<,$(USEROUT)/imgui-demo.bin,$@,$(LOAD_IMGUI_DEMO),imgui-demo)

$(BUILD)/user/%.o: $(SRC)/user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USERCFLAGS) -c $< -o $@

$(BUILD)/user/%.o: $(SRC)/user/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
