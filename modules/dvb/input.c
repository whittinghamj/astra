/*
 * For more information, visit https://cesbo.com
 * Copyright (C) 2012, Andrey Dyldin <and@cesbo.com>
 */

#if !defined(__linux)
#   error "DVB module for linux only"
#else

#include <astra.h>

#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <linux/dvb/version.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>

#if (DVB_API_VERSION < 5)
#   error "Please, update DVB drivers"
#endif

#define TS_PACKET_SIZE 188
#define MAX_PID 8192
#define LOG_MSG(_msg) "[dvb_input %d:%d] " _msg \
                      , mod->config.adapter, mod->config.device
#define STAT_INTERVAL (2 * 1000) // TODO: moved to thread (STATUS_INTERVAL)

// seconds
#define STATUS_INTERVAL 1
#define RETUNE_INTERVAL 5

#define BUFFER_SIZE (1022 * TS_PACKET_SIZE)

typedef enum
{
    DVB_TYPE_S,
    DVB_TYPE_S2,
    DVB_TYPE_T,
    DVB_TYPE_T2,
    DVB_TYPE_C,
} dvb_type_t;

typedef enum
{
    TP_POL_H, // H, L
    TP_POL_V, // V, R
} tp_pol_t;

typedef struct
{
    const char *name;
    int value;
} fe_value_t;

static fe_value_t dvb_type_list[] =
{
    { "S", DVB_TYPE_S },
    { "S2", DVB_TYPE_S2 },
    { "T", DVB_TYPE_T },
    { "T2", DVB_TYPE_T2 },
    { "C", DVB_TYPE_C },
};

static fe_value_t polarization_list[] =
{
    { "H", TP_POL_H },
    { "L", TP_POL_H },
    { "V", TP_POL_V },
    { "R", TP_POL_V },
};

static fe_value_t modulation_list[] =
{
    { "QPSK", QPSK },
    { "QAM16", QAM_16 },
    { "QAM32", QAM_32 },
    { "QAM64", QAM_64 },
    { "QAM128", QAM_128 },
    { "QAM256", QAM_256 },
    { "AUTO", QAM_AUTO },
    { "VSB8", VSB_8 },
    { "VSB16", VSB_16 },
#if DVB_API_VERSION >= 5
    { "PSK8", PSK_8 },
    { "APSK16", APSK_16 },
    { "APSK32", APSK_32 },
    { "DQPSK", DQPSK },
#endif
};

static fe_value_t fec_list[] =
{
    { "NONE", FEC_NONE },
    { "1/2", FEC_1_2 },
    { "2/3", FEC_2_3 },
    { "3/4", FEC_3_4 },
    { "4/5", FEC_4_5 },
    { "5/6", FEC_5_6 },
    { "6/7", FEC_6_7 },
    { "7/8", FEC_7_8 },
    { "8/9", FEC_8_9 },
    { "AUTO", FEC_AUTO },
#if DVB_API_VERSION >= 5
    { "3/5", FEC_3_5 },
    { "9/10", FEC_9_10 },
#endif
};

static fe_value_t rolloff_list[] =
{
    { "35", ROLLOFF_35 },
    { "20", ROLLOFF_20 },
    { "25", ROLLOFF_25 },
    { "AUTO", ROLLOFF_AUTO },
};

static fe_value_t bandwidth_list[] =
{
    { "8MHZ", BANDWIDTH_8_MHZ },
    { "7MHZ", BANDWIDTH_7_MHZ },
    { "6MHZ", BANDWIDTH_6_MHZ },
    { "AUTO", BANDWIDTH_AUTO }
};

static fe_value_t guardinterval_list[] =
{
    { "1/32", GUARD_INTERVAL_1_32 },
    { "1/16", GUARD_INTERVAL_1_16 },
    { "1/8", GUARD_INTERVAL_1_8 },
    { "1/4", GUARD_INTERVAL_1_4 },
    { "AUTO", GUARD_INTERVAL_AUTO },
};

static fe_value_t transmitmode_list[] =
{
    { "2K", TRANSMISSION_MODE_2K },
    { "8K", TRANSMISSION_MODE_8K },
    { "AUTO", TRANSMISSION_MODE_AUTO },
#if DVB_API_VERSION >= 5
    { "4K", TRANSMISSION_MODE_4K },
#endif
};

static fe_value_t hierarchy_list[] =
{
    { "NONE", HIERARCHY_NONE },
    { "1", HIERARCHY_1 },
    { "2", HIERARCHY_2 },
    { "4", HIERARCHY_4 },
    { "AUTO", HIERARCHY_AUTO }
};

enum
{
    MESSAGE_LOCK = 1,
    MESSAGE_ERROR = 2,
    MESSAGE_RETUNE = 3,
};

struct module_data_s
{
    MODULE_BASE();
    MODULE_EVENT_BASE();

    struct
    {
        dvb_type_t type;

        int adapter;
        int device;
        int diseqc;

        int budget;
        int buffer_size; // dvr ring buffer

        int frequency;
        tp_pol_t polarization;
        int symbolrate;

        int lof1;
        int lof2;
        int slof;
        int lnb_sharing;

