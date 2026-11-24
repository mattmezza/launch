#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launch.h"

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    if (argc == 4 && strcmp(argv[1], "--serve-selection") == 0)
        return clipboard_serve(atoi(argv[2]), argv[3]);
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    bool compatible = strcmp(name, "dmenu") == 0 || strcmp(name, "launch-dmenu") == 0;
    if (!compatible && argc > 1 && strcmp(argv[1], "workflow") == 0) {
        if (argc < 3 || strcmp(argv[2], "--help") == 0) {
            usage(argc < 3 ? stderr : stdout, false);
            return argc < 3;
        }
        return workflow_run(argv[2], argc - 3, argv + 3);
    }
    if (argc > 1 && strcmp(argv[1], "--dmenu") == 0) {
        compatible = true;
        argc--;
        argv++;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            puts(compatible ? "dmenu-launch-0.1.0" : "launch 0.1.0");
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout, compatible);
            return 0;
        }
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-fn") == 0 ||
            strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-m") == 0 ||
            strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "-nb") == 0 ||
            strcmp(argv[i], "-nf") == 0 || strcmp(argv[i], "-sb") == 0 ||
            strcmp(argv[i], "-sf") == 0 || strcmp(argv[i], "-ob") == 0 ||
            strcmp(argv[i], "-of") == 0)
            i++;
    }

    Settings settings;
    settings_init(&settings, compatible);
    Mode mode;
    if (!settings_args(&settings, argc, argv, &mode)) {
        usage(stderr, compatible);
        settings_clear(&settings);
        return 1;
    }
    settings_clear(&settings);
    settings_init(&settings, compatible);
    int status = x11_run(&settings, mode, argc, argv);
    settings_clear(&settings);
    return status;
}
