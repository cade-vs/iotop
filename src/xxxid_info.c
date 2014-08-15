#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/taskstats.h>
#include <sys/syscall.h>
#include <assert.h>

#include "iotop.h"

#define IOPRIO_CLASS_SHIFT 13

/*
 * Generic macros for dealing with netlink sockets. Might be duplicated
 * elsewhere. It is recommended that commercial grade applications use
 * libnl or libnetlink and use the interfaces provided by the library
 */
#define GENLMSG_DATA(glh)       ((void *)(NLMSG_DATA(glh) + GENL_HDRLEN))
#define GENLMSG_PAYLOAD(glh)    (NLMSG_PAYLOAD(glh, 0) - GENL_HDRLEN)
#define NLA_DATA(na)            ((void *)((char*)(na) + NLA_HDRLEN))
#define NLA_PAYLOAD(len)        (len - NLA_HDRLEN)

#define MAX_MSG_SIZE 1024
#define MAX_CPUS     32

struct msgtemplate {
    struct nlmsghdr n;
    struct genlmsghdr g;
    char buf[MAX_MSG_SIZE];
};

const char *str_ioprio_class[] = { "-", "rt", "be", "id" };

static int _nl_sock = -1;
static int _nl_fam_id = 0;

int
_send_cmd(int sock_fd, __u16 nlmsg_type, __u32 nlmsg_pid,
         __u8 genl_cmd, __u16 nla_type,
         void *nla_data, int nla_len)
{
    struct nlattr *na;
    struct sockaddr_nl nladdr;
    int r, buflen;
    char *buf;

    struct msgtemplate msg;

    msg.n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    msg.n.nlmsg_type = nlmsg_type;
    msg.n.nlmsg_flags = NLM_F_REQUEST;
    msg.n.nlmsg_seq = 0;
    msg.n.nlmsg_pid = nlmsg_pid;
    msg.g.cmd = genl_cmd;
    msg.g.version = 0x1;
    na = (struct nlattr *) GENLMSG_DATA(&msg);
    na->nla_type = nla_type;
    na->nla_len = nla_len + 1 + NLA_HDRLEN;
    memcpy(NLA_DATA(na), nla_data, nla_len);
    msg.n.nlmsg_len += NLMSG_ALIGN(na->nla_len);

    buf = (char *) &msg;
    buflen = msg.n.nlmsg_len ;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;
    while ((r = sendto(sock_fd, buf, buflen, 0, (struct sockaddr *) &nladdr,
                       sizeof(nladdr))) < buflen) {
        if (r > 0) {
            buf += r;
            buflen -= r;
        } else if (errno != EAGAIN)
            return -1;
    }
    return 0;
}

int
_get_family_id(int sock_fd)
{
    static char name[256];

    struct {
        struct nlmsghdr n;
        struct genlmsghdr g;
        char buf[256];
    } ans;

    int id = 0, rc;
    struct nlattr *na;
    int rep_len;

    strcpy(name, TASKSTATS_GENL_NAME);
    rc = _send_cmd(sock_fd, GENL_ID_CTRL, getpid(), CTRL_CMD_GETFAMILY,
                    CTRL_ATTR_FAMILY_NAME, (void *)name,
                    strlen(TASKSTATS_GENL_NAME)+1);

    rep_len = recv(sock_fd, &ans, sizeof(ans), 0);
    if (ans.n.nlmsg_type == NLMSG_ERROR
    || (rep_len < 0) || !NLMSG_OK((&ans.n), rep_len))
        return 0;

    na = (struct nlattr *) GENLMSG_DATA(&ans);
    na = (struct nlattr *) ((char *) na + NLA_ALIGN(na->nla_len));
    if (na->nla_type == CTRL_ATTR_FAMILY_ID) {
        id = *(__u16 *) NLA_DATA(na);
    }
    return id;
}

void
nl_init(void)
{
    struct sockaddr_nl addr;
    int sock_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_GENERIC);

    if (sock_fd < 0)
        goto error;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    if (bind(sock_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        goto error;

    _nl_sock = sock_fd;
    _nl_fam_id = _get_family_id(sock_fd);

    return;

error:
    if (sock_fd > -1)
        close(sock_fd);

    fprintf(stderr, "nl_init: %s\n", strerror(errno));
    exit(-1);
}

int
nl_xxxid_info(pid_t xxxid, int isp, struct xxxid_stats *stats)
{
    assert(_nl_sock > -1);
    int cmd_type = isp ? TASKSTATS_CMD_ATTR_PID : TASKSTATS_CMD_ATTR_TGID;

    if (_send_cmd(_nl_sock, _nl_fam_id, xxxid, TASKSTATS_CMD_GET,
                    cmd_type, &xxxid, sizeof(__u32))) {
        fprintf(stderr, "get_xxxid_info: %s\n", strerror(errno));
        return -1;
    }

    struct msgtemplate msg;
    int rv = recv(_nl_sock, &msg, sizeof(msg), 0);

    if (msg.n.nlmsg_type == NLMSG_ERROR ||
            !NLMSG_OK((&msg.n), rv)) {
        struct nlmsgerr *err = NLMSG_DATA(&msg);
        fprintf(stderr, "fatal reply error, %d\n", err->error);
        return -1;
    }

    rv = GENLMSG_PAYLOAD(&msg.n);

    struct nlattr *na = (struct nlattr *) GENLMSG_DATA(&msg);
    int len = 0;
    int i = 0;

    while (len < rv) {
        len += NLA_ALIGN(na->nla_len);

        switch (na->nla_type) {
        case TASKSTATS_TYPE_AGGR_TGID:
        case TASKSTATS_TYPE_AGGR_PID:
            {
                int aggr_len = NLA_PAYLOAD(na->nla_len);
                int len2 = 0;

                na = (struct nlattr *) NLA_DATA(na);
                while (len2 < aggr_len) {
                    switch (na->nla_type) {
                    case TASKSTATS_TYPE_STATS:
                        {
                            struct taskstats *ts = NLA_DATA(na);

#define COPY(field) { stats->field = ts->field; }
                            COPY(cpu_run_real_total);
                            COPY(read_bytes);
                            COPY(write_bytes);
                            COPY(swapin_delay_total);
                            COPY(blkio_delay_total);
#undef COPY

                        }
                        break;
                    }
                    len2 += NLA_ALIGN(na->nla_len);
                    na = (struct nlattr *) ((char *) na + len2);
                }
            }
            break;
        }
        na = (struct nlattr *) (GENLMSG_DATA(&msg) + len);
    }

    stats->ioprio = syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, xxxid);
    stats->ioprio_class = stats->ioprio >> IOPRIO_CLASS_SHIFT;
    stats->ioprio &= 0xff;

    return 0;
}

void
nl_term(void)
{
    if (_nl_sock > -1)
        close(_nl_sock);
}

void
dump_xxxid_stats(struct xxxid_stats *stats) {
    printf("CPU: %llu\nSWAPIN: %llu\nIO: %llu\n"
           "READ: %llu\nWRITE: %llu\nIOPRIO: %i%s\n",
           stats->cpu_run_real_total, stats->swapin_delay_total,
           stats->blkio_delay_total, stats->read_bytes,
           stats->write_bytes, stats->ioprio,
           str_ioprio_class[stats->ioprio_class]);
}

