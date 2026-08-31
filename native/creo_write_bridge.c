#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProParameter.h>
#include <ProParamval.h>
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
        case '\b': fputs("\\b", out); break;
        case '\f': fputs("\\f", out); break;
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
    fputs("{\"ok\":false,\"safe_copy_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

static int is_allowed_parameter(const wchar_t *name)
{
    static const wchar_t *allowed_parameters[] = {
        L"CNAME",
        L"材质规格",
        L"用量",
        L"表面处理",
        L"类属",
        L"项目",
        L"备注",
        L"料号",
        L"版本号",
        L"设计"
    };
    size_t i;

    for (i = 0; i < sizeof(allowed_parameters) / sizeof(allowed_parameters[0]); ++i)
    {
        if (_wcsicmp(name, allowed_parameters[i]) == 0)
            return 1;
    }
    return 0;
}

static int output_model_already_exists(const wchar_t *directory, const wchar_t *copy_name)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;

    _snwprintf_s(
        pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.*",
        directory,
        copy_name);

    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;

    FindClose(find_handle);
    return 1;
}

static int find_saved_model(
    const wchar_t *directory,
    const wchar_t *copy_name,
    wchar_t *saved_path,
    size_t saved_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;

    _snwprintf_s(
        pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.*",
        directory,
        copy_name);

    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;

    _snwprintf_s(
        saved_path,
        saved_path_count,
        _TRUNCATE,
        L"%ls\\%ls",
        directory,
        find_data.cFileName);
    FindClose(find_handle);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    const wchar_t *expected_model_arg;
    const wchar_t *copy_name_arg;
    const wchar_t *output_dir_arg;
    const wchar_t *parameter_name_arg;
    const wchar_t *new_value_arg;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl source_model = NULL;
    ProMdl copy_model = NULL;
    ProMdlName source_name;
    ProMdlName expected_name;
    ProMdlName copy_name;
    ProMdlType source_type;
    ProModelitem copy_item;
    ProName parameter_name;
    ProParameter parameter;
    ProParamvalue original_value;
    ProParamvalue new_value;
    ProParamvalue verified_value;
    ProPath original_directory;
    ProPath output_directory;
    ProPath saved_path;
    wchar_t new_value_w[PRO_LINE_SIZE];
    int connected = 0;
    int copy_created = 0;
    int directory_changed = 0;
    int exit_code = 1;
    int saved_found = 0;

    if (argc != 7)
    {
        fwprintf(stderr,
            L"Usage: creo_write_bridge <result.json> <expected_model> <copy_name> "
            L"<output_dir> <parameter> <new_value>\n");
        return 2;
    }

    expected_model_arg = argv[2];
    copy_name_arg = argv[3];
    output_dir_arg = argv[4];
    parameter_name_arg = argv[5];
    new_value_arg = argv[6];

    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
    }

    if (!is_allowed_parameter(parameter_name_arg))
    {
        exit_code = write_error(out, "parameter_allowlist", PRO_TK_NO_PERMISSION);
        goto done;
    }

    wcsncpy_s(expected_name,
        sizeof(expected_name) / sizeof(expected_name[0]),
        expected_model_arg, _TRUNCATE);
    wcsncpy_s(copy_name,
        sizeof(copy_name) / sizeof(copy_name[0]),
        copy_name_arg, _TRUNCATE);
    wcsncpy_s(output_directory,
        sizeof(output_directory) / sizeof(output_directory[0]),
        output_dir_arg, _TRUNCATE);
    wcsncpy_s(parameter_name,
        sizeof(parameter_name) / sizeof(parameter_name[0]),
        parameter_name_arg, _TRUNCATE);
    wcsncpy_s(new_value_w,
        sizeof(new_value_w) / sizeof(new_value_w[0]),
        new_value_arg, _TRUNCATE);

    if (GetFileAttributesW(output_directory) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "output_directory", PRO_TK_INVALID_DIR);
        goto done;
    }

    if (output_model_already_exists(output_directory, copy_name))
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

    status = ProMdlCurrentGet(&source_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto cleanup;
    }

    status = ProMdlNameGet(source_model, source_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_name", status);
        goto cleanup;
    }

    if (_wcsicmp(source_name, expected_name) != 0)
    {
        exit_code = write_error(out, "source_model_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }

    status = ProMdlTypeGet(source_model, &source_type);
    if (status != PRO_TK_NO_ERROR || source_type != PRO_MDL_PART)
    {
        exit_code = write_error(out, "source_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }

    status = ProDirectoryCurrentGet(original_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "get_working_directory", status);
        goto cleanup;
    }

    status = ProDirectoryChange(output_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "change_output_directory", status);
        goto cleanup;
    }
    directory_changed = 1;

    status = ProMdlnameCopy(source_model, copy_name, &copy_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_model", status);
        goto cleanup;
    }
    copy_created = 1;

    ProDirectoryChange(original_directory);
    directory_changed = 0;

    status = ProMdlToModelitem(copy_model, &copy_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_model_item", status);
        goto cleanup;
    }

    status = ProParameterInit(&copy_item, parameter_name, &parameter);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "parameter_init", status);
        goto cleanup;
    }

    original_value.type = PRO_PARAM_VOID;
    status = ProParameterValueWithUnitsGet(&parameter, &original_value, NULL);
    if (status != PRO_TK_NO_ERROR || original_value.type != PRO_PARAM_STRING)
    {
        exit_code = write_error(out, "parameter_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }

    status = ProParamvalueSet(&new_value, new_value_w, PRO_PARAM_STRING);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "parameter_value_build", status);
        goto cleanup;
    }

    status = ProParameterValueWithUnitsSet(&parameter, &new_value, NULL);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "parameter_set", status);
        goto cleanup;
    }

    verified_value.type = PRO_PARAM_VOID;
    status = ProParameterValueWithUnitsGet(&parameter, &verified_value, NULL);
    if (status != PRO_TK_NO_ERROR ||
        verified_value.type != PRO_PARAM_STRING ||
        wcscmp(verified_value.value.s_val, new_value_w) != 0)
    {
        exit_code = write_error(out, "parameter_readback", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }

    status = ProSolidRegenerate((ProSolid)copy_model, PRO_REGEN_NO_FLAGS);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "regenerate", status);
        goto cleanup;
    }

    status = ProMdlSave(copy_model);

    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_copy", status);
        goto cleanup;
    }

    saved_found = find_saved_model(
        output_directory,
        copy_name,
        saved_path,
        sizeof(saved_path) / sizeof(saved_path[0]));
    if (!saved_found)
    {
        exit_code = write_error(out, "verify_saved_file", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"safe_copy_only\":true,\"source_model\":", out);
    write_wide_json_string(out, source_name);
    fputs(",\"copy_model\":", out);
    write_wide_json_string(out, copy_name);
    fputs(",\"parameter\":", out);
    write_wide_json_string(out, parameter_name);
    fputs(",\"old_value\":", out);
    write_wide_json_string(out, original_value.value.s_val);
    fputs(",\"new_value\":", out);
    write_wide_json_string(out, verified_value.value.s_val);
    fprintf(out, ",\"regenerate_status\":%d,\"saved_file\":", status);
    write_wide_json_string(out, saved_path);
    fputs("}\n", out);
    exit_code = 0;

cleanup:
    if (directory_changed)
        ProDirectoryChange(original_directory);
    if (copy_created)
        ProMdlErase(copy_model);
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
