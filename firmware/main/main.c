#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "hid_client.h"
#include "mac_hid.h"
#include "bridge_peers.h"
#include "input_log.h"

#define SCAN_DURATION_MS 30000
#define CONNECT_TIMEOUT_MS 10000
/* Legacy scan timing units are 0.625 ms. */
#define SCAN_INTERVAL_UNITS 160
#define SCAN_WINDOW_UNITS 80
#define HID_SERVICE_UUID 0x1812
#define CANDIDATE_CAPACITY 8
#define ADDRESS_FORMAT "%02X:%02X:%02X:%02X:%02X:%02X"
/* NimBLE stores address octets least significant first. */
#define ADDRESS_BYTES(a) (a)[5], (a)[4], (a)[3], (a)[2], (a)[1], (a)[0]

static const char *TAG = "REMOTE_PAIR";
static const char TARGET_NAME[] = "Bluetooth remote";
static uint8_t own_addr_type;
static bool scan_started;
static bool connection_attempted;
static bool host_synced;
static uint16_t remote_conn = BLE_HS_CONN_HANDLE_NONE;
static struct ble_npl_callout scan_retry;
static ble_addr_t remote_identity;
static bool have_remote_identity;
static bool bond_store_failed;
static unsigned report_count;
static ble_store_write_fn *original_store_write;

/* Active scan delivers ADV and SCAN_RSP separately; merge only the same address/type. */
typedef struct {
    bool used;
    ble_addr_t addr;
    bool name_match;
    bool hid_service;
    bool connectable;
} candidate_t;
static candidate_t candidates[CANDIDATE_CAPACITY];
static unsigned next_candidate;

static int gap_callback(struct ble_gap_event *event, void *arg);
static void start_scan(struct ble_npl_event *event);
void ble_store_config_init(void); /* Declaration used by the official blecent example. */

static void retry_scan(void)
{
    if (!host_synced || remote_conn != BLE_HS_CONN_HANDLE_NONE || connection_attempted) { return; }
    int rc = ble_npl_callout_reset(&scan_retry, ble_npl_time_ms_to_ticks32(2000));
    if (rc) { ESP_LOGE("REMOTE_STATUS", "scan retry scheduling failed: rc=%d", rc); }
}

static const char *address_type_name(uint8_t type)
{
    switch (type) {
    case BLE_ADDR_PUBLIC: return "PUBLIC";
    case BLE_ADDR_RANDOM: return "RANDOM";
    case BLE_ADDR_PUBLIC_ID: return "PUBLIC_ID";
    case BLE_ADDR_RANDOM_ID: return "RANDOM_ID";
    default: return "UNKNOWN";
    }
}

static bool same_address(const ble_addr_t *a, const ble_addr_t *b)
{
    return a->type == b->type && memcmp(a->val, b->val, sizeof(a->val)) == 0;
}

static bool esp_ok(const char *operation, esp_err_t rc)
{
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s (0x%X)", operation, esp_err_to_name(rc), (unsigned)rc);
        return false;
    }
    ESP_LOGI(TAG, "%s: %s (0x%X)", operation, esp_err_to_name(rc), (unsigned)rc);
    return true;
}

/* NimBLE rc values are not esp_err_t or raw HCI status codes. */
static void log_status(const char *operation, int rc)
{
    const char *domain = "HOST";
    int detail = rc;
    if (rc >= BLE_HS_ERR_SM_PEER_BASE && rc < BLE_HS_ERR_HW_BASE) {
        domain = "SMP_PEER"; detail -= BLE_HS_ERR_SM_PEER_BASE;
    } else if (rc >= BLE_HS_ERR_SM_US_BASE && rc < BLE_HS_ERR_SM_PEER_BASE) {
        domain = "SMP_LOCAL"; detail -= BLE_HS_ERR_SM_US_BASE;
    } else if (rc >= BLE_HS_ERR_HCI_BASE && rc < BLE_HS_ERR_L2C_BASE) {
        domain = "HCI"; detail -= BLE_HS_ERR_HCI_BASE;
    } else if (rc >= BLE_HS_ERR_L2C_BASE && rc < BLE_HS_ERR_SM_US_BASE) {
        domain = "L2CAP"; detail -= BLE_HS_ERR_L2C_BASE;
    } else if (rc >= BLE_HS_ERR_ATT_BASE && rc < BLE_HS_ERR_HCI_BASE) {
        domain = "ATT"; detail -= BLE_HS_ERR_ATT_BASE;
    } else if (rc >= BLE_HS_ERR_HW_BASE && rc < BLE_HS_ERR_HW_BASE + 0x100) {
        domain = "HARDWARE"; detail -= BLE_HS_ERR_HW_BASE;
    }
    if (rc) {
        ESP_LOGW(TAG, "%s: status=%d (0x%X) domain=%s code=0x%02X",
                 operation, rc, (unsigned)rc, domain, (unsigned)detail);
    } else {
        ESP_LOGI(TAG, "%s: status=%d (0x%X) domain=%s code=0x%02X",
                 operation, rc, (unsigned)rc, domain, (unsigned)detail);
    }
}

