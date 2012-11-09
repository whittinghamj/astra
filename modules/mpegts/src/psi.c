/*
 * For more information, visit https://cesbo.com
 * Copyright (C) 2012, Andrey Dyldin <and@cesbo.com>
 */

#include <astra.h>
#include "../mpegts.h"

mpegts_psi_t * mpegts_psi_init(mpegts_packet_type_t type, uint16_t pid)
{
    mpegts_psi_t *psi = calloc(1, sizeof(mpegts_psi_t));
    psi->type = type;
    psi->status = MPEGTS_ERROR_NOT_READY;
    psi->pid = pid;
    return psi;
}

void mpegts_psi_destroy(mpegts_psi_t *psi)
{
    if(!psi)
        return;

    free(psi);
}

uint32_t mpegts_psi_get_crc(mpegts_psi_t *psi)
{
    uint8_t *buffer = &psi->buffer[psi->buffer_size - CRC32_SIZE];
    const uint32_t crc = (buffer[0] << 24)
                       | (buffer[1] << 16)
                       | (buffer[2] << 8)
                       | (buffer[3]);
    return crc;
}

uint32_t mpegts_psi_calc_crc(mpegts_psi_t *psi)
{
    // TODO: SSE4.2
    const size_t size = psi->buffer_size - CRC32_SIZE;
    return crc32b(psi->buffer, size);
}

void mpegts_psi_mux(mpegts_psi_t *psi, uint8_t *ts
                    , void (*callback)(module_data_t *, mpegts_psi_t *)
                    , module_data_t *arg)
{
    const uint8_t cc = TS_CC(ts);
    uint8_t *payload = &ts[TS_HEADER_SIZE];

    const uint8_t af = TS_AF(ts);
    if(!(af & 0x10)) // skip packet without payload (CC not incremented)
        return;

    if(TS_PUSI(ts))
    {
        const uint8_t ptr_field = TS_PTR(ts);
        payload += 1; // skip pointer field
        if(ptr_field > 0)
        { // pointer field
            if(ptr_field >= TS_BODY_SIZE)
            {
                psi->buffer_skip = 0;
                return;
            }
            if(psi->buffer_skip > 0)
            {
                if(((psi->cc + 1) & 0x0f) != cc)
                { // discontinuity error
                    psi->buffer_skip = 0;
                    return;
                }
                memcpy(&psi->buffer[psi->buffer_skip], payload, ptr_field);
                if(psi->buffer_size != psi->buffer_skip + ptr_field)
                { // checking PSI length
                    psi->buffer_skip = 0;
                    return;
                }
                callback(arg, psi);
            }
            payload += ptr_field;
        }
        while(payload[0] != 0xff)
        {
            psi->buffer_size = 0;

            const size_t psi_buffer_size = PSI_SIZE(payload);
            if(psi_buffer_size <= 3 || psi_buffer_size > PSI_MAX_SIZE)
                break;

            const size_t cpy_len = (ts + TS_PACKET_SIZE) - payload;
            if(cpy_len > TS_BODY_SIZE)
                break;

            psi->buffer_size = psi_buffer_size;
            if(psi_buffer_size > cpy_len)
            {
                memcpy(psi->buffer, payload, cpy_len);
                psi->buffer_skip = cpy_len;
                break;
            }
            else
            {
                memcpy(psi->buffer, payload, psi_buffer_size);
                callback(arg, psi);
                payload += psi_buffer_size;
            }
        }
    }
    else
    { // !TS_PUSI(ts)
        if(!psi->buffer_skip)
            return;
        if(((psi->cc + 1) & 0x0f) != cc)
        { // discontinuity error
            psi->buffer_skip = 0;
            return;
        }
        const size_t remain = psi->buffer_size - psi->buffer_skip;
        if(remain <= TS_BODY_SIZE)
        {
            memcpy(&psi->buffer[psi->buffer_skip], payload, remain);
            callback(arg, psi);
        }
        else
        {
            memcpy(&psi->buffer[psi->buffer_skip], payload, TS_BODY_SIZE);
            psi->buffer_skip += TS_BODY_SIZE;
        }
    }
    psi->cc = cc;
} /* mpegts_psi_mux */

void mpegts_psi_demux(mpegts_psi_t *psi
                      , void (*callback)(module_data_t *, uint8_t *)
                      , module_data_t *arg)
{
    const size_t buffer_size = psi->buffer_size;
    if(!buffer_size)
        return;

    uint8_t *ts = psi->ts;

    ts[0] = 0x47;
    ts[1] = 0x40 /* PUSI */ | psi->pid >> 8;
    ts[2] = psi->pid & 0xff;
    ts[4] = 0x00;

    const uint8_t ts_3 = 0x10; /* payload without adaptation field */

    // 1 - pointer field
    size_t ts_skip = TS_HEADER_SIZE + 1;
    size_t ts_size = TS_BODY_SIZE - 1;
    size_t buffer_skip = 0;

    while(buffer_skip < buffer_size)
    {
        const size_t buffer_tail = buffer_size - buffer_skip;
        if(buffer_tail < ts_size)
        {
            ts_size = buffer_tail;
            const size_t ts_last_byte = ts_skip + ts_size;
            memset(&ts[ts_last_byte], 0xFF, TS_PACKET_SIZE - ts_last_byte);
        }

        memcpy(&ts[ts_skip], &psi->buffer[buffer_skip], ts_size);
        ts[3] = ts_3 | psi->cc;

        buffer_skip += ts_size;
        psi->cc = (psi->cc + 1) & 0x0F;

        callback(arg, ts);

        if(ts_skip == 5)
        {
            ts_skip = TS_HEADER_SIZE;
            ts_size = TS_BODY_SIZE;
            ts[1] &= ~0x40; /* turn off pusi bit */
        }
    }
} /* mpegts_packet_demux */