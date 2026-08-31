#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProSkeleton.h>
#include <ProWindows.h>
#include <ProDimension.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>

typedef struct dimension_find_context
{
    const wchar_t *target_symbol;
    ProDimension dimension;
    int match_count;
} DimensionFindContext;

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

static int nearly_equal(double actual, double expected)
{
    double tolerance = fabs(expected) * 1.0e-8;
    if (tolerance < 1.0e-7)
        tolerance = 1.0e-7;
    return fabs(actual - expected) <= tolerance;
}

static int parse_positive_dimension(
    const wchar_t *text,
    double maximum,
    double *value)
{
    wchar_t *end = NULL;
    double parsed;
    if (text == NULL || *text == L'\0')
        return 0;
    parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) ||
        parsed < 0.1 || parsed > maximum)
        return 0;
    *value = parsed;
    return 1;
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
    const wchar_t *extension,
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
        L"%ls\\%ls.%ls*",
        directory,
        name,
        extension);
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

static int dimensions_match(
    const Pro3dPnt outline[2],
    double length,
    double width,
    double height)
{
    double actual[3];
    double expected[3];
    int i;
    int j;
    actual[0] = fabs(outline[1][0] - outline[0][0]);
    actual[1] = fabs(outline[1][1] - outline[0][1]);
    actual[2] = fabs(outline[1][2] - outline[0][2]);
    expected[0] = length;
    expected[1] = width;
    expected[2] = height;
    for (i = 0; i < 2; ++i)
    {
        for (j = i + 1; j < 3; ++j)
        {
            if (actual[i] > actual[j])
            {
                double value = actual[i];
                actual[i] = actual[j];
                actual[j] = value;
            }
            if (expected[i] > expected[j])
            {
                double value = expected[i];
                expected[i] = expected[j];
                expected[j] = value;
            }
        }
    }
    for (i = 0; i < 3; ++i)
    {
        double tolerance = fabs(expected[i]) * 1.0e-7;
        if (tolerance < 1.0e-5)
            tolerance = 1.0e-5;
        if (fabs(actual[i] - expected[i]) > tolerance)
            return 0;
    }
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl current_model = NULL;
    ProMdl assembly_model = NULL;
    ProMdl skeleton_model = NULL;
    ProMdl skeleton_readback = NULL;
    ProMdl display_readback = NULL;
    ProMdlName current_name;
    ProMdlName assembly_name;
    ProMdlName skeleton_name;
    ProMdlName skeleton_readback_name;
    ProMdlName display_readback_name;
    ProName old_feature_name;
    ProName new_feature_name;
    ProName length_symbol;
    ProName width_symbol;
    ProName height_symbol;
    ProModelitem old_feature_item;
    ProModelitem new_feature_item;
    ProFeature owner_feature;
    ProFeature height_owner_feature;
    ProDimension length_dimension;
    ProDimension width_dimension;
    ProDimension height_dimension;
    ProBoolean relation_driven = PRO_B_FALSE;
    ProBoolean is_skeleton = PRO_B_FALSE;
    ProPath working_directory;
    ProPath skeleton_saved_path;
    ProPath assembly_saved_path;
    ProMassProperty mass_property;
    Pro3dPnt outline[2] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
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
    double old_length;
    double old_width;
    double height;
    double new_length;
    double new_width;
    double readback_length = 0.0;
    double readback_width = 0.0;
    double readback_height = 0.0;
    double expected_volume;
    double volume_tolerance;
    int connected = 0;
    int length_changed = 0;
    int width_changed = 0;
    int feature_renamed = 0;
    int rename_requested = 0;
    int regenerate_attempts = 0;
    int assembly_regenerate_attempts = 0;
    ProError assembly_regenerate_status = PRO_TK_NO_ERROR;
    int window_id = -1;
    int exit_code = 1;

    if (argc != 14)
    {
        fwprintf(stderr,
            L"Usage: creo_skeleton_resize_box_bridge <result.json> "
            L"<assembly_name> <skeleton_name> <feature_name> "
            L"<new_feature_name> <length_symbol> <width_symbol> "
            L"<height_symbol> <expected_length> <expected_width> "
            L"<expected_height> <new_length> <new_width>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(assembly_name,
        sizeof(assembly_name) / sizeof(assembly_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(skeleton_name,
        sizeof(skeleton_name) / sizeof(skeleton_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(old_feature_name,
        sizeof(old_feature_name) / sizeof(old_feature_name[0]),
        argv[4], _TRUNCATE);
    wcsncpy_s(new_feature_name,
        sizeof(new_feature_name) / sizeof(new_feature_name[0]),
        argv[5], _TRUNCATE);
    wcsncpy_s(length_symbol,
        sizeof(length_symbol) / sizeof(length_symbol[0]),
        argv[6], _TRUNCATE);
    wcsncpy_s(width_symbol,
        sizeof(width_symbol) / sizeof(width_symbol[0]),
        argv[7], _TRUNCATE);
    wcsncpy_s(height_symbol,
        sizeof(height_symbol) / sizeof(height_symbol[0]),
        argv[8], _TRUNCATE);
    if (!parse_positive_dimension(argv[9], 100000.0, &old_length) ||
        !parse_positive_dimension(argv[10], 100000.0, &old_width) ||
        !parse_positive_dimension(argv[11], 100000.0, &height) ||
        !parse_positive_dimension(argv[12], 100000.0, &new_length) ||
        !parse_positive_dimension(argv[13], 100000.0, &new_width))
    {
        exit_code = write_error(out, "dimension_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    rename_requested = _wcsicmp(old_feature_name, new_feature_name) != 0;

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
    status = ProMdlNameGet(current_model, current_name);
    if (status != PRO_TK_NO_ERROR ||
        (_wcsicmp(current_name, skeleton_name) != 0 &&
         _wcsicmp(current_name, assembly_name) != 0))
    {
        exit_code = write_error(out, "current_skeleton_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProMdlnameInit(
        assembly_name, PRO_MDLFILE_ASSEMBLY, &assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "assembly_init", status);
        goto cleanup;
    }
    status = ProAsmSkeletonGet(
        (ProAssembly)assembly_model, &skeleton_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_get", status);
        goto cleanup;
    }
    status = ProMdlNameGet(skeleton_model, skeleton_readback_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(skeleton_readback_name, skeleton_name) != 0)
    {
        exit_code = write_error(out, "skeleton_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProMdlIsSkeleton(skeleton_model, &is_skeleton);
    if (status != PRO_TK_NO_ERROR || is_skeleton != PRO_B_TRUE)
    {
        exit_code = write_error(out, "skeleton_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProAsmSkeletonGet(
        (ProAssembly)assembly_model, &skeleton_readback);
    if (status != PRO_TK_NO_ERROR || skeleton_readback != skeleton_model)
    {
        exit_code = write_error(out, "skeleton_identity_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        skeleton_model, PRO_FEATURE, old_feature_name, &old_feature_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_feature_guard", status);
        goto cleanup;
    }
    if (rename_requested)
    {
        status = ProModelitemByNameInit(
            skeleton_model, PRO_FEATURE, new_feature_name, &new_feature_item);
        if (status == PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "target_feature_name_guard", PRO_TK_E_FOUND);
            goto cleanup;
        }
    }

    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, length_symbol, &length_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "length_dimension_find", status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, width_symbol, &width_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "width_dimension_find", status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, height_symbol, &height_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "height_dimension_find", status);
        goto cleanup;
    }
    status = ProDimensionOwnerfeatureGet(&length_dimension, &owner_feature);
    if (status != PRO_TK_NO_ERROR || owner_feature.id != old_feature_item.id)
    {
        exit_code = write_error(out, "length_owner_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDimensionOwnerfeatureGet(&width_dimension, &owner_feature);
    if (status != PRO_TK_NO_ERROR || owner_feature.id != old_feature_item.id)
    {
        exit_code = write_error(out, "width_owner_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDimensionOwnerfeatureGet(&height_dimension, &height_owner_feature);
    if (status != PRO_TK_NO_ERROR ||
        height_owner_feature.id != old_feature_item.id)
    {
        exit_code = write_error(out, "height_owner_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDimensionValueGet(&length_dimension, &readback_length);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(readback_length, old_length))
    {
        exit_code = write_error(out, "old_length_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDimensionValueGet(&width_dimension, &readback_width);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(readback_width, old_width))
    {
        exit_code = write_error(out, "old_width_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDimensionValueGet(&height_dimension, &readback_height);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(readback_height, height))
    {
        exit_code = write_error(out, "height_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDimensionIsReldriven(&length_dimension, &relation_driven);
    if (status != PRO_TK_NO_ERROR || relation_driven == PRO_B_TRUE)
    {
        exit_code = write_error(out, "length_relation_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_NO_PERMISSION : status);
        goto cleanup;
    }
    status = ProDimensionIsReldriven(&width_dimension, &relation_driven);
    if (status != PRO_TK_NO_ERROR || relation_driven == PRO_B_TRUE)
    {
        exit_code = write_error(out, "width_relation_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_NO_PERMISSION : status);
        goto cleanup;
    }

    status = ProDimensionValueSet(&length_dimension, new_length);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "set_length", status);
        goto cleanup;
    }
    length_changed = 1;
    status = ProDimensionValueSet(&width_dimension, new_width);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "set_width", status);
        goto cleanup;
    }
    width_changed = 1;

    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)skeleton_model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR &&
        regenerate_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "regenerate_skeleton", regenerate_status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, length_symbol, &length_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&length_dimension, &readback_length);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(readback_length, new_length))
    {
        exit_code = write_error(out, "length_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, width_symbol, &width_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&width_dimension, &readback_width);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(readback_width, new_width))
    {
        exit_code = write_error(out, "width_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, height_symbol, &height_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&height_dimension, &readback_height);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(readback_height, height))
    {
        exit_code = write_error(out, "height_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }

    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)skeleton_model,
        NULL,
        PRO_MP_DENS_USE_ALWAYS,
        1.0,
        &mass_property);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "mass_properties", status);
        goto cleanup;
    }
    expected_volume = new_length * new_width * height;
    volume_tolerance = expected_volume * 1.0e-7;
    if (!_finite(mass_property.volume) ||
        fabs(mass_property.volume - expected_volume) > volume_tolerance)
    {
        exit_code = write_error(out, "volume_guard", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }
    status = ProSolidOutlineCompute(
        (ProSolid)skeleton_model,
        outline_matrix,
        outline_excludes,
        (int)(sizeof(outline_excludes) / sizeof(outline_excludes[0])),
        outline);
    if (status != PRO_TK_NO_ERROR ||
        !dimensions_match(outline, new_length, new_width, height))
    {
        exit_code = write_error(out, "outline_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    if (rename_requested)
    {
        status = ProModelitemNameSet(&old_feature_item, new_feature_name);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "feature_rename", status);
            goto cleanup;
        }
        feature_renamed = 1;
        status = ProModelitemByNameInit(
            skeleton_model, PRO_FEATURE, new_feature_name, &new_feature_item);
        if (status != PRO_TK_NO_ERROR || new_feature_item.id != old_feature_item.id)
        {
            exit_code = write_error(out, "feature_rename_readback",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto cleanup;
        }
    }

    do
    {
        assembly_regenerate_status = ProSolidRegenerate(
            (ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
        ++assembly_regenerate_attempts;
    } while (assembly_regenerate_status == PRO_TK_REGEN_AGAIN &&
             assembly_regenerate_attempts < 3);
    if (assembly_regenerate_status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(
            out, "regenerate_assembly", assembly_regenerate_status);
        goto cleanup;
    }
    status = ProMdlSave(skeleton_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_skeleton", status);
        goto cleanup;
    }
    status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_assembly", status);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            working_directory,
            skeleton_name,
            L"prt",
            skeleton_saved_path,
            sizeof(skeleton_saved_path) / sizeof(skeleton_saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_skeleton", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            working_directory,
            assembly_name,
            L"asm",
            assembly_saved_path,
            sizeof(assembly_saved_path) / sizeof(assembly_saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_assembly", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    status = ProMdlDisplay(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "display_assembly", status);
        goto cleanup;
    }
    status = ProMdlCurrentGet(&display_readback);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(display_readback, display_readback_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(display_readback_name, assembly_name) != 0)
    {
        status = ProObjectwindowMdlnameCreate(
            assembly_name, PRO_ASSEMBLY, &window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "create_assembly_window", status);
            goto cleanup;
        }
        status = ProWindowCurrentSet(window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "set_skeleton_window", status);
            goto cleanup;
        }
        status = ProWindowActivate(window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "activate_skeleton_window", status);
            goto cleanup;
        }
    }
    if (window_id < 0)
        ProWindowCurrentGet(&window_id);
    if (window_id >= 0)
    {
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"assembly\":", out);
    write_wide_json_string(out, assembly_name);
    fputs(",\"skeleton\":", out);
    write_wide_json_string(out, skeleton_name);
    fputs(",\"is_skeleton\":true,\"feature\":", out);
    write_wide_json_string(out, new_feature_name);
    fputs(",\"dimensions\":{\"length\":{\"symbol\":", out);
    write_wide_json_string(out, length_symbol);
    fprintf(out,
        ",\"old\":%.15g,\"new\":%.15g},\"width\":{\"symbol\":",
        old_length,
        new_length);
    write_wide_json_string(out, width_symbol);
    fprintf(out,
        ",\"old\":%.15g,\"new\":%.15g},\"height\":{\"symbol\":",
        old_width,
        new_width);
    write_wide_json_string(out, height_symbol);
    fprintf(out,
        ",\"value\":%.15g}},\"volume\":%.17g,"
        "\"outline\":{\"min\":[%.15g,%.15g,%.15g],"
        "\"max\":[%.15g,%.15g,%.15g],"
        "\"size\":[%.15g,%.15g,%.15g]},"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"assembly_regenerate_status\":%d,"
        "\"assembly_regenerate_attempts\":%d,"
        "\"skeleton_saved_file\":",
        height,
        mass_property.volume,
        outline[0][0], outline[0][1], outline[0][2],
        outline[1][0], outline[1][1], outline[1][2],
        outline[1][0] - outline[0][0],
        outline[1][1] - outline[0][1],
        outline[1][2] - outline[0][2],
        regenerate_status,
        regenerate_attempts,
        assembly_regenerate_status,
        assembly_regenerate_attempts);
    write_wide_json_string(out, skeleton_saved_path);
    fputs(",\"assembly_saved_file\":", out);
    write_wide_json_string(out, assembly_saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0 && skeleton_model != NULL)
    {
        if (feature_renamed)
            ProModelitemNameSet(&old_feature_item, old_feature_name);
        if (length_changed)
        {
            if (find_dimension_by_symbol(
                    (ProSolid)skeleton_model,
                    length_symbol,
                    &length_dimension) == PRO_TK_NO_ERROR)
                ProDimensionValueSet(&length_dimension, old_length);
        }
        if (width_changed)
        {
            if (find_dimension_by_symbol(
                    (ProSolid)skeleton_model,
                    width_symbol,
                    &width_dimension) == PRO_TK_NO_ERROR)
                ProDimensionValueSet(&width_dimension, old_width);
        }
        if (length_changed || width_changed || feature_renamed)
        {
            ProSolidRegenerate((ProSolid)skeleton_model, PRO_REGEN_NO_FLAGS);
            if (assembly_model != NULL)
            {
                int rollback_assembly_attempts = 0;
                do
                {
                    status = ProSolidRegenerate(
                        (ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
                    ++rollback_assembly_attempts;
                } while (status == PRO_TK_REGEN_AGAIN &&
                         rollback_assembly_attempts < 3);
            }
        }
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
