#include <sys/stat.h>
#include <sys/unistd.h>
#include <errno.h>
#include <stdint.h>

// Acest simbol este definit în linker script
extern uint32_t _end;
extern uint32_t _estack;
//extern uint32_t __StackTop;
//#define _estack __StackTop

static uint8_t *heap = (uint8_t *)&_end;

caddr_t _sbrk(int incr) {
    uint8_t *prev_heap = heap;
    uint8_t *stack = (uint8_t *)&_estack;

    if (heap + incr > stack) {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    heap += incr;
    return (caddr_t)prev_heap;
}

int _write(int file, char *ptr, int len) {
    // Dacă ai configurat un UART, poți implementa trimiterea aici.
    // Pentru moment doar ignorăm și returnăm len.
    return len;
}

int _read(int file, char *ptr, int len) {
    // Nu implementăm citire (ex: stdin)
    errno = EBADF;
    return -1;
}

int _close(int file) {
    return -1;
}

int _fstat(int file, struct stat *st) {
    st->st_mode = S_IFCHR;
    return 0;
}

int _lseek(int file, int ptr, int dir) {
    return 0;
}

int _isatty(int file) {
    return 1;
}

void _exit(int status) {
    (void)status;
    while (1);
}

int _kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

int _getpid(void) {
    return 1;
}