        fe_modulation_t modulation;
        fe_code_rate_t fec;
        fe_rolloff_t rolloff;
        fe_bandwidth_t bandwidth;
        fe_guard_interval_t guardinterval;
        fe_transmit_mode_t transmitmode;
        fe_hierarchy_t hierarchy;
    } config;

    /* frontend */

    int fe_fd;

    thread_t *thread;
    stream_t *thread_stream;

    struct
    {
        const char *error_message;
        int error_code;

        fe_status_t fe;
        int lock;
        int signal;
        int snr;

        int ber;
        int unc;
    } status;
    int is_lock; // to send event
    int is_retune; // to demux bounce

    /* dvr */

    int dvr_fd;

    struct
    {
        char dev_name[32]; // demux dev name
        int fd_8192; // for budget mode, pid 8192
        int fd_list[MAX_PID];
    } demux;

    int join_list[MAX_PID];

    uint32_t bitrate;

    uint32_t stat_count;
    void *stat_timer;

    uint8_t buffer[BUFFER_SIZE];
};

/* frontend */

/*
 * ooooooooo   o88    oooooooo8 ooooooooooo              oooooooo8
 *  888    88o oooo  888         888    88   ooooooooo o888     88
 *  888    888  888   888oooooo  888ooo8   888    888  888
 *  888    888  888          888 888    oo 888    888  888o     oo
 * o888ooo88   o888o o88oooo888 o888ooo8888  88ooo888   888oooo88
 *                                                888o
 */

/* in thread function */
static int diseqc_setup(module_data_t *mod
                        , int hiband, int freq, int voltage, int tone)
{
    static struct timespec ns = { .tv_sec = 0, .tv_nsec = 15 * 1000 * 1000 };

    const int i = 4 * (mod->config.diseqc - 1)
                | 2 * hiband
                | ((voltage == SEC_VOLTAGE_18) ? 1 : 0);

    if(ioctl(mod->fe_fd, FE_SET_TONE, SEC_TONE_OFF) != 0)
    {
        mod->status.error_message = "FE_SET_TONE";
        return 0;
    }

    if(ioctl(mod->fe_fd, FE_SET_VOLTAGE, voltage) != 0)
    {
        mod->status.error_message = "FE_SET_VOLTAGE";
        return 0;
    }

    nanosleep(&ns, NULL);

    struct dvb_diseqc_master_cmd cmd =
    {
        .msg_len = 4,
        .msg = { 0xE0, 0x10, 0x38, 0xF0 | i, 0x00, 0x00 }
    };

    if(ioctl(mod->fe_fd, FE_DISEQC_SEND_MASTER_CMD, &cmd) != 0)
    {
        mod->status.error_message = "FE_DISEQC_SEND_MASTER_CMD";
        return 0;
    }

    nanosleep(&ns, NULL);

    fe_sec_mini_cmd_t burst = ((i / 4) % 2) ? SEC_MINI_B : SEC_MINI_A;
    if(ioctl(mod->fe_fd, FE_DISEQC_SEND_BURST, burst) != 0)
    {
        mod->status.error_message = "FE_DISEQC_SEND_BURST";
        return 0;
    }

    nanosleep(&ns, NULL);

    if(ioctl(mod->fe_fd, FE_SET_TONE, tone) != 0)
    {
        mod->status.error_message = "FE_SET_TONE";
        return 0;
    }

    return 1;
} /* diseqc_setup */


/*
 * ooooooooo  ooooo  oooo oooooooooo           oooooooo8
 *  888    88o 888    88   888    888         888
 *  888    888  888  88    888oooo88 ooooooooo 888oooooo
 *  888    888   88888     888    888                 888
 * o888ooo88      888     o888ooo888          o88oooo888
 *
 */

