#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#include <limits.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProWindows.h>
#include <ProDimension.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>

#define MAX_MODIFICATIONS 8

typedef struct dimension_find_context
{
    const wchar_t *target_symbol;
    ProDimension dimension;
    int match_count;
} DimensionFindContext;

typedef struct dimension_change
{
    ProName symbol;
    double expected_value;
    double new_value;
    double original_value;
    double verified_value;
    ProDimensiontype type;
    int dimension_id;
    int changed;
} DimensionChange;

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

static int parse_positive_value(const wchar_t *text, double *value)
{
    wchar_t *end = NULL;
    double parsed;
    if (text == NULL || *text == L'\0')
        return 0;
    parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) ||
        parsed < 0.0 || parsed > 1000000.0)
        return 0;
    *value = parsed;
    return 1;
}

static int nearly_equal(double actual, double expected)
{
    double tolerance = fabs(expected) * 1.0e-8;
    if (tolerance < 1.0e-7)
        tolerance = 1.0e-7;
    return fabs(actual - expected) <= tolerance;
}

static ProError dimension_find_action(
    ProDimension *dimension,
    ProError filter_status,
    ProAppData app_data)
{
    DimensionFindContext *context = (DimensionFindContext *)app_data;
    ProName symbol;
    (void)filter_status;
    if (ProDimensionSymbolGet(dimension, symbol) == PRO_TK_NO_ERROR &&
        _wcsicmp(symbol, context->target_symbol) == 0)
    {
        context->dimension = *dimension;
        ++context->match_count;
    }
    return PRO_TK_NO_ERROR;
}

