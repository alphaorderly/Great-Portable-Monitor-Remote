#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { uint8_t type, val[6]; } ble_addr_t;
#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1
#define BLE_ADDR_PUBLIC_ID 2
#define BLE_ADDR_RANDOM_ID 3
#define CONFIG_BT_NIMBLE_MAX_BONDS 4
#define CONFIG_BT_NIMBLE_MAX_CCCDS 16
#define CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT 12
int os_msys_count(void);
int os_msys_num_free(void);
#define BLE_HS_EDONE 14
#define BLE_HS_EAPP 9
#define BLE_HS_EBADDATA 10
#define BLE_HS_ENOENT 5
#define BLE_HS_EMSGSIZE 4
#define BLE_HS_EENCRYPT 25
#define BLE_GATT_CHR_PROP_NOTIFY 0x10
#define BLE_GATT_CHR_PROP_READ 0x02
#define BLE_UUID_STR_LEN 37
typedef struct { uint16_t value; } ble_uuid_t;
typedef struct { ble_uuid_t u; } ble_uuid16_t;
typedef union { ble_uuid_t u; } ble_uuid_any_t;
#define BLE_UUID16_INIT(n) {{n}}
struct os_mbuf { const uint8_t *data; unsigned len; };
#define OS_MBUF_PKTLEN(om) ((om)->len)
struct ble_gatt_error { int status; uint16_t att_handle; };
struct ble_gatt_svc { uint16_t start_handle, end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_attr { uint16_t handle, offset; struct os_mbuf *om; };
struct ble_gap_conn_desc {
    struct { bool encrypted, bonded, authenticated; uint8_t key_size; } sec_state;
    ble_addr_t peer_id_addr, peer_ota_addr;
    uint16_t conn_handle, conn_itvl, conn_latency, supervision_timeout;
};
struct ble_gap_disc_desc { int event_type,rssi; ble_addr_t addr; const uint8_t *data; unsigned length_data; };
struct ble_gap_upd_params { unsigned itvl_min,itvl_max,latency,supervision_timeout; };
struct ble_gap_repeat_pairing {
    uint16_t conn_handle;
    uint8_t cur_key_size, cur_authenticated, cur_sc;
    uint8_t new_key_size, new_authenticated, new_sc, new_bonding;
};
struct ble_gap_event {
    int type;
    struct { struct os_mbuf *om; uint16_t attr_handle, conn_handle; bool indication; } notify_rx;
    struct { int status; uint16_t conn_handle; } connect, enc_change;
    struct { uint16_t attr_handle, conn_handle; bool cur_notify; } subscribe;
    struct { int reason; struct ble_gap_conn_desc conn; } disconnect;
    struct { int reason; } adv_complete, disc_complete;
    struct ble_gap_disc_desc disc;
    struct { unsigned status; } pairing_complete;
    struct { struct { unsigned action; } params; uint16_t conn_handle; } passkey;
    struct { const struct ble_gap_upd_params *peer_params; } conn_update_req;
    struct { int status; uint16_t conn_handle; } conn_update;
    struct { int status; uint16_t attr_handle, conn_handle; bool indication; } notify_tx;
    struct ble_gap_repeat_pairing repeat_pairing;
};
typedef int ble_gatt_disc_svc_fn(uint16_t,const struct ble_gatt_error *,const struct ble_gatt_svc *,void *);
typedef int ble_gatt_chr_fn(uint16_t,const struct ble_gatt_error *,const struct ble_gatt_chr *,void *);
typedef int ble_gatt_dsc_fn(uint16_t,const struct ble_gatt_error *,uint16_t,const struct ble_gatt_dsc *,void *);
typedef int ble_gatt_attr_fn(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *);
int ble_gap_conn_find(uint16_t,struct ble_gap_conn_desc *);
int ble_gattc_disc_svc_by_uuid(uint16_t,const ble_uuid_t *,ble_gatt_disc_svc_fn *,void *);
int ble_gattc_disc_all_chrs(uint16_t,uint16_t,uint16_t,ble_gatt_chr_fn *,void *);
int ble_gattc_disc_all_dscs(uint16_t,uint16_t,uint16_t,ble_gatt_dsc_fn *,void *);
int ble_gattc_read_long(uint16_t,uint16_t,uint16_t,ble_gatt_attr_fn *,void *);
int ble_gattc_read(uint16_t,uint16_t,ble_gatt_attr_fn *,void *);
int ble_gattc_write_flat(uint16_t,uint16_t,const void *,uint16_t,ble_gatt_attr_fn *,void *);
int os_mbuf_copydata(const struct os_mbuf *,int,int,void *);
uint16_t ble_uuid_u16(const ble_uuid_t *);
char *ble_uuid_to_str(const ble_uuid_t *,char *);
void test_log(const char *,...);
void test_hex(const void *,unsigned);
#define ESP_LOG_INFO 3
#define ESP_LOGI(tag,...) ((void)(tag),test_log(__VA_ARGS__))
#define ESP_LOGW(tag,...) ((void)(tag),test_log(__VA_ARGS__))
#define ESP_LOGE(tag,...) ((void)(tag),test_log(__VA_ARGS__))
#define ESP_LOG_BUFFER_HEX_LEVEL(tag,data,len,level) ((void)(tag),test_hex(data,len))

/* Peripheral-only test declarations; the real ABI is checked by the IDF build. */
#define BLE_UUID16_DECLARE(n) (&(const ble_uuid_t){n})
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EALREADY 2
#define BLE_HS_FOREVER -1
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 5
#define BLE_GATT_ACCESS_OP_READ_DSC 1
#define BLE_GATT_ACCESS_OP_READ_CHR 2
#define BLE_GATT_ACCESS_OP_WRITE_CHR 3
#define BLE_GATT_CHR_F_READ 1
#define BLE_GATT_CHR_F_READ_ENC 2
#define BLE_GATT_CHR_F_NOTIFY 4
#define BLE_GATT_CHR_F_WRITE_NO_RSP 8
#define BLE_GATT_CHR_F_WRITE_ENC 16
#define BLE_GATT_CHR_F_WRITE 32
#define BLE_ATT_F_READ 1
#define BLE_ATT_F_READ_ENC 2
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_ENC_CHANGE 2
#define BLE_GAP_EVENT_SUBSCRIBE 3
#define BLE_GAP_EVENT_DISCONNECT 4
#define BLE_GAP_EVENT_NOTIFY_TX 5
#define BLE_GAP_EVENT_REPEAT_PAIRING 6
#define BLE_GAP_EVENT_PASSKEY_ACTION 7
#define BLE_GAP_EVENT_ADV_COMPLETE 8
#define BLE_GAP_REPEAT_PAIRING_IGNORE 2
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
#define BLE_ERR_REM_USER_CONN_TERM 19
#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 1
struct ble_gatt_chr_def;
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; const struct ble_gatt_chr_def *chr; };
typedef int access_fn(uint16_t,uint16_t,struct ble_gatt_access_ctxt *,void *);
struct ble_gatt_dsc_def { const ble_uuid_t *uuid; access_fn *access_cb; unsigned att_flags; void *arg; };
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid; access_fn *access_cb; unsigned flags; void *arg;
    uint16_t *val_handle; const struct ble_gatt_dsc_def *descriptors;
};
struct ble_gatt_svc_def { int type; const ble_uuid_t *uuid; const struct ble_gatt_chr_def *characteristics; };
struct ble_npl_event { int unused; };
struct ble_npl_callout { int unused; };
struct ble_hs_adv_fields {
    unsigned flags; const ble_uuid16_t *uuids16; unsigned num_uuids16, uuids16_is_complete;
    unsigned appearance, appearance_is_present; const uint8_t *name; unsigned name_len, name_is_complete;
    unsigned num_uuids32,num_uuids128; const ble_uuid16_t *uuids32,*uuids128;
};
struct ble_gap_adv_params { int conn_mode, disc_mode; uint16_t itvl_min, itvl_max; };
#define BLE_GAP_CONN_MODE_DIR 2
#define BLE_GAP_DISC_MODE_NON 0
struct os_mbuf *ble_hs_mbuf_from_flat(const void *,uint16_t);
int ble_gatts_notify_custom(uint16_t,uint16_t,struct os_mbuf *);
int os_mbuf_append(struct os_mbuf *,const void *,unsigned);
int ble_npl_callout_reset(struct ble_npl_callout *,uint32_t);
void ble_npl_callout_stop(struct ble_npl_callout *);
int ble_gap_adv_active(void);
uint32_t ble_npl_time_ms_to_ticks32(uint32_t);
uint32_t ble_npl_time_ticks_to_ms32(uint32_t);
uint32_t ble_npl_time_get(void);
int ble_npl_callout_init(struct ble_npl_callout *,void *,void (*)(struct ble_npl_event *),void *);
void *nimble_port_get_dflt_eventq(void);
int ble_gap_security_initiate(uint16_t);
int ble_gap_terminate(uint16_t,int);
void ble_svc_gatt_changed(uint16_t,uint16_t);
void ble_svc_gap_init(void);
void ble_svc_gatt_init(void);
int ble_svc_gap_device_name_set(const char *);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *);
int ble_gap_adv_start(uint8_t,const ble_addr_t *,int,const struct ble_gap_adv_params *,int (*)(struct ble_gap_event *,void *),void *);
struct ble_store_key_cccd { ble_addr_t peer_addr; uint16_t chr_val_handle; };
struct ble_store_value_cccd { unsigned flags; };
int ble_store_read_cccd(const struct ble_store_key_cccd *,struct ble_store_value_cccd *);
int ble_store_util_bonded_peers(ble_addr_t *,int *,int);
int ble_store_util_delete_peer(const ble_addr_t *);

