#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProFeatType.h>

typedef struct ComponentAudit
{
    int total;
    int active;
    int inactive;
    int status_read_failures;
} ComponentAudit;

static ProError component_audit_action(
    ProFeature *feature, ProError filter_status, ProAppData app_data)
{
    ComponentAudit *audit = (ComponentAudit *)app_data;
    ProFeattype type = PRO_FEAT_INVALID;
    ProFeatStatus status = PRO_FEAT_INVALID;
    (void)filter_status;
    if (ProFeatureTypeGet(feature, &type) != PRO_TK_NO_ERROR ||
        type != PRO_FEAT_COMPONENT)
        return PRO_TK_NO_ERROR;
    ++audit->total;
    if (ProFeatureStatusGet(feature, &status) != PRO_TK_NO_ERROR)
        ++audit->status_read_failures;
    else if (status == PRO_FEAT_ACTIVE)
        ++audit->active;
    else
        ++audit->inactive;
    return PRO_TK_NO_ERROR;
}

static void write_utf8_json_string(FILE *out, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    fputc('"', out);
    while (*cursor)
    {
        if (*cursor == '"' || *cursor == '\\') fputc('\\', out);
        if (*cursor < 0x20) fprintf(out, "\\u%04x", (unsigned int)*cursor);
        else fputc(*cursor, out);
        ++cursor;
    }
    fputc('"', out);
}

static void write_wide_json_string(FILE *out, const wchar_t *text)
{
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *utf8;
    if (bytes < 1) { fputs("\"\"", out); return; }
    utf8 = (char *)malloc((size_t)bytes);
    if (utf8 == NULL) { fputs("\"\"", out); return; }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, bytes, NULL, NULL);
    write_utf8_json_string(out, utf8);
    free(utf8);
}

static int fail(FILE *out, const char *stage, ProError status)
{
    fputs("{\"ok\":false,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", status);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = NULL;
    ProMdl model = NULL;
    ProMdlType type = PRO_MDL_UNUSED;
    ProMdlName actual_name;
    ProError status = PRO_TK_GENERAL_ERROR;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    ProError save_status = PRO_TK_NO_ERROR;
    ComponentAudit audit;
    int attempts = 0;
    int used_standard_fallback = 0;
    int save_requested;
    int exit_code = 1;

    if (argc != 4) return 2;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL) return 2;
    save_requested = wcscmp(argv[3], L"1") == 0;
    if (!save_requested && wcscmp(argv[3], L"0") != 0)
    { exit_code = fail(out, "save_flag", PRO_TK_BAD_INPUTS); goto done; }
    status = ProMdlCurrentGet(&model);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "current_model", status); goto done; }
    status = ProMdlTypeGet(model, &type);
    if (status != PRO_TK_NO_ERROR || type != PRO_MDL_ASSEMBLY)
    { exit_code = fail(out, "top_assembly_type", status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status); goto done; }
    status = ProMdlNameGet(model, actual_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_name, argv[2]) != 0)
    { exit_code = fail(out, "top_assembly_name", status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto done; }
    do
    {
        regenerate_status = ProSolidRegenerate((ProSolid)model, PRO_REGEN_FORCE_REGEN);
        ++attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && attempts < 3);
    if (regenerate_status == PRO_TK_GENERAL_ERROR)
    {
        used_standard_fallback = 1;
        attempts = 0;
        do
        {
            regenerate_status = ProSolidRegenerate(
                (ProSolid)model, PRO_REGEN_NO_FLAGS);
            ++attempts;
        } while (regenerate_status == PRO_TK_REGEN_AGAIN && attempts < 3);
    }
    if (regenerate_status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "regenerate_top_assembly", regenerate_status); goto done; }
    memset(&audit, 0, sizeof(audit));
    status = ProSolidFeatVisit((ProSolid)model, component_audit_action, NULL, (ProAppData)&audit);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "component_audit", status); goto done; }
    if (audit.status_read_failures > 0)
    { exit_code = fail(out, "component_status_readback", PRO_TK_GENERAL_ERROR); goto done; }
    if (save_requested)
    {
        save_status = ProMdlSave(model);
        if (save_status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "save_top_assembly", save_status); goto done; }
    }
    fputs("{\"ok\":true,\"model\":", out);
    write_wide_json_string(out, actual_name);
    fprintf(out,
        ",\"regenerated\":true,\"regenerate_status\":%d,"
        "\"regenerate_attempts\":%d,\"regenerate_mode\":\"%s\","
        "\"component_audit\":{"
        "\"total\":%d,\"active\":%d,\"inactive\":%d,"
        "\"status_read_failures\":%d},\"save_requested\":%s,"
        "\"saved\":%s,\"save_status\":%d}\n",
        regenerate_status, attempts,
        used_standard_fallback ? "standard_fallback" : "force",
        audit.total, audit.active, audit.inactive,
        audit.status_read_failures, save_requested ? "true" : "false",
        save_requested ? "true" : "false", save_status);
    exit_code = 0;
done:
    fclose(out);
    return exit_code;
}
