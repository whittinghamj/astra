/*
 * For more information, visit https://cesbo.com
 * Copyright (C) 2012, Andrey Dyldin <and@cesbo.com>
 */

#include <astra.h>

#include <modules/mpegts/mpegts.h>

#include <fcntl.h>

#define BUFFER_SIZE (188 * 1000)

#define LOG_MSG(_msg) "[file_intput %s] " _msg, mod->config.filename

struct module_data_s
{
    MODULE_BASE();

    struct
    {
        const char *filename;
        const char *lock;
        // int loop;
    } config;

    thread_t *thread;
    stream_t *thread_stream;

    int fd;
    size_t skip;
    void *timer_skip;

    uint64_t pcr;

    struct
    {
        uint8_t data[BUFFER_SIZE];
        uint8_t *ptr;
        uint8_t *end;
        uint8_t *block_end;
    } buffer;
};

/* module code */

static inline int check_pcr(uint8_t *ts)
{
    return (   (ts[0] == 0x47) /* sync byte */
            && (TS_AF(ts) & 0x20) /* adaptation field */
            && (ts[4] > 0) /* adaptation field length */
            && (ts[5] & 0x10) /* PCR_flag */
            );
}

static inline uint64_t calc_pcr(uint8_t *ts)
{
    const uint64_t pcr_base = (ts[6] << 25)
                            | (ts[7] << 17)
                            | (ts[8] << 9 )
                            | (ts[9] << 1 )
                            | (ts[10] >> 7);
    const uint64_t pcr_ext = ((ts[10] & 1) << 8) | (ts[11]);
    return (pcr_base * 300 + pcr_ext);
}

static uint8_t * seek_pcr(uint8_t *buffer, uint8_t *buffer_end)
{
    buffer += TS_PACKET_SIZE;
    for(; buffer < buffer_end; buffer += TS_PACKET_SIZE)
    {
        if(check_pcr(buffer))
            return buffer;
    }

    return NULL;
}

static double time_per_block(uint8_t *block_end, uint64_t *last_pcr)
{
    uint64_t pcr = calc_pcr(block_end);

    const uint64_t dpcr = pcr - *last_pcr;
    *last_pcr = pcr;
    const uint64_t dpcr_base = dpcr / 300;
    const uint64_t dpcr_ext = dpcr % 300;

    const double dt = ((double)(dpcr_base / 90.0)     // 90 kHz
                    + (double)(dpcr_ext / 27000.0));  // 27 MHz

    if(dt < 1 || dt > 100)
        return -1;

    return dt; // ms
}

static double timeval_diff(struct timeval *start, struct timeval *end)
{
    const int64_t s_us = start->tv_sec * 1000000 + start->tv_usec;
    const int64_t e_us = end->tv_sec * 1000000 + end->tv_usec;
    return (e_us - s_us) / 1000; // ms
}

static int open_file(module_data_t *mod)
{
    if(mod->fd)
    {
        mod->skip = 0; // reopen file
        close(mod->fd);
    }

    mod->fd = open(mod->config.filename, O_RDONLY);
    if(mod->fd <= 0)
    {
        mod->fd = 0;
        return 0;
    }

    if(mod->skip)
        lseek(mod->fd, mod->skip, SEEK_SET);

    // sync file position
    uint8_t *data = mod->buffer.data;
    ssize_t len = read(mod->fd, data, TS_PACKET_SIZE);
    if(len <= 0)
    {
        close(mod->fd);
        mod->fd = 0;
        return 0;
    }

    int i = 0;
    for(; i < TS_PACKET_SIZE && mod->buffer.data[i] != 0x47; ++i)
        ;
    if(i == TS_PACKET_SIZE)
    {
        close(mod->fd);
        mod->fd = 0;
        return 0;
    }
    size_t buffer_size = TS_PACKET_SIZE - i;
    memcpy(&data[TS_PACKET_SIZE], &data[i], buffer_size);
    memcpy(data, &data[TS_PACKET_SIZE], buffer_size);
    len = read(mod->fd, &data[buffer_size], BUFFER_SIZE - buffer_size);

    // init first pcr
    mod->buffer.end = mod->buffer.data + buffer_size + len;
    mod->buffer.ptr = seek_pcr(mod->buffer.data, mod->buffer.end);
    if(!mod->buffer.ptr)
    {
        close(mod->fd);
        mod->fd = 0;
        return 0;
    }
    mod->pcr = calc_pcr(mod->buffer.ptr);
    mod->buffer.block_end = NULL;

    return 1;
}