/* Central lifecycle test declarations. */
#define BLE_HS_ERR_ATT_BASE 0x100
#define BLE_HS_ERR_HCI_BASE 0x200
#define BLE_HS_ERR_L2C_BASE 0x300
#define BLE_HS_ERR_SM_US_BASE 0x400
#define BLE_HS_ERR_SM_PEER_BASE 0x500
#define BLE_HS_ERR_HW_BASE 0x600
#define BLE_HS_ESTORE_CAP 29
#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
#define BLE_STORE_OBJ_TYPE_CCCD 3
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP 4
#define BLE_HCI_ADV_RPT_EVTYPE_ADV_IND 0
#define BLE_HCI_ADV_RPT_EVTYPE_DIR_IND 1
#define BLE_HS_ADV_MAX_SZ 31
#define BLE_GAP_EVENT_DISC 9
#define BLE_GAP_EVENT_DISC_COMPLETE 10
#define BLE_GAP_EVENT_PARING_COMPLETE 11
#define BLE_GAP_EVENT_NOTIFY_RX 12
#define BLE_GAP_EVENT_CONN_UPDATE_REQ 13
#define BLE_GAP_EVENT_L2CAP_UPDATE_REQ 14
#define BLE_GAP_EVENT_CONN_UPDATE 15
#define BLE_GAP_EVENT_IDENTITY_RESOLVED 16
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOG_WARN 2
struct ble_store_key_sec { ble_addr_t peer_addr; };
struct ble_store_value_sec { bool ltk_present,sc,authenticated; uint8_t key_size; };
union ble_store_value { int unused; };
struct ble_store_status_event { unsigned event_code; };
typedef int ble_store_write_fn(int,const union ble_store_value *);
struct test_hs_cfg {
    void (*sync_cb)(void); void (*reset_cb)(int);
    int sm_io_cap,sm_bonding,sm_mitm,sm_sc,sm_sc_only,sm_oob_data_flag,sm_our_key_dist,sm_their_key_dist;
    ble_store_write_fn *store_write_cb;
    int (*store_status_cb)(struct ble_store_status_event *,void *);
};
extern struct test_hs_cfg ble_hs_cfg;
struct ble_gap_disc_params { int itvl,window,passive,filter_duplicates,limited,filter_policy; };
int ble_gap_disc(uint8_t,int,const struct ble_gap_disc_params *,int (*)(struct ble_gap_event *,void *),void *);
int ble_gap_disc_cancel(void);
int ble_gap_disc_active(void);
int ble_gap_connect(uint8_t,const ble_addr_t *,int,const void *,int (*)(struct ble_gap_event *,void *),void *);
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *,const uint8_t *,unsigned);
int ble_store_read_peer_sec(const struct ble_store_key_sec *,struct ble_store_value_sec *);
int ble_store_read_our_sec(const struct ble_store_key_sec *,struct ble_store_value_sec *);
int ble_hs_util_ensure_addr(int);
int ble_hs_id_infer_auto(int,uint8_t *);
const char *esp_err_to_name(esp_err_t);
const char *esp_get_idf_version(void);
void esp_log_level_set(const char *,int);
int nvs_flash_init(void);
int nimble_port_init(void);
void nimble_port_run(void);
void nimble_port_freertos_init(void (*)(void *));
void nimble_port_freertos_deinit(void);