static ProError find_dimension_by_symbol(
    ProSolid solid,
    const wchar_t *symbol,
    ProDimension *dimension)
{
    DimensionFindContext context;
    ProError status;
    context.target_symbol = symbol;
    context.match_count = 0;
    status = ProSolidDimensionVisit(
        solid,
        PRO_B_FALSE,
        dimension_find_action,
        NULL,
        (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    *dimension = context.dimension;
    return PRO_TK_NO_ERROR;
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
    ProError disconnect_status;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProMdl current_model = NULL;
    ProMdl assembly_model = NULL;
    ProMdlName expected_model_name;
    ProMdlName actual_model_name;
    ProMdlName current_model_name;
    ProMdlName assembly_name;
    ProName feature_name;
    ProMdlType model_type;
    ProModelitem feature_item;
    ProFeature owner_feature;
    ProFeature feature;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProPath working_directory;
    ProPath saved_path;
    ProMassProperty source_mass;
    ProMassProperty final_mass;
    Pro3dPnt source_outline[2] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
    Pro3dPnt final_outline[2] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
    Pro3dPnt assembly_outline[2] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
    ProMatrix outline_matrix = {
        {1.0, 0.0, 0.0, 0.0},
        {0.0, 1.0, 0.0, 0.0},
        {0.0, 0.0, 1.0, 0.0},
        {0.0, 0.0, 0.0, 1.0}
    };
    ProSolidOutlExclTypes outline_excludes[] = {
        PRO_OUTL_EXC_DATUM_PLANE,
        PRO_OUTL_EXC_DATUM_POINT,
        PRO_OUTL_EXC_DATUM_CSYS,
        PRO_OUTL_EXC_DATUM_AXES,
        PRO_OUTL_EXC_ALL_CRVS
    };
    DimensionChange changes[MAX_MODIFICATIONS];
    int count;
    int index;
    int other;
    int connected = 0;
    int changed_count = 0;
    int regenerate_attempts = 0;
    int source_mass_status = PRO_TK_E_NOT_FOUND;
    int final_mass_status = PRO_TK_E_NOT_FOUND;
    int source_outline_status = PRO_TK_E_NOT_FOUND;
    int final_outline_status = PRO_TK_E_NOT_FOUND;
    int assembly_outline_status = PRO_TK_E_NOT_FOUND;
    int assembly_regenerate_attempts = 0;
    int has_assembly = 0;
    int window_id = -1;
    int exit_code = 1;

    if (argc < 8)
    {
        fwprintf(stderr,
            L"Usage: creo_dimension_modify_bridge <result.json> "
            L"<expected_model> <feature_name> <count> "
            L"(<symbol> <expected_value> <new_value>)+\n");
        return 2;
    }
    count = _wtoi(argv[4]);
    if (count < 1 || count > MAX_MODIFICATIONS ||
        (argc != 5 + 3 * count && argc != 6 + 3 * count))
        return 2;
    has_assembly = argc == 6 + 3 * count;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(expected_model_name,
        sizeof(expected_model_name) / sizeof(expected_model_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(feature_name,
        sizeof(feature_name) / sizeof(feature_name[0]),
        argv[3], _TRUNCATE);
    assembly_name[0] = L'\0';
    if (has_assembly)
        wcsncpy_s(assembly_name,
            sizeof(assembly_name) / sizeof(assembly_name[0]),
            argv[5 + 3 * count], _TRUNCATE);
    ZeroMemory(changes, sizeof(changes));
    for (index = 0; index < count; ++index)
    {
        int argument_index = 5 + 3 * index;
        wcsncpy_s(changes[index].symbol,
            sizeof(changes[index].symbol) / sizeof(changes[index].symbol[0]),
            argv[argument_index], _TRUNCATE);
        if (!parse_positive_value(
                argv[argument_index + 1], &changes[index].expected_value) ||
            !parse_positive_value(
                argv[argument_index + 2], &changes[index].new_value))
        {
            exit_code = write_error(out, "dimension_value_input", PRO_TK_BAD_INPUTS);
            goto done;
        }
        for (other = 0; other < index; ++other)
        {
            if (_wcsicmp(changes[index].symbol, changes[other].symbol) == 0)
            {
                exit_code = write_error(out, "duplicate_dimension_symbol", PRO_TK_BAD_INPUTS);
                goto done;
            }
        }
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
    status = ProMdlCurrentGet(&current_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto cleanup;
    }
    status = ProMdlNameGet(current_model, current_model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model_name", status);
        goto cleanup;
    }
    if (_wcsicmp(current_model_name, expected_model_name) == 0)
        model = current_model;
    else if (has_assembly && _wcsicmp(current_model_name, assembly_name) == 0)
    {
        assembly_model = current_model;
        status = ProMdlnameInit(
            expected_model_name, PRO_MDLFILE_PART, &model);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "part_init", status);
            goto cleanup;
        }
    }
    else
    {
        exit_code = write_error(out, "model_name_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProMdlNameGet(model, actual_model_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(actual_model_name, expected_model_name) != 0)
    {
        exit_code = write_error(out, "part_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    if (has_assembly && assembly_model == NULL)
    {
        status = ProMdlnameInit(
            assembly_name, PRO_MDLFILE_ASSEMBLY, &assembly_model);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "assembly_init", status);
            goto cleanup;
        }
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART)
    {
        exit_code = write_error(out, "part_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    if (feature_name[0] == L'#')
    {
        wchar_t *feature_id_end = NULL;
        long feature_id_long = wcstol(feature_name + 1, &feature_id_end, 10);
        if (feature_name[1] == L'\0' || *feature_id_end != L'\0' ||
            feature_id_long <= 0 || feature_id_long > INT_MAX)
        {
            exit_code = write_error(out, "feature_id_guard", PRO_TK_BAD_INPUTS);
            goto cleanup;
        }
        status = ProFeatureInit((ProSolid)model, (int)feature_id_long, &feature);
        if (status == PRO_TK_NO_ERROR)
            feature_item = *(ProModelitem *)&feature;
    }
    else
    {
        status = ProModelitemByNameInit(
            model, PRO_FEATURE, feature_name, &feature_item);
        if (status == PRO_TK_NO_ERROR)
            feature = *(ProFeature *)&feature_item;
    }
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out,
            feature_name[0] == L'#' ? "feature_id_guard" : "feature_name_guard",
            status);
        goto cleanup;
    }
    status = ProFeatureStatusGet(&feature, &feature_status);
    if (status != PRO_TK_NO_ERROR || feature_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "feature_status_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }

    for (index = 0; index < count; ++index)
    {
        ProDimension dimension;
        ProBoolean relation_driven = PRO_B_FALSE;
        status = find_dimension_by_symbol(
            (ProSolid)model, changes[index].symbol, &dimension);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "dimension_symbol_guard", status);
            goto cleanup;
        }
        status = ProDimensionOwnerfeatureGet(&dimension, &owner_feature);
        if (status != PRO_TK_NO_ERROR || owner_feature.id != feature_item.id)
        {
            exit_code = write_error(out, "dimension_owner_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
            goto cleanup;
        }
        status = ProDimensionIsReldriven(&dimension, &relation_driven);
        if (status != PRO_TK_NO_ERROR || relation_driven == PRO_B_TRUE)
        {
            exit_code = write_error(out, "relation_driven_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_NO_PERMISSION : status);
            goto cleanup;
        }
        status = ProDimensionValueGet(
            &dimension, &changes[index].original_value);
        if (status != PRO_TK_NO_ERROR ||
            !nearly_equal(
                changes[index].original_value,
                changes[index].expected_value))
        {
            exit_code = write_error(out, "expected_value_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
            goto cleanup;
        }
        status = ProDimensionTypeGet(&dimension, &changes[index].type);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "dimension_type_guard", status);
            goto cleanup;
        }
        changes[index].dimension_id = dimension.id;
    }

    source_mass_status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &source_mass);
    source_outline_status = ProSolidOutlineCompute(
        (ProSolid)model,
        outline_matrix,
        outline_excludes,
        (int)(sizeof(outline_excludes) / sizeof(outline_excludes[0])),
        source_outline);

    for (index = 0; index < count; ++index)
    {
        ProDimension dimension;
        status = find_dimension_by_symbol(
            (ProSolid)model, changes[index].symbol, &dimension);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "dimension_refind_before_set", status);
            goto cleanup;
        }
        status = ProDimensionValueSet(&dimension, changes[index].new_value);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "dimension_set", status);
            goto cleanup;
        }
        changes[index].changed = 1;
        ++changed_count;
    }

    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR &&
        regenerate_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "regenerate", regenerate_status);
        goto cleanup;
    }
    status = ProFeatureStatusGet(&feature, &feature_status);
    if (status != PRO_TK_NO_ERROR || feature_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "post_regen_feature_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    for (index = 0; index < count; ++index)
    {
        ProDimension dimension;
        status = find_dimension_by_symbol(
            (ProSolid)model, changes[index].symbol, &dimension);
        if (status == PRO_TK_NO_ERROR)
            status = ProDimensionValueGet(
                &dimension, &changes[index].verified_value);
        if (status != PRO_TK_NO_ERROR ||
            !nearly_equal(
                changes[index].verified_value,
                changes[index].new_value))
        {
            exit_code = write_error(out, "post_regen_dimension_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto cleanup;
        }
    }

    final_mass_status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &final_mass);
    final_outline_status = ProSolidOutlineCompute(
        (ProSolid)model,
        outline_matrix,
        outline_excludes,
        (int)(sizeof(outline_excludes) / sizeof(outline_excludes[0])),
        final_outline);
    if (has_assembly)
    {
        do
        {
            status = ProSolidRegenerate(
                (ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
            ++assembly_regenerate_attempts;
        } while (status == PRO_TK_REGEN_AGAIN &&
            assembly_regenerate_attempts < 3);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "assembly_regenerate", status);
            goto cleanup;
        }
        assembly_outline_status = ProSolidOutlineCompute(
            (ProSolid)assembly_model,
            outline_matrix,
            outline_excludes,
            (int)(sizeof(outline_excludes) / sizeof(outline_excludes[0])),
            assembly_outline);
    }
    status = ProMdlSave(model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_model", status);
        goto cleanup;
    }
    if (has_assembly)
    {
        status = ProMdlSave(assembly_model);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "save_assembly", status);
            goto cleanup;
        }
        if (ProMdlWindowGet(assembly_model, &window_id) == PRO_TK_NO_ERROR)
        {
            ProWindowCurrentSet(window_id);
            ProWindowActivate(window_id);
            ProWindowRefit(window_id);
            ProWindowRepaint(window_id);
        }
    }
    if (!find_latest_saved_model(
            working_directory,
            actual_model_name,
            saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    {
        exit_code = write_error(out, "saved_file_guard", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    if (ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
    {
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"model\":", out);
    write_wide_json_string(out, actual_model_name);
    fputs(",\"feature\":", out);
    write_wide_json_string(out, feature_name);
    fprintf(out,
        ",\"feature_id\":%d,\"feature_status\":%d,"
        "\"modifications\":[",
        feature_item.id,
        feature_status);
    for (index = 0; index < count; ++index)
    {
        if (index > 0)
            fputc(',', out);
        fputs("{\"symbol\":", out);
        write_wide_json_string(out, changes[index].symbol);
        fprintf(out,
            ",\"dimension_id\":%d,\"type_code\":%d,"
            "\"old_value\":%.17g,\"new_value\":%.17g,"
            "\"verified_value\":%.17g}",
            changes[index].dimension_id,
            changes[index].type,
            changes[index].original_value,
            changes[index].new_value,
            changes[index].verified_value);
    }
    fprintf(out,
        "],\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"mass\":{\"before_status\":%d,\"after_status\":%d,"
        "\"before_volume\":%.17g,\"after_volume\":%.17g},"
        "\"outline\":{\"before_status\":%d,\"after_status\":%d,"
        "\"before_size\":[%.15g,%.15g,%.15g],"
        "\"after_size\":[%.15g,%.15g,%.15g]},"
        "\"saved_file\":",
        regenerate_status,
        regenerate_attempts,
        source_mass_status,
        final_mass_status,
        source_mass_status == PRO_TK_NO_ERROR ? source_mass.volume : 0.0,
        final_mass_status == PRO_TK_NO_ERROR ? final_mass.volume : 0.0,
        source_outline_status,
        final_outline_status,
        source_outline[1][0] - source_outline[0][0],
        source_outline[1][1] - source_outline[0][1],
        source_outline[1][2] - source_outline[0][2],
        final_outline[1][0] - final_outline[0][0],
        final_outline[1][1] - final_outline[0][1],
        final_outline[1][2] - final_outline[0][2]);
    write_wide_json_string(out, saved_path);
    fprintf(out,
        ",\"assembly_regenerate_attempts\":%d,"
        "\"assembly_outline_status\":%d,"
        "\"assembly_size\":[%.15g,%.15g,%.15g],"
        "\"window_id\":%d}\n",
        assembly_regenerate_attempts,
        assembly_outline_status,
        assembly_outline[1][0] - assembly_outline[0][0],
        assembly_outline[1][1] - assembly_outline[0][1],
        assembly_outline[1][2] - assembly_outline[0][2],
        window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0 && model != NULL && changed_count > 0)
    {
        for (index = 0; index < count; ++index)
        {
            ProDimension dimension;
            if (changes[index].changed &&
                find_dimension_by_symbol(
                    (ProSolid)model,
                    changes[index].symbol,
                    &dimension) == PRO_TK_NO_ERROR)
                ProDimensionValueSet(
                    &dimension, changes[index].original_value);
        }
        ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
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
