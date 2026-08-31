#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProWindows.h>

static void write_utf8_json_string(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', out);
    while (*p)
    {
        if (*p == '"' || *p == '\\')
        {
            fputc('\\', out);
            fputc(*p, out);
        }
        else if (*p < 0x20)
            fprintf(out, "\\u%04x", (unsigned int)*p);
        else
            fputc(*p, out);
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
    fputs("{\"ok\":false,\"api_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

static int output_model_exists(const wchar_t *directory, const wchar_t *name)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.prt*",
        directory,
        name);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(handle);
    return 1;
}

static int find_latest_saved_model(
    const wchar_t *directory,
    const wchar_t *name,
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
        L"%ls\\%ls.prt*",
        directory,
        name);
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
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl template_model = NULL;
    ProMdl new_model = NULL;
    ProMdl current_model = NULL;
    ProMdlName new_name;
    ProMdlName current_name;
    ProMdlsubtype model_subtype = PROMDLSTYPE_NONE;
    ProPath template_path;
    ProPath output_directory;
    ProPath original_directory;
    ProPath saved_path;
    int connected = 0;
    int directory_changed = 0;
    int keep_working_directory = 0;
    int window_id = -1;
    int exit_code = 1;

    if (argc != 5 && argc != 6)
    {
        fwprintf(stderr,
            L"Usage: creo_template_copy_bridge <result.json> <template.prt> "
            L"<new_name> <output_dir> [KEEP_WORKDIR]\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(template_path,
        sizeof(template_path) / sizeof(template_path[0]), argv[2], _TRUNCATE);
    wcsncpy_s(new_name,
        sizeof(new_name) / sizeof(new_name[0]), argv[3], _TRUNCATE);
    wcsncpy_s(output_directory,
        sizeof(output_directory) / sizeof(output_directory[0]), argv[4], _TRUNCATE);
    if (argc == 6)
    {
        if (_wcsicmp(argv[5], L"KEEP_WORKDIR") != 0)
        {
            exit_code = write_error(out, "working_directory_option", PRO_TK_BAD_INPUTS);
            goto done;
        }
        keep_working_directory = 1;
    }

    if (GetFileAttributesW(template_path) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "template_file", PRO_TK_E_NOT_FOUND);
        goto done;
    }
    if (GetFileAttributesW(output_directory) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "output_directory", PRO_TK_INVALID_DIR);
        goto done;
    }
    if (output_model_exists(output_directory, new_name))
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
        template_path, PRO_MDLFILE_PART, PRO_B_FALSE, &template_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "load_template", status);
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
    status = ProMdlnameCopy(template_model, new_name, &new_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_template", status);
        goto cleanup;
    }
    status = ProMdlSubtypeGet(new_model, &model_subtype);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_subtype", status);
        goto cleanup;
    }
    if (!keep_working_directory)
    {
        ProDirectoryChange(original_directory);
        directory_changed = 0;
    }
    status = ProMdlSave(new_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_new_model", status);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            output_directory,
            new_name,
            saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_model", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    status = ProMdlDisplay(new_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "display_new_model", status);
        goto cleanup;
    }
    status = ProMdlCurrentGet(&current_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(current_model, current_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(current_name, new_name) != 0)
    {
        status = ProObjectwindowMdlnameCreate(new_name, PRO_PART, &window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "create_model_window", status);
            goto cleanup;
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
    }
    if (window_id < 0 && ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
        ProWindowActivate(window_id);
    if (window_id >= 0)
    {
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"template\":", out);
    write_wide_json_string(out, template_path);
    fputs(",\"model\":", out);
    write_wide_json_string(out, new_name);
    fprintf(out, ",\"model_subtype_code\":%d,\"saved_file\":", model_subtype);
    write_wide_json_string(out, saved_path);
    fputs(",\"working_directory\":", out);
    write_wide_json_string(out,
        keep_working_directory ? output_directory : original_directory);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;
    if (keep_working_directory)
        directory_changed = 0;

cleanup:
    if (directory_changed)
        ProDirectoryChange(original_directory);
    if (template_model != NULL && template_model != new_model)
        ProMdlErase(template_model);
    if (connected)
        ProEngineerDisconnect(&process_handle, 10);
done:
    fclose(out);
    return exit_code;
}
