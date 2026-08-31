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
#include <ProWindows.h>
#include <ProDimension.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProAsmcomp.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProElemId.h>
#include <ProExtrude.h>
#include <ProArray.h>

typedef struct dimension_find_context
{
    const wchar_t *target_symbol;
    ProDimension dimension;
    int match_count;
} DimensionFindContext;

typedef struct component_find_context
{
    const wchar_t *expected_name;
    ProMdl model;
    int match_count;
} ComponentFindContext;

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
    fputs("{\"ok\":false,\"api_only\":true,\"transaction_rolled_back\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", status);
    return 1;
}

static int parse_positive_dimension(const wchar_t *text, double *value)
{
    wchar_t *end = NULL;
    double parsed;
    if (text == NULL || *text == L'\0')
        return 0;
    parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) ||
        parsed < 0.01 || parsed > 100000.0)
        return 0;
    *value = parsed;
    return 1;
}

static int parse_positive_int(const wchar_t *text, int *value)
{
    wchar_t *end = NULL;
    long parsed;
    if (text == NULL || *text == L'\0')
        return 0;
    parsed = wcstol(text, &end, 10);
    if (end == text || *end != L'\0' || parsed < 1 || parsed > 1000000)
        return 0;
    *value = (int)parsed;
    return 1;
}