static void thread_loop(void *arg)
{
    module_data_t *mod = arg;

    if(!open_file(mod))
        return;

    // block sync
    struct timeval time_sync[3];
    struct timeval *time_sync_b = &time_sync[0];
    struct timeval *time_sync_c = &time_sync[1];
    gettimeofday(time_sync_b, NULL);
    double block_time_total = 0;

    double ts_sync_accuracy = 0;
    struct timespec ts_sync = { .tv_sec = 0, .tv_nsec = 0 };

    while(thread_is_started(mod->thread))
    {
        mod->buffer.block_end = seek_pcr(mod->buffer.ptr, mod->buffer.end);
        if(!mod->buffer.block_end)
        {
            uint8_t *dst = mod->buffer.data;
            uint8_t *src = mod->buffer.ptr;
            uint8_t *end = mod->buffer.end;
            while(src < end)
            {
                memcpy(dst, src, TS_PACKET_SIZE);
                dst += TS_PACKET_SIZE;
                src += TS_PACKET_SIZE;
            }
            mod->buffer.ptr = mod->buffer.data;
            mod->buffer.end = dst;
            const size_t buffer_size = mod->buffer.end - mod->buffer.ptr;
            const size_t read_size = BUFFER_SIZE - buffer_size;
            const ssize_t rlen = read(mod->fd, mod->buffer.end, read_size);
            if(rlen != read_size)
            {
                // TODO: mod->config.loop
                if(!open_file(mod))
                    break;
                continue;
            }
            mod->skip += rlen;
            mod->buffer.end += rlen;
            mod->buffer.block_end = seek_pcr(dst, mod->buffer.end);
            if(!mod->buffer.block_end)
            {
                // TODO: mod->config.loop
                if(!open_file(mod))
                    break;
                continue;
            }
        }
        double block_time = time_per_block(mod->buffer.block_end, &mod->pcr);
        if(block_time == -1)
        {
            mod->buffer.ptr = mod->buffer.block_end;
            continue;
        }
        block_time_total += block_time;

        const uint32_t tscount = (mod->buffer.block_end - mod->buffer.ptr)
                               / TS_PACKET_SIZE;
        const double block_accuracy = block_time
                                    + (ts_sync_accuracy * tscount);
        ts_sync.tv_nsec = (block_accuracy * 1000000) / tscount;

        uint8_t *const ptr_end = mod->buffer.block_end;
        while(mod->buffer.ptr < ptr_end)
        {
            stream_send(mod->thread_stream, mod->buffer.ptr, TS_PACKET_SIZE);
            mod->buffer.ptr += TS_PACKET_SIZE;

            nanosleep(&ts_sync, NULL);
        }

        gettimeofday(time_sync_c, NULL);
        const double time_sync_diff = timeval_diff(time_sync_b, time_sync_c);

        ts_sync_accuracy = (block_time_total - time_sync_diff) / tscount;
    }

    close(mod->fd);
}

static void thread_callback(void *arg)
{
    module_data_t *mod = arg;

    uint8_t ts[TS_PACKET_SIZE];
    ssize_t len = stream_recv(mod->thread_stream, ts, sizeof(ts));
    if(len == sizeof(ts))
        stream_ts_send(mod, ts);
}

static void timer_skip_set(void *arg)
{
    module_data_t *mod = arg;
    char skip_str[64];
    int fd = open(mod->config.lock, O_CREAT | O_WRONLY | O_TRUNC
                  , S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if(fd > 0)
    {
        const int l = sprintf(skip_str, "%lu", mod->skip);
        write(fd, skip_str, l);
        close(fd);
    }
}

/* methods */

static int method_attach(module_data_t *mod)
{
    stream_ts_attach(mod);
    return 0;
}

static int method_detach(module_data_t *mod)
{
    stream_ts_detach(mod);
    return 0;
}

/* required */

static void module_initialize(module_data_t *mod)
{
    module_set_string(mod, "filename", 1, NULL, &mod->config.filename);
    module_set_string(mod, "lock", 0, NULL, &mod->config.lock);

    stream_ts_init(mod, NULL, NULL, NULL, NULL, NULL);

    if(mod->config.lock)
    {
        int fd = open(mod->config.lock, O_RDONLY);
        if(fd)
        {
            char skip_str[64];
            const int l = read(fd, skip_str, sizeof(skip_str));
            if(l > 0)
                mod->skip = strtoul(skip_str, NULL, 10);
            close(fd);
        }
        mod->timer_skip = timer_attach(2000, timer_skip_set, mod);
    }

    mod->thread_stream = stream_init(thread_callback, mod);
    thread_init(&mod->thread, thread_loop, mod);
}

static void module_destroy(module_data_t *mod)
{
    timer_detach(mod->timer_skip);
    thread_destroy(&mod->thread);
    stream_destroy(mod->thread_stream);

    stream_ts_destroy(mod);
}

MODULE_METHODS()
{
    METHOD(attach)
    METHOD(detach)
};

MODULE(file_input)