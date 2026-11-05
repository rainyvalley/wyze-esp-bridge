idf_component_register(SRCS "main.c" "util.c" "wyze_proto.c" "relay_state.c" "log_dedup.c" "log_snap.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "web/index.html" "web/setup.html")
