idf_component_register(SRCS "hooks.c" REQUIRES esp_rom)
# hooks are weak symbols: force this library into the bootloader link
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
