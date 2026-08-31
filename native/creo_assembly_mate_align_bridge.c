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
#include <ProWindows.h>
#include <ProModelitem.h>
#include <ProSelection.h>
#include <ProGeomitem.h>
#include <ProSurface.h>

#define MAX_PLANE_CONSTRAINTS 3

typedef struct component_count
{
    int count;
} ComponentCount;

typedef struct plane_constraint_input
{
    ProAsmcompConstrType type;
    ProName assembly_plane;
    ProName component_plane;
    ProDatumside assembly_side;
    ProDatumside component_side;
} PlaneConstraintInput;

static ProError component_count_action(
    ProFeature *feature,
    ProError filter_status,
    ProAppData app_data)
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
        (ProSolid)assembly,
        component_count_action,
        NULL,
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

static const char *constraint_type_name(ProAsmcompConstrType type)
{
    return type == PRO_ASM_MATE ? "mate" : "align";
}

static const char *datum_side_name(ProDatumside side)
{
    return side == PRO_DATUM_SIDE_RED ? "red" : "yellow";
}

static int parse_constraint_type(
    const wchar_t *text,
    ProAsmcompConstrType *type)
{
    if (_wcsicmp(text, L"mate") == 0)
    {
        *type = PRO_ASM_MATE;
        return 1;
    }
    if (_wcsicmp(text, L"align") == 0)
    {
        *type = PRO_ASM_ALIGN;
        return 1;
    }
    return 0;
}

static int parse_datum_side(const wchar_t *text, ProDatumside *side)
{
    if (_wcsicmp(text, L"yellow") == 0)
    {
        *side = PRO_DATUM_SIDE_YELLOW;
        return 1;
    }
    if (_wcsicmp(text, L"red") == 0)
    {
        *side = PRO_DATUM_SIDE_RED;
        return 1;
    }
    return 0;
}

static ProError named_planar_surface_get(
    ProMdl model,
    const wchar_t *name,
    ProModelitem *item)
{
    ProError status;
    ProSurface surface;
    ProSrftype surface_type;
    status = ProModelitemByNameInit(
        model, PRO_SURFACE, (wchar_t *)name, item);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProGeomitemToSurface((ProGeomitem *)item, &surface);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProSurfaceTypeGet(surface, &surface_type);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (surface_type != PRO_SRF_PLANE)
        return PRO_TK_INVALID_TYPE;
    return PRO_TK_NO_ERROR;
}

