#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProParameter.h>
#include <ProParamval.h>

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
    fputs("{\"ok\":false,\"readonly\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProModelitem model_item;
    ProParameter parameter;
    ProParamvalue value;
    ProPath model_path;
    ProName parameter_name;
    ProLine expected_value;
    ProMdlName model_name;
    int connected = 0;
    int loaded = 0;
    int matches = 0;
    int exit_code = 1;

    if (argc != 5)
    {
        fwprintf(stderr,
            L"Usage: creo_verify_copy <result.json> <model_file> <parameter> <expected_value>\n");
        return 2;
    }

    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
    }

    wcsncpy_s(model_path,
        sizeof(model_path) / sizeof(model_path[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(parameter_name,
        sizeof(parameter_name) / sizeof(parameter_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(expected_value,
        sizeof(expected_value) / sizeof(expected_value[0]),
        argv[4], _TRUNCATE);

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

    status = ProMdlToModelitem(model, &model_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_item", status);
        goto cleanup;
    }

    status = ProParameterInit(&model_item, parameter_name, &parameter);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "parameter_init", status);
        goto cleanup;
    }

    value.type = PRO_PARAM_VOID;
    status = ProParameterValueWithUnitsGet(&parameter, &value, NULL);
    if (status != PRO_TK_NO_ERROR || value.type != PRO_PARAM_STRING)
    {
        exit_code = write_error(out, "parameter_read",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }

    matches = wcscmp(value.value.s_val, expected_value) == 0;
    fputs("{\"ok\":", out);
    fputs(matches ? "true" : "false", out);
    fputs(",\"readonly\":true,\"model\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"file\":", out);
    write_wide_json_string(out, model_path);
    fputs(",\"parameter\":", out);
    write_wide_json_string(out, parameter_name);
    fputs(",\"expected_value\":", out);
    write_wide_json_string(out, expected_value);
    fputs(",\"actual_value\":", out);
    write_wide_json_string(out, value.value.s_val);
    fputs(",\"matches\":", out);
    fputs(matches ? "true" : "false", out);
    fputs("}\n", out);
    exit_code = matches ? 0 : 4;

cleanup:
    if (loaded)
        ProMdlErase(model);
    if (connected)
        ProEngineerDisconnect(&process_handle, 10);

done:
    fclose(out);
    return exit_code;
}
