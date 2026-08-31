#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

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
#include <ProSurface.h>
#include <ProWindows.h>
#include <ProAsmcomppath.h>

typedef struct ComponentCount
{
    int count;
} ComponentCount;

static ProError component_count_action(
    ProFeature *feature, ProError filter_status, ProAppData app_data)
{
    ComponentCount *context = (ComponentCount *)app_data;
    ProFeattype type = PRO_FEAT_INVALID;
    ProFeatStatus status = PRO_FEAT_INVALID;
    (void)filter_status;
    if (ProFeatureTypeGet(feature, &type) == PRO_TK_NO_ERROR &&
        ProFeatureStatusGet(feature, &status) == PRO_TK_NO_ERROR &&
        type == PRO_FEAT_COMPONENT && status == PRO_FEAT_ACTIVE)
        ++context->count;
    return PRO_TK_NO_ERROR;
}

static ProError active_component_count_get(ProAssembly assembly, int *count)
{
    ComponentCount context;
    ProError status;
    memset(&context, 0, sizeof(context));
    status = ProSolidFeatVisit(
        (ProSolid)assembly, component_count_action, NULL, (ProAppData)&context);
    if (status == PRO_TK_NO_ERROR)
        *count = context.count;
    return status;
}

typedef struct NamedComponentCount
{
    const wchar_t *expected_name;
    int count;
} NamedComponentCount;

static ProError named_component_count_action(
    ProFeature *feature, ProError filter_status, ProAppData app_data)
{
    NamedComponentCount *context = (NamedComponentCount *)app_data;
    ProFeattype type = PRO_FEAT_INVALID;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProMdl component_model = NULL;
    ProMdlName component_name;
    (void)filter_status;
    if (ProFeatureTypeGet(feature, &type) == PRO_TK_NO_ERROR &&
        ProFeatureStatusGet(feature, &feature_status) == PRO_TK_NO_ERROR &&
        type == PRO_FEAT_COMPONENT && feature_status == PRO_FEAT_ACTIVE &&
        ProAsmcompMdlGet((ProAsmcomp *)feature, &component_model) == PRO_TK_NO_ERROR &&
        ProMdlNameGet(component_model, component_name) == PRO_TK_NO_ERROR &&
        _wcsicmp(component_name, context->expected_name) == 0)
        ++context->count;
    return PRO_TK_NO_ERROR;
}

static ProError named_component_count_get(
    ProAssembly assembly, const wchar_t *expected_name, int *count)
{
    NamedComponentCount context;
    ProError status;
    context.expected_name = expected_name;
    context.count = 0;
    status = ProSolidFeatVisit(
        (ProSolid)assembly, named_component_count_action, NULL,
        (ProAppData)&context);
    if (status == PRO_TK_NO_ERROR)
        *count = context.count;
    return status;
}

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

static int parse_positive_int(const wchar_t *text, int *value)
{
    wchar_t *end = NULL;
    long parsed = wcstol(text, &end, 10);
    if (text[0] == L'\0' || *end != L'\0' || parsed <= 0 || parsed > 2000000000L)
        return 0;
    *value = (int)parsed;
    return 1;
}

static int parse_translation(const wchar_t *text, double *value)
{
    wchar_t *end = NULL;
    double parsed = wcstod(text, &end);
    if (text[0] == L'\0' || *end != L'\0' || !_finite(parsed) || fabs(parsed) > 100000.0)
        return 0;
    *value = parsed;
    return 1;
}

static int find_latest_saved_assembly(
    const wchar_t *directory, const wchar_t *name,
    wchar_t *saved_path, size_t saved_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    int found = 0;
    int best_version = -1;
    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
        L"%ls\\%ls.asm*", directory, name);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        wchar_t *dot = wcsrchr(data.cFileName, L'.');
        int version = dot == NULL ? 0 : _wtoi(dot + 1);
        if (!found || version > best_version)
        {
            _snwprintf_s(saved_path, saved_path_count, _TRUNCATE,
                L"%ls\\%ls", directory, data.cFileName);
            best_version = version;
            found = 1;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return found;
}

