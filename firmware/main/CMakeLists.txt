# No REQUIRES list: the "main" component then depends on every component
# of the build (app_update, driver, esp_app_format, fw_protocol, ...).
idf_component_register(SRCS "main.c"
                            "uart_updater.c"
                            "self_test.c"
                       INCLUDE_DIRS ".")
