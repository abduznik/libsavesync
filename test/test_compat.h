#ifndef TEST_COMPAT_H
#define TEST_COMPAT_H

/*
 * Portable test helpers — mkdtemp equivalent for Windows/MinGW.
 * Include this header in any test file that needs temp directory creation.
 */

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <stdio.h>

/*
 * Portable mkdtemp: creates a unique directory from a template.
 * Template must end with at least 6 X's (same as POSIX mkdtemp).
 * Returns the template pointer on success, NULL on failure.
 */
static char *sv_mkdtemp(char *template) {
    /* Find the trailing X run */
    size_t len = strlen(template);
    if (len < 6) return NULL;
    char *xs = template + len - 6;

    /* Get a temp base path if template starts with /tmp or similar */
    char base[MAX_PATH];
    const char *slash = strrchr(template, '/');
    if (!slash) slash = strrchr(template, '\\');
    if (slash) {
        size_t prefix_len = slash - template;
        if (prefix_len >= sizeof(base)) return NULL;
        strncpy(base, template, prefix_len);
        base[prefix_len] = '\0';
    } else {
        /* No directory component — use current dir */
        strcpy(base, ".");
    }

    /* Ensure base directory exists */
    _mkdir(base);

    /* Try random suffixes until we get a unique name */
    for (int attempt = 0; attempt < 100; attempt++) {
        /* Generate 6 random alphanumeric chars */
        char suffix[7];
        for (int i = 0; i < 6; i++) {
            int r = rand() % 36;
            suffix[i] = r < 10 ? ('0' + r) : ('a' + r - 10);
        }
        suffix[6] = '\0';
        memcpy(xs, suffix, 6);

        /* Try to create the directory */
        if (_mkdir(template) == 0) {
            return template;
        }
        /* EEXIST means collision — try again */
    }
    return NULL; /* failed after 100 attempts */
}

#else
#include <stdlib.h>

/* On POSIX, mkdtemp is available directly */
#define sv_mkdtemp mkdtemp

#endif

#endif /* TEST_COMPAT_H */