static ProError constraint_append(
    ProAsmcompconstraint **constraints,
    ProAsmcompConstrType type,
    ProSelection assembly_selection,
    ProDatumside assembly_side,
    ProSelection component_selection,
    ProDatumside component_side)
{
    ProAsmcompconstraint constraint = NULL;
    ProError status;
    status = ProAsmcompconstraintAlloc(&constraint);
    if (status == PRO_TK_NO_ERROR)
        status = ProAsmcompconstraintTypeSet(constraint, type);
    if (status == PRO_TK_NO_ERROR)
        status = ProAsmcompconstraintAsmreferenceSet(
            constraint, assembly_selection, assembly_side);
    if (status == PRO_TK_NO_ERROR)
        status = ProAsmcompconstraintCompreferenceSet(
            constraint, component_selection, component_side);
    if (status == PRO_TK_NO_ERROR)
        status = ProArrayObjectAdd(
            (ProArray *)constraints, PRO_VALUE_UNUSED, 1, &constraint);
    if (status != PRO_TK_NO_ERROR && constraint != NULL)
        ProAsmcompconstraintFree(constraint);
    return status;
}

static ProError create_constrained_component(
    ProAssembly assembly,
    ProSolid component_model,
    ProMatrix source_position,
    double target_x,
    ProAsmcomppath *host_path,
    ProModelitem *assembly_align_item,
    ProModelitem *assembly_insert_item,
    ProModelitem *component_align_item,
    ProModelitem *component_insert_item,
    ProDatumside assembly_align_side,
    ProDatumside component_align_side,
    ProDatumside assembly_insert_side,
    ProDatumside component_insert_side,
    ProAsmcomp *component_feature)
{
    ProMatrix placement;
    ProSelection assembly_align_selection = NULL;
    ProSelection assembly_insert_selection = NULL;
    ProSelection component_align_selection = NULL;
    ProSelection component_insert_selection = NULL;
    ProAsmcompconstraint *constraints = NULL;
    ProError status;
    int row;
    int column;

    for (row = 0; row < 4; ++row)
        for (column = 0; column < 4; ++column)
            placement[row][column] = source_position[row][column];
    placement[3][0] = target_x;

    status = ProAsmcompAssemble(
        assembly, component_model, placement, component_feature);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProSelectionAlloc(
        host_path, assembly_align_item, &assembly_align_selection);
    if (status == PRO_TK_NO_ERROR)
        status = ProSelectionAlloc(
            host_path, assembly_insert_item, &assembly_insert_selection);
    if (status == PRO_TK_NO_ERROR)
        status = ProSelectionAlloc(
            NULL, component_align_item, &component_align_selection);
    if (status == PRO_TK_NO_ERROR)
        status = ProSelectionAlloc(
            NULL, component_insert_item, &component_insert_selection);
    if (status == PRO_TK_NO_ERROR)
        status = ProArrayAlloc(
            0, sizeof(ProAsmcompconstraint), 2, (ProArray *)&constraints);
    if (status == PRO_TK_NO_ERROR)
        status = constraint_append(
            &constraints, PRO_ASM_ALIGN,
            assembly_align_selection, assembly_align_side,
            component_align_selection, component_align_side);
    if (status == PRO_TK_NO_ERROR)
        status = constraint_append(
            &constraints, PRO_ASM_INSERT,
            assembly_insert_selection, assembly_insert_side,
            component_insert_selection, component_insert_side);
    if (status == PRO_TK_NO_ERROR)
        status = ProAsmcompConstraintsSet(NULL, component_feature, constraints);

    if (constraints != NULL)
        ProAsmcompconstraintArrayFree(constraints);
    if (assembly_align_selection != NULL)
        ProSelectionFree(&assembly_align_selection);
    if (assembly_insert_selection != NULL)
        ProSelectionFree(&assembly_insert_selection);
    if (component_align_selection != NULL)
        ProSelectionFree(&component_align_selection);
    if (component_insert_selection != NULL)
        ProSelectionFree(&component_insert_selection);
    return status;
}