static bool read_peer_bond(const ble_addr_t *identity)
{
    struct ble_store_key_sec key = {.peer_addr = *identity};
    struct ble_store_value_sec value = {0};
    const int rc = ble_store_read_peer_sec(&key, &value);
    log_status("bond read (peer security record)", rc);
    if (rc == 0) {
        ESP_LOGI(TAG, "bond record: peer=" ADDRESS_FORMAT
                 " type=%s LTK_present=%u key_size=%u SC=%u authenticated=%u",
                 ADDRESS_BYTES(identity->val), address_type_name(identity->type),
                 value.ltk_present, value.key_size, value.sc, value.authenticated);
    }
    /* Never print the key bytes. */
    return rc == 0 && value.ltk_present;
}

static int store_write_logged(int type, const union ble_store_value *value)
{
    const int rc = original_store_write(type, value);
    if (type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        ESP_LOGI(TAG, "bond storage write: type=%s status=%d (0x%X) NVS_enabled=1",
                 type == BLE_STORE_OBJ_TYPE_OUR_SEC ? "OUR_SEC" : "PEER_SEC", rc, (unsigned)rc);
        if (rc != 0) {
            bond_store_failed = true;
            ESP_LOGE(TAG, "bond storage failed; encryption alone is not bonding success");
        }
    }
    return rc;
}

static int store_status(struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    bond_store_failed = true;
    ESP_LOGE(TAG, "bond storage capacity event=%u; existing bonds retained; status=BLE_HS_ESTORE_CAP (0x%X)",
             event->event_code, BLE_HS_ESTORE_CAP);
    return BLE_HS_ESTORE_CAP;
}

static void log_connection(uint16_t handle, bool check_security_result)
{
    struct ble_gap_conn_desc desc;
    const int rc = ble_gap_conn_find(handle, &desc);
    log_status("ble_gap_conn_find", rc);
    if (rc != 0) { return; }
    ESP_LOGI(TAG, "connection: handle=%u peer=" ADDRESS_FORMAT " type=%s"
             " interval_units=%u latency=%u timeout_units=%u",
             handle, ADDRESS_BYTES(desc.peer_ota_addr.val), address_type_name(desc.peer_ota_addr.type),
             desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
    ESP_LOGI(TAG, "encryption status: encrypted=%u authenticated=%u bonded=%u key_size=%u",
             desc.sec_state.encrypted, desc.sec_state.authenticated,
             desc.sec_state.bonded, desc.sec_state.key_size);
    ESP_LOGI("REMOTE_STATUS", "link handle=%u interval=%u latency=%u timeout=%u encrypted=%u bonded=%u",
             handle, desc.conn_itvl, desc.conn_latency, desc.supervision_timeout,
             desc.sec_state.encrypted, desc.sec_state.bonded);
    if (check_security_result) {
        const bool stored_ltk = read_peer_bond(&desc.peer_id_addr);
        if (desc.sec_state.encrypted && desc.sec_state.bonded && stored_ltk && !bond_store_failed) {
            remote_identity = desc.peer_id_addr;
            have_remote_identity = true;
            bridge_peer_save("remote", &remote_identity);
            ESP_LOGI("REMOTE_STATUS", "SECURITY OK: encrypted=1 bonded=1");
            hid_client_start(handle);
        } else {
            hid_client_stop(handle);
            ESP_LOGW(TAG, "SECURITY CHECK INCOMPLETE: peer_LTK_present=%u storage_error=%u",
                     stored_ltk, bond_store_failed);
        }
    }
}

static void connect_once(const candidate_t *candidate)
{
    /* Reserve this attempt before cancellation can dispatch DISC_COMPLETE. */
    connection_attempted = true;
    ESP_LOGI("REMOTE_STATUS", "target found: address=" ADDRESS_FORMAT " address_type=%s(0x%02X) match=%s",
             ADDRESS_BYTES(candidate->addr.val), address_type_name(candidate->addr.type), candidate->addr.type,
             candidate->name_match ? "name+HID_UUID" : "stored bonded identity");
    int rc = ble_gap_disc_cancel();
    log_status("scan stop: ble_gap_disc_cancel", rc);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        connection_attempted = false;
        ESP_LOGW("REMOTE_STATUS", "scan cancel failed; retry scheduled");
        retry_scan();
        return;
    }
    scan_started = false;
    ESP_LOGI(TAG, "connection attempt: timeout=%dms", CONNECT_TIMEOUT_MS);
    rc = ble_gap_connect(own_addr_type, &candidate->addr, CONNECT_TIMEOUT_MS, NULL, gap_callback, NULL);
    log_status("ble_gap_connect (request)", rc);
    if (rc != 0) { connection_attempted = false; retry_scan(); }
}

