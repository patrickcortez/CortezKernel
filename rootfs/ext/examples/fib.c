/*
 * fib.c — Standalone Runtime-Compiled C Program
 * Run directly from the Cortez shell with:
 *   run /ext/examples/fib.c 15
 */

#include <cortez.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, const char** argv) {
    int n = 12;
    if (argc > 1) {
        n = atoi(argv[1]);
    }
    if (n < 1)  n = 1;
    if (n > 45) n = 45;

    uint64_t* seq = (uint64_t*)malloc((size_t)(n + 1) * sizeof(uint64_t));
    if (!seq) {
        printf("Out of memory!\n");
        return 1;
    }

    seq[0] = 0;
    if (n >= 1) seq[1] = 1;
    for (int i = 2; i <= n; i++) {
        seq[i] = seq[i - 1] + seq[i - 2];
    }

    printf("Fibonacci sequence up to F(%d):\n  ", n);
    for (int i = 0; i <= n; i++) {
        printf("%llu%s", seq[i], (i == n) ? "\n" : ", ");
    }

    free(seq);
    return 0;
}
