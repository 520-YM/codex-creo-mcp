#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>

static void write_wide_json_string(FILE *out, const wchar_t *text)
{
    int bytes;
    char *utf8;
    const unsigned char *cursor;
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
    fputc('"', out);
    cursor = (const unsigned char *)utf8;
    while (*cursor)
    {
        if (*cursor == '"' || *cursor == '\\')
            fputc('\\', out);
        if (*cursor < 0x20)
            fprintf(out, "\\u%04x", (unsigned int)*cursor);
        else
            fputc(*cursor, out);
        ++cursor;
    }
    fputc('"', out);
    free(utf8);
}

static int write_error(FILE *out, const char *stage, ProError status)
{
    fprintf(out,
        "{\"ok\":false,\"api_only\":true,\"stage\":\"%s\",\"error_code\":%d}\n",
        stage,
        status);
    return 1;
}

static void trim_directory_separators(wchar_t *value)
{
    size_t length = wcslen(value);
    while (length > 3 &&
        (value[length - 1] == L'\\' || value[length - 1] == L'/'))
    {
        value[length - 1] = L'\0';
        --length;
    }
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    DWORD attributes;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProPath requested_directory;
    ProPath current_directory;
    ProPath requested_compare;
    ProPath current_compare;
    int connected = 0;
    int exit_code = 1;

    if (argc != 3)
    {
        fwprintf(stderr,
            L"Usage: creo_project_workdir_bridge <result.json> <project_directory>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(
        requested_directory,
        sizeof(requested_directory) / sizeof(requested_directory[0]),
        argv[2],
        _TRUNCATE);

    attributes = GetFileAttributesW(requested_directory);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        exit_code = write_error(out, "project_directory", PRO_TK_INVALID_DIR);
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

    status = ProDirectoryChange(requested_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "change_working_directory", status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(current_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "readback_working_directory", status);
        goto cleanup;
    }
    wcsncpy_s(
        requested_compare,
        sizeof(requested_compare) / sizeof(requested_compare[0]),
        requested_directory,
        _TRUNCATE);
    wcsncpy_s(
        current_compare,
        sizeof(current_compare) / sizeof(current_compare[0]),
        current_directory,
        _TRUNCATE);
    trim_directory_separators(requested_compare);
    trim_directory_separators(current_compare);
    if (_wcsicmp(requested_compare, current_compare) != 0)
    {
        fputs("{\"ok\":false,\"api_only\":true,\"stage\":\"working_directory_guard\","
              "\"error_code\":-8,\"requested_directory\":", out);
        write_wide_json_string(out, requested_compare);
        fputs(",\"actual_directory\":", out);
        write_wide_json_string(out, current_compare);
        fputs("}\n", out);
        exit_code = 1;
        goto cleanup;
    }

    fputs("{\"ok\":true,\"api_only\":true,\"working_directory\":", out);
    write_wide_json_string(out, current_directory);
    fputs(",\"changed\":true}\n", out);
    exit_code = 0;

cleanup:
    if (connected)
    {
        disconnect_status = ProEngineerDisconnect(&process_handle, 10);
        if (disconnect_status != PRO_TK_NO_ERROR && exit_code == 0)
            exit_code = 3;
    }
done:
    fclose(out);
    return exit_code;
}
