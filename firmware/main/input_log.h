#pragma once
#include <stdbool.h>
#include <stdint.h>

bool input_log_init(void);
void input_log_submit(unsigned sequence, int id, uint16_t handle, unsigned length,
                      unsigned offset, bool indication, const uint8_t *data, unsigned count);
