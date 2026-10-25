/* Operation Neptune recomp: host entry point.  main <ON.EXE> [game dir] */
#include "recomp/runtime16.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_exe, *g_dir;
static int g_rc;

static void *run(void *arg)
{
    (void)arg;
    CPU cpu; cpu_init(&cpu);
    if (cpu_alloc_mem(&cpu)) { g_rc = 1; return NULL; }
    if (nep_load_image(&cpu, g_exe)) { g_rc = 1; return NULL; }
    nep_run(&cpu);
    g_rc = nep_exit_code;
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s ON.EXE [game-dir]\n", argv[0]); return 2; }
    g_exe = argv[1];
    g_dir = argc > 2 ? argv[2] : ".";
    nep_set_game_dir(g_dir);
    /* lifted code nests C calls as deeply as the guest nests calls: big stack */
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, (size_t)512 << 20);
    pthread_t t;
    if (pthread_create(&t, &at, run, NULL)) { perror("pthread_create"); return 1; }
    pthread_join(t, NULL);
    return g_rc;
}
