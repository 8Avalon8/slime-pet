CORE := ../components/slime_core
SRCS := host_main.c $(CORE)/sg.c $(CORE)/slime_anim.c $(CORE)/slime_render.c $(CORE)/slime_bg.c $(CORE)/slime_text.c $(CORE)/slime_glyphs.c $(CORE)/slime_glyphs_ext.c $(CORE)/slime_menu.c
CFLAGS := -O2 -fno-math-errno -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(CORE)/include
OUT := out

all: host_sim cc_test

# ---- slime_sim: the real firmware (main/) on top of sim/, the stand-in for ESP-IDF and the board ----
MAIN := ../main
SIM_SRCS := sim/sim_main.c sim/sim_idf.c sim/sim_drivers.c $(MAIN)/main.c $(MAIN)/config.c $(MAIN)/inbox.c \
	$(MAIN)/cc_track.c $(MAIN)/settings_ui.c $(MAIN)/web.c $(filter-out host_main.c,$(SRCS))
SIM_CFLAGS := -O2 -g -std=gnu11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -pthread -Wall -Wno-unused-parameter \
	-Wno-missing-field-initializers -Wno-unused-function -Wno-format-truncation -Wno-unknown-warning-option \
	-Isim -Isim/idf -I$(MAIN) -I$(CORE)/include
SIM_LIBS := -lm -pthread
# SDL2 gives the window; without it the simulator runs headless (HTTP and stdin only)
ifneq ($(SIM_HEADLESS),1)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
endif
ifneq ($(SDL_LIBS),)
SIM_CFLAGS += -DSIM_SDL $(shell pkg-config --cflags sdl2)
SIM_LIBS += $(SDL_LIBS)
endif
# cJSON (web.c): the system's, else the copy inside ESP-IDF
CJSON_LIBS := $(shell pkg-config --libs libcjson 2>/dev/null)
ifneq ($(CJSON_LIBS),)
SIM_CFLAGS += $(shell pkg-config --cflags libcjson)
SIM_LIBS += $(CJSON_LIBS)
else ifneq ($(wildcard $(IDF_PATH)/components/json/cJSON/cJSON.c),)
SIM_CFLAGS += -I$(IDF_PATH)/components/json/cJSON
SIM_SRCS += $(IDF_PATH)/components/json/cJSON/cJSON.c
endif

slime_sim: $(SIM_SRCS) sim/index_html.S $(MAIN)/web/index.html $(wildcard sim/*.h sim/idf/*.h sim/idf/*/*.h $(MAIN)/*.h $(CORE)/include/*.h)
	@[ -n "$(CJSON_LIBS)$(wildcard $(IDF_PATH)/components/json/cJSON/cJSON.c)" ] || \
		{ echo "slime_sim needs cJSON: brew install cjson / apt install libcjson-dev, or set IDF_PATH"; exit 1; }
	$(CC) $(SIM_CFLAGS) -DINDEX_HTML='"$(MAIN)/web/index.html"' -o $@ $(SIM_SRCS) sim/index_html.S $(SIM_LIBS)

sim: slime_sim
	./slime_sim

# scripted end-to-end run of the real firmware: hook lines in, status and screenshots out
simtest: slime_sim
	python3 sim/sim_test.py

cc_test: cc_test.c ../main/cc_track.c ../main/cc_track.h
	$(CC) $(CFLAGS) -I../main -o $@ cc_test.c ../main/cc_track.c

test: cc_test host_sim
	./cc_test && ./host_sim splitcheck

host_sim: $(SRCS) $(wildcard $(CORE)/include/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

sheet: host_sim
	mkdir -p $(OUT)
	./host_sim sheet $(OUT)/sheet.bmp
	if command -v sips >/dev/null; then sips -s format png $(OUT)/sheet.bmp --out $(OUT)/sheet.png >/dev/null; fi

bench: host_sim
	./host_sim bench

clean:
	rm -rf host_sim cc_test slime_sim slime_sim.dSYM $(OUT)

.PHONY: all sheet bench clean test sim simtest