static int frontend_tune_s(module_data_t *mod)
{
    int freq = mod->config.frequency;

    int hiband = (mod->config.slof && mod->config.lof2
                  && freq > mod->config.slof);
    if(hiband)
        freq = freq - mod->config.lof2;
    else
    {
        if(freq < mod->config.lof1)
            freq = mod->config.lof1 - freq;
        else
            freq = freq - mod->config.lof1;
    }

    int voltage = SEC_VOLTAGE_OFF;
    int tone = SEC_TONE_OFF;
    if(!mod->config.lnb_sharing)
    {
        voltage = (mod->config.polarization == TP_POL_V)
                ? SEC_VOLTAGE_13 : SEC_VOLTAGE_18;
        if(hiband)
            tone = SEC_TONE_ON;
    }

    if(mod->config.diseqc && !diseqc_setup(mod, hiband, freq, voltage, tone))
        return 0;

    if(mod->config.type == DVB_TYPE_S)
    {
        struct dvb_frontend_parameters feparams;

        memset(&feparams, 0, sizeof(feparams));
        feparams.frequency = freq;
        feparams.inversion = INVERSION_AUTO;
        feparams.u.qpsk.symbol_rate = mod->config.symbolrate;
        feparams.u.qpsk.fec_inner = mod->config.fec;

        if(!mod->config.diseqc)
        {
            if(ioctl(mod->fe_fd, FE_SET_TONE, tone) != 0)
            {
                mod->status.error_message = "FE_SET_TONE";
                return 0;
            }

            if(ioctl(mod->fe_fd, FE_SET_VOLTAGE, voltage) != 0)
            {
                mod->status.error_message = "FE_SET_VOLTAGE";
                return 0;
            }
        }

        if(ioctl(mod->fe_fd, FE_SET_FRONTEND, &feparams) != 0)
        {
            mod->status.error_message = "FE_SET_FRONTEND";
            return 0;
        }
    }
    else
    {
#if DVB_API_VERSION >= 5
        /* clear */
        struct dtv_properties cmdseq;

        struct dtv_property cmd_clear[] =
        {
            { .cmd = DTV_CLEAR }
        };
        cmdseq.num = 1;
        cmdseq.props = cmd_clear;

        if(ioctl(mod->fe_fd, FE_SET_PROPERTY, &cmdseq) != 0)
        {
            mod->status.error_message = "FE_SET_PROPERTY clear";
            return 0;
        }

        /* tune */
        struct dtv_property cmd_tune[] =
        {
            { .cmd = DTV_DELIVERY_SYSTEM,   .u.data = SYS_DVBS2             },
            { .cmd = DTV_FREQUENCY,         .u.data = freq                  },
            { .cmd = DTV_SYMBOL_RATE,       .u.data = mod->config.symbolrate},
            { .cmd = DTV_INNER_FEC,         .u.data = mod->config.fec       },
            { .cmd = DTV_INVERSION,         .u.data = INVERSION_AUTO        },
            { .cmd = DTV_VOLTAGE,           .u.data = voltage               },
            { .cmd = DTV_MODULATION,        .u.data = mod->config.modulation},
            { .cmd = DTV_ROLLOFF,           .u.data = mod->config.rolloff   },
            { .cmd = DTV_TONE,              .u.data = tone                  },
            { .cmd = DTV_TUNE                                               }
        };
        cmdseq.num = ARRAY_SIZE(cmd_tune);
        cmdseq.props = cmd_tune;

        struct dvb_frontend_event ev;
        while(ioctl(mod->fe_fd, FE_GET_EVENT, &ev) != -1)
            ;

        if(ioctl(mod->fe_fd, FE_SET_PROPERTY, &cmdseq) != 0)
        {
            mod->status.error_message = "FE_SET_PROPERTY tune";
            return 0;
        }
#else
#       warning "DVB-S2 is disabled. Update driver"
        return 0;
#endif
    }

    return 1;
}

/*
 * ooooooooo  ooooo  oooo oooooooooo       ooooooooooo
 *  888    88o 888    88   888    888      88  888  88
 *  888    888  888  88    888oooo88 ooooooooo 888
 *  888    888   88888     888    888          888
 * o888ooo88      888     o888ooo888          o888o
 *
 */

static int frontend_tune_t(module_data_t *mod)
{
    if(mod->config.type == DVB_TYPE_T)
    {
        struct dvb_frontend_parameters feparams;

        memset(&feparams, 0, sizeof(feparams));
        feparams.frequency = mod->config.frequency;
        feparams.inversion = INVERSION_AUTO;

        feparams.u.ofdm.code_rate_HP = FEC_AUTO;
        feparams.u.ofdm.code_rate_LP = FEC_AUTO;

        feparams.u.ofdm.bandwidth = mod->config.bandwidth;
        feparams.u.ofdm.constellation = mod->config.modulation;
        feparams.u.ofdm.transmission_mode = mod->config.transmitmode;
        feparams.u.ofdm.guard_interval = mod->config.guardinterval;
        feparams.u.ofdm.hierarchy_information = mod->config.hierarchy;

        if(ioctl(mod->fe_fd, FE_SET_FRONTEND, &feparams) != 0)
        {
            mod->status.error_message = "FE_SET_FRONTEND";
            return 0;
        }
    }
    else
    {
#if DVB_API_VERSION >= 5
        /* clear */
        struct dtv_properties cmdseq;

        struct dtv_property cmd_clear[] =
        {
            { .cmd = DTV_CLEAR }
        };
        cmdseq.num = 1;
        cmdseq.props = cmd_clear;

        if(ioctl(mod->fe_fd, FE_SET_PROPERTY, &cmdseq) != 0)
        {
            mod->status.error_message = "FE_SET_PROPERTY clear";
            return 0;
        }

        int bandwidth = 8000000;
        switch(mod->config.bandwidth)
        {
            case BANDWIDTH_7_MHZ:
                bandwidth = 7000000;
                break;
            case BANDWIDTH_6_MHZ:
                bandwidth = 6000000;
                break;
            default:
                break;
        }

        /* tune */
        struct dtv_property cmd_tune[] =
        {
            { .cmd = DTV_FREQUENCY,         .u.data = mod->config.frequency },
            { .cmd = DTV_MODULATION,        .u.data = mod->config.modulation},
            { .cmd = DTV_INVERSION,         .u.data = INVERSION_AUTO        },
            { .cmd = DTV_BANDWIDTH_HZ,      .u.data = bandwidth             },
            { .cmd = DTV_CODE_RATE_HP,      .u.data = FEC_AUTO              },
            { .cmd = DTV_CODE_RATE_LP,      .u.data = FEC_AUTO              },
            { .cmd = DTV_GUARD_INTERVAL
              ,                         .u.data = mod->config.guardinterval },
            { .cmd = DTV_TRANSMISSION_MODE
              ,                          .u.data = mod->config.transmitmode },
            { .cmd = DTV_HIERARCHY,         .u.data = mod->config.hierarchy },
            { .cmd = DTV_DELIVERY_SYSTEM,   .u.data = SYS_DVBT              },
            { .cmd = DTV_TUNE                                               },
        };
        cmdseq.num = ARRAY_SIZE(cmd_tune);
        cmdseq.props = cmd_tune;

        struct dvb_frontend_event ev;
        while(ioctl(mod->fe_fd, FE_GET_EVENT, &ev) != -1)
            ;

        if(ioctl(mod->fe_fd, FE_SET_PROPERTY, &cmdseq) != 0)
        {
            mod->status.error_message = "FE_SET_PROPERTY tune";
            return 0;
        }
#else
#       warning "DVB-T2 is disabled. Update driver"
        return 0;
#endif
    }

    return 1;
} /* frontend_tune_t */

