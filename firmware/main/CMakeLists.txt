idf_component_register(SRCS "main.c" "hid_client.c" "mac_hid.c" "input_codec.c" "bridge_peers.c" "input_log.c" "keymap.c" "keymap_store.c" INCLUDE_DIRS "." PRIV_REQUIRES bt nvs_flash)
