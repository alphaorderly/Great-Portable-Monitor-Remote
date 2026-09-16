idf_component_register(SRCS "main.c" "bond_store.c" "hid_client.c" "hid_input.c"
    "input_codec.c" "bridge_peers.c" "input_log.c" "keymap.c" "keymap_store.c" "macro.c" "macro_store.c" "usb_hid.c"
    INCLUDE_DIRS "." PRIV_REQUIRES bt nvs_flash esp_tinyusb)
# Feature ID 5 includes a 64-byte payload plus its report ID.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_compile_definitions(${tusb_lib} PUBLIC CFG_TUD_HID_EP_BUFSIZE=128)
