idf_component_register(SRCS 
                            "ppi_process.c"
                            "ppi_time_feat.c"
                            "ppi_fre_feat.c"
                            "ppi_nl_feat.c"
                            "anxiety_idx_cal.c"
                       PRIV_REQUIRES 
                             app_common
                             log
                             esp_driver_gpio
                             esp_adc
                       INCLUDE_DIRS 
                              "include")

target_link_libraries(${COMPONENT_LIB} INTERFACE m)