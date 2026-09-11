idf_component_register(
    SRCS "camera_lab_main.c" "camera_lab_ui.c"
         "usb_camera.c" "onboard_camera.c" "face_recognition_service.cpp"
    INCLUDE_DIRS "."
    REQUIRES console esp_board_manager esp_lvgl_port lvgl
             usb usb_host_uvc esp_driver_jpeg human_face_recognition
)
