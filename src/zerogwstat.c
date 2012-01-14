#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>

#include <zmq.h>

#include "config.h"

#define TRUE 1
#define FALSE 0
#define bool int
#ifndef MAX_ADDRESSES
#define MAX_ADDRESSES 1024
#endif
#define MAX_STAT_BUFFER 4096
#define STDASSERT(val) if((val) < 0) {fprintf(stderr, #val ": %m\n"); abort();}

typedef struct statistics_s {
    double time;
    double interval;
    int nvalues;
    #define DEFINE_VALUE(name) size_t name;
    #include "statistics.h"
    #undef DEFINE_VALUE
} statistics_t;

enum {
    COLLECTD_OPTION = 1,
    COLLECTD_HOST,
    COLLECTD_PORT
};

struct option options[] = {
    {name:"config", has_arg:TRUE, flag:NULL, val:'c'},
    {name:"zerogw", has_arg:TRUE, flag:NULL, val:'a'},
    {name:"interval", has_arg:TRUE, flag:NULL, val:'i'},
    {name:"socket", has_arg:TRUE, flag:NULL, val:'s'},
    {name:"print", has_arg:FALSE, flag:NULL, val:'p'},
    {name:"human-readable", has_arg:FALSE, flag:NULL, val:'H'},
    {name:"oneline", has_arg:FALSE, flag:NULL, val:'l'},
    {name:"once", has_arg:FALSE, flag:NULL, val:'1'},
    {name:"collectd", has_arg:FALSE, flag:NULL, val:COLLECTD_OPTION},
    {name:"host", has_arg:TRUE, flag:NULL, val:COLLECTD_HOST},
    {name:"port", has_arg:TRUE, flag:NULL, val:COLLECTD_PORT},
    {name:"help", has_arg:FALSE, flag:NULL, val:'h'},
    {name:NULL, 0, 0, 0}
    };

typedef struct zerogwstat_flags {
    char *config;
    int interval;
    char *socket;
    bool print;
    bool human_readable;
    bool oneline;
    bool once;
    bool collectd;
    char *host;
    unsigned short port;
    int zerogw_naddr;
    char *zerogw_addrs[MAX_ADDRESSES];
} zerogwstat_flags_t;

void print_usage(FILE *stream) {
    fprintf(stream,
        "Usage:\n"
        "    zerogwstat {-c CONFIG_FILE|-a ZMQADDR -i INTERVAL}\n"
        "        --  monitor statistics\n"
        "    zerogwstat {-c CONFIG_FILE|-a ZMQADDR -i INTERVAL} --collectd \\\n"
        "           { --collectd-host HOST ---collectd-port PORT \\\n"
        "           | --collectd-socket UNIXSOCK }\n"
        "        -- send data to collectd\n"
        "\n"
        "Options:\n"
        "  -c, --config FILE  Zerogw configuration file (to get socket from)\n"
        "  -a, --zerogw ADDR  Zeromq address of zerogw to collect info from.\n"
        "                     Repeatable. Multiple zerogw instances will be\n"
        "                     summarized.\n"
        "  -i, --interval SEC Overrides interval sent to collectd, useful mainly\n"
        "                     when using -a option without any real config\n"
        "  -p, --print        Print statistics even when sending to collectd\n"
        "  -H, --human-readable\n"
        "                     Print statistics in human-frienldy format\n"
        "  -l, --oneline      Print short statistics (line per snapshot)\n"
        "  -1, --once         Print first received statistics packet and exit\n"
        "                     (don't use with collectd and multiple servers)\n"
        "  --collectd         Send data to collectd. Enables following options\n"
        "                     (uses socket /var/run/collectd-unixsock by default)\n"
        "  --collectd-socket  Collectd socket to send data to\n"
        "  --collectd-host    Collectd host to send data to\n"
        "  --collectd-port    Collectd port to send data to\n"
        "\n"
        );
}

void parse_arguments(zerogwstat_flags_t *flags, int argc, char **argv) {
    int opt;
    while((opt = getopt_long(argc, argv,
                             "c:a:i:s:pHl1h", options, NULL)) != -1) {
        switch(opt) {
            case COLLECTD_OPTION:
                flags->collectd = TRUE;
                break;
            case COLLECTD_HOST:
                flags->host = optarg;
                break;
            case COLLECTD_PORT:
                flags->port = atoi(optarg);
                break;
            case 'c':
                flags->config = optarg;
                break;
            case 'a':
                flags->zerogw_addrs[flags->zerogw_naddr++] = optarg;
                break;
            case 'i':
                flags->interval = atoi(optarg);
                break;
            case 's':
                flags->socket = optarg;
                break;
            case 'p': flags->print = TRUE; break;
            case 'H': flags->human_readable = TRUE; break;
            case 'l': flags->oneline = TRUE; break;
            case '1': flags->once = TRUE; break;
            case 'h':
                print_usage(stdout);
                exit(0);
                break;
            default:
                printf("OPT %d %c\n", opt);
                print_usage(stderr);
                exit(1);
                break;
        }
    }
}

