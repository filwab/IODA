#include <stdio.h>
#include <linux/kernel.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>

void help()
{
    printf("Usage: md-config [0-3]\n");
    printf("\t0: normal read\n");
    printf("\t1: PL-io(IOD1)\n");
    printf("\t2: PL-brt(IOD2)\n");
    printf("\t3: PL-win(IOD3)\n");
}

int main(int argc, char *argv[])
{
	unsigned long i;
	if(argc == 1)
	{
		help();
		return 0;
	}
	i = strtol(argv[1], NULL, 10);
	syscall(436, i);
    return 0;
}
