#pragma once

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static inline int greatest_color_enabled_for(FILE *out) {
    const char *force = getenv("FORCE_COLOR");
    if (force && *force && strcmp(force, "0") != 0) return 1;
    const char *no_color = getenv("NO_COLOR");
    if (no_color) return 0;
    if (!isatty(fileno(out))) return 0;
    const char *term = getenv("TERM");
    if (!term || strcmp(term, "dumb") == 0) return 0;
    return 1;
}

static inline int greatest_color_fprintf(FILE *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return len;

    if (!greatest_color_enabled_for(out)) {
        fputs(buf, out);
        return len;
    }

    const char *reset     = "\x1b[0m";
    const char *green     = "\x1b[32m";
    const char *red       = "\x1b[31m";
    const char *yellow    = "\x1b[33m";
    const char *bold_cyan = "\x1b[36;1m";
    const char *bold      = "\x1b[1m";

    const char *color = NULL;
    const char *msg = buf;
    while (*msg == '\n' || *msg == '\r') msg++;

    if      (strncmp(msg, "PASS ", 5) == 0) color = green;
    else if (strncmp(msg, "FAIL ", 5) == 0) color = red;
    else if (strncmp(msg, "SKIP ", 5) == 0) color = yellow;
    else if (strncmp(msg, "* Suite ", 8) == 0) color = bold_cyan;
    else if (strcmp(msg, ".") == 0 || strcmp(msg, ".\n") == 0) color = green;
    else if (strcmp(msg, "F") == 0 || strcmp(msg, "F\n") == 0) color = red;
    else if (strcmp(msg, "s") == 0 || strcmp(msg, "s\n") == 0) color = yellow;
    else if (strncmp(msg, "Total:", 6) == 0) color = bold;
    else if (strncmp(msg, "Pass:", 5) == 0)  color = bold;

    if (color) {
        fputs(color, out);
        fputs(buf, out);
        fputs(reset, out);
    } else {
        fputs(buf, out);
    }
    return len;
}

// Define the hook macro before including greatest.h
#define GREATEST_FPRINTF greatest_color_fprintf
