idf_component_register(SRCS "sg.c" "slime_anim.c" "slime_render.c" "slime_text.c" "slime_glyphs.c" "slime_menu.c"
                       INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-math-errno)
