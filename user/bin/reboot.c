#include <unistd.h>

int main(void)
{
    reboot(REBOOT_CMD_RESTART);
    return 0;
}
