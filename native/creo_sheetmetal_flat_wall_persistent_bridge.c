#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <wincrypt.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProUtil.h>
#include "codex_external_host.h"

int codex_external_command_file_execute(const wchar_t *command_path);
int codex_external_special_can_handle(const char *request);
int codex_external_special_execute(
    const char *request, char *response, size_t response_capacity);

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

static ProProcessHandle persistent_process;
static int persistent_connected = 0;
static int persistent_spawned_by_creo = 0;
static int persistent_registry_managed = 0;
static const char *persistent_registry_startup = "creo_spawn";
static volatile LONG persistent_session_lost = 0;
static DWORD persistent_target_process_id = 0;
static HANDLE persistent_target_process = NULL;
static ULONGLONG persistent_target_start_time = 0;
static wchar_t persistent_pipe_name[256];
static wchar_t persistent_mutex_name[256];
static wchar_t persistent_registry_path[MAX_PATH];
static char persistent_session_nonce[65];
static char persistent_session_fingerprint[33];

int user_initialize(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return 0;
}

void user_terminate(void)
{
    InterlockedExchange(&persistent_session_lost, 1);
}

static ProError persistent_engineer_connect(
    char *proe_session_id,
    char *display,
    char *user,
    char *textpath,
    ProBoolean allow_random,
    unsigned int timeout_sec,
    ProBoolean *random_choice,
    ProProcessHandle *p_handle)
{
    (void)proe_session_id;
    (void)display;
    (void)user;
    (void)textpath;
    (void)allow_random;
    (void)timeout_sec;
    if (!persistent_connected || p_handle == NULL)
        return PRO_TK_COMM_ERROR;
    if (random_choice != NULL)
        *random_choice = PRO_B_FALSE;
    *p_handle = persistent_process;
    return PRO_TK_NO_ERROR;
}

static ProError persistent_engineer_disconnect(
    ProProcessHandle *p_handle,
    unsigned int timeout_sec)
{
    (void)p_handle;
    (void)timeout_sec;
    return persistent_connected ? PRO_TK_NO_ERROR : PRO_TK_COMM_ERROR;
}

ProError codex_host_connect(
    char *session, char *display, char *user, char *textpath,
    ProBoolean allow_random, unsigned int timeout_sec,
    ProBoolean *random_choice, ProProcessHandle *handle)
{
    return persistent_engineer_connect(
        session, display, user, textpath, allow_random, timeout_sec,
        random_choice, handle);
}

ProError codex_host_disconnect(
    ProProcessHandle *handle, unsigned int timeout_sec)
{
    return persistent_engineer_disconnect(handle, timeout_sec);
}

#define ProEngineerConnect persistent_engineer_connect
#define ProEngineerDisconnect persistent_engineer_disconnect
#define wmain flat_wall_command_wmain
#include "creo_sheetmetal_flat_wall_bridge.c"
#undef wmain
#undef ProEngineerDisconnect
#undef ProEngineerConnect

#define MAX_COMMAND_ARGS 32
#define MAX_COMMAND_LINE 2048
#define WATCHDOG_INTERVAL_MS 1000

typedef struct persistent_feature_context
{
    FILE *out;
    int count;
} PersistentFeatureContext;

static const char *persistent_feature_type_name(ProFeattype type)
{
    switch (type)
    {
        case PRO_FEAT_PROTRUSION: return "protrusion";
        case PRO_FEAT_CUT: return "cut";
        case PRO_FEAT_HOLE: return "hole";
        case PRO_FEAT_ROUND: return "round";
        case PRO_FEAT_CHAMFER: return "chamfer";
        case PRO_FEAT_SHELL: return "shell";
        case PRO_FEAT_DATUM: return "datum";
        case PRO_FEAT_DATUM_SURF: return "datum_surface";
        case PRO_FEAT_DATUM_AXIS: return "datum_axis";
        case PRO_FEAT_DATUM_POINT: return "datum_point";
        case PRO_FEAT_CSYS: return "coordinate_system";
        case PRO_FEAT_ZONE: return "zone";
        default: return "other";
    }
}

static const char *persistent_feature_status_name(ProFeatStatus status)
{
    switch (status)
    {
        case PRO_FEAT_ACTIVE: return "active";
        case PRO_FEAT_INACTIVE: return "inactive";
        case PRO_FEAT_FAMTAB_SUPPRESSED: return "family_table_suppressed";
        case PRO_FEAT_SIMP_REP_SUPPRESSED: return "simplified_rep_suppressed";
        case PRO_FEAT_PROG_SUPPRESSED: return "program_suppressed";
        case PRO_FEAT_SUPPRESSED: return "suppressed";
        case PRO_FEAT_UNREGENERATED: return "unregenerated";
        default: return "invalid";
    }
}

static void persistent_write_id_array(FILE *out, int *ids, int count)
{
    int index;
    fputc('[', out);
    for (index = 0; index < count; ++index)
    {
        if (index > 0)
            fputc(',', out);
        fprintf(out, "%d", ids[index]);
    }
    fputc(']', out);
}