/*
 * ooooooooo  ooooo  oooo oooooooooo             oooooooo8
 *  888    88o 888    88   888    888          o888     88
 *  888    888  888  88    888oooo88 ooooooooo 888
 *  888    888   88888     888    888          888o     oo
 * o888ooo88      888     o888ooo888            888oooo88
 *
 */

static int frontend_tune_c(module_data_t *mod)
{
    struct dvb_frontend_parameters feparams;

    memset(&feparams, 0, sizeof(feparams));
    feparams.frequency = mod->config.frequency;
    feparams.inversion = INVERSION_AUTO;
    feparams.u.qam.symbol_rate = mod->config.symbolrate;
    feparams.u.qam.modulation = mod->config.modulation;
    feparams.u.qam.fec_inner = mod->config.fec;

    if(ioctl(mod->fe_fd, FE_SET_FRONTEND, &feparams) != 0)
    {
        mod->status.error_message = "FE_SET_FRONTEND";
        return 0;
    }

    return 1;
} /* frontend_tune_c */

/*
 * ooooooooooo ooooo  oooo oooo   oooo ooooooooooo
 * 88  888  88  888    88   8888o  88   888    88
 *     888      888    88   88 888o88   888ooo8
 *     888      888    88   88   8888   888    oo
 *    o888o      888oo88   o88o    88  o888ooo8888
 *
 */

/* in thread function */
static void frontend_tune(module_data_t *mod)
{
    int ret = 0;
    do
    {
        switch(mod->config.type)
        {
            case DVB_TYPE_S:
            case DVB_TYPE_S2:
                ret = frontend_tune_s(mod);
                break;
            case DVB_TYPE_T:
            case DVB_TYPE_T2:
                ret = frontend_tune_t(mod);
                break;
            case DVB_TYPE_C:
                ret = frontend_tune_c(mod);
                break;
            default:
                mod->status.error_message = "unknown dvb type";
                break;
        }
    } while(0);

    if(!ret)
    {
        mod->status.error_code = errno;
        static const int message = MESSAGE_ERROR;
        stream_send(mod->thread_stream, (void *)&message, sizeof(message));
    }
} /* frontend_tune */

/*
 *  oooooooo8 ooooooooooo   o   ooooooooooo ooooo  oooo oooooooo8
 * 888        88  888  88  888  88  888  88  888    88 888
 *  888oooooo     888     8  88     888      888    88  888oooooo
 *         888    888    8oooo88    888      888    88         888
 * o88oooo888    o888o o88o  o888o o888o      888oo88  o88oooo888
 *
 */

/* in thread function */
static void frontend_status(module_data_t *mod)
{
    if(ioctl(mod->fe_fd, FE_READ_STATUS, &mod->status.fe) != 0)
    {
        mod->status.error_message = "FE_READ_STATUS";
        mod->status.error_code = errno;
        static const int message = MESSAGE_ERROR;
        stream_send(mod->thread_stream, (void *)&message, sizeof(message));
        return;
    }

    uint16_t snr, signal;
    if(ioctl(mod->fe_fd, FE_READ_SIGNAL_STRENGTH, &signal) != 0)
        signal = -2;
    if(ioctl(mod->fe_fd, FE_READ_SNR, &snr) == -1)
        snr = -2;
    if(ioctl(mod->fe_fd, FE_READ_BER, &mod->status.ber) == -1)
        mod->status.ber = -2;
    if(ioctl(mod->fe_fd, FE_READ_UNCORRECTED_BLOCKS, &mod->status.unc) == -1)
        mod->status.unc = -2;

    mod->status.lock = ((mod->status.fe & FE_HAS_LOCK) != 0);
    mod->status.signal = (signal * 100) / 0xffff;
    mod->status.snr = (snr * 100) / 0xffff;

    // send message to main thread
    if(!mod->status.lock)
    {
        // retune
        frontend_tune(mod);
        static const int message = MESSAGE_RETUNE;
        stream_send(mod->thread_stream, (void *)&message, sizeof(message));
        sleep(RETUNE_INTERVAL - STATUS_INTERVAL);
    }
    else
    {
        if(!mod->is_lock)
        {
            static const int message = MESSAGE_LOCK;
            stream_send(mod->thread_stream, (void *)&message, sizeof(message));
        }
    }
} /* frontend status */

