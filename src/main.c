#include "modules.h"
#include "game_data.h"
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Programme principal du jeu (tools/games.py) ; build_game.py le définit.
#ifndef RT_GAME_PROGRAM
#define RT_GAME_PROGRAM "portal2_linux"
#endif

static uint32_t guest_string(const char *value) {
    size_t length = strlen(value) + 1;
    uint32_t address = rt_alloc((uint32_t)length);
    if (!address) { fputs("guest argument allocation failed\n", stderr); exit(2); }
    memcpy(GPTR(address), value, length);
    return address;
}
int rt_d3d9_available(void);   // runtime/d3d9.c

int rt_renderer_request(const char *value);   // runtime/d3d9.c
const char *rt_minidump_argument(int count, char *const *args);   // runtime/process.c

// Rendu Direct3D 9 natif par défaut : le jeu prend alors shaderapivk.so
// (-vulkan), dont le Direct3D 9 vient de libd3d9metal (runtime/d3d9.c).
// La bibliothèque est chargée et initialisée ici : absente ou refusant
// d3d9metal_init, on reste en OpenGL plutôt que d'échouer au démarrage.
// P2_RENDERER=opengl force OpenGL ; P2_RENDERER=d3d9 le signale s'il manque.
int rt_renderer_plan(int request, int vulkan_given, int (*available)(void));   // runtime/d3d9.c
static int wants_d3d9(int first, int argc, char **argv) {
    int request = rt_renderer_request(getenv("P2_RENDERER")), given = 0;
    for (int i = first; i < argc; ++i) if (!strcmp(argv[i], "-vulkan")) given = 1;
    int plan = rt_renderer_plan(request, given, rt_d3d9_available);
    if (plan > 0) fputs("renderer: Direct3D 9 (native library), adding -vulkan\n", stderr);
    else if (plan < 0) fputs("renderer: OpenGL (P2_RENDERER), ignoring -vulkan\n", stderr);
    else if (request < 0) fputs("renderer: OpenGL (P2_RENDERER)\n", stderr);
    else if (!given) fputs(request > 0 ? "renderer: P2_RENDERER=d3d9 but libd3d9metal is missing or failed to initialize, staying on OpenGL\n"
                                       : "renderer: OpenGL (no usable libd3d9metal)\n", stderr);
    return plan;
}

#ifdef __PROSPERO__
// Console payloads start without a command line (the ELF loader passes none):
// the data lives under /data/portal2 (docs/ps5.md), laid out by tools/package_ps5.py.
#ifndef RT_PS5_ROOT
#define RT_PS5_ROOT "/data/portal2"
#endif
static char *ps5_default_argv[] = {
    "portal2", "--images", RT_PS5_ROOT "/prep", "--game-dir", RT_PS5_ROOT "/game", "--linux-root", RT_PS5_ROOT "/game",
    "--user-dir", RT_PS5_ROOT "/user", "--", "-game", "portal2", "-novid", "-fullscreen", "-w", "3840", "-h", "2160", NULL,
};
#endif