static ProError persistent_feature_visit(
    ProFeature *feature, ProError filter_status, ProAppData app_data)
{
    PersistentFeatureContext *context =
        (PersistentFeatureContext *)app_data;
    ProName name = L"";
    ProFeattype type = -1;
    ProFeatStatus status = PRO_FEAT_INVALID;
    ProBoolean visible = PRO_B_FALSE;
    int *parents = NULL;
    int *children = NULL;
    int parent_count = 0;
    int child_count = 0;
    ProError name_status;
    ProError type_status;
    ProError feature_status;
    ProError visible_status;
    ProError parents_status;
    ProError children_status;

    (void)filter_status;
    name_status = ProModelitemNameGet((ProModelitem *)feature, name);
    type_status = ProFeatureTypeGet(feature, &type);
    feature_status = ProFeatureStatusGet(feature, &status);
    visible_status = ProFeatureVisibilityGet(feature, &visible);
    parents_status = ProFeatureParentsGet(feature, &parents, &parent_count);
    children_status = ProFeatureChildrenGet(feature, &children, &child_count);
    if (context->count > 0)
        fputc(',', context->out);
    fprintf(context->out, "{\"id\":%d,\"name\":", feature->id);
    if (name_status == PRO_TK_NO_ERROR)
        json_wide(context->out, name);
    else
        fputs("null", context->out);
    fprintf(context->out,
        ",\"type\":\"%s\",\"type_code\":%d,"
        "\"status\":\"%s\",\"status_code\":%d,\"visible\":%s,"
        "\"parent_ids\":",
        type_status == PRO_TK_NO_ERROR ?
            persistent_feature_type_name(type) : "other",
        type_status == PRO_TK_NO_ERROR ? (int)type : -1,
        feature_status == PRO_TK_NO_ERROR ?
            persistent_feature_status_name(status) : "invalid",
        feature_status == PRO_TK_NO_ERROR ? (int)status : -1,
        visible_status == PRO_TK_NO_ERROR && visible == PRO_B_TRUE ?
            "true" : "false");
    persistent_write_id_array(
        context->out, parents,
        parents_status == PRO_TK_NO_ERROR ? parent_count : 0);
    fputs(",\"child_ids\":", context->out);
    persistent_write_id_array(
        context->out, children,
        children_status == PRO_TK_NO_ERROR ? child_count : 0);
    fputc('}', context->out);
    if (parents != NULL)
        ProArrayFree((ProArray *)&parents);
    if (children != NULL)
        ProArrayFree((ProArray *)&children);
    ++context->count;
    return PRO_TK_NO_ERROR;
}

static int persistent_feature_snapshot(const wchar_t *output_path)
{
    FILE *out = NULL;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProMdlType model_type;
    ProError status;
    PersistentFeatureContext context;

    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;
    status = ProMdlCurrentGet(&model);
    if (status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            "{\"ok\":false,\"readonly\":true,\"stage\":\"current_model\","
            "\"error_code\":%d}\n", status);
        fclose(out);
        return 1;
    }
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            "{\"ok\":false,\"readonly\":true,\"stage\":\"model_name\","
            "\"error_code\":%d}\n", status);
        fclose(out);
        return 1;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART)
    {
        fprintf(out,
            "{\"ok\":false,\"readonly\":true,"
            "\"stage\":\"model_type_guard\",\"error_code\":%d}\n",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        fclose(out);
        return 1;
    }
    fputs("{\"ok\":true,\"readonly\":true,\"persistent\":true,"
          "\"model\":", out);
    json_wide(out, model_name);
    fputs(",\"features\":[", out);
    context.out = out;
    context.count = 0;
    status = ProSolidFeatVisit(
        (ProSolid)model, persistent_feature_visit, NULL,
        (ProAppData)&context);
    fprintf(out,
        "],\"feature_count\":%d,\"visit_status\":%d}\n",
        context.count, status);
    fclose(out);
    return status == PRO_TK_NO_ERROR || status == PRO_TK_E_NOT_FOUND ? 0 : 1;
}

static int persistent_hex_token_valid(
    const char *value, size_t expected_length)
{
    size_t index;
    if (value == NULL || strlen(value) != expected_length)
        return 0;
    for (index = 0; index < expected_length; ++index)
    {
        char current = value[index];
        if (!((current >= '0' && current <= '9') ||
              (current >= 'a' && current <= 'f') ||
              (current >= 'A' && current <= 'F')))
            return 0;
    }
    return 1;
}

static int persistent_environment_read(char *session_id, size_t session_count)
{
    char target_process[32];
    char *end = NULL;
    unsigned long parsed_process;
    DWORD length;

    length = GetEnvironmentVariableA(
        "CREO_CONNECT_ID", session_id, (DWORD)session_count);
    if (length == 0 || length >= session_count)
        return 0;
    length = GetEnvironmentVariableA(
        "CREO_TARGET_PROCESS_ID", target_process,
        (DWORD)sizeof(target_process));
    if (length == 0 || length >= sizeof(target_process))
        return 0;
    parsed_process = strtoul(target_process, &end, 10);
    if (end == target_process || *end != '\0' || parsed_process == 0 ||
        parsed_process > MAXDWORD)
        return 0;
    persistent_target_process_id = (DWORD)parsed_process;

    length = GetEnvironmentVariableA(
        "CREO_SESSION_NONCE", persistent_session_nonce,
        (DWORD)sizeof(persistent_session_nonce));
    if (length == 0 || length >= sizeof(persistent_session_nonce) ||
        !persistent_hex_token_valid(persistent_session_nonce, 64))
        return 0;
    length = GetEnvironmentVariableA(
        "CREO_SESSION_FINGERPRINT", persistent_session_fingerprint,
        (DWORD)sizeof(persistent_session_fingerprint));
    if (length == 0 || length >= sizeof(persistent_session_fingerprint) ||
        !persistent_hex_token_valid(persistent_session_fingerprint, 32))
        return 0;

    length = GetEnvironmentVariableW(
        L"CREO_RESIDENT_PIPE", persistent_pipe_name,
        (DWORD)(sizeof(persistent_pipe_name) / sizeof(persistent_pipe_name[0])));
    if (length == 0 ||
        length >= sizeof(persistent_pipe_name) / sizeof(persistent_pipe_name[0]) ||
        wcsncmp(persistent_pipe_name, L"\\\\.\\pipe\\codex_creo_", 20) != 0)
        return 0;
    length = GetEnvironmentVariableW(
        L"CREO_RESIDENT_MUTEX", persistent_mutex_name,
        (DWORD)(sizeof(persistent_mutex_name) / sizeof(persistent_mutex_name[0])));
    if (length == 0 ||
        length >= sizeof(persistent_mutex_name) / sizeof(persistent_mutex_name[0]) ||
        wcsncmp(persistent_mutex_name, L"Local\\CodexCreoResident_", 24) != 0)
        return 0;
    return 1;
}

static void persistent_target_process_open(void)
{
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;
    ULARGE_INTEGER converted;

    persistent_target_process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, persistent_target_process_id);
    if (persistent_target_process == NULL)
        persistent_target_process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_INFORMATION,
            FALSE, persistent_target_process_id);
    if (persistent_target_process == NULL)
        return;
    if (GetProcessTimes(
            persistent_target_process, &created, &exited, &kernel, &user))
    {
        converted.LowPart = created.dwLowDateTime;
        converted.HighPart = created.dwHighDateTime;
        persistent_target_start_time = converted.QuadPart;
    }
}

