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
        INCLUDE_DIRS "."
        EMBED_FILES website/index.html
        EMBED_FILES website/alpine.js
)
