idf_component_register(
        SRCS 
        "main.c" 
        "frame_io.c"
        "driver_setup.c"
        "socketcand_translate.c"
        "socketcand_server.c"
        "http_server.c"
        "persistent_settings.c"
        "discovery_beacon.c"
        "status_report.c"
        "cyphal_node.c"
        "can_listener.c"
        INCLUDE_DIRS "."
        EMBED_FILES
        website/index.html
        website/favicon.svg
        website/script.js
        website/alpine.js
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Werror -Wextra -Wshadow -Wmissing-field-initializers -fanalyzer)