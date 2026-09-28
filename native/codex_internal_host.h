#ifndef CODEX_INTERNAL_HOST_H
#define CODEX_INTERNAL_HOST_H

#include <ProToolkit.h>
#include <ProCore.h>

ProError codex_internal_host_connect(
    char *session, char *display, char *user, char *textpath,
    ProBoolean allow_random, unsigned int timeout_sec,
    ProBoolean *random_choice, ProProcessHandle *handle);

ProError codex_internal_host_disconnect(
    ProProcessHandle *handle, unsigned int timeout_sec);

ProError codex_internal_host_connection_start(
    char *proe_path, char *prodev_text_path, ProProcessHandle *handle);

#endif
