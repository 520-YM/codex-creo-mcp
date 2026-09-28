#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProToolkitDll.h>

static int wide_to_ansi(const wchar_t *source, char *target, size_t capacity)
{
    int count;
    if (source == NULL || target == NULL || capacity < 2)
        return 0;
    count = WideCharToMultiByte(
        CP_ACP, 0, source, -1, target, (int)capacity, NULL, NULL);
    return count > 0;
}

static void write_result(
    const wchar_t *path, int ok, const char *stage,
    ProError connect_status, ProError load_status,
    ProError user_status, int already_loaded)
{
    FILE *out = NULL;
    if (path == NULL || _wfopen_s(&out, path, L"wb") != 0 || out == NULL)
        return;
    fprintf(out,
        "{\"ok\":%s,\"stage\":\"%s\","
        "\"connect_status\":%d,\"load_status\":%d,"
        "\"user_status\":%d,\"already_loaded\":%s,"
        "\"loader_process_id\":%lu}\n",
        ok ? "true" : "false", stage,
        (int)connect_status, (int)load_status, (int)user_status,
        already_loaded ? "true" : "false",
        (unsigned long)GetCurrentProcessId());
    fclose(out);
}

int wmain(int argc, wchar_t **argv)
{
    char connect_id[2048];
    char display[512];
    char user[512];
    char textpath[2048];
    ProCharPath dll_path;
    ProCharPath text_dir;
    ProName application_name = L"CodexCreoInternalV11";
    ProPath user_message = L"";
    ProProcessHandle process;
    ProToolkitDllHandle dll_handle = NULL;
    ProBoolean random_choice = PRO_B_FALSE;
    ProError connect_status = PRO_TK_GENERAL_ERROR;
    ProError handle_status;
    ProError load_status = PRO_TK_GENERAL_ERROR;
    ProError user_status = PRO_TK_GENERAL_ERROR;
    int already_loaded = 0;
    int start_mode = 0;
    const wchar_t *result_path;
    const wchar_t *dll_argument;
    const wchar_t *text_dir_argument;

    if (argc == 7 && wcscmp(argv[1], L"--start") == 0)
        start_mode = 1;
    else if (argc != 8)
        return 2;
    result_path = start_mode ? argv[2] : argv[1];
    dll_argument = start_mode ? argv[5] : argv[6];
    text_dir_argument = start_mode ? argv[6] : argv[7];
    if (!wide_to_ansi(dll_argument, dll_path, sizeof(dll_path)) ||
        !wide_to_ansi(text_dir_argument, text_dir, sizeof(text_dir)))
    {
        write_result(result_path, 0, "argument_conversion", -1, -1, -1, 0);
        return 2;
    }

    memset(&process, 0, sizeof(process));
    if (start_mode)
    {
        char creo_command[2048];
        if (!wide_to_ansi(argv[3], creo_command, sizeof(creo_command)) ||
            !SetCurrentDirectoryW(argv[4]))
        {
            write_result(
                result_path, 0, "startup_arguments", -1, -1, -1, 0);
            return 2;
        }
        connect_status = ProEngineerConnectionStart(
            creo_command, "", &process);
    }
    else
    {
        if (!wide_to_ansi(argv[2], connect_id, sizeof(connect_id)) ||
            !wide_to_ansi(argv[3], display, sizeof(display)) ||
            !wide_to_ansi(argv[4], user, sizeof(user)) ||
            !wide_to_ansi(argv[5], textpath, sizeof(textpath)))
        {
            write_result(
                result_path, 0, "argument_conversion", -1, -1, -1, 0);
            return 2;
        }
        connect_status = ProEngineerConnect(
            connect_id, display, user, textpath, PRO_B_FALSE, 8,
            &random_choice, &process);
    }
    if (connect_status != PRO_TK_NO_ERROR)
    {
        write_result(
            result_path, 0,
            start_mode ? "start_creo" : "connect_exact_creo", connect_status,
            load_status, user_status, 0);
        return 1;
    }

    handle_status = ProToolkitDllHandleGet(application_name, &dll_handle);
    if (handle_status == PRO_TK_NO_ERROR)
    {
        already_loaded = 1;
        load_status = PRO_TK_NO_ERROR;
        user_status = PRO_TK_NO_ERROR;
    }
    else
    {
        load_status = ProToolkitDllLoad(
            application_name, dll_path, text_dir, PRO_B_FALSE,
            &dll_handle, &user_status, user_message);
    }

    ProEngineerDisconnect(&process, 5);
    if (load_status != PRO_TK_NO_ERROR || user_status != PRO_TK_NO_ERROR)
    {
        write_result(
            result_path, 0, "load_internal_dll", connect_status,
            load_status, user_status, already_loaded);
        return 1;
    }
    write_result(
        result_path, 1, "ready", connect_status,
        load_status, user_status, already_loaded);
    return 0;
}