/*
 * ooooooooooo ooooo ooooo oooooooooo  ooooooooooo      o      ooooooooo
 * 88  888  88  888   888   888    888  888    88      888      888    88o
 *     888      888ooo888   888oooo88   888ooo8       8  88     888    888
 *     888      888   888   888  88o    888    oo    8oooo88    888    888
 *    o888o    o888o o888o o888o  88o8 o888ooo8888 o88o  o888o o888ooo88
 *
 */

static void frontend_thread(void *arg)
{
    module_data_t *mod = arg;

    frontend_tune(mod);
    while(thread_is_started(mod->thread))
    {
        sleep(STATUS_INTERVAL);
        frontend_status(mod);
    }
} /* frontend_thread */

static void demux_bounce(module_data_t *);

static void frontend_thread_stream(void *arg)
{
    /* messages from thread */

    module_data_t *mod = arg;
    int message = 0;
    stream_recv(mod->thread_stream, &message, sizeof(message));

    switch(message)
    {
        case MESSAGE_LOCK:
        {
            log_info(LOG_MSG("lock. signal:%3u%% snr:%3u%%")
                     , mod->status.signal, mod->status.snr);
            if(!mod->is_lock)
            {
                if(mod->is_retune)
                {
                    mod->is_retune = 0;
                    demux_bounce(mod);
                }
                mod->is_lock = 1;
                module_event_call(mod);
            }
            break;
        }
        case MESSAGE_ERROR:
        {
            log_error(LOG_MSG("%s failed [%s]")
                      , mod->status.error_message
                      , strerror(mod->status.error_code));
            mod->is_lock = 0;
            break;
        }
        case MESSAGE_RETUNE:
        {
            const char ss = (mod->status.fe & FE_HAS_SIGNAL) ? 'S' : '_';
            const char sc = (mod->status.fe & FE_HAS_CARRIER) ? 'C' : '_';
            const char sv = (mod->status.fe & FE_HAS_VITERBI) ? 'V' : '_';
            const char sy = (mod->status.fe & FE_HAS_SYNC) ? 'Y' : '_';
            const char sl = (mod->status.fe & FE_HAS_LOCK) ? 'L' : '_';
            log_info(LOG_MSG("retune. status:%c%c%c%c%c "
                             "signal:%3u%% snr:%3u%%")
                     , ss, sc, sv, sy, sl
                     , mod->status.signal, mod->status.snr);
            if(mod->is_lock)
            {
                mod->is_retune = 1;
                mod->is_lock = 0;
                module_event_call(mod);
            }
            break;
        }
        default:
            break;
    }
}

/*
 * ooooooooooo ooooooooooo
 *  888    88   888    88
 *  888ooo8     888ooo8
 *  888         888    oo
 * o888o       o888ooo8888
 *
 */

static int frontend_open(module_data_t *mod)
{
    char dev_name[32];
    sprintf(dev_name, "/dev/dvb/adapter%d/frontend%d"
            , mod->config.adapter, mod->config.device);

    mod->fe_fd = open(dev_name, O_RDWR | O_NONBLOCK);
    if(mod->fe_fd <= 0)
    {
        log_error(LOG_MSG("failed to open frontend %s [%s]")
                  , dev_name, strerror(errno));
        return 0;
    }

    /* check dvb */
    struct dvb_frontend_info feinfo;
    if(ioctl(mod->fe_fd, FE_GET_INFO, &feinfo) != 0)
        return 0;

    switch(feinfo.type)
    {
        case FE_QPSK:   // DVB-S
        {
            if(mod->config.type == DVB_TYPE_S2
               && !(feinfo.caps & FE_CAN_2G_MODULATION))
            {
                log_error(LOG_MSG("S2 not suppoerted by DVB card"));
                return 0;
            }
            break;
        }
        case FE_OFDM:   // DVB-T
            break;
        case FE_QAM:    // DVB-C
            break;
        default:
            log_error(LOG_MSG("unknown card type"));
            return 0;
    }

    mod->thread_stream = stream_init(frontend_thread_stream, mod);
    thread_init(&mod->thread, frontend_thread, mod);

    return 1;
} /* frontend_open */

static void frontend_close(module_data_t *mod)
{
    thread_destroy(&mod->thread);
    stream_destroy(mod->thread_stream);

    if(mod->fe_fd > 0)
        close(mod->fe_fd);
} /* frontend_close */

/*
 * ooooooooo  ooooo  oooo oooooooooo
 *  888    88o 888    88   888    888
 *  888    888  888  88    888oooo88
 *  888    888   88888     888  88o
 * o888ooo88      888     o888o  88o8
 *
 */

static int dvr_open(module_data_t *);

static void dvr_close(module_data_t *mod)
{
    mod->bitrate = 0;
    mod->stat_count = 0;

    if(mod->dvr_fd > 0)
    {
        event_detach(mod->dvr_fd);
        close(mod->dvr_fd);
        mod->dvr_fd = 0;
    }
}

