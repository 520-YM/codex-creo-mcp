#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProDimension.h>
#include <ProUtil.h>

typedef struct dimension_find_context
{
    const wchar_t *target_symbol;
    ProDimension dimension;
    int match_count;
} DimensionFindContext;

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
    fputs("{\"ok\":false,\"safe_copy_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

static ProError dimension_find_action(
    ProDimension *dimension,
    ProError filter_status,
    ProAppData app_data)
{
    DimensionFindContext *context = (DimensionFindContext *)app_data;
    ProName symbol;

    (void)filter_status;
    if (ProDimensionSymbolGet(dimension, symbol) == PRO_TK_NO_ERROR &&
        _wcsicmp(symbol, context->target_symbol) == 0)
    {
        context->dimension = *dimension;
        ++context->match_count;
    }
    return PRO_TK_NO_ERROR;
}

static ProError find_dimension_by_symbol(
    ProSolid solid,
    const wchar_t *symbol,
    ProDimension *dimension)
{
    DimensionFindContext context;
    ProError status;

    context.target_symbol = symbol;
    context.match_count = 0;
    status = ProSolidDimensionVisit(
        solid,
        PRO_B_FALSE,
        dimension_find_action,
        NULL,
        (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    *dimension = context.dimension;
    return PRO_TK_NO_ERROR;
}

static int output_model_already_exists(
    const wchar_t *directory,
    const wchar_t *copy_name)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;

    _snwprintf_s(pattern,
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

static int find_latest_saved_model(
    const wchar_t *directory,
    const wchar_t *copy_name,
    wchar_t *saved_path,
    size_t saved_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    int found = 0;
    int best_version = -1;

    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.*",
        directory,
        copy_name);
    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;

    do
    {
        wchar_t *last_dot = wcsrchr(find_data.cFileName, L'.');
        int version = last_dot == NULL ? 0 : _wtoi(last_dot + 1);
        if (!found || version > best_version)
        {
            _snwprintf_s(saved_path,
                saved_path_count,
                _TRUNCATE,
                L"%ls\\%ls",
                directory,
                find_data.cFileName);
            best_version = version;
            found = 1;
        }
    } while (FindNextFileW(find_handle, &find_data));

    FindClose(find_handle);
    return found;
}

static int nearly_equal(double left, double right)
{
    double scale = fabs(left) > fabs(right) ? fabs(left) : fabs(right);
    double tolerance = (scale > 1.0 ? scale : 1.0) * 1.0e-8;
    return fabs(left - right) <= tolerance;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError regenerate_status = PRO_TK_NO_ERROR;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProBoolean relation_driven = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl source_model = NULL;
    ProMdl copy_model = NULL;
    ProMdlName source_name;
    ProMdlName expected_name;
    ProMdlName copy_name;
    ProMdlType source_type;
    ProName dimension_symbol;
    ProDimension source_dimension;
    ProDimension copy_dimension;
    ProDimensiontype dimension_type = PRODIMTYPE_UNKNOWN;
    ProPath original_directory;
    ProPath output_directory;
    ProPath saved_path;
    wchar_t *parse_end;
    double expected_value;
    double new_value;
    double source_value = 0.0;
    double verified_value = 0.0;
    double ratio;
    int regenerate_attempts = 0;
    int connected = 0;
    int copy_created = 0;
    int directory_changed = 0;
    int exit_code = 1;

    if (argc != 8)
    {
        fwprintf(stderr,
            L"Usage: creo_dimension_write_bridge <result.json> <expected_model> "
            L"<copy_name> <output_dir> <dimension_symbol> <expected_value> <new_value>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
    }

    expected_value = wcstod(argv[6], &parse_end);
    if (*argv[6] == L'\0' || *parse_end != L'\0' || !_finite(expected_value))
    {
        exit_code = write_error(out, "expected_value_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    new_value = wcstod(argv[7], &parse_end);
    if (*argv[7] == L'\0' || *parse_end != L'\0' || !_finite(new_value))
    {
        exit_code = write_error(out, "new_value_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    if (expected_value <= 0.0 || new_value <= 0.0)
    {
        exit_code = write_error(out, "positive_value_guard", PRO_TK_OUT_OF_RANGE);
        goto done;
    }
    ratio = new_value / expected_value;
    if (ratio < 0.5 || ratio > 2.0)
    {
        exit_code = write_error(out, "relative_change_guard", PRO_TK_OUT_OF_RANGE);
        goto done;
    }

    wcsncpy_s(expected_name,
        sizeof(expected_name) / sizeof(expected_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(copy_name,
        sizeof(copy_name) / sizeof(copy_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(output_directory,
        sizeof(output_directory) / sizeof(output_directory[0]),
        argv[4], _TRUNCATE);
    wcsncpy_s(dimension_symbol,
        sizeof(dimension_symbol) / sizeof(dimension_symbol[0]),
        argv[5], _TRUNCATE);

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

    status = find_dimension_by_symbol(
        (ProSolid)source_model,
        dimension_symbol,
        &source_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_dimension_find", status);
        goto cleanup;
    }
    status = ProDimensionValueGet(&source_dimension, &source_value);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_dimension_value", status);
        goto cleanup;
    }
    if (!nearly_equal(source_value, expected_value))
    {
        exit_code = write_error(out, "expected_value_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProDimensionIsReldriven(&source_dimension, &relation_driven);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "relation_guard_read", status);
        goto cleanup;
    }
    if (relation_driven == PRO_B_TRUE)
    {
        exit_code = write_error(out, "relation_driven_guard", PRO_TK_NO_PERMISSION);
        goto cleanup;
    }
    ProDimensionTypeGet(&source_dimension, &dimension_type);

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

    status = find_dimension_by_symbol(
        (ProSolid)copy_model,
        dimension_symbol,
        &copy_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_dimension_find", status);
        goto cleanup;
    }
    status = ProDimensionValueSet(&copy_dimension, new_value);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "dimension_set", status);
        goto cleanup;
    }
    status = ProDimensionValueGet(&copy_dimension, &verified_value);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(verified_value, new_value))
    {
        exit_code = write_error(out, "dimension_readback", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }

    do
    {
        regenerate_status = ProSolidRegenerate((ProSolid)copy_model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR &&
        regenerate_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "regenerate", regenerate_status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)copy_model,
        dimension_symbol,
        &copy_dimension);
    if (status != PRO_TK_NO_ERROR ||
        ProDimensionValueGet(&copy_dimension, &verified_value) != PRO_TK_NO_ERROR ||
        !nearly_equal(verified_value, new_value))
    {
        exit_code = write_error(out, "post_regen_readback", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }

    status = ProMdlSave(copy_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_copy", status);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            output_directory,
            copy_name,
            saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_file", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"safe_copy_only\":true,\"source_model\":", out);
    write_wide_json_string(out, source_name);
    fputs(",\"copy_model\":", out);
    write_wide_json_string(out, copy_name);
    fputs(",\"dimension_symbol\":", out);
    write_wide_json_string(out, dimension_symbol);
    fprintf(out,
        ",\"dimension_type_code\":%d,\"old_value\":%.17g,\"new_value\":%.17g,"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,\"saved_file\":",
        (int)dimension_type,
        source_value,
        verified_value,
        regenerate_status,
        regenerate_attempts);
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
