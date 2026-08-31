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

typedef struct ComponentCount
{
    int count;
} ComponentCount;

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

static int parse_translation(const wchar_t *text, double *value)
{
    wchar_t *end = NULL;
    double parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) || fabs(parsed) > 10000.0)
        return 0;
    *value = parsed;
    return 1;
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
    ProAsmcompconstraint constraint = NULL;
    ProAsmcompconstraint *constraints = NULL;
    ProAsmcompconstraint *readback_constraints = NULL;
    ProAsmcompConstrType readback_type = PRO_ASM_UNDEF;
    ProFeatStatus component_status = PRO_FEAT_INVALID;
    double tx;
    double ty;
    double tz;
    int connected = 0;
    int constraint_in_array = 0;
    int source_component_count = 0;
    int final_component_count = 0;
    int constraint_count = 0;
    int regenerate_attempts = 0;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    int window_id = -1;
    int exit_code = 1;

    if (argc != 8)
    {
        fwprintf(stderr,
            L"Usage: creo_assembly_add_fixed_bridge <result.json> <expected_assembly> "
            L"<component.prt> <expected_component> <tx> <ty> <tz>\n");
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
    if (!parse_translation(argv[5], &tx) ||
        !parse_translation(argv[6], &ty) ||
        !parse_translation(argv[7], &tz))
    {
        exit_code = write_error(out, "translation_input", PRO_TK_BAD_INPUTS);
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

    placement[3][0] = tx;
    placement[3][1] = ty;
    placement[3][2] = tz;
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
    status = ProAsmcompconstraintAlloc(&constraint);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "constraint_alloc", status);
        goto cleanup;
    }
    status = ProAsmcompconstraintTypeSet(constraint, PRO_ASM_FIX);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "fixed_constraint_type", status);
        goto cleanup;
    }
    status = ProArrayObjectAdd(
        (ProArray *)&constraints,
        PRO_VALUE_UNUSED,
        1,
        &constraint);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "constraint_add", status);
        goto cleanup;
    }
    constraint_in_array = 1;
    constraint = NULL;
    status = ProAsmcompConstraintsSet(NULL, &component_feature, constraints);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "set_fixed_constraint", status);
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
    status = ProFeatureStatusGet((ProFeature *)&component_feature, &component_status);
    if (status != PRO_TK_NO_ERROR || component_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "component_status",
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
    status = ProArraySizeGet((ProArray)readback_constraints, &constraint_count);
    if (status != PRO_TK_NO_ERROR || constraint_count != 1)
    {
        exit_code = write_error(out, "constraint_count_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProAsmcompconstraintTypeGet(readback_constraints[0], &readback_type);
    if (status != PRO_TK_NO_ERROR || readback_type != PRO_ASM_FIX)
    {
        exit_code = write_error(out, "fixed_constraint_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProAsmcompPositionGet(&component_feature, readback_position);
    if (status != PRO_TK_NO_ERROR ||
        fabs(readback_position[3][0] - tx) > 1.0e-6 ||
        fabs(readback_position[3][1] - ty) > 1.0e-6 ||
        fabs(readback_position[3][2] - tz) > 1.0e-6)
    {
        exit_code = write_error(out, "position_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
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
    if (!find_latest_saved_assembly(
            working_directory,
            actual_assembly_name,
            saved_path,
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
        ",\"component_feature_id\":%d,\"component_status\":%d,"
        "\"source_component_count\":%d,\"final_component_count\":%d,"
        "\"constraint_count\":%d,\"constraint_type\":%d,"
        "\"fixed_placement\":true,\"translation\":[%.17g,%.17g,%.17g],"
        "\"readback_translation\":[%.17g,%.17g,%.17g],"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"saved_file\":",
        component_feature.id,
        component_status,
        source_component_count,
        final_component_count,
        constraint_count,
        readback_type,
        tx,
        ty,
        tz,
        readback_position[3][0],
        readback_position[3][1],
        readback_position[3][2],
        regenerate_status,
        regenerate_attempts);
    write_wide_json_string(out, saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (readback_constraints != NULL)
        ProAsmcompconstraintArrayFree(readback_constraints);
    if (constraints != NULL)
        ProAsmcompconstraintArrayFree(constraints);
    if (constraint != NULL && !constraint_in_array)
        ProAsmcompconstraintFree(constraint);
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