static DWORD persistent_creo_ancestor_process_id(void)
{
    HANDLE snapshot;
    DWORD current = GetCurrentProcessId();
    DWORD fallback = 0;
    int depth;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    for (depth = 0; depth < 12; ++depth)
    {
        PROCESSENTRY32W entry;
        int found = 0;
        memset(&entry, 0, sizeof(entry));
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (entry.th32ProcessID == current)
                {
                    current = entry.th32ParentProcessID;
                    if (fallback == 0)
                        fallback = current;
                    found = 1;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        if (!found || current == 0)
            break;
        memset(&entry, 0, sizeof(entry));
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (entry.th32ProcessID == current)
                {
                    if (_wcsicmp(entry.szExeFile, L"xtop.exe") == 0)
                    {
                        CloseHandle(snapshot);
                        return current;
                    }
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
    }
    CloseHandle(snapshot);
    return fallback;
}

static int persistent_random_identity_create(void)
{
    HCRYPTPROV provider = 0;
    BYTE random_bytes[32];
    size_t index;
    if (!CryptAcquireContextW(
            &provider, NULL, NULL, PROV_RSA_FULL,
            CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return 0;
    if (!CryptGenRandom(provider, (DWORD)sizeof(random_bytes), random_bytes))
    {
        CryptReleaseContext(provider, 0);
        return 0;
    }
    CryptReleaseContext(provider, 0);
    for (index = 0; index < sizeof(random_bytes); ++index)
    {
        sprintf_s(
            persistent_session_nonce + index * 2,
            sizeof(persistent_session_nonce) - index * 2,
            "%02x", random_bytes[index]);
    }
    memcpy(
        persistent_session_fingerprint,
        persistent_session_nonce,
        sizeof(persistent_session_fingerprint) - 1);
    persistent_session_fingerprint[
        sizeof(persistent_session_fingerprint) - 1] = '\0';
    return 1;
}

static int persistent_identity_prepare_for_process(DWORD process_id)
{
    wchar_t temp_path[MAX_PATH];
    DWORD temp_length;

    persistent_target_process_id = process_id;
    if (persistent_target_process_id == 0 ||
        !persistent_random_identity_create())
        return 0;
    persistent_target_process_open();
    swprintf_s(
        persistent_pipe_name,
        sizeof(persistent_pipe_name) / sizeof(persistent_pipe_name[0]),
        L"\\\\.\\pipe\\codex_creo_spawn_%lu_%.24S",
        (unsigned long)persistent_target_process_id,
        persistent_session_fingerprint);
    swprintf_s(
        persistent_mutex_name,
        sizeof(persistent_mutex_name) / sizeof(persistent_mutex_name[0]),
        L"Local\\CodexCreoSpawnResident_%lu",
        (unsigned long)persistent_target_process_id);
    temp_length = GetTempPathW(
        (DWORD)(sizeof(temp_path) / sizeof(temp_path[0])), temp_path);
    if (temp_length == 0 ||
        temp_length >= sizeof(temp_path) / sizeof(temp_path[0]))
        return 0;
    swprintf_s(
        persistent_registry_path,
        sizeof(persistent_registry_path) / sizeof(persistent_registry_path[0]),
        L"%scodex-creo-resident-%lu.json",
        temp_path, (unsigned long)persistent_target_process_id);
    return 1;
}

static int persistent_spawn_identity_prepare(void)
{
    return persistent_identity_prepare_for_process(
        persistent_creo_ancestor_process_id());
}

static DWORD persistent_single_creo_process_id(void)
{
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    DWORD process_id = 0;
    int match_count = 0;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"xtop.exe") == 0)
            {
                process_id = entry.th32ProcessID;
                ++match_count;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return match_count == 1 ? process_id : 0;
}

static DWORD persistent_wait_for_single_creo_process_id(DWORD timeout_ms)
{
    DWORD started_at = GetTickCount();
    DWORD process_id;
    do
    {
        process_id = persistent_single_creo_process_id();
        if (process_id != 0)
            return process_id;
        Sleep(100);
    } while (GetTickCount() - started_at < timeout_ms);
    return 0;
}

static int persistent_spawn_registry_write(void)
{
    FILE *out = NULL;
    if (_wfopen_s(&out, persistent_registry_path, L"wb") != 0 || out == NULL)
        return 0;
    fprintf(out,
        "{\"protocol\":\"codex-creo-session-v2\","
        "\"startup\":\"%s\",\"pipe_name\":",
        persistent_registry_startup);
    json_wide(out, persistent_pipe_name);
    fprintf(out,
        ",\"target_creo_process_id\":%lu,"
        "\"worker_process_id\":%lu,"
        "\"session_nonce\":\"%s\","
        "\"session_fingerprint\":\"%s\",",
        (unsigned long)persistent_target_process_id,
        (unsigned long)GetCurrentProcessId(),
        persistent_session_nonce,
        persistent_session_fingerprint);
    if (persistent_target_start_time != 0)
        fprintf(out,
            "\"target_creo_start_time_100ns\":\"%I64u\"}\n",
            persistent_target_start_time);
    else
        fputs("\"target_creo_start_time_100ns\":null}\n", out);
    fclose(out);
    return 1;
}

static int persistent_session_lost_status(ProError status)
{
    return status == PRO_TK_COMM_ERROR || status == PRO_TK_E_NOT_FOUND;
}

static ProError persistent_health_check(void)
{
    ProPath working_directory;
    return ProDirectoryCurrentGet(working_directory);
}

static int persistent_session_is_alive(void)
{
    ProError health = persistent_health_check();
    if (persistent_target_process != NULL &&
        WaitForSingleObject(persistent_target_process, 0) == WAIT_OBJECT_0)
    {
        InterlockedExchange(&persistent_session_lost, 1);
        return 0;
    }
    if (persistent_session_lost_status(health))
    {
        InterlockedExchange(&persistent_session_lost, 1);
        return 0;
    }
    return 1;
}

static int persistent_wait_overlapped(
    HANDLE pipe, OVERLAPPED *operation, DWORD *bytes_transferred)
{
    for (;;)
    {
        DWORD wait_status = WaitForSingleObject(
            operation->hEvent, WATCHDOG_INTERVAL_MS);
        if (wait_status == WAIT_OBJECT_0)
        {
            return GetOverlappedResult(
                pipe, operation, bytes_transferred, FALSE) ? 1 : 0;
        }
        if (wait_status != WAIT_TIMEOUT || !persistent_session_is_alive())
        {
            CancelIoEx(pipe, operation);
            return 0;
        }
    }
}

static int persistent_accept_client(HANDLE pipe)
{
    OVERLAPPED operation;
    DWORD ignored = 0;
    BOOL connected;
    DWORD error;

    memset(&operation, 0, sizeof(operation));
    operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (operation.hEvent == NULL)
        return 0;
    connected = ConnectNamedPipe(pipe, &operation);
    if (connected)
    {
        CloseHandle(operation.hEvent);
        return 1;
    }
    error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED)
    {
        CloseHandle(operation.hEvent);
        return 1;
    }
    if (error == ERROR_IO_PENDING)
        connected = persistent_wait_overlapped(
            pipe, &operation, &ignored) ? TRUE : FALSE;
    else
        connected = FALSE;
    CloseHandle(operation.hEvent);
    return connected ? 1 : 0;
}

static int persistent_read_request(
    HANDLE pipe, char *request, DWORD capacity, DWORD *bytes_read)
{
    OVERLAPPED operation;
    BOOL completed;
    DWORD error;

    memset(&operation, 0, sizeof(operation));
    operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (operation.hEvent == NULL)
        return 0;
    completed = ReadFile(
        pipe, request, capacity, bytes_read, &operation);
    if (!completed)
    {
        error = GetLastError();
        if (error == ERROR_IO_PENDING)
            completed = persistent_wait_overlapped(
                pipe, &operation, bytes_read) ? TRUE : FALSE;
    }
    CloseHandle(operation.hEvent);
    return completed && *bytes_read > 0;
}

static void persistent_cleanup_own_comm_helpers(void)
{
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    DWORD own_pid = GetCurrentProcessId();

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ParentProcessID == own_pid &&
                _wcsicmp(entry.szExeFile, L"pro_comm_msg.exe") == 0)
            {
                HANDLE child = OpenProcess(
                    SYNCHRONIZE | PROCESS_TERMINATE,
                    FALSE,
                    entry.th32ProcessID);
                if (child != NULL)
                {
                    if (WaitForSingleObject(child, 1000) == WAIT_TIMEOUT)
                        TerminateProcess(child, 0);
                    CloseHandle(child);
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

static void persistent_startup_status_write(
    const char *stage, ProError status)
{
    char status_path[2048];
    DWORD path_length;
    FILE *out = NULL;

    path_length = GetEnvironmentVariableA(
        "CREO_BRIDGE_STARTUP_STATUS",
        status_path,
        (DWORD)sizeof(status_path));
    if (path_length == 0 || path_length >= sizeof(status_path))
        return;
    if (fopen_s(&out, status_path, "wb") != 0 || out == NULL)
        return;
    fprintf(out,
        "{\"stage\":\"%s\",\"error_code\":%d,\"process_id\":%lu}\n",
        stage, status, (unsigned long)GetCurrentProcessId());
    fclose(out);
}

static unsigned int persistent_connect_timeout_seconds(void)
{
    char value[32];
    char *end = NULL;
    unsigned long parsed;
    DWORD length = GetEnvironmentVariableA(
        "CREO_CONNECT_TIMEOUT_SEC", value, (DWORD)sizeof(value));

    if (length == 0 || length >= sizeof(value))
        return 6;
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0')
        return 6;
    if (parsed < 2)
        parsed = 2;
    if (parsed > 20)
        parsed = 20;
    return (unsigned int)parsed;
}

static void trim_line(wchar_t *line)
{
    size_t length;
    if (line == NULL)
        return;
    length = wcslen(line);
    while (length > 0 &&
           (line[length - 1] == L'\r' || line[length - 1] == L'\n'))
        line[--length] = L'\0';
}

static int utf8_to_wide(const char *source, wchar_t *target, size_t target_count)
{
    int converted;
    if (source == NULL || target == NULL || target_count == 0)
        return 0;
    converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
        target, (int)target_count);
    return converted > 0;
}

static char *persistent_authenticated_command(char *request)
{
    char *nonce;
    char *separator;
    if (request == NULL || strncmp(request, "AUTH|", 5) != 0)
        return NULL;
    nonce = request + 5;
    separator = strchr(nonce, '|');
    if (separator == NULL)
        return NULL;
    *separator = '\0';
    if (strcmp(nonce, persistent_session_nonce) != 0)
        return NULL;
    return separator + 1;
}

static int command_file_execute(const wchar_t *command_path)
{
    FILE *command = NULL;
    wchar_t line[MAX_COMMAND_LINE];
    wchar_t *arguments[MAX_COMMAND_ARGS];
    int argument_count = 1;
    int index;
    int result;

    memset(arguments, 0, sizeof(arguments));
    arguments[0] = _wcsdup(L"creo_sheetmetal_flat_wall_bridge.exe");
    if (arguments[0] == NULL)
        return 2;
    if (_wfopen_s(&command, command_path, L"rt, ccs=UTF-8") != 0 ||
        command == NULL)
    {
        free(arguments[0]);
        return 2;
    }
    while (argument_count < MAX_COMMAND_ARGS &&
           fgetws(line, (int)(sizeof(line) / sizeof(line[0])), command) != NULL)
    {
        trim_line(line);
        arguments[argument_count] = _wcsdup(line);
        if (arguments[argument_count] == NULL)
            break;
        ++argument_count;
    }
    fclose(command);
    result = flat_wall_command_wmain(argument_count, arguments);
    for (index = 0; index < argument_count; ++index)
        free(arguments[index]);
    return result;
}

static void pipe_reply(HANDLE pipe, const char *response)
{
    OVERLAPPED operation;
    DWORD written = 0;
    BOOL completed;
    memset(&operation, 0, sizeof(operation));
    operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (operation.hEvent == NULL)
        return;
    completed = WriteFile(
        pipe, response, (DWORD)strlen(response), &written, &operation);
    if (!completed && GetLastError() == ERROR_IO_PENDING &&
        WaitForSingleObject(operation.hEvent, 5000) == WAIT_OBJECT_0)
        completed = GetOverlappedResult(
            pipe, &operation, &written, FALSE);
    if (completed)
        FlushFileBuffers(pipe);
    CloseHandle(operation.hEvent);
}

static const char *basic_model_type_name(ProMdlType type)
{
    switch (type)
    {
        case PRO_MDL_PART: return "part";
        case PRO_MDL_ASSEMBLY: return "assembly";
        case PRO_MDL_DRAWING: return "drawing";
        case PRO_MDL_MFG: return "manufacturing";
        case PRO_MDL_LAYOUT: return "layout";
        case PRO_MDL_REPORT: return "report";
        case PRO_MDL_MARKUP: return "markup";
        case PRO_MDL_DIAGRAM: return "diagram";
        default: return "other";
    }
}

static int basic_snapshot_execute(const wchar_t *output_path)
{
    FILE *out = NULL;
    ProPath working_directory;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProMdlType model_type = PRO_MDL_UNUSED;
    ProError directory_status;
    ProError current_status;
    ProError name_status;
    ProError type_status;
    ProError outline_status = PRO_TK_INVALID_TYPE;
    Pro3dPnt outline[2] = {{0}};
    ProMatrix matrix = {
        {1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}
    };
    ProSolidOutlExclTypes excludes[] = {
        PRO_OUTL_EXC_DATUM_PLANE, PRO_OUTL_EXC_DATUM_POINT,
        PRO_OUTL_EXC_DATUM_CSYS, PRO_OUTL_EXC_DATUM_AXES,
        PRO_OUTL_EXC_ALL_CRVS
    };

    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;
    directory_status = ProDirectoryCurrentGet(working_directory);
    if (directory_status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            "{\"ok\":false,\"readonly\":true,\"stage\":\"working_directory\","
            "\"error_code\":%d}\n", directory_status);
        fclose(out);
        return 1;
    }
    current_status = ProMdlCurrentGet(&model);
    fputs("{\"ok\":true,\"readonly\":true,\"persistent\":true,"
          "\"session_bound\":true,\"working_directory\":", out);
    json_wide(out, working_directory);
    if (current_status == PRO_TK_BAD_CONTEXT)
    {
        fputs(",\"model_open\":false,\"current_model\":null,"
              "\"scan_scope\":[\"working_directory\"]}\n", out);
        fclose(out);
        return 0;
    }
    if (current_status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            ",\"ok\":false,\"model_open\":false,\"current_model\":null,"
            "\"stage\":\"current_model\",\"error_code\":%d}\n",
            current_status);
        fclose(out);
        return 1;
    }
    name_status = ProMdlNameGet(model, model_name);
    type_status = ProMdlTypeGet(model, &model_type);
    if (name_status != PRO_TK_NO_ERROR || type_status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            ",\"ok\":false,\"model_open\":true,\"current_model\":null,"
            "\"stage\":\"model_identity\",\"name_status\":%d,"
            "\"type_status\":%d}\n", name_status, type_status);
        fclose(out);
        return 1;
    }
    if (model_type == PRO_MDL_PART ||
        model_type == PRO_MDL_ASSEMBLY ||
        model_type == PRO_MDL_MFG)
    {
        outline_status = ProSolidOutlineCompute(
            (ProSolid)model, matrix, excludes,
            (int)(sizeof(excludes) / sizeof(excludes[0])), outline);
    }
    fputs(",\"model_open\":true,\"current_model\":{\"name\":", out);
    json_wide(out, model_name);
    fprintf(out,
        ",\"model_type\":\"%s\",\"model_type_code\":%d,"
        "\"outline_status\":%d,\"outline\":",
        basic_model_type_name(model_type), model_type, outline_status);
    if (outline_status == PRO_TK_NO_ERROR)
    {
        fprintf(out,
            "{\"min\":[%.17g,%.17g,%.17g],"
            "\"max\":[%.17g,%.17g,%.17g],"
            "\"size\":[%.17g,%.17g,%.17g]}",
            outline[0][0], outline[0][1], outline[0][2],
            outline[1][0], outline[1][1], outline[1][2],
            outline[1][0] - outline[0][0],
            outline[1][1] - outline[0][1],
            outline[1][2] - outline[0][2]);
    }
    else
    {
        fputs("null", out);
    }
    fputs("},\"scan_scope\":[\"working_directory\",\"model_identity\","
          "\"model_type\",\"outline\"]}\n", out);
    fclose(out);
    return 0;
}