#ifdef __PROSPERO__
void rt_ps5_trace(const char *text);
#define P2_TRACE(text) rt_ps5_trace(text)
#else
#define P2_TRACE(text) ((void)0)
#endif
int main(int argc, char **argv) {
#ifdef __PROSPERO__
#ifdef RT_PS5_TITLE
    // A title is started from the home screen: whatever the shell passes (an empty or odd argv) is not for the game.
    argc = 0;
    // No terminal to look at: DXVK says what failed (also written to a file next to the other logs).
    setenv("DXVK_LOG_LEVEL", "debug", 0);
    setenv("DXVK_LOG_PATH", "/data/portal2/tmp", 0);
#endif
    if (argc <= 1) { argv = ps5_default_argv; argc = (int)(sizeof(ps5_default_argv) / sizeof(*ps5_default_argv)) - 1; }
#ifdef RT_PS5_TITLE
    // Extra game arguments, whitespace-separated, from RT_PS5_ROOT/args.txt when present (e.g. "+map sp_a1_intro1"): testing
    // without a rebuild. They follow the defaults, after "--".
    {
        static char text[2048];
        static char *combined[64 + sizeof(ps5_default_argv) / sizeof(*ps5_default_argv)];
        FILE *extra = fopen(RT_PS5_ROOT "/args.txt", "r");
        size_t length = extra ? fread(text, 1, sizeof text - 1, extra) : 0;
        if (extra) fclose(extra);
        if (length) {
            text[length] = 0;
            int count = 0;
            for (int i = 0; i < argc; ++i) combined[count++] = argv[i];
            char *save = NULL;
            for (char *word = strtok_r(text, " \t\r\n", &save); word && count < (int)(sizeof combined / sizeof *combined) - 1;
                 word = strtok_r(NULL, " \t\r\n", &save))
                combined[count++] = word;
            combined[count] = NULL;
            argv = combined; argc = count;
            fprintf(stderr, "ps5: %d extra arguments from %s\n", count - (int)(sizeof(ps5_default_argv) / sizeof(*ps5_default_argv)) + 1, RT_PS5_ROOT "/args.txt");
        }
    }
#endif
#endif
    const char *program_name = RT_GAME_PROGRAM;
    // Qualité de service d'un jeu au premier plan : sans elle (lancé depuis un
    // terminal), macOS place souvent le thread principal sur un cœur économie
    // (cadence bimodale mesurée : ~200 ou ~240 images/s selon les lancements).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#if defined(__linux__) || defined(__PROSPERO__)
    // Offline Steam stub linked into this executable (decision of the user, 2026-11-23).
    setenv("P2_STEAM_API_DYLIB", "@self", 0);
    setenv("SteamAppId", "620", 0);
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);   // journal lisible même si le processus meurt
    const char *images = NULL, *game = NULL, *linux = NULL, *user = NULL;
    int first = 1, check_data = 0;
    while (first < argc) {
        if (!strcmp(argv[first], "--check-game-data")) { check_data = 1; ++first; }
        else if (!strcmp(argv[first], "--images") && first + 1 < argc) { images = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--game-dir") && first + 1 < argc) { game = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--linux-root") && first + 1 < argc) { linux = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--user-dir") && first + 1 < argc) { user = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--program") && first + 1 < argc) { program_name = argv[first + 1]; first += 2; }
        else if (!strcmp(argv[first], "--")) { ++first; break; }
        else break;
    }
    if ((!images && !check_data) || !game) { fprintf(stderr, "usage: %s [--check-game-data | --images /absolute/prep] --game-dir /path/to/game -- [game arguments]\n", argv[0]); return 2; }
    P2_TRACE("main: arguments parsed");
    int map_tool = !strcmp(program_name, "vbsp_linux") || !strcmp(program_name, "vvis_dll.so") || !strcmp(program_name, "vrad_dll.so");
    rt_paths_set_program(program_name);
    if (linux && user && rt_paths_init(game, linux, user)) { perror("game paths"); return 2; }
    P2_TRACE("main: paths ready");
    if (rt_validate_game_data(program_name, game, stderr)) { P2_TRACE("main: game data validation failed"); return 2; }
    P2_TRACE("main: game data valid");
    if (check_data) return 0;
    int init_failed = 1;
    if (rt_init()) P2_TRACE("main: rt_init failed");
    else if (rt_load_images(images)) P2_TRACE("main: rt_load_images failed");
    else if (rt_map(0x70000000, 0x800000)) P2_TRACE("main: stack map failed");
    else {
#ifdef __PROSPERO__
        { char note[120]; snprintf(note, sizeof note, "main: images loaded (fd1 %d, fd2 %d)", fcntl(1, F_GETFL), fcntl(2, F_GETFL)); P2_TRACE(note); }
#endif
        init_failed = 0;
    }
    if (init_failed) {
        fputs("native runtime initialization failed\n", stderr); return 2;
    }
#ifdef RT_PS5_TITLE
    // A title is refused chdir (observed): the guest's paths go through rt_paths, so the host's working directory is not needed.
    if (chdir(game)) { char note[160]; snprintf(note, sizeof note, "main: chdir(%s) refused (errno %d): continuing", game, errno); P2_TRACE(note); }
#else
    if (chdir(game)) { P2_TRACE("main: chdir(game) failed"); perror(game); rt_destroy(); return 2; }