static void log_advertisement(const struct ble_gap_disc_desc *report)
{
    if (connection_attempted || remote_conn != BLE_HS_CONN_HANDLE_NONE) { return; }
    ++report_count;
    const bool scan_response = report->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    const bool connectable = report->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                             report->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
    /* Bonded remotes may wake with a directed/nameless advertisement. */
    if (connectable && have_remote_identity && bridge_peer_equal(&remote_identity, &report->addr)) {
        candidate_t known = {.addr = report->addr};
        ESP_LOGI("REMOTE_STATUS", "bonded remote awake; reconnect using stored key");
        connect_once(&known);
        return;
    }
    ESP_LOGI(TAG, "advertisement #%u address=" ADDRESS_FORMAT
             " address_type=%s(0x%02X) RSSI=%d event_type=0x%02X",
             report_count, ADDRESS_BYTES(report->addr.val), address_type_name(report->addr.type),
             report->addr.type, report->rssi, report->event_type);
    ESP_LOGI(TAG, "%s raw len=%u", scan_response ? "SCAN_RSP" : "ADV", report->length_data);
    if (report->length_data) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, report->data, report->length_data, ESP_LOG_INFO);
    }
    struct ble_hs_adv_fields fields;
    const int rc = ble_hs_adv_parse_fields(&fields, report->data, report->length_data);
    if (rc != 0) {
        log_status("advertisement parse failed", rc);
        return;
    }
    char name[BLE_HS_ADV_MAX_SZ + 1] = {0};
    for (size_t i = 0; i < fields.name_len && i < sizeof(name) - 1; ++i) {
        if (fields.name[i] == 0) { break; }
        name[i] = fields.name[i] >= 0x20 && fields.name[i] <= 0x7e ? fields.name[i] : '.';
    }
    const bool name_match = fields.name_is_complete && fields.name_len == strlen(TARGET_NAME) &&
                            memcmp(fields.name, TARGET_NAME, strlen(TARGET_NAME)) == 0;
    bool hid = false;
    for (unsigned i = 0; i < fields.num_uuids16; ++i) {
        const uint16_t uuid = ble_uuid_u16(&fields.uuids16[i].u);
        ESP_LOGI(TAG, "service UUID=0x%04X%s", uuid, uuid == HID_SERVICE_UUID ? " (HID)" : "");
        hid |= uuid == HID_SERVICE_UUID;
    }
    char uuid_text[BLE_UUID_STR_LEN];
    for (unsigned i = 0; i < fields.num_uuids32; ++i) {
        ESP_LOGI(TAG, "service UUID=%s", ble_uuid_to_str(&fields.uuids32[i].u, uuid_text));
    }
    for (unsigned i = 0; i < fields.num_uuids128; ++i) {
        ESP_LOGI(TAG, "service UUID=%s", ble_uuid_to_str(&fields.uuids128[i].u, uuid_text));
    }
    if (fields.appearance_is_present) {
        ESP_LOGI(TAG, "appearance=0x%04X", fields.appearance);
    }
    ESP_LOGI(TAG, "device name=\"%s\" name_kind=%s HID_UUID=%s",
             name[0] ? name : "<absent>", fields.name_is_complete ? "complete" : "short/absent",
             hid ? "yes" : "not_in_this_packet");

    candidate_t *candidate = NULL;
    for (unsigned i = 0; i < CANDIDATE_CAPACITY; ++i) {
        if (candidates[i].used && same_address(&candidates[i].addr, &report->addr)) {
            candidate = &candidates[i];
            break;
        }
    }
    if (!candidate) {
        /* A flags-only ADV can be followed by a SCAN_RSP containing both name and UUID. */
        if (!name_match && !hid && !connectable) { return; }
        candidate = &candidates[next_candidate++ % CANDIDATE_CAPACITY];
        *candidate = (candidate_t){.used = true, .addr = report->addr};
    }
    if (fields.name_is_complete) { candidate->name_match = name_match; }
    candidate->hid_service |= hid;
    if (!scan_response) {
        candidate->connectable = connectable;
    }
    ESP_LOGI(TAG, "candidate accumulated: name_match=%u HID_UUID=%u connectable=%u",
             candidate->name_match, candidate->hid_service, candidate->connectable);
    if (candidate->name_match && candidate->hid_service && candidate->connectable) {
        connect_once(candidate);
    }
}