static int persistent_path_has_extension(
    const wchar_t *path, const wchar_t *extension)
{
    size_t path_length = wcslen(path);
    size_t extension_length = wcslen(extension);
    size_t index;
    if (extension_length > path_length)
        return 0;
    for (index = 0; index + extension_length <= path_length; ++index)
    {
        if (_wcsnicmp(path + index, extension, extension_length) == 0)
            return 1;
    }
    return 0;
}

static BOOL CALLBACK persistent_foreground_creo_window(
    HWND window, LPARAM app_data)
{
    wchar_t title[512];
    int length;
    (void)app_data;
    if (!IsWindowVisible(window))
        return TRUE;
    length = GetWindowTextW(window, title, 512);
    if (length > 0 && wcsstr(title, L"Creo Parametric") != NULL)
    {
        ShowWindow(window, SW_RESTORE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        return FALSE;
    }
    return TRUE;
}

static int persistent_display_execute(
    const wchar_t *output_path,
    const wchar_t *model_path,
    const wchar_t *expected_name)
{
    FILE *out = NULL;
    ProMdl model = NULL;
    ProMdl current = NULL;
    ProMdlName model_name;
    ProMdlName current_name;
    ProMdlfileType file_type = PRO_MDLFILE_PART;
    ProMdlType window_model_type = PRO_PART;
    ProError status;
    ProError activate_status = PRO_TK_NO_ERROR;
    ProError refit_status = PRO_TK_NO_ERROR;
    ProError repaint_status = PRO_TK_NO_ERROR;
    int window_id = -1;

    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;
    if (GetFileAttributesW(model_path) == INVALID_FILE_ATTRIBUTES)
    {
        fputs("{\"ok\":false,\"display_only\":true,"
              "\"stage\":\"model_file\",\"error_code\":-4}\n", out);
        fclose(out);
        return 1;
    }
    if (persistent_path_has_extension(model_path, L".asm"))
    {
        file_type = PRO_MDLFILE_ASSEMBLY;
        window_model_type = PRO_ASSEMBLY;
    }
    else if (!persistent_path_has_extension(model_path, L".prt"))
    {
        fputs("{\"ok\":false,\"display_only\":true,"
              "\"stage\":\"model_file_type\",\"error_code\":-5}\n", out);
        fclose(out);
        return 1;
    }
    status = ProMdlFiletypeLoad(
        (wchar_t *)model_path, file_type, PRO_B_FALSE, &model);
    if (status != PRO_TK_NO_ERROR)
        goto load_failure;
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
        goto name_failure;
    if (_wcsicmp(model_name, expected_name) != 0)
    {
        status = PRO_TK_BAD_CONTEXT;
        goto guard_failure;
    }
    status = ProMdlWindowGet(model, &window_id);
    if (status != PRO_TK_NO_ERROR)
    {
        status = ProObjectwindowMdlnameCreate(
            model_name, window_model_type, &window_id);
        if (status != PRO_TK_NO_ERROR)
            goto create_window_failure;
    }
    status = ProWindowCurrentSet(window_id);
    if (status != PRO_TK_NO_ERROR)
        goto set_window_failure;
    status = ProWindowActivate(window_id);
    if (status != PRO_TK_NO_ERROR)
        goto activate_window_failure;
    status = ProMdlDisplay(model);
    if (status != PRO_TK_NO_ERROR)
        goto display_failure;
    status = ProMdlCurrentGet(&current);
    if (status != PRO_TK_NO_ERROR)
        goto current_failure;
    status = ProMdlNameGet(current, current_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(current_name, expected_name) != 0)
    {
        if (status == PRO_TK_NO_ERROR)
            status = PRO_TK_BAD_CONTEXT;
        goto current_guard_failure;
    }
    if (ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
    {
        activate_status = ProWindowActivate(window_id);
        refit_status = ProWindowRefit(window_id);
        repaint_status = ProWindowRepaint(window_id);
    }
    fputs("{\"ok\":true,\"display_only\":true,"
          "\"saved_or_modified\":false,\"persistent\":true,"
          "\"model\":", out);
    json_wide(out, model_name);
    fprintf(out,
        ",\"window_id\":%d,\"activate_status\":%d,"
        "\"refit_status\":%d,\"repaint_status\":%d}\n",
        window_id, activate_status, refit_status, repaint_status);
    fclose(out);
    EnumWindows(persistent_foreground_creo_window, 0);
    return 0;

load_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"load_model_file\"", out);
    goto display_error;
name_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"model_name\"", out);
    goto display_error;
guard_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"model_name_guard\"", out);
    goto display_error;
