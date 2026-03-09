idf_component_register(
    SRCS "main.c"
         "app_manager.c"
         "wifi_time.c"
         "screen_clock.c"
         "screen_menu.c"
         "screen_settings.c"
         "boot_sound.c"
         "weather.c"
         "sd_log.c"
         "game_maze.c"
         "music_player.c"
    INCLUDE_DIRS "."
    REQUIRES bt
    PRIV_REQUIRES i2c_bsp lcd_bl_pwm_bsp nvs_flash esp_http_client esp_wifi esp_netif esp_event esp_timer driver esp_driver_i2s json spiffs mbedtls minimp3)