static int gap_callback(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        log_advertisement(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        scan_started = false;
        log_status("GAP BLE_GAP_EVENT_DISC_COMPLETE", event->disc_complete.reason);
        ESP_LOGI("REMOTE_STATUS", "scan finished: reports=%u connecting=%u; retry while remote absent",
                 report_count, connection_attempted);
        retry_scan();
        break;
    case BLE_GAP_EVENT_CONNECT: {
        connection_attempted = false;
        log_status("GAP BLE_GAP_EVENT_CONNECT", event->connect.status);
        if (event->connect.status != 0) {
            ESP_LOGW("REMOTE_STATUS", "connection failed: rc=%d; retry in 2s", event->connect.status);
            retry_scan();
            break;
        }
        remote_conn = event->connect.conn_handle;
        bond_store_failed = false;
        /* Issue security before detailed output or any application GATT procedure. */
        const int rc = ble_gap_security_initiate(event->connect.conn_handle);
        ESP_LOGI("REMOTE_STATUS", "CONNECTED: handle=%u", event->connect.conn_handle);
        log_status("security request (Central): ble_gap_security_initiate", rc);
        if (rc == 0) {
            ESP_LOGI(TAG, "authentication started (request accepted; pairing or stored-key encryption pending)");
        } else if (rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "security initiation failed");
            log_status("local disconnect request", ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM));
        }
        log_connection(event->connect.conn_handle, false);
        break;
    }
    /* PARING is the actual spelling in this ESP-IDF release's header. */
    case BLE_GAP_EVENT_PARING_COMPLETE:
        ESP_LOGI(TAG, "GAP BLE_GAP_EVENT_PARING_COMPLETE: pairing callback SMP_status=0x%02X; await ENC_CHANGE and bond storage",
                 event->pairing_complete.status);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        log_status("GAP BLE_GAP_EVENT_ENC_CHANGE", event->enc_change.status);
        log_connection(event->enc_change.conn_handle, event->enc_change.status == 0);
        if (event->enc_change.status != 0) {
            hid_client_stop(event->enc_change.conn_handle);
            ESP_LOGW("REMOTE_STATUS", "pairing/encryption failed; disconnect then retry");
            log_status("local disconnect request", ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM));
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        hid_client_stop(event->disconnect.conn.conn_handle);
        log_status("GAP BLE_GAP_EVENT_DISCONNECT", event->disconnect.reason);
        ESP_LOGI("REMOTE_STATUS", "DISCONNECTED: reason=%d (0x%X) encrypted=%u bonded=%u; retry in 2s",
                 event->disconnect.reason, (unsigned)event->disconnect.reason,
                 event->disconnect.conn.sec_state.encrypted, event->disconnect.conn.sec_state.bonded);
        remote_conn = BLE_HS_CONN_HANDLE_NONE;
        connection_attempted = false;
        retry_scan();
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        hid_client_on_notify(event);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ESP_LOGW(TAG, "GAP BLE_GAP_EVENT_REPEAT_PAIRING: retained existing bond; repeat request ignored");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGE(TAG, "GAP BLE_GAP_EVENT_PASSKEY_ACTION action=%u: unexpected for NoInputNoOutput",
                 event->passkey.params.action);
        log_status("local disconnect request", ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM));
        break;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
        const struct ble_gap_upd_params *p = event->conn_update_req.peer_params;
        ESP_LOGI(TAG, "GAP connection parameter request event=%u: min=%u max=%u latency=%u timeout=%u; accept stack defaults",
                 event->type, p->itvl_min, p->itvl_max, p->latency, p->supervision_timeout);
        break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE:
        log_status("GAP BLE_GAP_EVENT_CONN_UPDATE", event->conn_update.status);
        log_connection(event->conn_update.conn_handle, false);
        break;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        ESP_LOGI(TAG, "GAP BLE_GAP_EVENT_IDENTITY_RESOLVED");
        break;
    default:
        ESP_LOGI(TAG, "GAP event=%u (0x%X)", event->type, event->type);
        break;
    }
    return 0;
}