create_window_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"create_model_window\"", out);
    goto display_error;
set_window_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"set_current_window\"", out);
    goto display_error;
activate_window_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"activate_model_window\"", out);
    goto display_error;
display_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"display_model\"", out);
    goto display_error;
current_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"current_model_readback\"", out);
    goto display_error;
current_guard_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"current_model_guard\"", out);
display_error:
    fprintf(out, ",\"error_code\":%d}\n", status);
    fclose(out);
    return 1;
}

static char **persistent_wide_arguments_to_ansi(
    int argc, wchar_t **wide_arguments)
{
    char **arguments;
    int index;

    arguments = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (arguments == NULL)
        return NULL;
    for (index = 0; index < argc; index++)
    {
        int required = WideCharToMultiByte(
            CP_ACP, 0, wide_arguments[index], -1,
            NULL, 0, NULL, NULL);
        if (required <= 0)
            break;
        arguments[index] = (char *)calloc((size_t)required, sizeof(char));
        if (arguments[index] == NULL ||
            WideCharToMultiByte(
                CP_ACP, 0, wide_arguments[index], -1,
                arguments[index], required, NULL, NULL) <= 0)
            break;
    }
    if (index != argc)
    {
        while (index >= 0)
            free(arguments[index--]);
        free(arguments);
        return NULL;
    }
    return arguments;
}

