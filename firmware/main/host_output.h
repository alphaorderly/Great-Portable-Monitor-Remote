#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Called only on the NimBLE owner task. Payload excludes the Report ID.
 * False means not accepted; the input engine retries state, never stale motion. */
bool host_output_ready(uint8_t report_id);
bool host_output_send(uint8_t report_id, const uint8_t *data, uint8_t length);
void host_output_clear(void);
void host_output_discard_motion(void);
