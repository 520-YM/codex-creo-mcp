#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProSkeleton.h>
#include <ProWindows.h>

static void write_utf8_json_string(FILE *out, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    fputc('"', out);
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

static int write_error(FILE *out, const char *stage, ProError status)
{
    fputs("{\"ok\":false,\"api_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", status);
    return 1;
}

static int output_model_exists(
    const wchar_t *directory,
    const wchar_t *name,
    const wchar_t *extension)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.%ls*",
        directory,
        name,
        extension);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(handle);
    return 1;
}

static int find_latest_saved_model(
    const wchar_t *directory,
    const wchar_t *name,
    const wchar_t *extension,
    wchar_t *saved_path,
    size_t saved_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    int found = 0;
    int best_version = -1;
    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.%ls*",
        directory,
        name,
        extension);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        wchar_t *dot = wcsrchr(data.cFileName, L'.');
        int version = dot == NULL ? 0 : _wtoi(dot + 1);
        if (!found || version > best_version)
        {
            _snwprintf_s(saved_path,
                saved_path_count,
                _TRUNCATE,
                L"%ls\\%ls",
                directory,
                data.cFileName);
            best_version = version;
            found = 1;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return found;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl assembly_model = NULL;
    ProMdl template_model = NULL;
    ProMdl skeleton_model = NULL;
    ProMdl skeleton_readback = NULL;
    ProMdlType model_type;
    ProMdlsubtype skeleton_subtype = PROMDLSTYPE_NONE;
    ProMdlName expected_assembly_name;
    ProMdlName actual_assembly_name;
    ProMdlName skeleton_name;
    ProMdlName skeleton_readback_name;
    ProPath template_path;
    ProPath working_directory;
    ProPath assembly_saved_path;
    ProPath skeleton_saved_path;
    ProBoolean is_skeleton = PRO_B_FALSE;
    int connected = 0;
    int skeleton_created = 0;
    int window_id = -1;
    int regenerate_attempts = 0;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    int exit_code = 1;

    if (argc != 5)
    {
        fwprintf(stderr,
            L"Usage: creo_assembly_skeleton_bridge <result.json> <expected_assembly> "
            L"<skeleton_name> <template.prt>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(expected_assembly_name,
        sizeof(expected_assembly_name) / sizeof(expected_assembly_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(skeleton_name,
        sizeof(skeleton_name) / sizeof(skeleton_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(template_path,
        sizeof(template_path) / sizeof(template_path[0]),
        argv[4], _TRUNCATE);
    if (GetFileAttributesW(template_path) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "skeleton_template", PRO_TK_E_NOT_FOUND);
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
    status = ProMdlCurrentGet(&assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_assembly", status);
        goto cleanup;
    }
    status = ProMdlTypeGet(assembly_model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_ASSEMBLY)
    {
        exit_code = write_error(out, "assembly_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProMdlNameGet(assembly_model, actual_assembly_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(actual_assembly_name, expected_assembly_name) != 0)
    {
        exit_code = write_error(out, "assembly_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    if (output_model_exists(working_directory, skeleton_name, L"prt"))
    {
        exit_code = write_error(out, "refuse_overwrite", PRO_TK_E_FOUND);
        goto cleanup;
    }
    status = ProAsmSkeletonGet((ProAssembly)assembly_model, &skeleton_readback);
    if (status == PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_already_exists", PRO_TK_E_FOUND);
        goto cleanup;
    }
    if (status != PRO_TK_E_NOT_FOUND)
    {
        exit_code = write_error(out, "skeleton_precheck", status);
        goto cleanup;
    }
    skeleton_readback = NULL;

    status = ProMdlFiletypeLoad(
        template_path, PRO_MDLFILE_PART, PRO_B_FALSE, &template_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "load_skeleton_template", status);
        goto cleanup;
    }
    status = ProAsmSkeletonMdlnameCreate(
        (ProAssembly)assembly_model,
        skeleton_name,
        template_model,
        &skeleton_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "create_standard_skeleton", status);
        goto cleanup;
    }
    skeleton_created = 1;

    status = ProMdlIsSkeleton(skeleton_model, &is_skeleton);
    if (status != PRO_TK_NO_ERROR || is_skeleton != PRO_B_TRUE)
    {
        exit_code = write_error(out, "skeleton_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProAsmSkeletonGet((ProAssembly)assembly_model, &skeleton_readback);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_readback", status);
        goto cleanup;
    }
    status = ProMdlNameGet(skeleton_readback, skeleton_readback_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(skeleton_readback_name, skeleton_name) != 0)
    {
        exit_code = write_error(out, "skeleton_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProMdlSubtypeGet(skeleton_model, &skeleton_subtype);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_subtype_readback", status);
        goto cleanup;
    }

    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "regenerate_assembly", regenerate_status);
        goto cleanup;
    }
    status = ProMdlSave(skeleton_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_skeleton", status);
        goto cleanup;
    }
    status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_assembly", status);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            working_directory,
            skeleton_name,
            L"prt",
            skeleton_saved_path,
            sizeof(skeleton_saved_path) / sizeof(skeleton_saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_skeleton", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            working_directory,
            actual_assembly_name,
            L"asm",
            assembly_saved_path,
            sizeof(assembly_saved_path) / sizeof(assembly_saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_assembly", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    if (ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
    {
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"assembly\":", out);
    write_wide_json_string(out, actual_assembly_name);
    fputs(",\"skeleton\":", out);
    write_wide_json_string(out, skeleton_readback_name);
    fprintf(out,
        ",\"is_skeleton\":true,\"standard_subtype\":true,"
        "\"model_subtype_code\":%d,"
        "\"creation_api\":\"ProAsmSkeletonMdlnameCreate\","
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"skeleton_saved_file\":",
        skeleton_subtype,
        regenerate_status,
        regenerate_attempts);
    write_wide_json_string(out, skeleton_saved_path);
    fputs(",\"assembly_saved_file\":", out);
    write_wide_json_string(out, assembly_saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0 && skeleton_created && assembly_model != NULL)
        ProAsmSkeletonDelete((ProAssembly)assembly_model);
    if (template_model != NULL && template_model != skeleton_model)
        ProMdlErase(template_model);
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