static void persistent_arguments_free(int argc, char **arguments)
{
    int index;
    if (arguments == NULL)
        return;
    for (index = 0; index < argc; index++)
        free(arguments[index]);
    free(arguments);
}

int wmain(int argc, wchar_t **wide_arguments)
{
    HANDLE single_instance = NULL;
    ProError status;
    ProBoolean random_choice = PRO_B_FALSE;
    char session_id[PRO_CONNECTID_SIZE];
    char display[256];
    char user[256];
    char textpath[2];
    char start_command[4096];
    DWORD display_length;
    DWORD user_length;
    DWORD start_command_length;
    int running = 1;
    int spawn_request =
        argc > 1 && _wcsicmp(wide_arguments[1], L"-rpc") == 0;
    int launcher_request;

    memset(session_id, 0, sizeof(session_id));
    memset(display, 0, sizeof(display));
    memset(user, 0, sizeof(user));
    memset(textpath, 0, sizeof(textpath));
    memset(start_command, 0, sizeof(start_command));
    display_length = GetEnvironmentVariableA(
        "CREO_CONNECT_DISPLAY", display, (DWORD)sizeof(display));
    user_length = GetEnvironmentVariableA(
        "CREO_CONNECT_USER", user, (DWORD)sizeof(user));
    if (display_length == 0 || display_length >= sizeof(display))
        display[0] = '\0';
    if (user_length == 0 || user_length >= sizeof(user))
        user[0] = '\0';
    start_command_length = GetEnvironmentVariableA(
        "CREO_START_COMMAND", start_command, (DWORD)sizeof(start_command));
    launcher_request =
        start_command_length > 0 && start_command_length < sizeof(start_command);

    if (spawn_request)
    {
        char **arguments = persistent_wide_arguments_to_ansi(
            argc, wide_arguments);
        if (arguments == NULL)
            return 5;
        status = ProAsynchronousMain(argc, arguments);
        persistent_arguments_free(argc, arguments);
        if (status != PRO_TK_NO_ERROR)
            return 4;
        persistent_spawned_by_creo = 1;
        persistent_registry_managed = 1;
        if (!persistent_spawn_identity_prepare())
            return 6;
    }
    else if (launcher_request)
    {
        DWORD launched_process_id;
        if (persistent_single_creo_process_id() != 0)
        {
            persistent_startup_status_write(
                "creo_already_running", PRO_TK_E_IN_USE);
            return 8;
        }
        status = ProEngineerConnectionStart(
            start_command, textpath, &persistent_process);
        if (status != PRO_TK_NO_ERROR)
        {
            persistent_startup_status_write("start_creo", status);
            return 4;
        }
        launched_process_id = persistent_wait_for_single_creo_process_id(30000);
        if (launched_process_id == 0 ||
            !persistent_identity_prepare_for_process(launched_process_id))
        {
            persistent_startup_status_write(
                "started_creo_identity", PRO_TK_E_NOT_FOUND);
            ProEngineerDisconnect(&persistent_process, 1);
            return 6;
        }
        persistent_registry_managed = 1;
        persistent_registry_startup = "bridge_starts_creo";
    }
    else
    {
        if (!persistent_environment_read(session_id, sizeof(session_id)))
        {
            persistent_startup_status_write(
                "session_environment", PRO_TK_BAD_INPUTS);
            return 6;
        }
        persistent_target_process_open();
    }

    single_instance = CreateMutexW(NULL, TRUE, persistent_mutex_name);
    if (single_instance == NULL || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        persistent_startup_status_write("single_instance", PRO_TK_E_IN_USE);
        if (single_instance != NULL)
            CloseHandle(single_instance);
        return 3;
    }

    if (!spawn_request && !launcher_request)
    {
        status = ProEngineerConnect(
            session_id, display, user, textpath, PRO_B_FALSE,
            persistent_connect_timeout_seconds(),
            &random_choice, &persistent_process);
    }
    if (status != PRO_TK_NO_ERROR)
    {
        persistent_startup_status_write("connect", status);
        persistent_cleanup_own_comm_helpers();
        ReleaseMutex(single_instance);
        CloseHandle(single_instance);
        return 4;
    }
    persistent_connected = 1;
    persistent_startup_status_write("connected", PRO_TK_NO_ERROR);
    if (persistent_registry_managed && !persistent_spawn_registry_write())
    {
        persistent_cleanup_own_comm_helpers();
        if (persistent_target_process != NULL)
            CloseHandle(persistent_target_process);
        ReleaseMutex(single_instance);
        CloseHandle(single_instance);
        return 7;
    }

    while (running &&
           InterlockedCompareExchange(
               &persistent_session_lost, 0, 0) == 0)
    {
        HANDLE pipe;
        BOOL connected;
        char request[4096];
        DWORD bytes_read = 0;

        pipe = CreateNamedPipeW(
            persistent_pipe_name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            NULL);
        if (pipe == INVALID_HANDLE_VALUE)
            break;
        connected = persistent_accept_client(pipe) ? TRUE : FALSE;
        if (!connected)
        {
            CloseHandle(pipe);
            if (InterlockedCompareExchange(
                    &persistent_session_lost, 0, 0) != 0)
                break;
            continue;
        }
        memset(request, 0, sizeof(request));
        if (!persistent_read_request(
                pipe, request, (DWORD)sizeof(request) - 1, &bytes_read))
        {
            pipe_reply(pipe, "{\"ok\":false,\"stage\":\"pipe_read\"}\n");
        }
        else
        {
            char *command_request;
            wchar_t command_path[2048];
            request[bytes_read] = '\0';
            while (bytes_read > 0 &&
                   (request[bytes_read - 1] == '\r' ||
                    request[bytes_read - 1] == '\n'))
                request[--bytes_read] = '\0';
            command_request = persistent_authenticated_command(request);
            if (command_request == NULL)
            {
                pipe_reply(pipe,
                    "{\"ok\":false,\"stage\":\"session_authentication\"}\n");
            }
            else if (strcmp(command_request, "PING") == 0)
            {
                ProError health = persistent_health_check();
                char response[640];
                if (persistent_target_start_time != 0)
                {
                    sprintf_s(
                        response, sizeof(response),
                        "{\"ok\":%s,\"persistent\":true,\"connected\":%s,"
                        "\"session_bound\":true,\"health_code\":%d,"
                        "\"target_creo_process_id\":%lu,"
                        "\"target_creo_start_time_100ns\":\"%I64u\","
                        "\"worker_process_id\":%lu,"
                        "\"session_nonce\":\"%s\","
                        "\"session_fingerprint\":\"%s\"}\n",
                        health == PRO_TK_NO_ERROR ? "true" : "false",
                        health == PRO_TK_NO_ERROR ? "true" : "false",
                        health,
                        (unsigned long)persistent_target_process_id,
                        persistent_target_start_time,
                        (unsigned long)GetCurrentProcessId(),
                        persistent_session_nonce,
                        persistent_session_fingerprint);
                }
                else
                {
                    sprintf_s(
                        response, sizeof(response),
                        "{\"ok\":%s,\"persistent\":true,\"connected\":%s,"
                        "\"session_bound\":true,\"health_code\":%d,"
                        "\"target_creo_process_id\":%lu,"
                        "\"target_creo_start_time_100ns\":null,"
                        "\"worker_process_id\":%lu,"
                        "\"session_nonce\":\"%s\","
                        "\"session_fingerprint\":\"%s\"}\n",
                        health == PRO_TK_NO_ERROR ? "true" : "false",
                        health == PRO_TK_NO_ERROR ? "true" : "false",
                        health,
                        (unsigned long)persistent_target_process_id,
                        (unsigned long)GetCurrentProcessId(),
                        persistent_session_nonce,
                        persistent_session_fingerprint);
                }
                pipe_reply(pipe, response);
            }
            else if (strcmp(command_request, "RECONNECT") == 0)
            {
                pipe_reply(pipe,
                    "{\"ok\":false,\"persistent\":true,"
                    "\"session_bound\":true,"
                    "\"stage\":\"reconnect_disabled\"}\n");
            }
            else if (strcmp(command_request, "SHUTDOWN") == 0)
            {
                pipe_reply(pipe,
                    "{\"ok\":true,\"persistent\":true,\"shutdown\":true}\n");
                running = 0;
            }
            else if (strcmp(command_request, "SESSION_LOST") == 0)
            {
                running = 0;
                InterlockedExchange(&persistent_session_lost, 1);
            }
            else if (strncmp(command_request, "BASIC|", 6) == 0)
            {
                if (!utf8_to_wide(
                        command_request + 6, command_path,
                        sizeof(command_path) / sizeof(command_path[0])))
                {
                    pipe_reply(pipe,
                        "{\"ok\":false,\"stage\":\"basic_path_utf8\"}\n");
                }
                else
                {
                    int exit_code = basic_snapshot_execute(command_path);
                    char response[128];
                    sprintf_s(
                        response, sizeof(response),
                        "{\"ok\":%s,\"persistent\":true,"
                        "\"basic_snapshot\":true,\"exit_code\":%d}\n",
                        exit_code == 0 ? "true" : "false", exit_code);
                    pipe_reply(pipe, response);
                }
            }
            else if (strncmp(command_request, "FEATURES|", 9) == 0)
            {
                if (!utf8_to_wide(
                        command_request + 9, command_path,
                        sizeof(command_path) / sizeof(command_path[0])))
                {
                    pipe_reply(pipe,
                        "{\"ok\":false,\"stage\":\"features_path_utf8\"}\n");
                }
                else
                {
                    int exit_code = persistent_feature_snapshot(command_path);
                    char response[128];
                    sprintf_s(
                        response, sizeof(response),
                        "{\"ok\":%s,\"persistent\":true,"
                        "\"feature_snapshot\":true,\"exit_code\":%d}\n",
                        exit_code == 0 ? "true" : "false", exit_code);
                    pipe_reply(pipe, response);
                }
            }
            else if (strncmp(command_request, "DISPLAY|", 8) == 0)
            {
                char *result_utf8 = command_request + 8;
                char *model_utf8 = strchr(result_utf8, '|');
                char *expected_utf8 = NULL;
                wchar_t result_path[2048];
                wchar_t model_path[2048];
                wchar_t expected_name[PRO_NAME_SIZE];
                if (model_utf8 != NULL)
                {
                    *model_utf8++ = '\0';
                    expected_utf8 = strchr(model_utf8, '|');
                }
                if (expected_utf8 != NULL)
                    *expected_utf8++ = '\0';
                if (model_utf8 == NULL || expected_utf8 == NULL ||
                    !utf8_to_wide(result_utf8, result_path,
                        sizeof(result_path) / sizeof(result_path[0])) ||
                    !utf8_to_wide(model_utf8, model_path,
                        sizeof(model_path) / sizeof(model_path[0])) ||
                    !utf8_to_wide(expected_utf8, expected_name,
                        sizeof(expected_name) / sizeof(expected_name[0])))
                {
                    pipe_reply(pipe,
                        "{\"ok\":false,\"stage\":\"display_arguments\"}\n");
                }
                else
                {
                    int exit_code = persistent_display_execute(
                        result_path, model_path, expected_name);
                    char response[128];
                    sprintf_s(
                        response, sizeof(response),
                        "{\"ok\":%s,\"persistent\":true,"
                        "\"display\":true,\"exit_code\":%d}\n",
                        exit_code == 0 ? "true" : "false", exit_code);
                    pipe_reply(pipe, response);
                }
            }
            else if (strncmp(command_request, "EXECFILE|", 9) == 0)
            {
                if (!utf8_to_wide(
                        command_request + 9, command_path,
                        sizeof(command_path) / sizeof(command_path[0])))
                {
                    pipe_reply(pipe,
                        "{\"ok\":false,\"stage\":\"execfile_path_utf8\"}\n");
                }
                else
                {
                    int exit_code = codex_external_command_file_execute(command_path);
                    char response[160];
                    sprintf_s(
                        response, sizeof(response),
                        "{\"ok\":%s,\"persistent\":true,"
                        "\"external_command\":true,\"exit_code\":%d}\n",
                        exit_code == 0 ? "true" : "false", exit_code);
                    pipe_reply(pipe, response);
                }
            }
            else if (codex_external_special_can_handle(command_request))
            {
                char response[1024] = {0};
                if (!codex_external_special_execute(
                        command_request, response, sizeof(response)))
                    pipe_reply(pipe,
                        "{\"ok\":false,\"stage\":\"special_command\"}\n");
                else
                    pipe_reply(pipe, response);
            }
            else if (!utf8_to_wide(
                         command_request, command_path,
                         sizeof(command_path) / sizeof(command_path[0])))
            {
                pipe_reply(pipe,
                    "{\"ok\":false,\"stage\":\"command_path_utf8\"}\n");
            }
            else
            {
                int exit_code = command_file_execute(command_path);
                char response[128];
                sprintf_s(
                    response, sizeof(response),
                    "{\"ok\":%s,\"persistent\":true,\"exit_code\":%d}\n",
                    exit_code == 0 ? "true" : "false", exit_code);
                pipe_reply(pipe, response);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    if (persistent_connected && !persistent_spawned_by_creo &&
        InterlockedCompareExchange(&persistent_session_lost, 0, 0) == 0)
    {
        ProEngineerDisconnect(&persistent_process, 1);
        persistent_connected = 0;
    }
    persistent_cleanup_own_comm_helpers();
    if (persistent_registry_managed && persistent_registry_path[0] != L'\0')
        DeleteFileW(persistent_registry_path);
    if (persistent_target_process != NULL)
    {
        CloseHandle(persistent_target_process);
        persistent_target_process = NULL;
    }
    ReleaseMutex(single_instance);
    CloseHandle(single_instance);
    return 0;
}
