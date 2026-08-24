/* main.c - entry point. */
#include "shell.h"

int main(void)
{
    shell_init();
    shell_loop();
    return 0;
}
