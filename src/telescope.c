#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>

#include <pthread.h>
#include <dagapi.h>
#include <dag_config_api.h>

#include <numa.h>

#include "dagmultiplexer.h"
#include "ndagmulticaster.h"
#include "byteswap.h"
#include "darkfilter.h"

#define ENCAP_OVERHEAD (sizeof(ndag_common_t) + sizeof(ndag_encap_t))

static char * walk_stream_buffer(char *bottom, char *top,
        uint16_t *reccount, uint16_t *curiov, dagstreamthread_t *dst,
        darkfilter_t *filter) {

    uint32_t walked = 0;
    uint16_t streamnum = dst->params.streamnum;
    uint16_t maxsize = dst->params.mtu - ENCAP_OVERHEAD;
    int ret;

    *curiov = 0;
    dst->iovs[*curiov].iov_base = NULL;
    dst->iovs[*curiov].iov_len = 0;

    while (bottom < top && walked < maxsize) {
        dag_record_t *erfhdr = (dag_record_t *)bottom;
        uint16_t len = ntohs(erfhdr->rlen);
        uint16_t lctr = ntohs(erfhdr->lctr);

        if (lctr != 0) {
            fprintf(stderr, "Loss counter for stream %u is %u\n", streamnum,
                    lctr);
            halt_program();
            return bottom;
        }

        if (top - bottom < len) {
            /* Partial record in the buffer */
            break;
        }

        if (filter) {
            if (trace_prepare_packet(filter->dummytrace, filter->packet,
                    bottom, TRACE_RT_DATA_ERF, TRACE_PREP_DO_NOT_OWN_BUFFER)
                    == -1) {
                fprintf(stderr, "Unable to convert DAG buffer contents to libtrace packet.\n");
                halt_program();
                return bottom;
            }

            ret = apply_darkfilter(filter);
            if (ret < 0) {
                fprintf(stderr, "Error applying darknet filter to received traffic.\n");
                halt_program();
                return bottom;
            }

            if (ret == 0) {
                /* increment number of records walked */
                dst->stats.walked_records++;
                /* Skipping packet, so end current iovec if it has something
                 * in it already. */
                if (dst->iovs[*curiov].iov_len == 0) {
                    bottom += len;
                    continue;
                }
                *curiov = *curiov + 1;
                if (*curiov == dst->iov_alloc) {
                    dst->iovs = (struct iovec *)realloc(dst->iovs,
                            sizeof(struct iovec) * (dst->iov_alloc + 10));
                    dst->iov_alloc += 10;
                }
                dst->iovs[*curiov].iov_base = NULL;
                dst->iovs[*curiov].iov_len = 0;
                continue;
            }
        }

        if (walked > 0 && walked + len > maxsize) {
            /* Current record would push us over the end of our datagram */
            break;
        }

        if (dst->iovs[*curiov].iov_base == NULL) {
            dst->iovs[*curiov].iov_base = bottom;
        }
        dst->iovs[*curiov].iov_len += len;

        walked += len;
        bottom += len;
        (*reccount)++;
        dst->stats.walked_records++;
    }

    /* walked can be larger than maxsize if the first record is
     * very large. This is intentional; the multicaster will truncate the
     * packet record if it is too big and set the truncation flag.
     */

    return bottom;

}

uint16_t telescope_walk_records(char **bottom, char *top,
        dagstreamthread_t *dst, uint16_t *savedtosend,
        ndag_encap_params_t *state) {

    uint16_t available = 0;
    uint16_t total_walked = 0;
    uint16_t records_walked = 0;
    darkfilter_t *filter = (darkfilter_t *)dst->extra;

    /* Sadly, we have to walk whatever dag_advance_stream gives us because
     *   a) top is not guaranteed to be on a packet boundary.
     *   b) there is no way to put an upper limit on the amount of bytes
     *      that top is moved forward, so we can't guarantee we won't end
     *      up with too much data to fit in one datagram.
     */

    do {
        records_walked = 0;
        (*bottom) = walk_stream_buffer((*bottom), top,
                &records_walked, &available, dst, filter);

        total_walked += records_walked;
        if (records_walked > 0) {
            dst->idletime = 0;

            if (ndag_push_encap_iovecs(state, dst->iovs, available + 1,
                        records_walked, *savedtosend) == 0) {
                halt_program();
                break;
            }
            (*savedtosend) = (*savedtosend) + 1;

        }
    } while (!is_halted() && records_walked > 0 &&
        *savedtosend < NDAG_BATCH_SIZE);

    return total_walked;
}

static void *per_dagstream(void *threaddata) {

    ndag_encap_params_t state;
    dagstreamthread_t *dst = (dagstreamthread_t *)threaddata;

    if (init_dag_stream(dst, &state) == -1) {
        halt_dag_stream(dst, NULL);
    } else {
        dag_stream_loop(dst, &state, telescope_walk_records);
        ndag_destroy_encap(&state);
        halt_dag_stream(dst, &state);
    }

    fprintf(stderr, "Exiting thread for stream %d\n", dst->params.streamnum);
    pthread_exit(NULL);
}

void print_help(char *progname) {

    fprintf(stderr,
        "Usage: %s [ -d dagdevice ] [ -p beaconport ] [ -m monitorid ] \n"
        "          [ -a multicastaddress ] [ -s sourceaddress ]\n"
        "          [ -M exportmtu ] [ -E exclusionfile ] [ -o darknetoctet ]\n"
        "          [ -l loginterval ]\n",
        progname);

}

