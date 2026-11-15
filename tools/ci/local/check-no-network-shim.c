/*
 * LD_PRELOAD shim for check-no-network.sh: logs, and refuses, every name lookup and every
 * AF_INET/AF_INET6 connect or send that would leave the machine. Loopback is allowed except on
 * port 53; AF_UNIX (X11, D-Bus) is never touched. Each event is one line appended to the file
 * named by NATRON_NO_NETWORK_LOG: "loaded", "allow ..." or "block ...".
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static void
logLine(const char* fmt,
        ...)
{
    const char* path = getenv("NATRON_NO_NETWORK_LOG");
    char line[1024];
    int prefix;
    int len;
    va_list args;
    int fd;
    int savedErrno = errno;

    if (!path || !*path) {
        errno = savedErrno;

        return;
    }
    prefix = snprintf(line, sizeof(line), "pid=%d exe=%s ", (int)getpid(), program_invocation_short_name);
    va_start(args, fmt);
    len = vsnprintf(line + prefix, sizeof(line) - prefix - 1, fmt, args);
    va_end(args);
    if (len < 0) {
        len = 0;
    }
    len += prefix;
    if (len > (int)sizeof(line) - 2) {
        len = (int)sizeof(line) - 2;
    }
    line[len++] = '\n';
    /* One write(2) per line, O_APPEND: lines from concurrent threads and processes never interleave. */
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ssize_t ignored = write(fd, line, (size_t)len);
        (void)ignored;
        close(fd);
    }
    errno = savedErrno;
}

__attribute__((constructor)) static void
announce(void)
{
    logLine("loaded");
}

static int
isLocalName(const char* name)
{
    char self[256];
    unsigned char addr[sizeof(struct in6_addr)];
    size_t len;

    if (!name || !*name) {
        return 1;
    }
    if ((inet_pton(AF_INET, name, addr) == 1) || (inet_pton(AF_INET6, name, addr) == 1)) {
        return 1;
    }
    if (!strcasecmp(name, "localhost") || !strcasecmp(name, "localhost.localdomain")) {
        return 1;
    }
    len = strlen(name);
    if ((len > 10) && !strcasecmp(name + len - 10, ".localhost")) {
        return 1;
    }
    /* Qt and X11 resolve the machine's own name, which /etc/hosts answers without DNS. */
    if ((gethostname(self, sizeof(self)) == 0) && !strcasecmp(name, self)) {
        logLine("allow own-name lookup %s (glibc may still query DNS for it, which this shim cannot see)", name);

        return 1;
    }

    return 0;
}

/* Fills text with a printable form of an outbound destination and returns 1; returns 0 when the
 * destination stays on the machine or is not an internet address. */
static int
isOutbound(const struct sockaddr* addr,
           char* text,
           size_t size)
{
    char host[INET6_ADDRSTRLEN] = "";
    int port;
    int loopback;

    if (!addr) {
        return 0;
    }
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
        port = ntohs(in->sin_port);
        loopback = (ntohl(in->sin_addr.s_addr) >> 24) == 127;
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        snprintf(text, size, "%s:%d", host, port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)addr;
        port = ntohs(in6->sin6_port);
        loopback = IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)
            || (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr) && (in6->sin6_addr.s6_addr[12] == 127));
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        snprintf(text, size, "[%s]:%d", host, port);
    } else {
        return 0;
    }

    return !loopback || (port == 53);
}

#define NEXT(name)                                                \
    static __typeof__(name)* next_##name;                         \
    if (!next_##name) {                                           \
        next_##name = (__typeof__(name)*)dlsym(RTLD_NEXT, #name); \
    }

int
getaddrinfo(const char* node,
            const char* service,
            const struct addrinfo* hints,
            struct addrinfo** res)
{
    NEXT(getaddrinfo);
    if (!(hints && (hints->ai_flags & AI_NUMERICHOST)) && !isLocalName(node)) {
        logLine("block lookup %s (getaddrinfo)", node);

        return EAI_NONAME;
    }
    if (node) {
        logLine("allow lookup %s (getaddrinfo)", node);
    }

    return next_getaddrinfo(node, service, hints, res);
}

struct hostent*
gethostbyname(const char* name)
{
    NEXT(gethostbyname);
    if (!isLocalName(name)) {
        logLine("block lookup %s (gethostbyname)", name);
        h_errno = HOST_NOT_FOUND;

        return NULL;
    }

    return next_gethostbyname(name);
}

struct hostent*
gethostbyname2(const char* name,
               int af)
{
    NEXT(gethostbyname2);
    if (!isLocalName(name)) {
        logLine("block lookup %s (gethostbyname2)", name);
        h_errno = HOST_NOT_FOUND;

        return NULL;
    }

    return next_gethostbyname2(name, af);
}

int
gethostbyname_r(const char* name,
                struct hostent* ret,
                char* buf,
                size_t buflen,
                struct hostent** result,
                int* h_errnop)
{
    NEXT(gethostbyname_r);
    if (!isLocalName(name)) {
        logLine("block lookup %s (gethostbyname_r)", name);
        *result = NULL;
        *h_errnop = HOST_NOT_FOUND;

        return ENOENT;
    }

    return next_gethostbyname_r(name, ret, buf, buflen, result, h_errnop);
}

int
gethostbyname2_r(const char* name,
                 int af,
                 struct hostent* ret,
                 char* buf,
                 size_t buflen,
                 struct hostent** result,
                 int* h_errnop)
{
    NEXT(gethostbyname2_r);
    if (!isLocalName(name)) {
        logLine("block lookup %s (gethostbyname2_r)", name);
        *result = NULL;
        *h_errnop = HOST_NOT_FOUND;

        return ENOENT;
    }

    return next_gethostbyname2_r(name, af, ret, buf, buflen, result, h_errnop);
}

int
connect(int fd,
        const struct sockaddr* addr,
        socklen_t len)
{
    char text[INET6_ADDRSTRLEN + 16];

    NEXT(connect);
    if (isOutbound(addr, text, sizeof(text))) {
        logLine("block connect %s", text);
        errno = ENETUNREACH;

        return -1;
    }

    return next_connect(fd, addr, len);
}

ssize_t
sendto(int fd,
       const void* buf,
       size_t len,
       int flags,
       const struct sockaddr* addr,
       socklen_t addrlen)
{
    char text[INET6_ADDRSTRLEN + 16];

    NEXT(sendto);
    if (isOutbound(addr, text, sizeof(text))) {
        logLine("block sendto %s", text);
        errno = ENETUNREACH;

        return -1;
    }

    return next_sendto(fd, buf, len, flags, addr, addrlen);
}

ssize_t
sendmsg(int fd,
        const struct msghdr* msg,
        int flags)
{
    char text[INET6_ADDRSTRLEN + 16];

    NEXT(sendmsg);
    if (msg && isOutbound((const struct sockaddr*)msg->msg_name, text, sizeof(text))) {
        logLine("block sendmsg %s", text);
        errno = ENETUNREACH;

        return -1;
    }

    return next_sendmsg(fd, msg, flags);
}

int
sendmmsg(int fd,
         struct mmsghdr* msgs,
         unsigned int count,
         int flags)
{
    char text[INET6_ADDRSTRLEN + 16];
    unsigned int i;

    NEXT(sendmmsg);
    for (i = 0; msgs && (i < count); ++i) {
        if (isOutbound((const struct sockaddr*)msgs[i].msg_hdr.msg_name, text, sizeof(text))) {
            logLine("block sendmmsg %s", text);
            errno = ENETUNREACH;

            return -1;
        }
    }

    return next_sendmmsg(fd, msgs, count, flags);
}
