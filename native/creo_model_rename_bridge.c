#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProWindows.h>

static void json_wide(FILE *out, const wchar_t *text)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *utf8;
    const unsigned char *p;
    if (size <= 0) { fputs("\"\"", out); return; }
    utf8 = (char *)malloc((size_t)size);
    if (!utf8) { fputs("\"\"", out); return; }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, size, NULL, NULL);
    fputc('"', out);
    for (p = (const unsigned char *)utf8; *p; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        fputc(*p, out);
    }
    fputc('"', out);
    free(utf8);
}

static int fail(FILE *out, const char *stage, ProError error)
{
    fprintf(out, "{\"ok\":false,\"stage\":\"%s\",\"error_code\":%d}\n", stage, error);
    return 1;
}

static int model_family_exists(const wchar_t *directory, const wchar_t *name)
{
    WIN32_FIND_DATAW data;
    HANDLE handle;
    wchar_t pattern[MAX_PATH * 2];
    _snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
        L"%ls\\%ls.prt*", directory, name);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    FindClose(handle);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl current = NULL, probe = NULL;
    ProMdlType type = PRO_MDL_UNUSED;
    ProMdlName before, after;
    ProPath directory;
    ProError status, disconnect_status;
    int connected = 0, renamed = 0, exit_code = 1;

    if (argc != 4) {
        fwprintf(stderr, L"Usage: creo_model_rename_bridge <result.json> <expected_name> <target_name>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || !out) return 2;
    if (!*argv[2] || !*argv[3] || wcslen(argv[3]) >= PRO_NAME_SIZE) {
        exit_code = fail(out, "input", PRO_TK_BAD_INPUTS); goto done;
    }
    status = ProEngineerConnect("", "", "", "", PRO_B_TRUE, 10, &random_choice, &process_handle);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "connect", status); goto done; }
    connected = 1;
    status = ProMdlCurrentGet(&current);
    if (status == PRO_TK_NO_ERROR) status = ProMdlTypeGet(current, &type);
    if (status == PRO_TK_NO_ERROR) status = ProMdlNameGet(current, before);
    if (status != PRO_TK_NO_ERROR || type != PRO_MDL_PART || _wcsicmp(before, argv[2]) != 0) {
        exit_code = fail(out, "current_part_guard", status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(directory);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "working_directory", status); goto cleanup; }
    if (model_family_exists(directory, argv[3]) ||
        ProMdlnameInit(argv[3], PRO_MDLFILE_PART, &probe) == PRO_TK_NO_ERROR) {
        exit_code = fail(out, "target_exists", PRO_TK_E_FOUND); goto cleanup;
    }
    status = ProMdlnameRename(current, argv[3]);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "rename", status); goto cleanup; }
    renamed = 1;
    status = ProSolidRegenerate((ProSolid)current, PRO_REGEN_NO_FLAGS);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "regenerate", status); goto rollback; }
    status = ProMdlSave(current);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "save", status); goto rollback; }
    status = ProMdlNameGet(current, after);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(after, argv[3]) != 0 || !model_family_exists(directory, argv[3])) {
        exit_code = fail(out, "readback", status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    fputs("{\"ok\":true,\"old_name\":", out); json_wide(out, before);
    fputs(",\"new_name\":", out); json_wide(out, after);
    fputs(",\"working_directory\":", out); json_wide(out, directory);
    fputs(",\"saved\":true}\n", out);
    exit_code = 0;
    goto cleanup;

rollback:
    if (renamed) ProMdlnameRename(current, argv[2]);
cleanup:
    if (connected) {
        disconnect_status = ProEngineerDisconnect(&process_handle, 10);
        (void)disconnect_status;
    }
done:
    fclose(out);
    return exit_code;
}
