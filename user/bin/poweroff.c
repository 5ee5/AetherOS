#include <unistd.h>

int main(void)
{
    reboot(REBOOT_CMD_POWEROFF);
    return 0;
}
