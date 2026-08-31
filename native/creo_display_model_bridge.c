#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProWindows.h>
#include <ProUtil.h>

static void write_utf8_json_string(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', out);
    while (*p)
    {
        switch (*p)
        {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20)
                fprintf(out, "\\u%04x", (unsigned int)*p);
            else
                fputc(*p, out);
        }
        ++p;
    }
    fputc('"', out);
}

static void write_wide_json_string(FILE *out, const wchar_t *text)
{
    int bytes;
    char *utf8;
    if (text == NULL)
    {
        fputs("null", out);
        return;
    }
    bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0)
    {
        fputs("\"\"", out);
        return;
    }
    utf8 = (char *)malloc((size_t)bytes);
    if (utf8 == NULL)
    {
        fputs("\"\"", out);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, bytes, NULL, NULL);
    write_utf8_json_string(out, utf8);
    free(utf8);
}

static int write_error(FILE *out, const char *stage, ProError error_code)
{
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

static BOOL CALLBACK foreground_creo_window(HWND window, LPARAM app_data)
{
    wchar_t title[512];
    int length;
    (void)app_data;
    if (!IsWindowVisible(window))
        return TRUE;
    length = GetWindowTextW(window, title, 512);
    if (length <= 0)
        return TRUE;
    if (wcsstr(title, L"Creo Parametric") != NULL)
    {
        ShowWindow(window, SW_RESTORE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        return FALSE;
    }
    return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError activate_status = PRO_TK_NO_ERROR;
    ProError refit_status = PRO_TK_NO_ERROR;
    ProError repaint_status = PRO_TK_NO_ERROR;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProMdl current = NULL;
    ProMdlName model_name;
    ProMdlName expected_name;
    ProMdlName current_name;
    ProPath model_path;
    ProMdlfileType file_type = PRO_MDLFILE_PART;
    ProMdlType window_model_type = PRO_PART;
    const wchar_t *extension;
    int window_id = -1;
    int connected = 0;
    int exit_code = 1;

    if (argc != 4)
    {
        fwprintf(stderr,
            L"Usage: creo_display_model_bridge <result.json> <model_file> <expected_model>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
    }
    wcsncpy_s(model_path,
        sizeof(model_path) / sizeof(model_path[0]), argv[2], _TRUNCATE);
    wcsncpy_s(expected_name,
        sizeof(expected_name) / sizeof(expected_name[0]), argv[3], _TRUNCATE);
    if (GetFileAttributesW(model_path) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "model_file", PRO_TK_E_NOT_FOUND);
        goto done;
    }
    extension = wcsstr(model_path, L".asm");
    if (extension != NULL)
    {
        file_type = PRO_MDLFILE_ASSEMBLY;
        window_model_type = PRO_ASSEMBLY;
    }
    else if (wcsstr(model_path, L".prt") == NULL)
    {
        exit_code = write_error(out, "model_file_type", PRO_TK_INVALID_TYPE);
        goto done;
    }

    status = ProEngineerConnect(
        "", "", "", "", PRO_B_TRUE, 20,
        &random_choice, &process_handle);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "connect", status);
        goto done;
    }
    connected = 1;
    status = ProMdlFiletypeLoad(model_path, file_type, PRO_B_FALSE, &model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "load_model_file", status);
        goto cleanup;
    }
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_name", status);
        goto cleanup;
    }
    if (_wcsicmp(model_name, expected_name) != 0)
    {
        exit_code = write_error(out, "model_name_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProMdlWindowGet(model, &window_id);
    if (status != PRO_TK_NO_ERROR)
    {
        status = ProObjectwindowMdlnameCreate(
            model_name, window_model_type, &window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "create_model_window", status);
            goto cleanup;
        }
    }
    status = ProWindowCurrentSet(window_id);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "set_current_window", status);
        goto cleanup;
    }
    status = ProWindowActivate(window_id);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "activate_model_window", status);
        goto cleanup;
    }
    status = ProMdlDisplay(model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "display_model", status);
        goto cleanup;
    }
    status = ProMdlCurrentGet(&current);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model_readback", status);
        goto cleanup;
    }
    status = ProMdlNameGet(current, current_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(current_name, expected_name) != 0)
    {
        exit_code = write_error(out, "current_model_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProWindowCurrentGet(&window_id);
    if (status == PRO_TK_NO_ERROR)
    {
        activate_status = ProWindowActivate(window_id);
        refit_status = ProWindowRefit(window_id);
        repaint_status = ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"display_only\":true,\"saved_or_modified\":false,", out);
    fputs("\"model\":", out);
    write_wide_json_string(out, model_name);
    fprintf(out,
        ",\"window_id\":%d,\"window_status\":%d,"
        "\"activate_status\":%d,\"refit_status\":%d,"
        "\"repaint_status\":%d}\n",
        window_id, status, activate_status, refit_status, repaint_status);
    exit_code = 0;

cleanup:
    if (connected)
    {
        disconnect_status = ProEngineerDisconnect(&process_handle, 1);
        if (disconnect_status != PRO_TK_NO_ERROR && exit_code == 0)
            exit_code = 3;
    }
    EnumWindows(foreground_creo_window, 0);

done:
    fclose(out);
    return exit_code;
}