static void dvr_read_callback(void *arg, int event)
{
    module_data_t *mod = arg;

    if(event == EVENT_ERROR)
    {
        log_warning(LOG_MSG("read error [%s]. try to reopen")
                    , strerror(errno));
        dvr_close(mod);
        dvr_open(mod);
        return;
    }

    const ssize_t len = read(mod->dvr_fd, mod->buffer, BUFFER_SIZE);
    if(len <= 0)
    {
        dvr_read_callback(arg, EVENT_ERROR);
        return;
    }

    mod->stat_count += len;

    for(int i = 0; i < len; i += TS_PACKET_SIZE)
        stream_ts_send(mod, &mod->buffer[i]);
}

static void stat_timer_callback(void *arg)
{
    module_data_t *mod = arg;
    if(!mod->stat_count)
    {
        if(mod->bitrate)
        {
            mod->bitrate = 0;
            log_warning(LOG_MSG("bitrate: 0Kbit/s"));
        }
    }
    else
    {
        const int is_bitrate = (mod->bitrate > 1);
        mod->bitrate = (mod->stat_count * 8 / 1024) / (STAT_INTERVAL / 1000);
        mod->stat_count = 0;
        if(mod->bitrate)
        {
            if(!is_bitrate)
            {
                log_info(LOG_MSG("bitrate: %dKbit/s"), mod->bitrate);
            }
        }
    }
}

static void dvr_reopen_callback(void *arg)
{
    module_data_t *mod = arg;
    dvr_open(mod);
}

static int dvr_open(module_data_t *mod)
{
    char dev_name[32];
    sprintf(dev_name, "/dev/dvb/adapter%d/dvr%d"
            , mod->config.adapter, mod->config.device);
    mod->dvr_fd = open(dev_name, O_RDONLY | O_NONBLOCK);
    if(mod->dvr_fd <= 0)
    {
        log_error(LOG_MSG("failed to open dvr %s [%s]")
                  , dev_name, strerror(errno));
        if(mod->demux.dev_name[0]) // if not first time opening
        {
            log_warning(LOG_MSG("try to reopen after 5s"));
            timer_one_shot(5000, dvr_reopen_callback, mod);
        }
        return 0;
    }

    if(mod->config.buffer_size > 0)
    {
        const uint64_t buffer_size = mod->config.buffer_size * 4096;
        if(ioctl(mod->dvr_fd, DMX_SET_BUFFER_SIZE, buffer_size) < 0)
        {
            log_warning(LOG_MSG("failed to set dvr ring buffer [%s]")
                        , strerror(errno));
        }
    }

    event_attach(mod->dvr_fd, dvr_read_callback, mod, EVENT_READ);

    return 1;
}

/*
 * ooooooooo  ooooooooooo oooo     oooo ooooo  oooo ooooo  oooo
 *  888    88o 888    88   8888o   888   888    88    888  88
 *  888    888 888ooo8     88 888o8 88   888    88      888
 *  888    888 888    oo   88  888  88   888    88     88 888
 * o888ooo88  o888ooo8888 o88o  8  o88o   888oo88   o88o  o888o
 *
 */

static void demux_join_pid(module_data_t *mod, uint16_t pid)
{
    if(mod->config.budget)
    {
        if(pid != MAX_PID || mod->demux.fd_8192 > 0)
            return;
    }
    else
    {
        if(pid >= MAX_PID)
            return;
        ++mod->join_list[pid];
        if(mod->demux.fd_list[pid] > 0)
            return;
    }

    if(mod->demux.dev_name[0] == 0) // not yet initialized
        return;

    int fd = open(mod->demux.dev_name, O_WRONLY);
    if(fd <= 0)
    {
        log_error(LOG_MSG("failed to open demux %s [%s]")
                  , mod->demux.dev_name, strerror(errno));
        return;
    }

    struct dmx_pes_filter_params pes_filter;
    memset(&pes_filter, 0, sizeof(pes_filter));
    pes_filter.pid = pid;
    pes_filter.input = DMX_IN_FRONTEND;
    pes_filter.output = DMX_OUT_TS_TAP;
    pes_filter.pes_type = DMX_PES_OTHER;
    pes_filter.flags = DMX_IMMEDIATE_START;

    if(ioctl(fd, DMX_SET_PES_FILTER, &pes_filter) < 0)
    {
        log_error(LOG_MSG("failed to set PES filter [%s]")
                  , strerror(errno));
        close(fd);
        return;
    }

    if(pid < MAX_PID)
        mod->demux.fd_list[pid] = fd;
    else
        mod->demux.fd_8192 = fd;
} // demux_join_pid

static void demux_leave_pid(module_data_t *mod, uint16_t pid)
{
    if(mod->config.budget)
    {
        if(pid == MAX_PID && mod->demux.fd_8192 > 0)
        {
            close(mod->demux.fd_8192);
            mod->demux.fd_8192 = 0;
        }
    }
    else
    {
        --mod->join_list[pid];
        if(pid < MAX_PID
           && mod->demux.fd_list[pid] > 0
           && mod->join_list[pid] <= 0)
        {
            close(mod->demux.fd_list[pid]);
            mod->demux.fd_list[pid] = 0;
            mod->join_list[pid] = 0;
        }
    }
} // demux_leave_pid