static ProError verify_component(
    ProAsmcomp *component,
    int expected_align_surface,
    int expected_insert_surface,
    int expected_component_align_surface,
    int expected_component_insert_surface,
    double expected_x,
    ProMatrix readback_position)
{
    ProAsmcompconstraint *constraints = NULL;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProError status;
    int count = 0;
    int saw_align = 0;
    int saw_insert = 0;
    int i;

    status = ProFeatureStatusGet((ProFeature *)component, &feature_status);
    if (status != PRO_TK_NO_ERROR || feature_status != PRO_FEAT_ACTIVE)
        return status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status;
    status = ProAsmcompConstraintsWithComppathGet(component, NULL, &constraints);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProArraySizeGet((ProArray)constraints, &count);
    if (status != PRO_TK_NO_ERROR || count != 2)
    {
        ProAsmcompconstraintArrayFree(constraints);
        return status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status;
    }
    for (i = 0; i < count; ++i)
    {
        ProAsmcompConstrType type = PRO_ASM_UNDEF;
        ProSelection asm_selection = NULL;
        ProSelection comp_selection = NULL;
        ProDatumside asm_side = PRO_DATUM_SIDE_NONE;
        ProDatumside comp_side = PRO_DATUM_SIDE_NONE;
        ProModelitem asm_item;
        ProModelitem comp_item;
        status = ProAsmcompconstraintTypeGet(constraints[i], &type);
        if (status == PRO_TK_NO_ERROR)
            status = ProAsmcompconstraintAsmreferenceGet(
                constraints[i], &asm_selection, &asm_side);
        if (status == PRO_TK_NO_ERROR)
            status = ProAsmcompconstraintCompreferenceGet(
                constraints[i], &comp_selection, &comp_side);
        if (status == PRO_TK_NO_ERROR)
            status = ProSelectionModelitemGet(asm_selection, &asm_item);
        if (status == PRO_TK_NO_ERROR)
            status = ProSelectionModelitemGet(comp_selection, &comp_item);
        if (status == PRO_TK_NO_ERROR && type == PRO_ASM_ALIGN &&
            asm_item.id == expected_align_surface &&
            comp_item.id == expected_component_align_surface)
            saw_align = 1;
        if (status == PRO_TK_NO_ERROR && type == PRO_ASM_INSERT &&
            asm_item.id == expected_insert_surface &&
            comp_item.id == expected_component_insert_surface)
            saw_insert = 1;
        if (asm_selection != NULL)
            ProSelectionFree(&asm_selection);
        if (comp_selection != NULL)
            ProSelectionFree(&comp_selection);
        if (status != PRO_TK_NO_ERROR)
        {
            ProAsmcompconstraintArrayFree(constraints);
            return status;
        }
    }
    ProAsmcompconstraintArrayFree(constraints);
    if (!saw_align || !saw_insert)
        return PRO_TK_GENERAL_ERROR;
    status = ProAsmcompPositionGet(component, readback_position);
    if (status != PRO_TK_NO_ERROR ||
        fabs(readback_position[3][0] - expected_x) > 1.0e-5)
        return status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status;
    return PRO_TK_NO_ERROR;
}

