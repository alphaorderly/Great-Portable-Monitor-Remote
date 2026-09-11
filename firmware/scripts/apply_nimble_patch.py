"""Apply the narrowly scoped ESP-IDF 5.5.5 NimBLE identity fix, idempotently."""
from pathlib import Path
import sys

target = Path(sys.argv[1]) / "components/bt/host/nimble/nimble/nimble/host/src/ble_hs_resolv.c"
before = """            memcpy(g_ble_hs_resolv_list[i].rl_peer_rpa, addr, BLE_DEV_ADDR_LEN);
            g_ble_hs_resolv_list[i].rl_addr_type = addr_type;
            return rl;"""
after = """            memcpy(g_ble_hs_resolv_list[i].rl_peer_rpa, addr, BLE_DEV_ADDR_LEN);
            /* rl_addr_type belongs to the stored identity, not the RPA.
             * Replacing PUBLIC with RANDOM makes bonded LTK lookup fail. */
            return rl;"""
text = target.read_text()
if after in text:
    pass
elif text.count(before) == 1:
    target.write_text(text.replace(before, after, 1))
    print("Applied NimBLE RPA identity-type fix.")
else:
    sys.exit("NimBLE resolver changed; review patches/nimble-rpa-identity-type.patch before building.")