static int nearly_equal(double actual, double expected)
{
    double tolerance = fabs(expected) * 1.0e-8;
    if (tolerance < 1.0e-6)
        tolerance = 1.0e-6;
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
        solid, PRO_B_FALSE, dimension_find_action, NULL, (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    *dimension = context.dimension;
    return PRO_TK_NO_ERROR;
}

static ProError component_find_action(
    ProFeature *feature,
    ProError filter_status,
    ProAppData app_data)
{
    ComponentFindContext *context = (ComponentFindContext *)app_data;
    ProFeattype type;
    ProFeatStatus feature_status;
    ProMdl component_model = NULL;
    ProMdlName component_name;
    (void)filter_status;
    if (ProFeatureTypeGet(feature, &type) == PRO_TK_NO_ERROR &&
        ProFeatureStatusGet(feature, &feature_status) == PRO_TK_NO_ERROR &&
        type == PRO_FEAT_COMPONENT && feature_status == PRO_FEAT_ACTIVE &&
        ProAsmcompMdlGet((ProAsmcomp *)feature, &component_model) == PRO_TK_NO_ERROR &&
        ProMdlNameGet(component_model, component_name) == PRO_TK_NO_ERROR &&
        _wcsicmp(component_name, context->expected_name) == 0)
    {
        context->model = component_model;
        ++context->match_count;
    }
    return PRO_TK_NO_ERROR;
}

static ProError find_unique_component_model(
    ProAssembly assembly,
    const wchar_t *expected_name,
    ProMdl *model)
{
    ComponentFindContext context;
    ProError status;
    context.expected_name = expected_name;
    context.model = NULL;
    context.match_count = 0;
    status = ProSolidFeatVisit(
        (ProSolid)assembly, component_find_action, NULL, (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    *model = context.model;
    return PRO_TK_NO_ERROR;
}

static ProError element_by_ids_get(
    ProElement tree,
    const ProElemId *ids,
    int count,
    ProElement *element)
{
    ProElempath path = NULL;
    ProElempathItem items[3];
    ProError status;
    int index;
    if (count < 1 || count > 3)
        return PRO_TK_BAD_INPUTS;
    for (index = 0; index < count; ++index)
    {
        items[index].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
        items[index].path_item.elem_id = ids[index];
    }
    status = ProElempathAlloc(&path);
    if (status == PRO_TK_NO_ERROR)
        status = ProElempathDataSet(path, items, count);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementGet(tree, path, element);
    if (path != NULL)
        ProElempathFree(&path);
    return status;
}

static ProError regenerate_solid(ProSolid solid, int *attempts)
{
    ProError status;
    *attempts = 0;
    do
    {
        status = ProSolidRegenerate(solid, PRO_REGEN_NO_FLAGS);
        ++(*attempts);
    } while (status == PRO_TK_REGEN_AGAIN && *attempts < 3);
    return status;
}

static ProError compute_outline(ProSolid solid, Pro3dPnt outline[2])
{
    ProMatrix matrix = {
        {1.0, 0.0, 0.0, 0.0},
        {0.0, 1.0, 0.0, 0.0},
        {0.0, 0.0, 1.0, 0.0},
        {0.0, 0.0, 0.0, 1.0}
    };
    ProSolidOutlExclTypes excludes[] = {
        PRO_OUTL_EXC_DATUM_PLANE,
        PRO_OUTL_EXC_DATUM_POINT,
        PRO_OUTL_EXC_DATUM_CSYS,
        PRO_OUTL_EXC_DATUM_AXES,
        PRO_OUTL_EXC_ALL_CRVS
    };
    return ProSolidOutlineCompute(
        solid, matrix, excludes,
        (int)(sizeof(excludes) / sizeof(excludes[0])),
        outline);
}

static int outline_matches_front_wall(
    const Pro3dPnt outline[2],
    double length,
    double width,
    double thickness,
    int positive_z)
{
    return nearly_equal(outline[1][0] - outline[0][0], length) &&
        nearly_equal(outline[1][1] - outline[0][1], width) &&
        nearly_equal(outline[1][2] - outline[0][2], thickness) &&
        (positive_z
            ? (nearly_equal(outline[0][2], 0.0) &&
               nearly_equal(outline[1][2], thickness))
            : (nearly_equal(outline[0][2], -thickness) &&
               nearly_equal(outline[1][2], 0.0)));
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
    _snwprintf_s(pattern, _countof(pattern), _TRUNCATE,
        L"%ls\\%ls.%ls*", directory, name, extension);
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

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl current_model = NULL;
    ProMdl part_model = NULL;
    ProMdl assembly_model = NULL;
    ProMdl component_model = NULL;
    ProMdl current_readback = NULL;
    ProMdlName current_name;
    ProMdlName part_name;
    ProMdlName readback_name;
    ProMdlType model_type;
    ProModelitem feature_item;
    ProFeature feature;
    ProFeature owner_feature;
    ProFeattype feature_type = PRO_FEAT_INVALID;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProElement tree = NULL;
    ProElement direction_element = NULL;
    ProElement readback_tree = NULL;
    ProElement readback_direction_element = NULL;
    ProElemId direction_path_ids[1] = {PRO_E_STD_DIRECTION};
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    ProDimension thickness_dimension;
    ProDimension length_dimension;
    ProDimension width_dimension;
    ProBoolean relation_driven = PRO_B_FALSE;
    ProBoolean old_negative = PRO_B_FALSE;
    ProBoolean new_negative = PRO_B_FALSE;
    ProMassProperty mass_property;
    Pro3dPnt before_outline[2];
    Pro3dPnt after_outline[2];
    ProPath working_directory;
    ProPath part_saved_path;
    ProPath assembly_saved_path;
    double expected_length;
    double expected_width;
    double expected_thickness;
    double value = 0.0;
    int expected_feature_id;
    int connected = 0;
    int direction_changed = 0;
    int already_positive_z = 0;
    int old_direction_value = 0;
    int new_direction_value = 0;
    int saved = 0;
    int part_regen_attempts = 0;
    int assembly_regen_attempts = 0;
    int window_id = -1;
    int exit_code = 1;

    if (argc != 12)
        return 2;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    if (!parse_positive_int(argv[5], &expected_feature_id) ||
        !parse_positive_dimension(argv[9], &expected_length) ||
        !parse_positive_dimension(argv[10], &expected_width) ||
        !parse_positive_dimension(argv[11], &expected_thickness))
    {
        exit_code = write_error(out, "input", PRO_TK_BAD_INPUTS);
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
    status = ProMdlCurrentGet(&current_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(current_model, current_name);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlTypeGet(current_model, &model_type);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto cleanup;
    }
    if (model_type == PRO_MDL_ASSEMBLY && _wcsicmp(current_name, argv[2]) == 0)
    {
        assembly_model = current_model;
        status = ProMdlnameInit(argv[3], PRO_MDLFILE_PART, &part_model);
    }
    else if (model_type == PRO_MDL_PART && _wcsicmp(current_name, argv[3]) == 0)
    {
        part_model = current_model;
        status = ProMdlnameInit(argv[2], PRO_MDLFILE_ASSEMBLY, &assembly_model);
    }
    else
    {
        exit_code = write_error(out, "assembly_or_sheetmetal_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(part_model, part_name);
    if (status == PRO_TK_NO_ERROR)
        status = find_unique_component_model(
            (ProAssembly)assembly_model, argv[3], &component_model);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(part_name, argv[3]) != 0 ||
        component_model != part_model)
    {
        exit_code = write_error(out, "sheetmetal_component_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(part_model, PRO_FEATURE, argv[4], &feature_item);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureInit((ProSolid)part_model, feature_item.id, &feature);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureTypeGet(&feature, &feature_type);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureStatusGet(&feature, &feature_status);
    if (status != PRO_TK_NO_ERROR || feature_item.id != expected_feature_id ||
        feature_type != PRO_FEAT_DATUM_SURF || feature_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "planar_wall_feature_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }

#define FIND_AND_GUARD_DIM(SYMBOL, DIMENSION, EXPECTED, STAGE) \
    do { \
        status = find_dimension_by_symbol((ProSolid)part_model, (SYMBOL), &(DIMENSION)); \
        if (status == PRO_TK_NO_ERROR) \
            status = ProDimensionOwnerfeatureGet(&(DIMENSION), &owner_feature); \
        if (status == PRO_TK_NO_ERROR) \
            status = ProDimensionValueGet(&(DIMENSION), &value); \
        if (status != PRO_TK_NO_ERROR || owner_feature.id != expected_feature_id || \
            !nearly_equal(value, (EXPECTED))) { \
            exit_code = write_error(out, (STAGE), \
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); \
            goto cleanup; \
        } \
    } while (0)

    FIND_AND_GUARD_DIM(argv[6], thickness_dimension, expected_thickness,
        "thickness_dimension_guard");
    FIND_AND_GUARD_DIM(argv[7], length_dimension, expected_length,
        "length_dimension_guard");
    FIND_AND_GUARD_DIM(argv[8], width_dimension, expected_width,
        "width_dimension_guard");
    status = ProDimensionIsReldriven(&thickness_dimension, &relation_driven);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionIsRegenednegative(&thickness_dimension, &old_negative);
    if (status != PRO_TK_NO_ERROR || relation_driven == PRO_B_TRUE)
    {
        exit_code = write_error(out, "thickness_direction_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_NO_PERMISSION : status);
        goto cleanup;
    }
    status = compute_outline((ProSolid)part_model, before_outline);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "sheetmetal_outline", status);
        goto cleanup;
    }
    if (outline_matches_front_wall(before_outline,
            expected_length, expected_width, expected_thickness, 1))
    {
        already_positive_z = 1;
        memcpy(after_outline, before_outline, sizeof(after_outline));
        goto direction_ready;
    }
    if (!outline_matches_front_wall(before_outline,
            expected_length, expected_width, expected_thickness, 0))
    {
        exit_code = write_error(out, "negative_z_outline_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }

    status = ProFeatureElemtreeExtract(
        &feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &tree);
    if (status == PRO_TK_NO_ERROR)
        status = element_by_ids_get(
            tree, direction_path_ids, 1, &direction_element);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementIntegerGet(
            direction_element, NULL, &old_direction_value);
    if (status != PRO_TK_NO_ERROR ||
        old_direction_value != PRO_EXT_CR_IN_SIDE_ONE)
    {
        exit_code = write_error(out, "direction_element_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProElementIntegerSet(
        direction_element, PRO_EXT_CR_IN_SIDE_TWO);
    if (status == PRO_TK_NO_ERROR)
        status = ProArrayAlloc(
            1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&options);
    if (status == PRO_TK_NO_ERROR)
    {
        options[0] = PRO_FEAT_CR_DEFINE_MISS_ELEMS;
        status = ProFeatureWithoptionsRedefine(
            NULL, &feature, tree, options, PRO_REGEN_NO_FLAGS, &errors);
    }
    if (status != PRO_TK_NO_ERROR || errors.error_number != 0)
    {
        exit_code = write_error(out, "feature_direction_redefine",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    direction_changed = 1;
    status = regenerate_solid((ProSolid)part_model, &part_regen_attempts);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "regenerate_sheetmetal", status);
        goto cleanup;
    }
    status = ProFeatureElemtreeExtract(
        &feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &readback_tree);
    if (status == PRO_TK_NO_ERROR)
        status = element_by_ids_get(
            readback_tree, direction_path_ids, 1,
            &readback_direction_element);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementIntegerGet(
            readback_direction_element, NULL, &new_direction_value);
    if (status != PRO_TK_NO_ERROR ||
        new_direction_value != PRO_EXT_CR_IN_SIDE_TWO)
    {
        exit_code = write_error(out, "direction_element_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = find_dimension_by_symbol((ProSolid)part_model, argv[6], &thickness_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&thickness_dimension, &value);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionIsRegenednegative(&thickness_dimension, &new_negative);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(value, expected_thickness))
    {
        exit_code = write_error(out, "direction_dimension_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = compute_outline((ProSolid)part_model, after_outline);
    if (status != PRO_TK_NO_ERROR ||
        !outline_matches_front_wall(after_outline,
            expected_length, expected_width, expected_thickness, 1))
    {
        exit_code = write_error(out, "positive_z_outline_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
direction_ready:
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)part_model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &mass_property);
    if (status != PRO_TK_NO_ERROR ||
        !nearly_equal(mass_property.volume,
            expected_length * expected_width * expected_thickness))
    {
        exit_code = write_error(out, "volume_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = regenerate_solid((ProSolid)assembly_model, &assembly_regen_attempts);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "regenerate_top_assembly", status);
        goto cleanup;
    }
    status = compute_outline((ProSolid)part_model, after_outline);
    if (status != PRO_TK_NO_ERROR ||
        !outline_matches_front_wall(after_outline,
            expected_length, expected_width, expected_thickness, 1))
    {
        exit_code = write_error(out, "post_assembly_outline_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }

    status = ProDirectoryCurrentGet(working_directory);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlSave(part_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_models", status);
        goto cleanup;
    }
    saved = 1;
    if (!find_latest_saved_model(working_directory, part_name, L"prt",
            part_saved_path, _countof(part_saved_path)) ||
        !find_latest_saved_model(working_directory, argv[2], L"asm",
            assembly_saved_path, _countof(assembly_saved_path)))
    {
        exit_code = write_error(out, "saved_files_guard", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    status = ProMdlDisplay(part_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlWindowGet(part_model, &window_id);
    if (status != PRO_TK_NO_ERROR)
        status = ProObjectwindowMdlnameCreate(part_name, PRO_PART, &window_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProWindowCurrentSet(window_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProWindowActivate(window_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlCurrentGet(&current_readback);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(current_readback, readback_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(readback_name, part_name) != 0)
    {
        exit_code = write_error(out, "active_sheetmetal_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    if (window_id >= 0)
    {
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"assembly\":", out);
    write_wide_json_string(out, argv[2]);
    fputs(",\"sheetmetal\":", out);
    write_wide_json_string(out, part_name);
    fputs(",\"feature_name\":", out);
    write_wide_json_string(out, argv[4]);
    fprintf(out,
        ",\"feature_id\":%d,\"direction\":\"+Z\","
        "\"already_positive_z\":%s,"
        "\"old_direction_value\":%d,\"new_direction_value\":%d,"
        "\"old_regenerated_negative\":%s,\"new_regenerated_negative\":%s,"
        "\"dimensions\":[%.15g,%.15g,%.15g],\"volume\":%.17g,"
        "\"outline\":{\"min\":[%.15g,%.15g,%.15g],"
        "\"max\":[%.15g,%.15g,%.15g],\"size\":[%.15g,%.15g,%.15g]},"
        "\"regenerate_attempts\":{\"sheetmetal\":%d,\"top_assembly\":%d},"
        "\"sheetmetal_saved_file\":",
        expected_feature_id,
        already_positive_z ? "true" : "false",
        old_direction_value, new_direction_value,
        old_negative == PRO_B_TRUE ? "true" : "false",
        new_negative == PRO_B_TRUE ? "true" : "false",
        expected_length, expected_width, expected_thickness,
        mass_property.volume,
        after_outline[0][0], after_outline[0][1], after_outline[0][2],
        after_outline[1][0], after_outline[1][1], after_outline[1][2],
        after_outline[1][0] - after_outline[0][0],
        after_outline[1][1] - after_outline[0][1],
        after_outline[1][2] - after_outline[0][2],
        part_regen_attempts, assembly_regen_attempts);
    write_wide_json_string(out, part_saved_path);
    fputs(",\"assembly_saved_file\":", out);
    write_wide_json_string(out, assembly_saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0 && direction_changed && !saved && part_model != NULL)
    {
        ProErrorlist rollback_errors = {NULL, 0};
        if (direction_element != NULL && tree != NULL)
        {
            ProElementIntegerSet(direction_element, old_direction_value);
            ProFeatureWithoptionsRedefine(
                NULL, &feature, tree, options, PRO_REGEN_NO_FLAGS,
                &rollback_errors);
            ProSolidRegenerate((ProSolid)part_model, PRO_REGEN_NO_FLAGS);
            if (assembly_model != NULL)
                ProSolidRegenerate((ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
        }
        if (rollback_errors.error_list != NULL)
            ProArrayFree((ProArray *)&rollback_errors.error_list);
    }
    if (readback_tree != NULL)
        ProFeatureElemtreeFree(&feature, readback_tree);
    if (tree != NULL)
        ProFeatureElemtreeFree(&feature, tree);
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
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