static void start_scan(struct ble_npl_event *event)
{
    (void)event;
    if (!host_synced || connection_attempted || remote_conn != BLE_HS_CONN_HANDLE_NONE) { return; }
    if (ble_gap_disc_active()) { return; }
    memset(candidates, 0, sizeof(candidates));
    next_candidate = report_count = 0;
    const struct ble_gap_disc_params params = {
        .itvl = SCAN_INTERVAL_UNITS, .window = SCAN_WINDOW_UNITS,
        .passive = 0, .filter_duplicates = 1, .limited = 0, .filter_policy = 0,
    };
    scan_started = true;
    int rc = ble_gap_disc(own_addr_type, SCAN_DURATION_MS, &params, gap_callback, NULL);
    log_status("ble_gap_disc", rc);
    if (rc == 0) {
        ESP_LOGI("REMOTE_STATUS", "scan started: 30s, retry=2s; wake remote anytime (known_peer=%u)", have_remote_identity);
    } else { scan_started = false; retry_scan(); }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    log_status("ble_hs_util_ensure_addr", rc);
    if (rc != 0) { return; }
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    log_status("ble_hs_id_infer_auto", rc);
    if (rc != 0) { return; }
    host_synced = true;
    scan_started = connection_attempted = false;
    remote_conn = BLE_HS_CONN_HANDLE_NONE;
    have_remote_identity = bridge_peer_load("remote", &remote_identity) && bridge_peer_bonded(&remote_identity, true);
    rc = mac_hid_advertise(own_addr_type);
    if (rc) { ESP_LOGW("MAC_HID", "advertising start failed: rc=%d; scheduled retry", rc); }
    start_scan(NULL);
}

static void on_reset(int reason)
{
    log_status("NimBLE host reset", reason);
    host_synced = false;
    ble_npl_callout_stop(&scan_retry);
    mac_hid_on_reset();
    if (remote_conn != BLE_HS_CONN_HANDLE_NONE) { hid_client_stop(remote_conn); }
    remote_conn = BLE_HS_CONN_HANDLE_NONE;
    scan_started = connection_attempted = false;
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("REMOTE_INPUT", ESP_LOG_INFO);
    esp_log_level_set("REMOTE_STATUS", ESP_LOG_INFO);
    esp_log_level_set("MAC_HID", ESP_LOG_INFO);
    ESP_LOGI("REMOTE_STATUS", "GUI bridge: scanning for remote; advertising host HID");
    ESP_LOGI("REMOTE_STATUS", "bridge revision=mode-keymap-1; independent normal/cursor mappings");
    if (!input_log_init()) { ESP_LOGE("REMOTE_STATUS", "input logger initialization failed"); return; }
    ESP_LOGI(TAG, "ESP-IDF=%s target=esp32; stage=Encrypted HID Input to Serial", esp_get_idf_version());
    if (!esp_ok("nvs_flash_init", nvs_flash_init()) || !esp_ok("nimble_port_init", nimble_port_init())) {
        ESP_LOGE(TAG, "initialization stopped; existing NVS retained");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    int mac_rc = mac_hid_init();
    if (mac_rc) { ESP_LOGE("MAC_HID", "service initialization failed: rc=%d", mac_rc); return; }
    int retry_rc = ble_npl_callout_init(&scan_retry, nimble_port_get_dflt_eventq(), start_scan, NULL);
    if (retry_rc) { ESP_LOGE("REMOTE_STATUS", "scan timer initialization failed: rc=%d", retry_rc); return; }
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_oob_data_flag = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    original_store_write = ble_hs_cfg.store_write_cb;
    if (!original_store_write) {
        ESP_LOGE(TAG, "bond storage initialization has no write callback; stopped");
        return;
    }
    ble_hs_cfg.store_write_cb = store_write_logged;
    ble_hs_cfg.store_status_cb = store_status;
    ESP_LOGI(TAG, "security config: Bonding=1 IO=NoInputNoOutput MITM=0 SC=0 SC_only=0 Legacy=1 NVS=1");
    nimble_port_freertos_init(host_task);
}
