#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Called only on the NimBLE owner task. Payload excludes the Report ID.
 * False means not accepted. The input engine retains bounded pending input;
 * transport failures and host changes clear motion before retrying state. */
bool host_output_ready(uint8_t report_id);
bool host_output_send(uint8_t report_id, const uint8_t *data, uint8_t length);
/* Includes the active transfer; keep relative motion in the input accumulator
 * until the previous mouse report has completed. */
bool host_output_pending(uint8_t report_id);
void host_output_clear(void);
void host_output_discard_motion(void);
