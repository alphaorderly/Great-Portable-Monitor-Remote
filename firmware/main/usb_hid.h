#pragma once

/* Called on the NimBLE task, except init before starting it. */
int usb_hid_init(void);
int usb_hid_start(void);
void usb_hid_on_reset(void);
