#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProIntf3Dexport.h>

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
    fputs("{\"ok\":false,\"source_untouched\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

static int output_already_exists(const wchar_t *output_base)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;

    _snwprintf_s(
        pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls.*",
        output_base);
    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(find_handle);
    return 1;
}

static int find_exported_file(
    const wchar_t *output_base,
    wchar_t *exported_path,
    size_t exported_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    wchar_t *slash;

    _snwprintf_s(
        pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls.*",
        output_base);
    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;

    wcsncpy_s(exported_path, exported_path_count, output_base, _TRUNCATE);
    slash = wcsrchr(exported_path, L'\\');
    if (slash == NULL)
    {
        FindClose(find_handle);
        return 0;
    }
    slash[1] = L'\0';
    wcscat_s(exported_path, exported_path_count, find_data.cFileName);
    FindClose(find_handle);
    return 1;
}

int main(int argc, char **argv)
{
    FILE *out = stdout;
    ProError status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProPath model_path;
    ProPath output_base;
    ProPath exported_path;
    ProMdlName model_name;
    WIN32_FILE_ATTRIBUTE_DATA file_data;
    ULARGE_INTEGER file_size;
    int connected = 0;
    int loaded = 0;
    int exit_code = 1;

    if (argc != 4)
    {
        fprintf(stderr,
            "Usage: creo_export_bridge <result.json> <model_file> <output_base>\n");
        return 2;
    }

    if (fopen_s(&out, argv[1], "wb") != 0 || out == NULL)
    {
        fprintf(stderr, "Unable to open result file: %s\n", argv[1]);
        return 2;
    }

    ProStringToWstring(model_path, argv[2]);
    ProStringToWstring(output_base, argv[3]);

    if (output_already_exists(output_base))
    {
        exit_code = write_error(out, "refuse_overwrite", PRO_TK_E_FOUND);
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

    status = ProMdlFiletypeLoad(
        model_path, PRO_MDLFILE_PART, PRO_B_FALSE, &model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "load_copy", status);
        goto cleanup;
    }
    loaded = 1;

    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_name", status);
        goto cleanup;
    }

    status = ProIntf3DFileWriteWithDefaultProfile(
        (ProSolid)model, PRO_INTF_EXPORT_STEP, output_base);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_NO_CHANGE)
    {
        exit_code = write_error(out, "export_step", status);
        goto cleanup;
    }

    if (!find_exported_file(
            output_base,
            exported_path,
            sizeof(exported_path) / sizeof(exported_path[0])) ||
        !GetFileAttributesExW(exported_path, GetFileExInfoStandard, &file_data))
    {
        exit_code = write_error(out, "verify_exported_file", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    file_size.HighPart = file_data.nFileSizeHigh;
    file_size.LowPart = file_data.nFileSizeLow;
    fputs("{\"ok\":true,\"source_untouched\":true,\"model\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"source_file\":", out);
    write_wide_json_string(out, model_path);
    fputs(",\"format\":\"STEP\",\"exported_file\":", out);
    write_wide_json_string(out, exported_path);
    fprintf(out, ",\"size_bytes\":%llu,\"export_status\":%d}\n",
        file_size.QuadPart, status);
    exit_code = 0;

cleanup:
    if (loaded)
        ProMdlErase(model);
    if (connected)
        ProEngineerDisconnect(&process_handle, 10);

done:
    fclose(out);
    return exit_code;
}
