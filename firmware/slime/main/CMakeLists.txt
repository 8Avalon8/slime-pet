idf_component_register(SRCS "main.c" "usb_link.c" "cc_track.c" "haptic.c" "sensors.c" "config.c"
                            "inbox.c" "net.c" "web.c" "audio.c" "bootlog.c" "settings_ui.c" "vision.cpp" "sfxr.c" "ota.c" "wake.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html"
                       REQUIRES slime_core esp_lcd esp_timer esp_mm nvs_flash esp_wifi esp_netif esp_event espcoredump app_update esp_app_format
                                esp_http_server esp_driver_ppa)

# The wake word model rides inside the app image instead of a "model" flash partition: no
# partition table change, and it travels with every wireless update. esp-sr's own packing
# step only runs when that partition exists, so the same script is run here.
idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(python PYTHON)
idf_component_get_property(sr_dir espressif__esp-sr COMPONENT_DIR)
set(wake_model ${build_dir}/srmodels/srmodels.bin)
add_custom_command(
    OUTPUT ${wake_model}
    COMMENT "Packing the wake word model"
    COMMAND ${python} ${sr_dir}/model/movemodel.py -d1 ${SDKCONFIG} -d2 ${sr_dir} -d3 ${build_dir}
    DEPENDS ${SDKCONFIG}
    VERBATIM)
add_custom_target(wake_model_bin DEPENDS ${wake_model})
target_add_binary_data(${COMPONENT_LIB} ${wake_model} BINARY DEPENDS wake_model_bin)
