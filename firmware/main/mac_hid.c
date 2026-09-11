/* Compatibility entry points used by the remote Central side. */
#include "hid_internal.h"
int mac_hid_init(void)
{
    int rc = hid_service_init();
    if (!rc) { rc = hid_input_init(); }
    return rc ? rc : host_link_init();
}
int mac_hid_advertise(uint8_t own_addr_type) { return host_link_advertise(own_addr_type); }
void mac_hid_on_reset(void) { host_link_reset(); }
