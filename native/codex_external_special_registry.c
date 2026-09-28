#include <windows.h>
#include <string.h>

#define user_initialize codex_unused_user_initialize
#define user_terminate codex_unused_user_terminate
#define ProUITimerCreate codex_noop_timer_create
#define ProUITimerDestroy codex_noop_timer_destroy
#define ProUIDialogTimerStart codex_noop_timer_start
#define ProUIDialogTimerStop codex_noop_timer_stop
#include "creo_safe_resident_dll.c"
#undef ProUIDialogTimerStop
#undef ProUIDialogTimerStart
#undef ProUITimerDestroy
#undef ProUITimerCreate
#undef user_terminate
#undef user_initialize

ProError codex_noop_timer_create(
    ProUITimerAction action, ProAppData appdata, ProName timer_name,
    ProUITimerID *timer_id)
{
    (void)action; (void)appdata; (void)timer_name; (void)timer_id;
    return PRO_TK_NO_ERROR;
}
ProError codex_noop_timer_destroy(ProUITimerID timer_id)
{
    (void)timer_id; return PRO_TK_NO_ERROR;
}
ProError codex_noop_timer_start(
    char *dialog, ProUITimerID timer_id, int duration,
    ProBoolean write_in_trail_file)
{
    (void)dialog; (void)timer_id; (void)duration; (void)write_in_trail_file;
    return PRO_TK_NO_ERROR;
}
ProError codex_noop_timer_stop(ProUITimerID timer_id)
{
    (void)timer_id; return PRO_TK_NO_ERROR;
}

int codex_external_special_can_handle(const char *request)
{
    return request != NULL &&
        (strncmp(request, "DIMBATCH|", 9) == 0 ||
         strncmp(request, "DIMSET|", 7) == 0 ||
         strncmp(request, "COMPSWITCH|", 11) == 0 ||
         strncmp(request, "PATTERNSETID|", 13) == 0 ||
         strncmp(request, "PATTERNSET|", 11) == 0);
}

int codex_external_special_execute(
    const char *request, char *response, size_t response_capacity)
{
    static int initialized = 0;
    if (request == NULL || response == NULL || response_capacity < 2)
        return 0;
    if (!initialized)
    {
        InitializeCriticalSection(&resident_lock);
        resident_response_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (resident_response_event == NULL)
            return 0;
        initialized = 1;
    }
    ResetEvent(resident_response_event);
    resident_process_request(request);
    EnterCriticalSection(&resident_lock);
    strncpy_s(response, response_capacity, resident_response, _TRUNCATE);
    LeaveCriticalSection(&resident_lock);
    return response[0] != '\0';
}