int main(int argc, char **argv) {
    char *dagdev = NULL;
    char *multicastgroup = NULL;
    char *sourceaddr = NULL;
    streamparams_t params;
    int dagfd, errorstate;
    // int i, ret, maxstreams;
    // dagstreamthread_t *dagthreads = NULL;
    ndag_beacon_params_t beaconparams;
    uint16_t beaconport = 9001;
    uint16_t mtu = 1400;
    int log_interval = 0;
    time_t t;
    uint16_t firstport;
    struct timeval starttime;
    darkfilter_params_t darkp;
    struct sigaction sigact;

    srand((unsigned) time(&t));

    /* Process user config options */
    /*  options:
     *      dag device name
     *      monitor id
     *      beaconing port number
     *      multicast address/group
     *      starting port for multicast (if not set, choose at random)
     *      compress output - yes/no?
     *      max streams per core
     *      interfaces to send multicast on
     *      file with /24s to exclude
     *      first octet of darknet space
     *      anything else?
     */

    /* For now, I'm going to use getopt for config. If our config becomes
     * more complicated or we have so many options that configuration becomes
     * unwieldy, then we can look at using a config file instead.
     *
     * UPDATE: yes we definitely need to move to a config file, but I really
     * hate writing config parsing code...
     */

    params.monitorid = 1;

    darkp.first_octet = -1;
    darkp.excl_file = NULL;

    while (1) {
        int option_index = 0;
        int c;
        static struct option long_options[] = {
            { "device", 1, 0, 'd' },
            { "help", 0, 0, 'h' },
            { "monitorid", 1, 0, 'm' },
            { "beaconport", 1, 0, 'p' },
            { "groupaddr", 1, 0, 'a' },
            { "sourceaddr", 1, 0, 's' },
            { "mtu", 1, 0, 'M' },
            { "excludefile", 1, 0, 'E' },
            { "firstoctet", 1, 0, 'o' },
            { "loginterval", 1, 0, 'l' },
            { NULL, 0, 0, 0 }
        };

        c = getopt_long(argc, argv, "a:s:d:hm:M:p:E:o:l:", long_options,
                &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'd':
                dagdev = strdup(optarg);
                break;
            case 'm':
                params.monitorid = (uint16_t)(strtoul(optarg, NULL, 0) % 65536);
                break;
            case 'p':
                beaconport = (uint16_t)(strtoul(optarg, NULL, 0) % 65536);
                break;
            case 'a':
                multicastgroup = strdup(optarg);
                break;
            case 's':
                sourceaddr = strdup(optarg);
                break;
            case 'M':
                mtu = (uint16_t)(strtoul(optarg, NULL, 0) % 65536);
                break;
            case 'E':
                darkp.excl_file = optarg;
                break;
            case 'o':
                darkp.first_octet = atoi(optarg);
                break;
            case 'l':
                log_interval = atoi(optarg);
                break;
            case 'h':
            default:
                print_help(argv[0]);
                exit(1);
        }
    }

    /* Try to set a sensible default */
    if (dagdev == NULL) {
        dagdev = strdup("/dev/dag0");
    }
    if (multicastgroup == NULL) {
        multicastgroup = strdup("225.0.0.225");
    }
    if (sourceaddr == NULL) {
        fprintf(stderr,
            "Warning: no source address specified. Using default interface.");
        sourceaddr = strdup("0.0.0.0");
    }
    if (params.monitorid == 0) {
        fprintf(stderr,
            "0 is not a valid monitor ID -- choose another number.\n");
        goto finalcleanup;
    }

    /* Set signal callbacks */
    /* Interrupt for halt, hup to toggle pause */
    sigact.sa_handler = toggle_pause_signal;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sigact, NULL);

    sigact.sa_handler = halt_signal;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    /* Open DAG card */
    fprintf(stderr, "Attempting to open DAG device: %s\n", dagdev);

    dagfd = dag_open(dagdev);
    if (dagfd < 0) {
        fprintf(stderr, "Failed to open DAG device: %s\n", strerror(errno));
        goto finalcleanup;
    }
    params.dagdevname = dagdev;
    params.dagfd = dagfd;
    params.multicastgroup = multicastgroup;
    params.sourceaddr = sourceaddr;
    params.mtu = mtu;
    params.loginterval = log_interval;

    gettimeofday(&starttime, NULL);
    params.globalstart = bswap_host_to_be64(
            (starttime.tv_sec - 1509494400) * 1000) +
            (starttime.tv_usec / 1000.0);
    firstport = 10000 + (rand() % 50000);

    beaconparams.srcaddr = sourceaddr;
    beaconparams.groupaddr = multicastgroup;
    beaconparams.beaconport = beaconport;
    beaconparams.frequency = DAG_MULTIPLEX_BEACON_FREQ;
    beaconparams.monitorid = params.monitorid;

    while (!is_halted()) {
        if (darkp.excl_file != NULL) {
            errorstate = run_dag_streams(dagfd, firstport, &beaconparams,
                    &params, &darkp, create_darkfilter, per_dagstream,
                    destroy_darkfilter);
        } else {
            errorstate = run_dag_streams(dagfd, firstport, &beaconparams,
                    &params, NULL, NULL, per_dagstream, NULL);
        }

        if (errorstate != 0) {
            break;
        }

        while (is_paused()) {
            usleep(10000);
        }
    }

    fprintf(stderr, "Shutting down DAG multiplexer.\n");

    /* Close DAG card */
    dag_close(dagfd);

finalcleanup:
    free(dagdev);
    free(multicastgroup);
    free(sourceaddr);
}


// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
