/* Synchronous host sink: the firmware uses a FreeRTOS queue instead. */
void input_log_submit(unsigned sequence,int id,uint16_t handle,unsigned length,
                      unsigned offset,bool indication,const uint8_t *data,unsigned count)
{
    (void)data; (void)count;
    test_log("#%04u id=%d h=%04X len=%u off=%u ind=%u",sequence,id,handle,length,offset,indication);
}
