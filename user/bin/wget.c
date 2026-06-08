#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

/* Simple HTTP GET client.
   Usage: wget http://host/path
          wget http://host          (path defaults to /)  */

static int str_startswith(const char *s, const char *prefix)
{
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

static int str_len(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Print an IPv4 address in dotted-decimal from a big-endian uint32. */
static void print_ip(uint32_t ip)
{
    unsigned char *b = (unsigned char *)&ip;
    printf("%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: wget http://host[/path]");
        return 1;
    }

    const char *url = argv[1];
    if (str_startswith(url, "http://")) url += 7;

    /* Split host and path. */
    char host[128] = {0};
    char path[256] = "/";

    int i = 0;
    while (url[i] && url[i] != '/' && i < 127) { host[i] = url[i]; i++; }
    host[i] = '\0';
    if (url[i] == '/') str_copy(path, url + i, sizeof(path));

    /* DNS resolution. */
    printf("Resolving %s...\n", host);
    uint32_t ip = 0;
    if (dns_resolve(host, &ip) < 0) {
        printf("wget: could not resolve '%s'\n", host);
        return 1;
    }
    printf("Connecting to ");
    print_ip(ip);
    printf(":80...\n");

    /* Open TCP socket and connect. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { puts("wget: socket() failed"); return 1; }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(80);
    addr.sin_addr   = ip;

    if (connect(fd, &addr, (int)sizeof(addr)) < 0) {
        puts("wget: connect() failed");
        close(fd);
        return 1;
    }

    /* Send HTTP/1.0 GET request. */
    char req[512];
    int req_len = 0;
    const char *parts[] = {
        "GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\nConnection: close\r\n\r\n"
    };
    for (int p = 0; p < 5; p++) {
        int l = str_len(parts[p]);
        if (req_len + l < (int)sizeof(req) - 1) {
            str_copy(req + req_len, parts[p], sizeof(req) - req_len);
            req_len += l;
        }
    }
    write(fd, req, (size_t)req_len);

    /* Read response, skip headers, print body. */
    char buf[512];
    long n;
    int  header_done = 0;
    char leftover[4] = {0};
    int  leftover_len = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (!header_done) {
            /* Search for \r\n\r\n in the accumulated stream. */
            char combined[520];
            int clen = leftover_len;
            for (int k = 0; k < clen; k++) combined[k] = leftover[k];
            for (int k = 0; k < (int)n && clen < (int)sizeof(combined) - 1; k++)
                combined[clen++] = buf[k];

            /* Find \r\n\r\n */
            int found = -1;
            for (int k = 0; k <= clen - 4; k++) {
                if (combined[k]=='\r' && combined[k+1]=='\n' &&
                    combined[k+2]=='\r' && combined[k+3]=='\n') {
                    found = k + 4;
                    break;
                }
            }
            if (found >= 0) {
                header_done = 1;
                /* Print body bytes that came in the same read. */
                int body_bytes = clen - found;
                if (body_bytes > 0)
                    write(1, combined + found, (size_t)body_bytes);
            } else {
                /* Keep last 3 bytes as leftover for next iteration. */
                int keep = clen < 3 ? clen : 3;
                for (int k = 0; k < keep; k++)
                    leftover[k] = combined[clen - keep + k];
                leftover_len = keep;
            }
        } else {
            write(1, buf, (size_t)n);
        }
    }

    close(fd);
    return 0;
}
