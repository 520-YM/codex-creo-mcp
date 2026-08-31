#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProSkeleton.h>
#include <ProDimension.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>

typedef struct dimension_visit_context
{
    FILE *out;
    int count;
} DimensionVisitContext;

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

static const char *dimension_type_name(ProDimensiontype type)
{
    switch (type)
    {
    case PRODIMTYPE_LINEAR: return "linear";
    case PRODIMTYPE_RADIUS: return "radius";
    case PRODIMTYPE_DIAMETER: return "diameter";
    case PRODIMTYPE_ANGLE: return "angle";
    case PRODIMTYPE_ARC_LENGTH: return "arc_length";
    default: return "unknown";
    }
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
    case PRO_FEAT_OFFSET: return "offset";
    default: return "other";
    }
}

static ProError dimension_visit_action(
    ProDimension *dimension,
    ProError filter_status,
    ProAppData app_data)
{
    DimensionVisitContext *context = (DimensionVisitContext *)app_data;
    ProError symbol_status;
    ProError value_status;
    ProError type_status;
    ProError relation_status;
    ProError negative_status;
    ProError owner_status;
    ProError owner_name_status = PRO_TK_E_NOT_FOUND;
    ProError owner_type_status = PRO_TK_E_NOT_FOUND;
    ProName symbol = L"";
    ProName owner_name = L"";
    ProDimensiontype dimension_type = PRODIMTYPE_UNKNOWN;
    ProBoolean relation_driven = PRO_B_FALSE;
    ProBoolean regened_negative = PRO_B_FALSE;
    ProFeature owner_feature;
    ProFeattype owner_feature_type = -1;
    double value = 0.0;
    int owner_feature_id = -1;

    (void)filter_status;
    symbol_status = ProDimensionSymbolGet(dimension, symbol);
    value_status = ProDimensionValueGet(dimension, &value);
    type_status = ProDimensionTypeGet(dimension, &dimension_type);
    relation_status = ProDimensionIsReldriven(dimension, &relation_driven);
    negative_status = ProDimensionIsRegenednegative(dimension, &regened_negative);
    owner_status = ProDimensionOwnerfeatureGet(dimension, &owner_feature);
    if (owner_status == PRO_TK_NO_ERROR)
    {
        owner_feature_id = owner_feature.id;
        owner_name_status = ProModelitemNameGet((ProModelitem *)&owner_feature, owner_name);
        owner_type_status = ProFeatureTypeGet(&owner_feature, &owner_feature_type);
    }

    if (context->count > 0)
        fputc(',', context->out);
    fputs("{\"id\":", context->out);
    fprintf(context->out, "%d,\"symbol\":", dimension->id);
    if (symbol_status == PRO_TK_NO_ERROR)
        write_wide_json_string(context->out, symbol);
    else
        fputs("null", context->out);
    fputs(",\"value\":", context->out);
    if (value_status == PRO_TK_NO_ERROR)
        fprintf(context->out, "%.17g", value);
    else
        fputs("null", context->out);
    fputs(",\"type\":", context->out);
    write_utf8_json_string(context->out,
        type_status == PRO_TK_NO_ERROR ? dimension_type_name(dimension_type) : "unknown");
    fprintf(context->out,
        ",\"type_code\":%d,\"relation_driven\":%s,\"regenerated_negative\":%s,"
        "\"owner_feature_id\":%d,\"owner_feature_type\":",
        type_status == PRO_TK_NO_ERROR ? (int)dimension_type : -1,
        relation_status == PRO_TK_NO_ERROR && relation_driven == PRO_B_TRUE ? "true" : "false",
        negative_status == PRO_TK_NO_ERROR && regened_negative == PRO_B_TRUE ? "true" : "false",
        owner_feature_id);
    if (owner_type_status == PRO_TK_NO_ERROR)
        write_utf8_json_string(context->out, feature_type_name(owner_feature_type));
    else
        fputs("null", context->out);
    fprintf(context->out, ",\"owner_feature_type_code\":%d,\"owner_feature_name\":",
        owner_type_status == PRO_TK_NO_ERROR ? owner_feature_type : -1);
    if (owner_name_status == PRO_TK_NO_ERROR)
        write_wide_json_string(context->out, owner_name);
    else
        fputs("null", context->out);
    fprintf(context->out,
        ",\"read_status\":{\"symbol\":%d,\"value\":%d,\"type\":%d,"
        "\"relation\":%d,\"negative\":%d,\"owner\":%d,\"owner_type\":%d}}",
        symbol_status,
        value_status,
        type_status,
        relation_status,
        negative_status,
        owner_status,
        owner_type_status);
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
    ProMdl assembly_model = NULL;
    ProMdlName model_name;
    ProMdlName requested_skeleton_name;
    ProMdlType model_type;
    ProBoolean is_skeleton = PRO_B_FALSE;
    DimensionVisitContext context;
    int connected = 0;
    int loaded_from_file = 0;
    int exit_code = 1;

    if (argc != 2 && argc != 3 && argc != 4)
    {
        fwprintf(stderr,
            L"Usage: creo_dimensions_bridge <result.json> [model_file] "
            L"or <result.json> <assembly_name> <skeleton_name>\n");
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

    if (argc == 4)
    {
        ProMdlName assembly_name;
        wcsncpy_s(assembly_name,
            sizeof(assembly_name) / sizeof(assembly_name[0]),
            argv[2], _TRUNCATE);
        wcsncpy_s(requested_skeleton_name,
            sizeof(requested_skeleton_name) /
                sizeof(requested_skeleton_name[0]),
            argv[3], _TRUNCATE);
        status = ProMdlnameInit(
            assembly_name, PRO_MDLFILE_ASSEMBLY, &assembly_model);
        if (status == PRO_TK_NO_ERROR)
            status = ProAsmSkeletonGet(
                (ProAssembly)assembly_model, &model);
    }
    else if (argc == 3)
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
            argc == 4
                ? "assembly_skeleton_get"
                : (argc == 3 ? "load_model_file" : "current_model"),
            status);
        goto cleanup;
    }
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_name", status);
        goto cleanup;
    }
    if (argc == 4 &&
        _wcsicmp(model_name, requested_skeleton_name) != 0)
    {
        exit_code = write_error(out, "skeleton_name_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status == PRO_TK_NO_ERROR && model_type != PRO_MDL_PART)
        status = ProMdlIsSkeleton(model, &is_skeleton);
    if (status != PRO_TK_NO_ERROR ||
        (model_type != PRO_MDL_PART && is_skeleton != PRO_B_TRUE))
    {
        exit_code = write_error(out, "model_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"readonly\":true,\"loaded_from_file\":", out);
    fputs(loaded_from_file ? "true" : "false", out);
    fputs(",\"model\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"dimensions\":[", out);
    context.out = out;
    context.count = 0;
    status = ProSolidDimensionVisit(
        (ProSolid)model,
        PRO_B_FALSE,
        dimension_visit_action,
        NULL,
        (ProAppData)&context);
    fputs("],\"dimension_count\":", out);
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
