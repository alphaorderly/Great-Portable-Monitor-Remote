"""Compile the installed NimBLE resolver function with a fake IRK match.

This isolates address bookkeeping after successful resolution; it does not test
AES or radio behavior. The original SDK function fails the PUBLIC identity case.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / ".tools/esp-idf/components/bt/host/nimble/nimble/nimble/host/src/ble_hs_resolv.c"
text = source.read_text()
start = text.index("struct ble_hs_resolv_entry *\nble_hs_resolv_rpa_addr(")
end = text.index("\n/**", start)
function = text[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define MYNEWT_VAL(x) MYNEWT_VAL_##x
#define MYNEWT_VAL_BLE_STATIC_TO_DYNAMIC 0
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS 3
#define BLE_DEV_ADDR_LEN 6
#define BLE_HS_DBG_ASSERT(x) assert(x)
struct ble_hs_resolv_entry { uint8_t rl_peer_irk[16], rl_peer_rpa[6], rl_addr_type; };
static struct ble_hs_resolv_entry g_ble_hs_resolv_list[4];
static struct { int rl_cnt; } g_ble_hs_resolv_data = {3};
static bool ble_hs_locked_by_cur_task(void) { return true; }
static bool ble_hs_is_rpa(uint8_t *a,uint8_t type) { return type==1 && (a[5]&0xc0)==0x40; }
/* Model a successful match against exactly one stored IRK. */
static int ble_hs_resolv_rpa(uint8_t *a,uint8_t *irk) { return a[0]==irk[0]?0:5; }
'''
suffix = r'''
int main(void) {
    g_ble_hs_resolv_list[1].rl_peer_irk[0]=42;
    g_ble_hs_resolv_list[1].rl_addr_type=0; /* Mac PUBLIC identity */
    g_ble_hs_resolv_list[2].rl_peer_irk[0]=43;
    g_ble_hs_resolv_list[2].rl_addr_type=1; /* Other RANDOM identity */
    uint8_t rpa[6]={42,2,3,4,5,0x40};
    struct ble_hs_resolv_entry *entry=ble_hs_resolv_rpa_addr(rpa,1);
    assert(entry==&g_ble_hs_resolv_list[1]);
    assert(entry->rl_addr_type==0); /* Original SDK overwrites this with 1. */
    assert(!memcmp(entry->rl_peer_rpa,rpa,6));
    rpa[1]=9; assert(ble_hs_resolv_rpa_addr(rpa,1)==entry);
    assert(entry->rl_addr_type==0 && entry->rl_peer_rpa[1]==9);
    rpa[0]=43; entry=ble_hs_resolv_rpa_addr(rpa,1);
    assert(entry==&g_ble_hs_resolv_list[2] && entry->rl_addr_type==1);
    rpa[0]=99; assert(!ble_hs_resolv_rpa_addr(rpa,1));
    assert(!ble_hs_resolv_rpa_addr(rpa,0));
    assert(g_ble_hs_resolv_list[1].rl_addr_type==0);
    puts("PASS: actual NimBLE RPA resolver preserves public/random identity type and updates only RPA cache");
}
'''
with tempfile.TemporaryDirectory(prefix="remote-rpa-test-") as tmp:
    c_file = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    c_file.write_text(prefix + function + suffix)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