static int find_latest_saved_assembly(
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
        L"%ls\\%ls.asm*",
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
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl assembly_model = NULL;
    ProMdl component_model = NULL;
    ProMdlType model_type;
    ProMdlName expected_assembly_name;
    ProMdlName actual_assembly_name;
    ProMdlName expected_component_name;
    ProMdlName actual_component_name;
    ProPath component_path;
    ProPath working_directory;
    ProPath saved_path;
    ProAsmcomp component_feature;
    ProMatrix placement = {
        {1.0, 0.0, 0.0, 0.0},
        {0.0, 1.0, 0.0, 0.0},
        {0.0, 0.0, 1.0, 0.0},
        {0.0, 0.0, 0.0, 1.0}
    };
    ProMatrix readback_position;
    ProAsmcompconstraint *constraints = NULL;
    ProAsmcompconstraint *readback_constraints = NULL;
    ProAsmcompconstraint pending_constraint = NULL;
    ProModelitem assembly_items[MAX_PLANE_CONSTRAINTS];
    ProModelitem component_items[MAX_PLANE_CONSTRAINTS];
    ProSelection assembly_selections[MAX_PLANE_CONSTRAINTS];
    ProSelection component_selections[MAX_PLANE_CONSTRAINTS];
    PlaneConstraintInput inputs[MAX_PLANE_CONSTRAINTS];
    ProAsmcomppath assembly_path;
    ProIdTable component_id_table;
    ProFeatStatus component_status = PRO_FEAT_INVALID;
    ProBoolean is_packaged = PRO_B_TRUE;
    ProBoolean is_underconstrained = PRO_B_TRUE;
    int require_fully_constrained;
    int input_count;
    int index;
    int other;
    int connected = 0;
    int component_created = 0;
    int saved = 0;
    int source_component_count = 0;
    int final_component_count = 0;
    int readback_count = 0;
    int regenerate_attempts = 0;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    int window_id = -1;
    int exit_code = 1;

    ZeroMemory(&component_feature, sizeof(component_feature));
    component_feature.id = -1;
    ZeroMemory(assembly_selections, sizeof(assembly_selections));
    ZeroMemory(component_selections, sizeof(component_selections));
    ZeroMemory(inputs, sizeof(inputs));
    if (argc < 12)
    {
        fwprintf(stderr,
            L"Usage: creo_assembly_mate_align_bridge <result.json> "
            L"<expected_assembly> <component.prt> <expected_component> "
            L"<require_fully_constrained> <constraint_count> "
            L"(<mate|align> <assembly_plane> <component_plane> "
            L"<yellow|red> <yellow|red>)+\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(expected_assembly_name,
        sizeof(expected_assembly_name) / sizeof(expected_assembly_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(component_path,
        sizeof(component_path) / sizeof(component_path[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(expected_component_name,
        sizeof(expected_component_name) / sizeof(expected_component_name[0]),
        argv[4], _TRUNCATE);
    require_fully_constrained = _wtoi(argv[5]);
    input_count = _wtoi(argv[6]);
    if ((require_fully_constrained != 0 && require_fully_constrained != 1) ||
        input_count < 1 || input_count > MAX_PLANE_CONSTRAINTS ||
        argc != 7 + input_count * 5)
    {
        exit_code = write_error(out, "constraint_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    for (index = 0; index < input_count; ++index)
    {
        int base = 7 + index * 5;
        if (!parse_constraint_type(argv[base], &inputs[index].type) ||
            !parse_datum_side(argv[base + 3], &inputs[index].assembly_side) ||
            !parse_datum_side(argv[base + 4], &inputs[index].component_side))
        {
            exit_code = write_error(out, "constraint_value_input", PRO_TK_BAD_INPUTS);
            goto done;
        }
        wcsncpy_s(inputs[index].assembly_plane,
            sizeof(inputs[index].assembly_plane) /
                sizeof(inputs[index].assembly_plane[0]),
            argv[base + 1], _TRUNCATE);
        wcsncpy_s(inputs[index].component_plane,
            sizeof(inputs[index].component_plane) /
                sizeof(inputs[index].component_plane[0]),
            argv[base + 2], _TRUNCATE);
        for (other = 0; other < index; ++other)
        {
            if (_wcsicmp(
                    inputs[index].assembly_plane,
                    inputs[other].assembly_plane) == 0 ||
                _wcsicmp(
                    inputs[index].component_plane,
                    inputs[other].component_plane) == 0)
            {
                exit_code = write_error(
                    out, "duplicate_plane_constraint", PRO_TK_BAD_INPUTS);
                goto done;
            }
        }
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
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(actual_assembly_name, expected_assembly_name) != 0)
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
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    status = ProMdlFiletypeLoad(
        component_path, PRO_MDLFILE_PART, PRO_B_FALSE, &component_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "load_component", status);
        goto cleanup;
    }
    status = ProMdlNameGet(component_model, actual_component_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(actual_component_name, expected_component_name) != 0)
    {
        exit_code = write_error(out, "component_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    for (index = 0; index < input_count; ++index)
    {
        status = named_planar_surface_get(
            assembly_model,
            inputs[index].assembly_plane,
            &assembly_items[index]);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "assembly_plane_guard", status);
            goto cleanup;
        }
        status = named_planar_surface_get(
            component_model,
            inputs[index].component_plane,
            &component_items[index]);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "component_plane_guard", status);
            goto cleanup;
        }
    }

    status = ProAsmcompAssemble(
        (ProAssembly)assembly_model,
        (ProSolid)component_model,
        placement,
        &component_feature);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "assemble_component", status);
        goto cleanup;
    }
    component_created = 1;
    component_id_table[0] = -1;
    status = ProAsmcomppathInit(
        (ProAssembly)assembly_model,
        component_id_table,
        0,
        &assembly_path);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "assembly_reference_path", status);
        goto cleanup;
    }
    status = ProArrayAlloc(
        0,
        sizeof(ProAsmcompconstraint),
        1,
        (ProArray *)&constraints);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "constraint_array_alloc", status);
        goto cleanup;
    }
    for (index = 0; index < input_count; ++index)
    {
        status = ProSelectionAlloc(
            &assembly_path,
            &assembly_items[index],
            &assembly_selections[index]);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "assembly_reference_selection", status);
            goto cleanup;
        }
        status = ProSelectionAlloc(
            NULL,
            &component_items[index],
            &component_selections[index]);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "component_reference_selection", status);
            goto cleanup;
        }
        status = ProAsmcompconstraintAlloc(&pending_constraint);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "constraint_alloc", status);
            goto cleanup;
        }
        status = ProAsmcompconstraintTypeSet(
            pending_constraint, inputs[index].type);
        if (status == PRO_TK_NO_ERROR)
            status = ProAsmcompconstraintAsmreferenceSet(
                pending_constraint,
                assembly_selections[index],
                inputs[index].assembly_side);
        if (status == PRO_TK_NO_ERROR)
            status = ProAsmcompconstraintCompreferenceSet(
                pending_constraint,
                component_selections[index],
                inputs[index].component_side);
        if (status == PRO_TK_NO_ERROR)
            status = ProAsmcompconstraintAttributesSet(
                pending_constraint, PRO_ASM_CONSTR_ATTR_NONE);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "constraint_definition", status);
            goto cleanup;
        }
        status = ProArrayObjectAdd(
            (ProArray *)&constraints,
            PRO_VALUE_UNUSED,
            1,
            &pending_constraint);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "constraint_array_add", status);
            goto cleanup;
        }
        pending_constraint = NULL;
    }
    status = ProAsmcompConstraintsSet(
        NULL, &component_feature, constraints);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "set_plane_constraints", status);
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
    status = ProFeatureStatusGet(
        (ProFeature *)&component_feature, &component_status);
    if (status != PRO_TK_NO_ERROR || component_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "component_status_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProAsmcompConstraintsWithComppathGet(
        &component_feature, NULL, &readback_constraints);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "constraint_readback", status);
        goto cleanup;
    }
    status = ProArraySizeGet(
        (ProArray)readback_constraints, &readback_count);
    if (status != PRO_TK_NO_ERROR || readback_count != input_count)
    {
        exit_code = write_error(out, "constraint_count_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    for (index = 0; index < input_count; ++index)
    {
        ProAsmcompConstrType readback_type = PRO_ASM_UNDEF;
        ProSelection assembly_reference = NULL;
        ProSelection component_reference = NULL;
        ProDatumside assembly_side = PRO_DATUM_SIDE_NONE;
        ProDatumside component_side = PRO_DATUM_SIDE_NONE;
        ProModelitem assembly_item;
        ProModelitem component_item;
        ProName assembly_name;
        ProName component_name;
        status = ProAsmcompconstraintTypeGet(
            readback_constraints[index], &readback_type);
        if (status == PRO_TK_NO_ERROR)
            status = ProAsmcompconstraintAsmreferenceGet(
                readback_constraints[index],
                &assembly_reference,
                &assembly_side);
        if (status == PRO_TK_NO_ERROR)
            status = ProAsmcompconstraintCompreferenceGet(
                readback_constraints[index],
                &component_reference,
                &component_side);
        if (status == PRO_TK_NO_ERROR)
            status = ProSelectionModelitemGet(
                assembly_reference, &assembly_item);
        if (status == PRO_TK_NO_ERROR)
            status = ProSelectionModelitemGet(
                component_reference, &component_item);
        if (status == PRO_TK_NO_ERROR)
            status = ProModelitemNameGet(&assembly_item, assembly_name);
        if (status == PRO_TK_NO_ERROR)
            status = ProModelitemNameGet(&component_item, component_name);
        if (status != PRO_TK_NO_ERROR ||
            readback_type != inputs[index].type ||
            assembly_side != inputs[index].assembly_side ||
            component_side != inputs[index].component_side ||
            _wcsicmp(assembly_name, inputs[index].assembly_plane) != 0 ||
            _wcsicmp(component_name, inputs[index].component_plane) != 0)
        {
            if (assembly_reference != NULL)
                ProSelectionFree(&assembly_reference);
            if (component_reference != NULL)
                ProSelectionFree(&component_reference);
            exit_code = write_error(out, "constraint_reference_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto cleanup;
        }
        if (assembly_reference != NULL)
            ProSelectionFree(&assembly_reference);
        if (component_reference != NULL)
            ProSelectionFree(&component_reference);
    }
    status = ProAsmcompIsPackaged(&component_feature, &is_packaged);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "packaged_status", status);
        goto cleanup;
    }
    status = ProAsmcompIsUnderconstrained(
        &component_feature, &is_underconstrained);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "underconstrained_status", status);
        goto cleanup;
    }
    if (require_fully_constrained && is_underconstrained == PRO_B_TRUE)
    {
        exit_code = write_error(
            out, "fully_constrained_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProAsmcompPositionGet(&component_feature, readback_position);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "position_readback", status);
        goto cleanup;
    }
    status = active_component_count_get(
        (ProAssembly)assembly_model, &final_component_count);
    if (status != PRO_TK_NO_ERROR ||
        final_component_count != source_component_count + 1)
    {
        exit_code = write_error(out, "component_count_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
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
            working_directory,
            actual_assembly_name,
            saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    {
        exit_code = write_error(
            out, "verify_saved_assembly", PRO_TK_E_NOT_FOUND);
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
        ",\"component_feature_id\":%d,\"component_status\":%d,"
        "\"source_component_count\":%d,\"final_component_count\":%d,"
        "\"constraint_count\":%d,\"constraints\":[",
        component_feature.id,
        component_status,
        source_component_count,
        final_component_count,
        readback_count);
    for (index = 0; index < input_count; ++index)
    {
        if (index > 0)
            fputc(',', out);
        fputs("{\"type\":", out);
        write_utf8_json_string(out, constraint_type_name(inputs[index].type));
        fputs(",\"assembly_plane\":", out);
        write_wide_json_string(out, inputs[index].assembly_plane);
        fputs(",\"component_plane\":", out);
        write_wide_json_string(out, inputs[index].component_plane);
        fputs(",\"assembly_side\":", out);
        write_utf8_json_string(out, datum_side_name(inputs[index].assembly_side));
        fputs(",\"component_side\":", out);
        write_utf8_json_string(out, datum_side_name(inputs[index].component_side));
        fputc('}', out);
    }
    fprintf(out,
        "],\"is_packaged\":%s,\"is_underconstrained\":%s,"
        "\"require_fully_constrained\":%s,"
        "\"readback_translation\":[%.17g,%.17g,%.17g],"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"saved_file\":",
        is_packaged == PRO_B_TRUE ? "true" : "false",
        is_underconstrained == PRO_B_TRUE ? "true" : "false",
        require_fully_constrained ? "true" : "false",
        readback_position[3][0],
        readback_position[3][1],
        readback_position[3][2],
        regenerate_status,
        regenerate_attempts);
    write_wide_json_string(out, saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (pending_constraint != NULL)
        ProAsmcompconstraintFree(pending_constraint);
    if (readback_constraints != NULL)
        ProAsmcompconstraintArrayFree(readback_constraints);
    if (constraints != NULL)
        ProAsmcompconstraintArrayFree(constraints);
    for (index = 0; index < MAX_PLANE_CONSTRAINTS; ++index)
    {
        if (component_selections[index] != NULL)
            ProSelectionFree(&component_selections[index]);
        if (assembly_selections[index] != NULL)
            ProSelectionFree(&assembly_selections[index]);
    }
    if (exit_code != 0 && component_created && !saved && assembly_model != NULL)
    {
        int feature_id = component_feature.id;
        ProFeatureDeleteOptions delete_option = PRO_FEAT_DELETE_NO_OPTS;
        ProFeatureDelete(
            (ProSolid)assembly_model,
            &feature_id,
            1,
            &delete_option,
            1);
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
