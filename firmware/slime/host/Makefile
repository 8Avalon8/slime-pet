CORE := ../components/slime_core
SRCS := host_main.c $(CORE)/sg.c $(CORE)/slime_anim.c $(CORE)/slime_render.c $(CORE)/slime_text.c $(CORE)/slime_glyphs.c $(CORE)/slime_glyphs_ext.c $(CORE)/slime_menu.c
CFLAGS := -O2 -fno-math-errno -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(CORE)/include
OUT := out

all: host_sim cc_test

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
	rm -rf host_sim cc_test $(OUT)

.PHONY: all sheet bench clean test
