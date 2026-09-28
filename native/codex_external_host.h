#ifndef CODEX_EXTERNAL_HOST_H
#define CODEX_EXTERNAL_HOST_H

#include <ProToolkit.h>
#include <ProCore.h>

ProError codex_host_connect(
    char *session, char *display, char *user, char *textpath,
    ProBoolean allow_random, unsigned int timeout_sec,
    ProBoolean *random_choice, ProProcessHandle *handle);
ProError codex_host_disconnect(
    ProProcessHandle *handle, unsigned int timeout_sec);

#endif
