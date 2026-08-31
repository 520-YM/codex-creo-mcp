#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProWindows.h>

static void json_utf8(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', out);
    while (*p)
    {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        if (*p < 0x20) fprintf(out, "\\u%04x", (unsigned int)*p);
        else fputc(*p, out);
        ++p;
    }
    fputc('"', out);
}

static void json_wide(FILE *out, const wchar_t *text)
{
    int bytes;
    char *utf8;
    if (text == NULL) { fputs("null", out); return; }
    bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0) { fputs("\"\"", out); return; }
    utf8 = (char *)malloc((size_t)bytes);
    if (utf8 == NULL) { fputs("\"\"", out); return; }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, bytes, NULL, NULL);
    json_utf8(out, utf8);
    free(utf8);
}

static int fail(FILE *out, const char *stage, ProError status)
{
    fputs("{\"ok\":false,\"api_only\":true,\"stage\":", out);
    json_utf8(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", status);
    return 1;
}

static int assembly_exists(const wchar_t *directory, const wchar_t *name)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    _snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
        L"%ls\\%ls.asm*", directory, name);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    FindClose(handle);
    return 1;
}

static int latest_assembly(
    const wchar_t *directory, const wchar_t *name,
    wchar_t *saved_path, size_t saved_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    int found = 0;
    int best = -1;
    _snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
        L"%ls\\%ls.asm*", directory, name);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    do
    {
        wchar_t *dot = wcsrchr(data.cFileName, L'.');
        int version = dot == NULL ? 0 : _wtoi(dot + 1);
        if (!found || version > best)
        {
            _snwprintf_s(saved_path, saved_count, _TRUNCATE,
                L"%ls\\%ls", directory, data.cFileName);
            best = version;
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
    ProProcessHandle process;
    ProMdl template_model = NULL;
    ProMdl assembly_model = NULL;
    ProMdl current_model = NULL;
    ProMdlName assembly_name, actual_name;
    ProPath template_path, output_directory, saved_path;
    ProMdlType actual_type = PRO_MDL_UNUSED;
    int connected = 0;
    int window_id = -1;
    int attempts = 0;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    int exit_code = 1;

    if (argc != 5)
    {
        fwprintf(stderr,
            L"Usage: empty_assembly <result.json> <template.asm> "
            L"<assembly_name> <output_dir>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL) return 2;
    wcsncpy_s(template_path, sizeof(template_path) / sizeof(template_path[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(assembly_name, sizeof(assembly_name) / sizeof(assembly_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(output_directory,
        sizeof(output_directory) / sizeof(output_directory[0]), argv[4], _TRUNCATE);

    if (GetFileAttributesW(template_path) == INVALID_FILE_ATTRIBUTES)
    { exit_code = fail(out, "assembly_template", PRO_TK_E_NOT_FOUND); goto done; }
    if (GetFileAttributesW(output_directory) == INVALID_FILE_ATTRIBUTES)
    { exit_code = fail(out, "output_directory", PRO_TK_INVALID_DIR); goto done; }
    if (assembly_exists(output_directory, assembly_name))
    { exit_code = fail(out, "refuse_overwrite", PRO_TK_E_FOUND); goto done; }

    status = ProEngineerConnect("", "", "", "", PRO_B_TRUE, 20,
        &random_choice, &process);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "connect", status); goto done; }
    connected = 1;
    status = ProDirectoryChange(output_directory);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "set_project_directory", status); goto cleanup; }
    status = ProMdlFiletypeLoad(
        template_path, PRO_MDLFILE_ASSEMBLY, PRO_B_FALSE, &template_model);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "load_assembly_template", status); goto cleanup; }
    status = ProMdlnameCopy(template_model, assembly_name, &assembly_model);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "copy_assembly_template", status); goto cleanup; }
    status = ProMdlTypeGet(assembly_model, &actual_type);
    if (status != PRO_TK_NO_ERROR || actual_type != PRO_MDL_ASSEMBLY)
    { exit_code = fail(out, "assembly_type_guard",
        status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status); goto cleanup; }
    status = ProMdlNameGet(assembly_model, actual_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_name, assembly_name) != 0)
    { exit_code = fail(out, "assembly_name_guard",
        status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto cleanup; }
    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
        ++attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "regenerate_assembly", regenerate_status); goto cleanup; }
    status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "save_assembly", status); goto cleanup; }
    if (!latest_assembly(output_directory, assembly_name, saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    { exit_code = fail(out, "verify_saved_assembly", PRO_TK_E_NOT_FOUND); goto cleanup; }
    status = ProMdlDisplay(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "display_assembly", status); goto cleanup; }
    status = ProMdlCurrentGet(&current_model);
    if (status != PRO_TK_NO_ERROR || current_model != assembly_model)
    {
        status = ProObjectwindowMdlnameCreate(
            assembly_name, PRO_ASSEMBLY, &window_id);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "create_assembly_window", status); goto cleanup; }
        status = ProWindowCurrentSet(window_id);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "set_assembly_window", status); goto cleanup; }
        status = ProWindowActivate(window_id);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "activate_assembly_window", status); goto cleanup; }
        ProMdlDisplay(assembly_model);
    }
    if (window_id < 0) ProWindowCurrentGet(&window_id);
    if (window_id >= 0)
    {
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }
    fputs("{\"ok\":true,\"api_only\":true,\"assembly\":", out);
    json_wide(out, actual_name);
    fprintf(out,
        ",\"empty_assembly\":true,\"component_count\":0,"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"saved_file\":", regenerate_status, attempts);
    json_wide(out, saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (template_model != NULL && template_model != assembly_model)
        ProMdlErase(template_model);
    if (connected) ProEngineerDisconnect(&process, 10);
done:
    fclose(out);
    return exit_code;
}
