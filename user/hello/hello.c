/* First user-mode process. Exercises write, open, read, close, getpid, exit.
   C port of the original hello.asm. */
#include <unistd.h>

int main(void)
{
    static const char hello_msg[] = "Hello from userspace!\n";
    write(1, hello_msg, sizeof(hello_msg) - 1);

    int fd = open("/hello.txt", O_RDONLY, 0);
    if (fd >= 0) {
        char read_buf[64];
        ssize_t n = read(fd, read_buf, sizeof(read_buf));
        if (n > 0)
            write(1, read_buf, (size_t)n);
        close(fd);
    }

    (void)getpid();   /* result ignored — exercises the syscall */
    return 0;
}
