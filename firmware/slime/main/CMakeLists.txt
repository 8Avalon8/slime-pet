idf_component_register(SRCS "main.c" "usb_link.c" "cc_track.c" "haptic.c" "sensors.c" "config.c"
                            "inbox.c" "net.c" "web.c" "audio.c" "bootlog.c" "settings_ui.c" "vision.cpp" "sfxr.c" "ota.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html"
                       REQUIRES slime_core esp_lcd esp_timer esp_mm nvs_flash esp_wifi esp_netif esp_event espcoredump app_update esp_app_format
                                esp_http_server esp_driver_ppa)