static void demux_bounce(module_data_t *mod)
{
    if(mod->config.budget)
    {
        if(mod->demux.fd_8192 > 0)
        {
            ioctl(mod->demux.fd_8192, DMX_STOP);
            ioctl(mod->demux.fd_8192, DMX_START);
        }
    }
    else
    {
        for(int i = 0; i < MAX_PID; ++i)
        {
            const int fd = mod->demux.fd_list[i];
            if(fd <= 0)
                continue;
            ioctl(fd, DMX_STOP);
            ioctl(fd, DMX_START);
        }
    }
} /* demux_bounce */

/*
 * oooo     oooo  ooooooo  ooooooooo  ooooo  oooo ooooo       ooooooooooo
 *  8888o   888 o888   888o 888    88o 888    88   888         888    88
 *  88 888o8 88 888     888 888    888 888    88   888         888ooo8
 *  88  888  88 888o   o888 888    888 888    88   888      o  888    oo
 * o88o  8  o88o  88ooo88  o888ooo88    888oo88   o888ooooo88 o888ooo8888
 *
 */

static void callback_join_pid(module_data_t *mod
                              , module_data_t *child
                              , uint16_t pid)
{
    demux_join_pid(mod, pid);
}

static void callback_leave_pid(module_data_t *mod
                               , module_data_t *child
                               , uint16_t pid)
{
    demux_leave_pid(mod, pid);
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

static int method_status(module_data_t *mod)
{
    lua_State *L = LUA_STATE(mod);

    lua_newtable(L);
    lua_pushnumber(L, mod->status.fe);
    lua_setfield(L, -2, "status");
    lua_pushboolean(L, mod->status.lock);
    lua_setfield(L, -2, "lock");
    lua_pushnumber(L, mod->status.signal);
    lua_setfield(L, -2, "signal");
    lua_pushnumber(L, mod->status.snr);
    lua_setfield(L, -2, "snr");
    lua_pushnumber(L, mod->status.ber);
    lua_setfield(L, -2, "ber");
    lua_pushnumber(L, mod->status.unc);
    lua_setfield(L, -2, "unc");
    lua_pushnumber(L, mod->bitrate);
    lua_setfield(L, -2, "bitrate");

    return 1;
}

static int method_event(module_data_t *mod)
{
    module_event_set(mod);
    return 0;
}

/* public */

static void fe_option_set(module_data_t *mod, const char *name
                          , int is_required, const char *def
                          , fe_value_t *list, const int list_size
                          , int *value)
{
    const char *tmp_value = NULL;
    module_set_string(mod, name, is_required, def, &tmp_value);

    for(int i = 0; i < list_size; ++i)
    {
        if(!strcasecmp(list[i].name, tmp_value))
        {
            *value = list[i].value;
            return;
        }
    }

    log_error(LOG_MSG("unknown %s type \"%s\""), name, tmp_value);
    abort();
}

static void module_configure(module_data_t *mod)
{
    const char *tmp_value = NULL;

    fe_option_set(mod, "type", 1, NULL
                  , dvb_type_list, ARRAY_SIZE(dvb_type_list)
                  , (int *)&mod->config.type);

    module_set_number(mod, "adapter", 1, 0, &mod->config.adapter);
    module_set_number(mod, "device", 0, 0, &mod->config.device);

    module_set_number(mod, "budget", 0, 0, &mod->config.budget);
    module_set_number(mod, "buffer_size", 0, 0, &mod->config.buffer_size);

    fe_option_set(mod, "modulation", 0, "AUTO"
                  , modulation_list, ARRAY_SIZE(modulation_list)
                  , (int *)&mod->config.modulation);

    if(mod->config.type == DVB_TYPE_S || mod->config.type == DVB_TYPE_S2)
    {
        if(module_set_string(mod, "tp", 0, NULL, &tmp_value))
        {
            do
            {
                const char *conf = tmp_value;
                // set frequency
                mod->config.frequency = atoi(conf) * 1000;
                for(; *conf && *conf != ':'; ++conf)
                    ;
                if(!*conf)
                    break;
                ++conf;
                // set polarization
                char pol = *conf;
                if(pol > 'Z')
                    pol -= ('z' - 'Z'); // Upper Case
                // V - vertical/right // H - horizontal/left
                mod->config.polarization = (pol == 'V' || pol == 'R')
                                         ? TP_POL_V
                                         : TP_POL_H;
                for(; *conf && *conf != ':'; ++conf)
                    ;
                if(!*conf)
                    break;
                ++conf;
                // set symbol-rate
                mod->config.symbolrate = atoi(conf) * 1000;
            } while(0);
            if(!mod->config.symbolrate)
            {
                log_error(LOG_MSG("failed to parse tp option \"%s\"")
                          , tmp_value);
                abort();
            }
        }
        else
        {
            module_set_number(mod, "frequency", 1, 0, &mod->config.frequency);
            mod->config.frequency *= 1000;
            fe_option_set(mod, "polarization", 1, NULL
                          , polarization_list, ARRAY_SIZE(polarization_list)
                          , (int *)&mod->config.polarization);
            module_set_number(mod, "symbolrate", 1, 0
                              , &mod->config.symbolrate);
            mod->config.symbolrate *= 1000;
        }

        if(module_set_string(mod, "lnb", 0, NULL, &tmp_value))
        {
            do
            {
                const char *conf = tmp_value;
                // set lof1 (low)
                mod->config.lof1 = atoi(conf) * 1000;
                for(; *conf && *conf != ':'; ++conf)
                    ;
                if(!*conf)
                    break;
                ++conf;
                // set lof2 (high)
                mod->config.lof2 = atoi(conf) * 1000;
                for(; *conf && *conf != ':'; ++conf)
                    ;
                if(!*conf)
                    break;
                ++conf;
                // set slof (switch)
                mod->config.slof = atoi(conf) * 1000;
            } while(0);
            if(!mod->config.lof1)
            {
                log_error(LOG_MSG("failed to parse lnb option \"%s\"")
                          , tmp_value);
                abort();
            }
        }
        else
        {
            module_set_number(mod, "lof1", 1, 0, &mod->config.lof1);
            module_set_number(mod, "lof2", 0, mod->config.lof1
                              , &mod->config.lof2);
            module_set_number(mod, "slof", 0, mod->config.lof1
                              , &mod->config.slof);
            mod->config.lof1 *= 1000;
            mod->config.lof2 *= 1000;
            mod->config.slof *= 1000;
        }
        module_set_number(mod, "lnb_sharing", 0, 0, &mod->config.lnb_sharing);

        module_set_number(mod, "diseqc", 0, 0, &mod->config.diseqc);

        if(mod->config.type == DVB_TYPE_S2)
        {
            fe_option_set(mod, "rolloff", 0, "35"
                          , rolloff_list, ARRAY_SIZE(rolloff_list)
                          , (int *)&mod->config.rolloff);
        }
        fe_option_set(mod, "fec", 0, "AUTO"
                      , fec_list, ARRAY_SIZE(fec_list)
                      , (int *)&mod->config.fec);
    }
    else if(mod->config.type == DVB_TYPE_T || mod->config.type == DVB_TYPE_T2)
    {
        module_set_number(mod, "frequency", 1, 0, &mod->config.frequency);
        mod->config.frequency *= 1000000;

        fe_option_set(mod, "bandwidth", 0, "AUTO"
                      , bandwidth_list, ARRAY_SIZE(bandwidth_list)
                      , (int *)&mod->config.bandwidth);
        fe_option_set(mod, "guardinterval", 0, "AUTO"
                      , guardinterval_list, ARRAY_SIZE(guardinterval_list)
                      , (int *)&mod->config.guardinterval);
        fe_option_set(mod, "transmitmode", 0, "AUTO"
                      , transmitmode_list, ARRAY_SIZE(transmitmode_list)
                      , (int *)&mod->config.transmitmode);
        fe_option_set(mod, "hierarchy", 0, "AUTO"
                      , hierarchy_list, ARRAY_SIZE(hierarchy_list)
                      , (int *)&mod->config.hierarchy);
    }
    else if(mod->config.type == DVB_TYPE_C)
    {
        module_set_number(mod, "frequency", 1, 0, &mod->config.frequency);
        mod->config.frequency *= 1000000;

        module_set_number(mod, "symbolrate", 1, 0, &mod->config.symbolrate);
        mod->config.symbolrate *= 1000;
        fe_option_set(mod, "fec", 0, "AUTO"
                      , fec_list, ARRAY_SIZE(fec_list)
                      , (int *)&mod->config.fec);
    }
    else
    {
        log_error(LOG_MSG("unimplemented dvb-type"));
        abort();
    }
} /* module_configure */

static void module_initialize(module_data_t *mod)
{
    module_configure(mod);

    /* protocols */
    stream_ts_init(mod, NULL, NULL, NULL
                   , callback_join_pid, callback_leave_pid);

    if(mod->config.frequency)
        frontend_open(mod);

    dvr_open(mod);

    sprintf(mod->demux.dev_name, "/dev/dvb/adapter%d/demux%d"
            , mod->config.adapter, mod->config.device);

    if(mod->config.budget)
        demux_join_pid(mod, MAX_PID);

    for(int i = 0; i < MAX_PID; i++)
    {
        if(mod->join_list[i] <= 0)
            continue;
        --mod->join_list[i]; // cheat to prevent double count
        demux_join_pid(mod, i);
    }

    mod->bitrate = 1; // to detect error after module has started
    mod->stat_timer = timer_attach(STAT_INTERVAL, stat_timer_callback, mod);
}

static void module_destroy(module_data_t *mod)
{
    /* protocols */
    stream_ts_destroy(mod);
    module_event_destroy(mod);

    timer_detach(mod->stat_timer);

    if(mod->config.frequency)
        frontend_close(mod);

    if(mod->config.budget)
        demux_leave_pid(mod, MAX_PID);
    else
    {
        for(int i = 0; i < MAX_PID; i++)
        {
            if(mod->join_list[i] > 1) // cheat to close descriptor forcibly
                mod->join_list[i] = 1;
            demux_leave_pid(mod, i);
        }
    }

    dvr_close(mod);
}

MODULE_METHODS()
{
    METHOD(status)
    METHOD(attach)
    METHOD(detach)
    METHOD(event)
};

MODULE(dvb_input)

#endif