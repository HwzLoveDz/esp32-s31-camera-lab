# Generated Korvo-1 configuration, frozen alongside its board-specific sources.
idf_component_register(
    SRCS "gen_board_device_config.c" "gen_board_device_handles.c"
         "gen_board_info.c" "gen_board_periph_config.c" "gen_board_periph_handles.c"
         "board/camera_sensor_bringup.c" "board/setup_device.c"
    INCLUDE_DIRS "." "board"
    REQUIRES esp_board_manager
    WHOLE_ARCHIVE
)
