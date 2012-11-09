/*
 * For more information, visit https://cesbo.com
 * Copyright (C) 2012, Andrey Dyldin <and@cesbo.com>
 */

#include <astra.h>
#include "../mpegts.h"

mpegts_packet_type_t mpegts_pes_type(uint8_t t)
{
    switch(t)
    {
        case 0x01:  // MPEG-1 video
        case 0x02:  // MPEG-2 video
        case 0x80:  // MPEG-2 MOTO video
        case 0x10:  // MPEG-4 video
        case 0x1B:  // H264 video
        case 0xA0:  // MSCodec vlc video
            return MPEGTS_PACKET_VIDEO;
        case 0x03:  // MPEG-1 audio
        case 0x04:  // MPEG-2 audio
        case 0x11:  // MPEG-4 audio (LATM)
        case 0x0F:  // Audio with ADTS
        case 0x81:  // A52 audio
        case 0x83:  // LPCM audio
        case 0x84:  // SDDS audio
        case 0x85:  // DTS audio
        case 0x87:  // E-AC3
        case 0x91:  // A52 vls audio
        case 0x94:  // SDDS audio
            return MPEGTS_PACKET_AUDIO;
        case 0x82:  // DVB_SPU
        case 0x92:  // DVB_SPU vls
            return MPEGTS_PACKET_SUB;
        case 0x06:  // PES_PRIVATE
        case 0x12:  // MPEG-4 generic (sub/scene/...)
        case 0xEA:  // privately managed ES
        default:
            return MPEGTS_PACKET_DATA;
    }
}

const char * mpegts_pes_name(mpegts_packet_type_t t)
{
    switch(t)
    {
        case MPEGTS_PACKET_VIDEO:
            return "VIDEO";
        case MPEGTS_PACKET_AUDIO:
            return "AUDIO";
        case MPEGTS_PACKET_SUB:
            return "SUB";
        case MPEGTS_PACKET_DATA:
            return "DATA";
        default:
            return "UNKNOWN";
    }
}

mpegts_pes_t * mpegts_pes_init(mpegts_packet_type_t type, uint16_t pid)
{
    mpegts_pes_t *pes = malloc(sizeof(mpegts_pes_t));
    pes->type = type;
    pes->pid = pid;
    pes->cc = 0;
    pes->buffer_size = 0;
    pes->buffer_skip = 0;
    return pes;
}

void mpegts_pes_destroy(mpegts_pes_t *pes)
{
    if(!pes)
        return;

    free(pes);
}

void mpegts_pes_mux(mpegts_pes_t *pes, uint8_t *ts
                    , void (*callback)(module_data_t *, mpegts_pes_t *)
                    , module_data_t *arg)
{
    const uint8_t cc = TS_CC(ts);
    uint8_t *payload = &ts[TS_HEADER_SIZE];

    const uint8_t af = TS_AF(ts);
    if(!(af & 0x10)) // skip packet without payload (CC not incremented)
        return;

    if(af == 0x30)
        payload += (ts[4] + 1);

    if(TS_PUSI(ts))
    {
        pes->buffer_size = 0;

        // TODO: PES length is 0
        const size_t pes_buffer_size = PES_SIZE(payload);
        if(pes_buffer_size <= 6 || pes_buffer_size > PES_MAX_SIZE)
            return;

        const size_t cpy_len = (ts + TS_PACKET_SIZE) - payload;
        if(cpy_len > TS_BODY_SIZE)
            return;

        pes->buffer_size = pes_buffer_size;
        if(pes_buffer_size > cpy_len)
        {
            memcpy(pes->buffer, payload, cpy_len);
            pes->buffer_skip = cpy_len;
        }
        else
        {
            memcpy(pes->buffer, payload, pes_buffer_size);
            callback(arg, pes);
        }
    }
    else
    { // !TS_PUSI(ts)
        if(!pes->buffer_skip)
            return;
        if(((pes->cc + 1) & 0x0f) != cc)
        { // discontinuity error
            pes->buffer_skip = 0;
            return;
        }
        const size_t remain = pes->buffer_size - pes->buffer_skip;
        if(remain <= TS_BODY_SIZE)
        {
            memcpy(&pes->buffer[pes->buffer_skip], payload, remain);
            callback(arg, pes);
        }
        else
        {
            memcpy(&pes->buffer[pes->buffer_skip], payload, TS_BODY_SIZE);
            pes->buffer_skip += TS_BODY_SIZE;
        }
    }
    pes->cc = cc;
}

void mpegts_pes_demux(mpegts_pes_t *pes
                      , void (*callback)(module_data_t *, uint8_t *)
                      , module_data_t *arg)
{
    const size_t buffer_size = pes->buffer_size;
    if(!buffer_size)
        return;

    // PES length
    uint8_t *b = pes->buffer;
    const uint16_t pes_size = pes->buffer_size - 6;
    b[4] = (pes_size >> 8) & 0xFF;
    b[5] = (pes_size     ) & 0xFF;
    // TODO: PTS

    uint8_t *ts = pes->ts;

    ts[0] = 0x47;
    ts[1] = 0x40 /* PUSI */ | pes->pid >> 8;
    ts[2] = pes->pid & 0xff;

    const uint8_t ts_3_10 = 0x10; /* payload without adaptation field */
    const uint8_t ts_3_30 = 0x30; /* payload after adaptation field */

    size_t ts_skip = TS_HEADER_SIZE;
    size_t ts_size = TS_BODY_SIZE;
    size_t buffer_skip = 0;

    while(buffer_skip < buffer_size)
    {
        const size_t buffer_tail = buffer_size - buffer_skip;
        if(buffer_tail < ts_size)
        {
            const uint8_t af_size = ts_size - buffer_tail;
            ts[3] = ts_3_30 | pes->cc;
            ts[4] = af_size - 1;
            ts[5] = 0x00;

            // 2 - adaptation field size + adaptation descriptors
            memset(&ts[TS_HEADER_SIZE + 2], 0xFF, af_size - 2);
            ts_skip = TS_HEADER_SIZE + af_size;
            ts_size = buffer_tail;
        }
        else
            ts[3] = ts_3_10 | pes->cc;

        memcpy(&ts[ts_skip], &pes->buffer[buffer_skip], ts_size);

        buffer_skip += ts_size;
        pes->cc = (pes->cc + 1) & 0x0F;

        callback(arg, ts);

        if(TS_PUSI(ts))
            ts[1] &= ~0x40; /* turn off pusi bit */
    }
}

void mpegts_pes_add_data(mpegts_pes_t *pes, const uint8_t *data, size_t size)
{
    if(!pes->buffer_size)
    {
        uint8_t *b = pes->buffer;
        // PES header
        b[0] = 0x00;
        b[1] = 0x00;
        b[2] = 0x01;
        //
        b[3] = pes->stream_id;
        //
        b[6] = 0x00;
        b[7] = 0x00;
        // PES header length
        b[8] = 0;
        pes->buffer_size = 9;

        if(pes->pts)
        {
            b[7] |= 0x80;
            pes->buffer_size += 5;
        }
    }

    const size_t nsize = pes->buffer_size + size;
    if(nsize > PES_MAX_SIZE)
        return;

    memcpy(&pes->buffer[pes->buffer_size], data, size);
    pes->buffer_size = nsize;
}