static int direct_three_main(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl assembly_model = NULL;
    ProMdl host_model = NULL;
    ProMdl component_model = NULL;
    ProMdlType model_type;
    ProMdlName actual_assembly_name;
    ProMdlName actual_host_name;
    ProMdlName actual_component_name;
    ProPath component_path;
    ProPath working_directory;
    ProPath saved_path;
    ProFeature host_feature;
    ProFeattype host_feature_type = PRO_FEAT_INVALID;
    ProFeatStatus host_feature_status = PRO_FEAT_INVALID;
    ProIdTable host_ids;
    ProAsmcomppath host_path;
    ProModelitem assembly_align_item;
    ProModelitem component_align_item;
    ProModelitem component_insert_item;
    ProModelitem target_insert_items[3];
    ProSurface surface;
    ProSrftype surface_type;
    ProMatrix source_position = {
        {1.0, 0.0, 0.0, 0.0},
        {0.0, 1.0, 0.0, 0.0},
        {0.0, 0.0, 1.0, 0.0},
        {0.0, 0.0, -1.0, 1.0}
    };
    ProMatrix readback_positions[3];
    ProAsmcomp created_components[3];
    int host_feature_id;
    int assembly_align_surface_id;
    int component_align_surface_id;
    int component_insert_surface_id;
    int target_surface_ids[3];
    double target_x[3];
    int source_component_count = 0;
    int existing_named_count = 0;
    int final_component_count = 0;
    int created_count = 0;
    int saved = 0;
    int connected = 0;
    int regenerate_attempts = 0;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    int window_id = -1;
    int i;
    int exit_code = 1;

    (void)argc;
    memset(created_components, 0, sizeof(created_components));
    memset(&host_path, 0, sizeof(host_path));
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(component_path,
        sizeof(component_path) / sizeof(component_path[0]), argv[5], _TRUNCATE);
    if (!parse_positive_int(argv[3], &host_feature_id) ||
        !parse_positive_int(argv[7], &assembly_align_surface_id) ||
        !parse_positive_int(argv[8], &component_align_surface_id) ||
        !parse_positive_int(argv[9], &component_insert_surface_id) ||
        !parse_positive_int(argv[10], &target_surface_ids[0]) ||
        !parse_translation(argv[11], &target_x[0]) ||
        !parse_positive_int(argv[12], &target_surface_ids[1]) ||
        !parse_translation(argv[13], &target_x[1]) ||
        !parse_positive_int(argv[14], &target_surface_ids[2]) ||
        !parse_translation(argv[15], &target_x[2]) ||
        target_surface_ids[0] == target_surface_ids[1] ||
        target_surface_ids[0] == target_surface_ids[2] ||
        target_surface_ids[1] == target_surface_ids[2])
    {
        exit_code = write_error(out, "direct_input_guard", PRO_TK_BAD_INPUTS);
        goto done;
    }
    if (GetFileAttributesW(component_path) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "component_file", PRO_TK_E_NOT_FOUND);
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
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlTypeGet(assembly_model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_ASSEMBLY)
    {
        exit_code = write_error(out, "assembly_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProMdlNameGet(assembly_model, actual_assembly_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_assembly_name, argv[2]) != 0)
    {
        exit_code = write_error(out, "assembly_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = active_component_count_get(
        (ProAssembly)assembly_model, &source_component_count);
    if (status == PRO_TK_NO_ERROR)
        status = named_component_count_get(
            (ProAssembly)assembly_model, argv[6], &existing_named_count);
    if (status != PRO_TK_NO_ERROR || existing_named_count != 0)
    {
        exit_code = write_error(out, "existing_component_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProFeatureInit(
        (ProSolid)assembly_model, host_feature_id, &host_feature);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureTypeGet(&host_feature, &host_feature_type);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureStatusGet(&host_feature, &host_feature_status);
    if (status == PRO_TK_NO_ERROR)
        status = ProAsmcompMdlGet((ProAsmcomp *)&host_feature, &host_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(host_model, actual_host_name);
    if (status != PRO_TK_NO_ERROR || host_feature_type != PRO_FEAT_COMPONENT ||
        host_feature_status != PRO_FEAT_ACTIVE ||
        _wcsicmp(actual_host_name, argv[4]) != 0)
    {
        exit_code = write_error(out, "host_component_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    host_ids[0] = host_feature_id;
    status = ProAsmcomppathInit(
        (ProSolid)assembly_model, host_ids, 1, &host_path);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "host_component_path", status);
        goto cleanup;
    }
    status = ProMdlFiletypeLoad(
        component_path, PRO_MDLFILE_PART, PRO_B_FALSE, &component_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(component_model, actual_component_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_component_name, argv[6]) != 0)
    {
        exit_code = write_error(out, "component_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProModelitemInit(
        host_model, assembly_align_surface_id, PRO_SURFACE,
        &assembly_align_item);
    if (status == PRO_TK_NO_ERROR)
        status = ProSurfaceInit(
            (ProSolid)host_model, assembly_align_surface_id, &surface);
    if (status == PRO_TK_NO_ERROR)
        status = ProSurfaceTypeGet(surface, &surface_type);
    if (status != PRO_TK_NO_ERROR || surface_type != PRO_SRF_PLANE)
    {
        exit_code = write_error(out, "assembly_align_surface_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProModelitemInit(
        component_model, component_align_surface_id, PRO_SURFACE,
        &component_align_item);
    if (status == PRO_TK_NO_ERROR)
        status = ProSurfaceInit(
            (ProSolid)component_model, component_align_surface_id, &surface);
    if (status == PRO_TK_NO_ERROR)
        status = ProSurfaceTypeGet(surface, &surface_type);
    if (status != PRO_TK_NO_ERROR || surface_type != PRO_SRF_PLANE)
    {
        exit_code = write_error(out, "component_align_surface_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProModelitemInit(
        component_model, component_insert_surface_id, PRO_SURFACE,
        &component_insert_item);
    if (status == PRO_TK_NO_ERROR)
        status = ProSurfaceInit(
            (ProSolid)component_model, component_insert_surface_id, &surface);
    if (status == PRO_TK_NO_ERROR)
        status = ProSurfaceTypeGet(surface, &surface_type);
    if (status != PRO_TK_NO_ERROR || surface_type != PRO_SRF_CYL)
    {
        exit_code = write_error(out, "component_insert_surface_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    for (i = 0; i < 3; ++i)
    {
        status = ProModelitemInit(
            host_model, target_surface_ids[i], PRO_SURFACE,
            &target_insert_items[i]);
        if (status == PRO_TK_NO_ERROR)
            status = ProSurfaceInit(
                (ProSolid)host_model, target_surface_ids[i], &surface);
        if (status == PRO_TK_NO_ERROR)
            status = ProSurfaceTypeGet(surface, &surface_type);
        if (status != PRO_TK_NO_ERROR || surface_type != PRO_SRF_CYL)
        {
            exit_code = write_error(out, "target_insert_surface_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
            goto cleanup;
        }
    }
    for (i = 0; i < 3; ++i)
    {
        status = create_constrained_component(
            (ProAssembly)assembly_model, (ProSolid)component_model,
            source_position, target_x[i], &host_path,
            &assembly_align_item, &target_insert_items[i],
            &component_align_item, &component_insert_item,
            PRO_DATUM_SIDE_YELLOW, PRO_DATUM_SIDE_YELLOW,
            PRO_DATUM_SIDE_YELLOW, PRO_DATUM_SIDE_YELLOW,
            &created_components[i]);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "create_constrained_component", status);
            goto cleanup;
        }
        ++created_count;
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
    for (i = 0; i < 3; ++i)
    {
        status = verify_component(
            &created_components[i], assembly_align_surface_id,
            target_surface_ids[i], component_align_surface_id,
            component_insert_surface_id, target_x[i], readback_positions[i]);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "verify_constrained_component", status);
            goto cleanup;
        }
    }
    status = active_component_count_get(
        (ProAssembly)assembly_model, &final_component_count);
    if (status != PRO_TK_NO_ERROR || final_component_count != source_component_count + 3)
    {
        exit_code = write_error(out, "component_count_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_assembly", status);
        goto cleanup;
    }
    saved = 1;
    if (!find_latest_saved_assembly(
        working_directory, actual_assembly_name, saved_path,
        sizeof(saved_path) / sizeof(saved_path[0])))
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
    fputs(",\"component\":", out);
    write_wide_json_string(out, actual_component_name);
    fprintf(out,
        ",\"host_component_feature_id\":%d,"
        "\"created_feature_ids\":[%d,%d,%d],"
        "\"target_insert_surface_ids\":[%d,%d,%d],"
        "\"target_x_positions\":[%.17g,%.17g,%.17g],"
        "\"readback_x_positions\":[%.17g,%.17g,%.17g],"
        "\"constraint_types_per_component\":[%d,%d],"
        "\"constraint_count_per_component\":2,"
        "\"source_component_count\":%d,\"final_component_count\":%d,"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"saved_file\":",
        host_feature_id,
        created_components[0].id, created_components[1].id, created_components[2].id,
        target_surface_ids[0], target_surface_ids[1], target_surface_ids[2],
        target_x[0], target_x[1], target_x[2],
        readback_positions[0][3][0], readback_positions[1][3][0],
        readback_positions[2][3][0],
        PRO_ASM_ALIGN, PRO_ASM_INSERT,
        source_component_count, final_component_count,
        regenerate_status, regenerate_attempts);
    write_wide_json_string(out, saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0 && !saved && assembly_model != NULL && created_count > 0)
    {
        int feature_ids[3];
        ProFeatureDeleteOptions delete_options[3];
        for (i = 0; i < created_count; ++i)
        {
            feature_ids[i] = created_components[i].id;
            delete_options[i] = PRO_FEAT_DELETE_NO_OPTS;
        }
        ProFeatureDelete(
            (ProSolid)assembly_model, feature_ids, created_count,
            delete_options, created_count);
        ProSolidRegenerate((ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
    }
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

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl assembly_model = NULL;
    ProMdl component_model = NULL;
    ProMdlType model_type;
    ProMdlName actual_assembly_name;
    ProMdlName actual_component_name;
    ProPath component_path;
    ProPath working_directory;
    ProPath saved_path;
    ProFeature source_feature;
    ProFeattype source_type = PRO_FEAT_INVALID;
    ProFeatStatus source_status = PRO_FEAT_INVALID;
    ProAsmcompconstraint *source_constraints = NULL;
    ProSelection source_align_asm_selection = NULL;
    ProSelection source_align_comp_selection = NULL;
    ProSelection source_insert_asm_selection = NULL;
    ProSelection source_insert_comp_selection = NULL;
    ProDatumside align_asm_side = PRO_DATUM_SIDE_NONE;
    ProDatumside align_comp_side = PRO_DATUM_SIDE_NONE;
    ProDatumside insert_asm_side = PRO_DATUM_SIDE_NONE;
    ProDatumside insert_comp_side = PRO_DATUM_SIDE_NONE;
    ProModelitem align_asm_item;
    ProModelitem align_comp_item;
    ProModelitem insert_asm_item;
    ProModelitem insert_comp_item;
    ProModelitem target_insert_item[2];
    ProAsmcomppath host_path;
    ProMatrix source_position;
    ProMatrix readback_position[2];
    ProAsmcomp created_components[2];
    ProSurface target_surface;
    ProSrftype surface_type;
    int source_feature_id;
    int target_surface_ids[2];
    double target_x[2];
    int source_constraint_count = 0;
    int source_component_count = 0;
    int final_component_count = 0;
    int created_count = 0;
    int saved = 0;
    int connected = 0;
    int regenerate_attempts = 0;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    int window_id = -1;
    int i;
    int exit_code = 1;

    memset(created_components, 0, sizeof(created_components));
    memset(&host_path, 0, sizeof(host_path));
    if (argc == 16)
        return direct_three_main(argc, argv);
    if (argc != 10)
        return 2;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(component_path,
        sizeof(component_path) / sizeof(component_path[0]), argv[4], _TRUNCATE);
    if (!parse_positive_int(argv[3], &source_feature_id) ||
        !parse_positive_int(argv[6], &target_surface_ids[0]) ||
        !parse_translation(argv[7], &target_x[0]) ||
        !parse_positive_int(argv[8], &target_surface_ids[1]) ||
        !parse_translation(argv[9], &target_x[1]) ||
        target_surface_ids[0] == target_surface_ids[1] ||
        fabs(target_x[0] - target_x[1]) < 1.0e-6)
    {
        exit_code = write_error(out, "input_guard", PRO_TK_BAD_INPUTS);
        goto done;
    }
    if (GetFileAttributesW(component_path) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "component_file", PRO_TK_E_NOT_FOUND);
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
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_assembly_name, argv[2]) != 0)
    {
        exit_code = write_error(out, "assembly_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = active_component_count_get(
        (ProAssembly)assembly_model, &source_component_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_component_count", status);
        goto cleanup;
    }
    status = ProFeatureInit(
        (ProSolid)assembly_model, source_feature_id, &source_feature);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureTypeGet(&source_feature, &source_type);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureStatusGet(&source_feature, &source_status);
    if (status != PRO_TK_NO_ERROR || source_type != PRO_FEAT_COMPONENT ||
        source_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "source_component_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProAsmcompMdlGet((ProAsmcomp *)&source_feature, &component_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(component_model, actual_component_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_component_name, argv[5]) != 0)
    {
        exit_code = write_error(out, "component_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProAsmcompPositionGet((ProAsmcomp *)&source_feature, source_position);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_position", status);
        goto cleanup;
    }
    status = ProAsmcompConstraintsWithComppathGet(
        (ProAsmcomp *)&source_feature, NULL, &source_constraints);
    if (status == PRO_TK_NO_ERROR)
        status = ProArraySizeGet((ProArray)source_constraints, &source_constraint_count);
    if (status != PRO_TK_NO_ERROR || source_constraint_count != 2)
    {
        exit_code = write_error(out, "source_constraint_count_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    for (i = 0; i < source_constraint_count; ++i)
    {
        ProAsmcompConstrType type = PRO_ASM_UNDEF;
        status = ProAsmcompconstraintTypeGet(source_constraints[i], &type);
        if (status == PRO_TK_NO_ERROR && type == PRO_ASM_ALIGN)
        {
            status = ProAsmcompconstraintAsmreferenceGet(
                source_constraints[i], &source_align_asm_selection, &align_asm_side);
            if (status == PRO_TK_NO_ERROR)
                status = ProAsmcompconstraintCompreferenceGet(
                    source_constraints[i], &source_align_comp_selection, &align_comp_side);
        }
        else if (status == PRO_TK_NO_ERROR && type == PRO_ASM_INSERT)
        {
            status = ProAsmcompconstraintAsmreferenceGet(
                source_constraints[i], &source_insert_asm_selection, &insert_asm_side);
            if (status == PRO_TK_NO_ERROR)
                status = ProAsmcompconstraintCompreferenceGet(
                    source_constraints[i], &source_insert_comp_selection, &insert_comp_side);
        }
        else if (status == PRO_TK_NO_ERROR)
            status = PRO_TK_BAD_CONTEXT;
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "source_constraint_type_guard", status);
            goto cleanup;
        }
    }
    if (source_align_asm_selection == NULL || source_align_comp_selection == NULL ||
        source_insert_asm_selection == NULL || source_insert_comp_selection == NULL)
    {
        exit_code = write_error(out, "source_constraint_pair_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProSelectionModelitemGet(source_align_asm_selection, &align_asm_item);
    if (status == PRO_TK_NO_ERROR)
        status = ProSelectionModelitemGet(source_align_comp_selection, &align_comp_item);
    if (status == PRO_TK_NO_ERROR)
        status = ProSelectionModelitemGet(source_insert_asm_selection, &insert_asm_item);
    if (status == PRO_TK_NO_ERROR)
        status = ProSelectionModelitemGet(source_insert_comp_selection, &insert_comp_item);
    if (status == PRO_TK_NO_ERROR)
        status = ProSelectionAsmcomppathGet(source_insert_asm_selection, &host_path);
    if (status != PRO_TK_NO_ERROR ||
        align_asm_item.type != PRO_SURFACE || align_comp_item.type != PRO_SURFACE ||
        insert_asm_item.type != PRO_SURFACE || insert_comp_item.type != PRO_SURFACE ||
        align_asm_item.owner != insert_asm_item.owner ||
        align_comp_item.owner != insert_comp_item.owner)
    {
        exit_code = write_error(out, "source_reference_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    for (i = 0; i < 2; ++i)
    {
        status = ProModelitemInit(
            insert_asm_item.owner, target_surface_ids[i], PRO_SURFACE,
            &target_insert_item[i]);
        if (status == PRO_TK_NO_ERROR)
            status = ProSurfaceInit(
                (ProSolid)insert_asm_item.owner, target_surface_ids[i], &target_surface);
        if (status == PRO_TK_NO_ERROR)
            status = ProSurfaceTypeGet(target_surface, &surface_type);
        if (status != PRO_TK_NO_ERROR || surface_type != PRO_SRF_CYL)
        {
            exit_code = write_error(out, "target_cylindrical_surface_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
            goto cleanup;
        }
    }

    for (i = 0; i < 2; ++i)
    {
        status = create_constrained_component(
            (ProAssembly)assembly_model, (ProSolid)component_model,
            source_position, target_x[i], &host_path,
            &align_asm_item, &target_insert_item[i],
            &align_comp_item, &insert_comp_item,
            align_asm_side, align_comp_side,
            insert_asm_side, insert_comp_side,
            &created_components[i]);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out,
                i == 0 ? "create_first_component" : "create_second_component", status);
            goto cleanup;
        }
        ++created_count;
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
    for (i = 0; i < 2; ++i)
    {
        status = verify_component(
            &created_components[i], align_asm_item.id, target_surface_ids[i],
            align_comp_item.id, insert_comp_item.id, target_x[i],
            readback_position[i]);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out,
                i == 0 ? "verify_first_component" : "verify_second_component", status);
            goto cleanup;
        }
    }
    status = active_component_count_get(
        (ProAssembly)assembly_model, &final_component_count);
    if (status != PRO_TK_NO_ERROR || final_component_count != source_component_count + 2)
    {
        exit_code = write_error(out, "component_count_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_assembly", status);
        goto cleanup;
    }
    saved = 1;
    if (!find_latest_saved_assembly(
        working_directory, actual_assembly_name, saved_path,
        sizeof(saved_path) / sizeof(saved_path[0])))
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
    fputs(",\"component\":", out);
    write_wide_json_string(out, actual_component_name);
    fprintf(out,
        ",\"source_component_feature_id\":%d,"
        "\"source_constraint_types\":[%d,%d],"
        "\"assembly_align_surface_id\":%d,"
        "\"component_align_surface_id\":%d,"
        "\"component_insert_surface_id\":%d,"
        "\"created_feature_ids\":[%d,%d],"
        "\"target_insert_surface_ids\":[%d,%d],"
        "\"target_x_positions\":[%.17g,%.17g],"
        "\"readback_x_positions\":[%.17g,%.17g],"
        "\"constraint_types_per_component\":[%d,%d],"
        "\"source_component_count\":%d,\"final_component_count\":%d,"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"saved_file\":",
        source_feature_id,
        PRO_ASM_ALIGN, PRO_ASM_INSERT,
        align_asm_item.id, align_comp_item.id, insert_comp_item.id,
        created_components[0].id, created_components[1].id,
        target_surface_ids[0], target_surface_ids[1],
        target_x[0], target_x[1],
        readback_position[0][3][0], readback_position[1][3][0],
        PRO_ASM_ALIGN, PRO_ASM_INSERT,
        source_component_count, final_component_count,
        regenerate_status, regenerate_attempts);
    write_wide_json_string(out, saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (source_constraints != NULL)
        ProAsmcompconstraintArrayFree(source_constraints);
    if (source_align_asm_selection != NULL)
        ProSelectionFree(&source_align_asm_selection);
    if (source_align_comp_selection != NULL)
        ProSelectionFree(&source_align_comp_selection);
    if (source_insert_asm_selection != NULL)
        ProSelectionFree(&source_insert_asm_selection);
    if (source_insert_comp_selection != NULL)
        ProSelectionFree(&source_insert_comp_selection);
    if (exit_code != 0 && !saved && assembly_model != NULL && created_count > 0)
    {
        int feature_ids[2];
        ProFeatureDeleteOptions delete_options[2];
        for (i = 0; i < created_count; ++i)
        {
            feature_ids[i] = created_components[i].id;
            delete_options[i] = PRO_FEAT_DELETE_NO_OPTS;
        }
        ProFeatureDelete(
            (ProSolid)assembly_model,
            feature_ids, created_count,
            delete_options, created_count);
        ProSolidRegenerate((ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
    }
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
