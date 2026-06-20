#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>



int main (int argc, char* argv[]) {

    printf("You typed %d things\n", argc);
    printf("\nFirst thing: %s", argv[1]);

    return 0;
}