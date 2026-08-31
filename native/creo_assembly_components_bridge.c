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
#include <ProAsmcomp.h>
#include <ProArray.h>
#include <ProSelection.h>
#include <ProModelitem.h>

typedef struct ComponentContext
{
    FILE *out;
    int count;
} ComponentContext;

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
    fputs("{\"ok\":false,\"readonly\":true,\"api_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", status);
    return 1;
}

static ProError component_action(
    ProFeature *feature,
    ProError filter_status,
    ProAppData app_data)
{
    ComponentContext *context = (ComponentContext *)app_data;
    ProFeattype type = PRO_FEAT_INVALID;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProMdl component_model = NULL;
    ProMdlName component_name;
    ProMatrix position;
    ProAsmcompconstraint *constraints = NULL;
    ProError status;
    int constraint_count = 0;
    int i;

    (void)filter_status;
    if (ProFeatureTypeGet(feature, &type) != PRO_TK_NO_ERROR ||
        type != PRO_FEAT_COMPONENT ||
        ProFeatureStatusGet(feature, &feature_status) != PRO_TK_NO_ERROR ||
        feature_status != PRO_FEAT_ACTIVE)
        return PRO_TK_NO_ERROR;

    status = ProAsmcompMdlGet((ProAsmcomp *)feature, &component_model);
    if (status != PRO_TK_NO_ERROR ||
        ProMdlNameGet(component_model, component_name) != PRO_TK_NO_ERROR ||
        ProAsmcompPositionGet((ProAsmcomp *)feature, position) != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;

    status = ProAsmcompConstraintsWithComppathGet(
        (ProAsmcomp *)feature, NULL, &constraints);
    if (status == PRO_TK_NO_ERROR)
        ProArraySizeGet((ProArray)constraints, &constraint_count);

    if (context->count > 0)
        fputc(',', context->out);
    fputs("{\"feature_id\":", context->out);
    fprintf(context->out, "%d,\"name\":", feature->id);
    write_wide_json_string(context->out, component_name);
    fprintf(context->out,
        ",\"feature_status\":%d,\"translation\":[%.17g,%.17g,%.17g],"
        "\"matrix\":[",
        feature_status,
        position[3][0], position[3][1], position[3][2]);
    for (i = 0; i < 16; ++i)
    {
        if (i > 0)
            fputc(',', context->out);
        fprintf(context->out, "%.17g", position[i / 4][i % 4]);
    }
    fprintf(context->out, "],\"constraint_count\":%d,\"constraints\":[", constraint_count);
    for (i = 0; i < constraint_count; ++i)
    {
        ProAsmcompConstrType constraint_type = PRO_ASM_UNDEF;
        ProSelection assembly_reference = NULL;
        ProSelection component_reference = NULL;
        ProDatumside assembly_side = PRO_DATUM_SIDE_NONE;
        ProDatumside component_side = PRO_DATUM_SIDE_NONE;
        ProModelitem assembly_item;
        ProModelitem component_item;
        ProMdlName assembly_owner_name;
        ProMdlName component_owner_name;
        int references_ok = 0;
        if (i > 0)
            fputc(',', context->out);
        if (ProAsmcompconstraintTypeGet(constraints[i], &constraint_type) == PRO_TK_NO_ERROR &&
            ProAsmcompconstraintAsmreferenceGet(
                constraints[i], &assembly_reference, &assembly_side) == PRO_TK_NO_ERROR &&
            ProAsmcompconstraintCompreferenceGet(
                constraints[i], &component_reference, &component_side) == PRO_TK_NO_ERROR &&
            ProSelectionModelitemGet(assembly_reference, &assembly_item) == PRO_TK_NO_ERROR &&
            ProSelectionModelitemGet(component_reference, &component_item) == PRO_TK_NO_ERROR &&
            ProMdlNameGet(assembly_item.owner, assembly_owner_name) == PRO_TK_NO_ERROR &&
            ProMdlNameGet(component_item.owner, component_owner_name) == PRO_TK_NO_ERROR)
            references_ok = 1;
        fprintf(context->out, "{\"type\":%d,\"assembly_side\":%d,\"component_side\":%d",
            constraint_type, assembly_side, component_side);
        if (references_ok)
        {
            fputs(",\"assembly_ref\":{\"owner\":", context->out);
            write_wide_json_string(context->out, assembly_owner_name);
            fprintf(context->out, ",\"item_type\":%d,\"item_id\":%d},\"component_ref\":{\"owner\":",
                assembly_item.type, assembly_item.id);
            write_wide_json_string(context->out, component_owner_name);
            fprintf(context->out, ",\"item_type\":%d,\"item_id\":%d}",
                component_item.type, component_item.id);
        }
        fputc('}', context->out);
        if (assembly_reference != NULL)
            ProSelectionFree(&assembly_reference);
        if (component_reference != NULL)
            ProSelectionFree(&component_reference);
    }
    fputs("]}", context->out);
    ++context->count;

    if (constraints != NULL)
        ProAsmcompconstraintArrayFree(constraints);
    return PRO_TK_NO_ERROR;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProMdlType model_type;
    ProMdlName actual_name;
    ComponentContext context;
    int connected = 0;
    int exit_code = 1;

    memset(&context, 0, sizeof(context));
    if (argc != 3)
        return 2;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    context.out = out;

    status = ProEngineerConnect(
        "", "", "", "", PRO_B_TRUE, 20,
        &random_choice, &process_handle);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "connect", status);
        goto done;
    }
    connected = 1;
    status = ProMdlCurrentGet(&model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_assembly", status);
        goto cleanup;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_ASSEMBLY)
    {
        exit_code = write_error(out, "assembly_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProMdlNameGet(model, actual_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_name, argv[2]) != 0)
    {
        exit_code = write_error(out, "assembly_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"readonly\":true,\"api_only\":true,\"assembly\":", out);
    write_wide_json_string(out, actual_name);
    fputs(",\"components\":[", out);
    status = ProSolidFeatVisit(
        (ProSolid)model, component_action, NULL, (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR)
    {
        fputs("]}", out);
        exit_code = 1;
        goto cleanup;
    }
    fprintf(out, "],\"component_count\":%d}\n", context.count);
    exit_code = 0;

cleanup:
    if (connected)
    {
        status = ProEngineerDisconnect(&process_handle, 10);
        if (status != PRO_TK_NO_ERROR && exit_code == 0)
            exit_code = 3;
    }
done:
    fclose(out);
    return exit_code;
}
