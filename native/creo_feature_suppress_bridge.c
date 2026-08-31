#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <limits.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProArray.h>
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
    fputs("{\"ok\":false,\"safe_copy_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

static int is_suppressible_feature_type(ProFeattype type)
{
    return type == PRO_FEAT_ROUND ||
        type == PRO_FEAT_CHAMFER ||
        type == PRO_FEAT_HOLE ||
        type == PRO_FEAT_CUT;
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

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError regenerate_status = PRO_TK_NO_ERROR;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProBoolean visible = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl source_model = NULL;
    ProMdl copy_model = NULL;
    ProMdlName source_name;
    ProMdlName expected_name;
    ProMdlName copy_name;
    ProMdlType source_type;
    ProFeature source_feature;
    ProFeature copy_feature;
    ProFeattype source_feature_type = -1;
    ProFeatStatus source_feature_status = PRO_FEAT_INVALID;
    ProFeatStatus copy_feature_status = PRO_FEAT_INVALID;
    ProFeatureDeleteOptions suppress_option = PRO_FEAT_DELETE_NO_OPTS;
    ProPath original_directory;
    ProPath output_directory;
    ProPath saved_path;
    wchar_t *parse_end;
    long feature_id_long;
    long expected_type_long;
    int feature_id;
    int expected_type;
    int *children = NULL;
    int child_count = 0;
    int regenerate_attempts = 0;
    int connected = 0;
    int copy_created = 0;
    int directory_changed = 0;
    int exit_code = 1;

    if (argc != 7)
    {
        fwprintf(stderr,
            L"Usage: creo_feature_suppress_bridge <result.json> <expected_model> "
            L"<copy_name> <output_dir> <feature_id> <expected_type_code>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
    }

    feature_id_long = wcstol(argv[5], &parse_end, 10);
    if (*argv[5] == L'\0' || *parse_end != L'\0' || feature_id_long <= 0 || feature_id_long > INT_MAX)
    {
        exit_code = write_error(out, "feature_id_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    expected_type_long = wcstol(argv[6], &parse_end, 10);
    if (*argv[6] == L'\0' || *parse_end != L'\0' || expected_type_long <= 0 || expected_type_long > INT_MAX)
    {
        exit_code = write_error(out, "feature_type_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    feature_id = (int)feature_id_long;
    expected_type = (int)expected_type_long;

    wcsncpy_s(expected_name,
        sizeof(expected_name) / sizeof(expected_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(copy_name,
        sizeof(copy_name) / sizeof(copy_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(output_directory,
        sizeof(output_directory) / sizeof(output_directory[0]),
        argv[4], _TRUNCATE);

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

    status = ProFeatureInit((ProSolid)source_model, feature_id, &source_feature);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_feature_init", status);
        goto cleanup;
    }
    status = ProFeatureTypeGet(&source_feature, &source_feature_type);
    if (status != PRO_TK_NO_ERROR || source_feature_type != expected_type)
    {
        exit_code = write_error(out, "feature_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    if (!is_suppressible_feature_type(source_feature_type))
    {
        exit_code = write_error(out, "suppressible_type_guard", PRO_TK_NO_PERMISSION);
        goto cleanup;
    }
    status = ProFeatureStatusGet(&source_feature, &source_feature_status);
    if (status != PRO_TK_NO_ERROR || source_feature_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "feature_status_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProFeatureVisibilityGet(&source_feature, &visible);
    if (status != PRO_TK_NO_ERROR || visible != PRO_B_TRUE)
    {
        exit_code = write_error(out, "feature_visibility_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_NO_PERMISSION : status);
        goto cleanup;
    }
    status = ProFeatureChildrenGet(&source_feature, &children, &child_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_children_guard", status);
        goto cleanup;
    }
    if (children != NULL)
    {
        ProArrayFree((ProArray *)&children);
        children = NULL;
    }
    if (child_count != 0)
    {
        exit_code = write_error(out, "childless_feature_guard", PRO_TK_NO_PERMISSION);
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

    status = ProFeatureInit((ProSolid)copy_model, feature_id, &copy_feature);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_feature_init", status);
        goto cleanup;
    }
    status = ProFeatureSuppress(
        (ProSolid)copy_model,
        &feature_id,
        1,
        &suppress_option,
        1);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_suppress", status);
        goto cleanup;
    }
    status = ProFeatureInit((ProSolid)copy_model, feature_id, &copy_feature);
    if (status != PRO_TK_NO_ERROR ||
        ProFeatureStatusGet(&copy_feature, &copy_feature_status) != PRO_TK_NO_ERROR ||
        copy_feature_status != PRO_FEAT_SUPPRESSED)
    {
        exit_code = write_error(out, "suppression_readback", PRO_TK_GENERAL_ERROR);
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
    fprintf(out,
        ",\"feature_id\":%d,\"feature_type_code\":%d,"
        "\"old_status\":%d,\"new_status\":%d,"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,\"saved_file\":",
        feature_id,
        source_feature_type,
        source_feature_status,
        copy_feature_status,
        regenerate_status,
        regenerate_attempts);
    write_wide_json_string(out, saved_path);
    fputs("}\n", out);
    exit_code = 0;

cleanup:
    if (children != NULL)
        ProArrayFree((ProArray *)&children);
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