double get_time() {
  struct timeval tv;
  gettimeofday (&tv, 0);
  return tv.tv_sec + tv.tv_usec * 1e-6;
}

void collectd_loop(void *input, zerogwstat_flags_t *flags) {
}

void reset_statistics(statistics_t *stat) {
    stat->time = 0;
    stat->interval = 0;
    stat->nvalues = 0;
    #define DEFINE_VALUE(name) stat->name = 0;
    #include "statistics.h"
    #undef DEFINE_VALUE
}

void read_data(void *input, statistics_t *result) {
    zmq_msg_t msg;
    zmq_msg_init(&msg);
    STDASSERT(zmq_recv(input, &msg, 0));
    assert(zmq_msg_size(&msg) < 32);
    int64_t opt = 1; \
    size_t optlen = sizeof(opt); \
    STDASSERT(zmq_getsockopt(input, ZMQ_RCVMORE, &opt, &optlen));
    assert(opt);
    STDASSERT(zmq_recv(input, &msg, 0));
    STDASSERT(zmq_getsockopt(input, ZMQ_RCVMORE, &opt, &optlen));
    assert(!opt);

    double time = 0;
    char *data = zmq_msg_data(&msg);
    int len = zmq_msg_size(&msg);
    char linebuf[64];
    char *tmp = data;
    char *enddata = data + len;
    while(tmp < enddata) {
        char *endline = memchr(tmp, '\n', enddata - tmp);
        if(!endline) break;
        memcpy(linebuf, tmp, endline - tmp);
        linebuf[endline - tmp] = 0;
        tmp = endline + 1;
        char name[sizeof(linebuf)];
        size_t value;
        if(!strncmp(linebuf, "time:", 5)) {
            int rc = sscanf(linebuf, "%[^:]: %lf\n", &name, &time);
            assert(rc == 2);
            continue;
        }
        if(!strncmp(linebuf, "interval:", 9)) {
            int rc = sscanf(linebuf, "%[^:]: %lf\n", &name, &result->interval);
            assert(rc == 2);
            continue;
        }
        int rc = sscanf(linebuf, "%[^:]: %lu\n", &name, &value);
        assert(rc == 2);
        #define DEFINE_VALUE(_name) if(!strcmp(name, #_name)) { \
            result->_name += value; \
            continue; \
            }
        #include "statistics.h"
        #undef DEFINE_VALUE
    }
    assert(time);
    result->time += time;
    result->nvalues += 1;
}

void read_between(void *input, statistics_t *result, double begin, double end){
    statistics_t one;
    reset_statistics(&one);
    while(get_time() < end) {
        zmq_pollitem_t pollstr = {
            socket: input,
            fd: -1,
            events: ZMQ_POLLIN,
            revents: 0};
        long msec = (end - get_time())*1000;
        if(msec < 0) msec = 0;
        if(zmq_poll(&pollstr, 1, msec) != 1)
            continue;
        reset_statistics(&one);
        read_data(input, &one);
        if(one.time < begin || one.time >= end)
            continue;
        result->time += one.time;
        result->nvalues += 1;
        #define DEFINE_VALUE(name) result->name += one.name;
        #include "statistics.h"
        #undef DEFINE_VALUE
    }
}

void print_once(statistics_t *stat, zerogwstat_flags_t *flags) {
    if(flags->oneline) {
        printf("NOT IMPLEMENTED\n");
        exit(1);
    } else {
        char buf[MAX_STAT_BUFFER];
        int res = snprintf(buf, sizeof(buf),
            "%-26s %27.6lf\n"
            "%-26s %22.1lf\n"
            #define DEFINE_EXTRA(name, _1, _2, _3) "%-26s %20lu\n"
            #include "statextra.h"
            #undef DEFINE_EXTRA
            "----\n"
            #define DEFINE_VALUE(name) "%-26s %20lu\n"
            #include "statistics.h"
            #undef DEFINE_VALUE
            "%s\n",
            "time:", stat->time,
            "interval:", stat->interval,
            #define newstat stat
            #define DEFINE_EXTRA(name, _1, _2, value) #name ":", value,
            #include "statextra.h"
            #undef DEFINE_EXTRA
            #undef newstat
            #define DEFINE_VALUE(name) #name ":", stat->name,
            #include "statistics.h"
            #undef DEFINE_VALUE
            "");
        buf[sizeof(buf)-1] = 0;
        STDASSERT(fputs(buf, stdout));
    }
}