#endif
    P2_TRACE("main: in the game directory");
    if (map_tool)
        setenv("PORTAL2_NO_LAUNCHERMGR_REQUIRED_IN_MATERIAL_SYSTEM", "1", 0);
    CPU cpu; rt_cpu_reset(&cpu); cpu.esp = 0x70800000;
    int renderer = map_tool ? 0 : wants_d3d9(first, argc, argv);
    // OpenGL : sans sampler objects sur macOS, togl doit filtrer par texture (runtime/gl.c).
    const char *sampler = map_tool || renderer > 0 ? NULL : rt_gl_sampler_argument(argc - first, argv + first);
    // Sys_Error sort proprement au lieu d'écrire volontairement à l'adresse 0 (runtime/process.c).
    const char *minidump = map_tool ? NULL : rt_minidump_argument(argc - first, argv + first);
    uint32_t count = 1, vector = rt_alloc(((uint32_t)(argc - first) + 5) * 4);
    if (!vector) { fputs("guest argument vector allocation failed\n", stderr); rt_destroy(); return 2; }
    W32(vector, guest_string(program_name));
    for (int i = first; i < argc; ++i)
        if (renderer >= 0 || strcmp(argv[i], "-vulkan")) W32(vector + count++ * 4, guest_string(argv[i]));
    if (renderer > 0) W32(vector + count++ * 4, guest_string("-vulkan"));
    if (sampler) { W32(vector + count++ * 4, guest_string(sampler)); fprintf(stderr, "renderer: OpenGL without sampler objects, adding %s\n", sampler); }
    if (minidump) W32(vector + count++ * 4, guest_string(minidump));
    W32(vector + count * 4, 0);
    {   // /proc/self/cmdline : arguments invités séparés par des NUL
        size_t total = 0;
        for (uint32_t i = 0; i < count; ++i) total += strlen((const char *)GPTR(R32(vector + i * 4))) + 1;
        char *joined = malloc(total ? total : 1), *at = joined;
        for (uint32_t i = 0; joined && i < count; ++i) {
            size_t n = strlen((const char *)GPTR(R32(vector + i * 4))) + 1;
            memcpy(at, GPTR(R32(vector + i * 4)), n); at += n;
        }
        if (joined) { rt_procfs_set_cmdline(joined, total); free(joined); }
    }
    rt_loader_set_arguments(count, vector, 0);
    P2_TRACE("main: arguments set");
    const GuestModule *program = rt_find_module(program_name);
    P2_TRACE(program ? "main: program module found" : "main: program module NOT found");
    int launchable = !strcmp(program_name, "vvis_dll.so") || !strcmp(program_name, "vrad_dll.so");
    // Compiler DLLs normally have small ELF launchers. Reuse the native tool
    // host and call their ILaunchableDLL::main instead of executing Linux ELF.
    const GuestModule *tool_host = launchable ? rt_find_module("vbsp_linux") : NULL;
    if ((tool_host && rt_initialize_module(&cpu, tool_host)) || !program || rt_initialize_module(&cpu, program)) {
        P2_TRACE("main: module initialization failed");
        fprintf(stderr, "guest program %s: module initialization failed\n", program_name);
        rt_destroy(); return 2;
    }
    uint32_t entry, args[3];
    if (launchable) {
        // vrad_linux initializes tier0's command-line singleton before calling
        // the DLL (VVIS repeats this itself, VRAD relies on its launcher).
        const GuestModule *tier0 = rt_find_module("libtier0.so");
        uint32_t command_line = rt_invoke(&cpu, rt_module_symbol(tier0, "CommandLine"), NULL, 0);
        uint32_t command_args[] = {command_line, count, vector};
        rt_invoke(&cpu, R32(R32(command_line) + 4), command_args, 3);
        uint32_t factory = rt_module_symbol(program, "CreateInterface");
        uint32_t request[] = {guest_string("launchable_dll_1"), 0};
        uint32_t object = factory ? rt_invoke(&cpu, factory, request, 2) : 0;
        if (!object) { fprintf(stderr, "%s: launchable_dll_1 unavailable\n", program_name); rt_destroy(); return 2; }
        entry = R32(R32(object));
        args[0] = object; args[1] = count; args[2] = vector;
    } else {
        entry = rt_module_symbol(program, "main");
        args[0] = count; args[1] = vector; args[2] = 0;
    }
    if (!entry) { P2_TRACE("main: guest main is absent"); fprintf(stderr, "%s: main is absent\n", program_name); rt_destroy(); return 2; }
    P2_TRACE("main: entering the guest's main");
    fprintf(stderr, "entering recompiled %s main at %#x\n", program_name, entry);
    int result = (int32_t)rt_invoke(&cpu, entry, args, 3);
    fprintf(stderr, "%s main returned %d\n", program_name, result);
#ifdef __PROSPERO__
    { char note[80]; snprintf(note, sizeof note, "main: the guest's main returned %d", result); P2_TRACE(note); }
#endif
    rt_destroy();
    return result;
}
