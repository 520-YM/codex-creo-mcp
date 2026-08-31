#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProArray.h>

typedef struct feature_visit_context
{
    FILE *out;
    int count;
} FeatureVisitContext;

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

static const char *feature_type_name(ProFeattype type)
{
    switch (type)
    {
    case PRO_FEAT_PROTRUSION: return "protrusion";
    case PRO_FEAT_CUT: return "cut";
    case PRO_FEAT_HOLE: return "hole";
    case PRO_FEAT_ROUND: return "round";
    case PRO_FEAT_CHAMFER: return "chamfer";
    case PRO_FEAT_SHELL: return "shell";
    case PRO_FEAT_DATUM: return "datum";
    case PRO_FEAT_DATUM_SURF: return "datum_surface";
    case PRO_FEAT_DATUM_AXIS: return "datum_axis";
    case PRO_FEAT_DATUM_POINT: return "datum_point";
    case PRO_FEAT_CSYS: return "coordinate_system";
    case PRO_FEAT_ZONE: return "zone";
    default: return "other";
    }
}

static const char *feature_status_name(ProFeatStatus status)
{
    switch (status)
    {
    case PRO_FEAT_ACTIVE: return "active";
    case PRO_FEAT_INACTIVE: return "inactive";
    case PRO_FEAT_FAMTAB_SUPPRESSED: return "family_table_suppressed";
    case PRO_FEAT_SIMP_REP_SUPPRESSED: return "simplified_rep_suppressed";
    case PRO_FEAT_PROG_SUPPRESSED: return "program_suppressed";
    case PRO_FEAT_SUPPRESSED: return "suppressed";
    case PRO_FEAT_UNREGENERATED: return "unregenerated";
    default: return "invalid";
    }
}

static void write_id_array(FILE *out, int *ids, int count)
{
    int i;
    fputc('[', out);
    for (i = 0; i < count; ++i)
    {
        if (i > 0)
            fputc(',', out);
        fprintf(out, "%d", ids[i]);
    }
    fputc(']', out);
}

static ProError feature_visit_action(
    ProFeature *feature,
    ProError filter_status,
    ProAppData app_data)
{
    FeatureVisitContext *context = (FeatureVisitContext *)app_data;
    ProError name_status;
    ProError type_status;
    ProError status_status;
    ProError visibility_status;
    ProError parents_status;
    ProError children_status;
    ProName name = L"";
    ProFeattype feature_type = -1;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProBoolean visible = PRO_B_FALSE;
    int *parents = NULL;
    int *children = NULL;
    int parent_count = 0;
    int child_count = 0;

    (void)filter_status;
    name_status = ProModelitemNameGet((ProModelitem *)feature, name);
    type_status = ProFeatureTypeGet(feature, &feature_type);
    status_status = ProFeatureStatusGet(feature, &feature_status);
    visibility_status = ProFeatureVisibilityGet(feature, &visible);
    parents_status = ProFeatureParentsGet(feature, &parents, &parent_count);
    children_status = ProFeatureChildrenGet(feature, &children, &child_count);

    if (context->count > 0)
        fputc(',', context->out);
    fprintf(context->out, "{\"id\":%d,\"name\":", feature->id);
    if (name_status == PRO_TK_NO_ERROR)
        write_wide_json_string(context->out, name);
    else
        fputs("null", context->out);
    fputs(",\"type\":", context->out);
    write_utf8_json_string(context->out,
        type_status == PRO_TK_NO_ERROR ? feature_type_name(feature_type) : "other");
    fprintf(context->out, ",\"type_code\":%d,\"status\":",
        type_status == PRO_TK_NO_ERROR ? feature_type : -1);
    write_utf8_json_string(context->out,
        status_status == PRO_TK_NO_ERROR ? feature_status_name(feature_status) : "invalid");
    fprintf(context->out,
        ",\"status_code\":%d,\"visible\":%s,\"parent_ids\":",
        status_status == PRO_TK_NO_ERROR ? (int)feature_status : -1,
        visibility_status == PRO_TK_NO_ERROR && visible == PRO_B_TRUE ? "true" : "false");
    write_id_array(context->out, parents, parents_status == PRO_TK_NO_ERROR ? parent_count : 0);
    fputs(",\"child_ids\":", context->out);
    write_id_array(context->out, children, children_status == PRO_TK_NO_ERROR ? child_count : 0);
    fprintf(context->out,
        ",\"read_status\":{\"name\":%d,\"type\":%d,\"status\":%d,"
        "\"visibility\":%d,\"parents\":%d,\"children\":%d}}",
        name_status,
        type_status,
        status_status,
        visibility_status,
        parents_status,
        children_status);

    if (parents != NULL)
        ProArrayFree((ProArray *)&parents);
    if (children != NULL)
        ProArrayFree((ProArray *)&children);
    ++context->count;
    return PRO_TK_NO_ERROR;
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
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProMdlType model_type;
    FeatureVisitContext context;
    int connected = 0;
    int loaded_from_file = 0;
    int exit_code = 1;

    if (argc != 2 && argc != 3)
    {
        fwprintf(stderr, L"Usage: creo_features_bridge <result.json> [model_file]\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
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

    if (argc == 3)
    {
        ProPath model_path;
        wcsncpy_s(model_path,
            sizeof(model_path) / sizeof(model_path[0]),
            argv[2], _TRUNCATE);
        status = ProMdlFiletypeLoad(model_path, PRO_MDLFILE_PART, PRO_B_FALSE, &model);
        if (status == PRO_TK_NO_ERROR)
            loaded_from_file = 1;
    }
    else
    {
        status = ProMdlCurrentGet(&model);
    }
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out,
            argc == 3 ? "load_model_file" : "current_model",
            status);
        goto cleanup;
    }
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_name", status);
        goto cleanup;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART)
    {
        exit_code = write_error(out, "model_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"readonly\":true,\"loaded_from_file\":", out);
    fputs(loaded_from_file ? "true" : "false", out);
    fputs(",\"model\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"features\":[", out);
    context.out = out;
    context.count = 0;
    status = ProSolidFeatVisit(
        (ProSolid)model,
        feature_visit_action,
        NULL,
        (ProAppData)&context);
    fputs("],\"feature_count\":", out);
    fprintf(out, "%d,\"visit_status\":%d}\n", context.count, status);
    exit_code = (status == PRO_TK_NO_ERROR || status == PRO_TK_E_NOT_FOUND) ? 0 : 1;

cleanup:
    if (loaded_from_file)
        ProMdlErase(model);
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