void print_diff(statistics_t *oldstat, statistics_t *newstat,
    zerogwstat_flags_t *flags) {
    statistics_t diff;
    #define DEFINE_VALUE(name) diff.name = newstat->name - oldstat->name;
    #include "statistics.h"
    #undef DEFINE_VALUE
    if(flags->oneline) {
        printf("NOT IMPLEMENTED\n");
        exit(1);
    } else {
        printf(
            "%-26s %25.6f\n"
            "%-26s %20.1f\n"
            #define DEFINE_EXTRA(_0, _1, _2, _3) "%-26s %18lu\n"
            #include "statextra.h"
            #undef DEFINE_EXTRA
            "----\n"
            #define DEFINE_VALUE(name) "%-26s %20.1lf /s\n"
            #include "statistics.h"
            #undef DEFINE_VALUE
            "%s\n",
            "time:", newstat->time,
            "interval:", newstat->interval,
            #define DEFINE_EXTRA(name, _1, _2, value) #name ":", value,
            #include "statextra.h"
            #undef DEFINE_EXTRA
            #define DEFINE_VALUE(name) #name ":", diff.name / newstat->interval,
            #include "statistics.h"
            #undef DEFINE_VALUE
            "---------------------------------------------------------------");
    }
}


void print_loop(void *input, zerogwstat_flags_t *flags) {
    if(flags->once) {
        statistics_t iteration;
        reset_statistics(&iteration);
        read_data(input, &iteration);
        print_once(&iteration, flags);
    } else {
        statistics_t start;
        statistics_t iteration;
        reset_statistics(&iteration);
        double time = get_time();
        long interval = flags->interval;
        long start_slice = time/interval;
        double until = time - start_slice*interval + interval*0.5;
        if(until - time < interval * 0.75) {
            read_between(input, &iteration, until - interval, until);
            reset_statistics(&iteration);
            start_slice += 1;
        }
        until = start_slice*interval + interval*0.5;
        read_between(input, &iteration, until - interval, until);
        while(TRUE) {
            memcpy(&start, &iteration, sizeof(iteration));
            reset_statistics(&iteration);
            start_slice += 1;
            until = start_slice*interval + interval*0.5;
            read_between(input, &iteration, until - interval, until);
            iteration.interval = interval;
            iteration.time /= iteration.nvalues;
            print_diff(&start, &iteration, flags);
        }
    }
}

int main(int argc, char **argv) {
    zerogwstat_flags_t flags = {
        config: NULL,
        interval: 0,
        socket: "/var/run/collectd-unixsock",
        print: FALSE,
        human_readable: FALSE,
        oneline: FALSE,
        once: FALSE,
        collectd: FALSE,
        host: NULL,
        port: 25826,
        zerogw_naddr: 0
        };
    parse_arguments(&flags, argc, argv);
    config_main_t config;
    if(flags.config || !flags.zerogw_naddr || !flags.interval) {
        coyaml_context_t *ctx = config_context(NULL, &config);
        if(flags.config) {
            ctx->root_filename = flags.config;
        }
        assert(ctx);
        assert(coyaml_readfile(ctx) == 0);
        coyaml_context_free(ctx);
        if(!flags.interval) {
            flags.interval = (int)config.Server.status.interval;
        }
    }
    void *zmq = zmq_init(1);
    assert(zmq);
    void *socket = zmq_socket(zmq, ZMQ_SUB);
    STDASSERT(zmq_setsockopt(socket, ZMQ_SUBSCRIBE, "", 0));
    if(flags.zerogw_naddr) {
        for(int i = 0; i < flags.zerogw_naddr; ++i) {
            STDASSERT(zmq_connect(socket, flags.zerogw_addrs[i]));
        }
    } else {
        CONFIG_ZMQADDR_LOOP(line, config.Server.status.socket.value) {
            if(line->value.kind == CONFIG_zmq_Connect) {
                // We are other party, so let's bind
                STDASSERT(zmq_bind(socket, line->value.value));
            } else {
                STDASSERT(zmq_connect(socket, line->value.value));
            }
        }
    }
    if(flags.collectd) {
        collectd_loop(socket, &flags);
    } else {
        print_loop(socket, &flags);
    }

    zmq_close(socket);
    zmq_term(zmq);
    config_free(&config);
}
