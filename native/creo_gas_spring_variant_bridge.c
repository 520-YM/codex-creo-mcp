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
#include <ProArray.h>

typedef struct DimensionByOwnerValueContext
{
    int owner_feature_id;
    double expected_value;
    int match_count;
    ProDimension dimension;
} DimensionByOwnerValueContext;

typedef struct NamedComponentContext
{
    const wchar_t *model_name;
    int match_count;
    ProFeature feature;
    ProMdl model;
} NamedComponentContext;

typedef struct MaxLimitContext
{
    double expected_value;
    int match_count;
    ProElement element;
    double old_value;
} MaxLimitContext;

static void json_utf8(FILE *out, const char *text)
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

static void json_wide(FILE *out, const wchar_t *text)
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
    json_utf8(out, utf8);
    free(utf8);
}

static int fail(FILE *out, const char *stage, ProError status)
{
    fputs("{\"ok\":false,\"api_only\":true,\"stage\":", out);
    json_utf8(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", status);
    return 1;
}

static int parse_value(const wchar_t *text, double *value)
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

static int near_value(double actual, double expected)
{
    double tolerance = fabs(expected) * 1.0e-8;
    if (tolerance < 1.0e-7)
        tolerance = 1.0e-7;
    return fabs(actual - expected) <= tolerance;
}

static ProError feature_by_names(
    ProMdl model,
    const wchar_t *primary_name,
    const wchar_t *fallback_name,
    ProFeature *feature,
    ProName actual_name)
{
    ProModelitem item;
    ProError status;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    status = ProModelitemByNameInit(
        model, PRO_FEATURE, (wchar_t *)primary_name, &item);
    if (status != PRO_TK_NO_ERROR && fallback_name != NULL)
        status = ProModelitemByNameInit(
            model, PRO_FEATURE, (wchar_t *)fallback_name, &item);
    if (status != PRO_TK_NO_ERROR)
        return status;
    *feature = *(ProFeature *)&item;
    status = ProFeatureStatusGet(feature, &feature_status);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (feature_status != PRO_FEAT_ACTIVE)
        return PRO_TK_BAD_CONTEXT;
    if (actual_name != NULL)
    {
        status = ProModelitemNameGet(&item, actual_name);
        if (status != PRO_TK_NO_ERROR)
            return status;
    }
    return PRO_TK_NO_ERROR;
}

static ProError dimension_by_owner_value_action(
    ProDimension *dimension,
    ProError filter_status,
    ProAppData app_data)
{
    DimensionByOwnerValueContext *context =
        (DimensionByOwnerValueContext *)app_data;
    ProFeature owner;
    double value = 0.0;
    (void)filter_status;
    if (ProDimensionOwnerfeatureGet(dimension, &owner) == PRO_TK_NO_ERROR &&
        owner.id == context->owner_feature_id &&
        ProDimensionValueGet(dimension, &value) == PRO_TK_NO_ERROR &&
        near_value(value, context->expected_value))
    {
        context->dimension = *dimension;
        ++context->match_count;
    }
    return PRO_TK_NO_ERROR;
}

static ProError unique_dimension_by_owner_value(
    ProSolid solid,
    int owner_feature_id,
    double expected_value,
    ProDimension *dimension)
{
    DimensionByOwnerValueContext context;
    ProError status;
    ProBoolean relation_driven = PRO_B_FALSE;
    memset(&context, 0, sizeof(context));
    context.owner_feature_id = owner_feature_id;
    context.expected_value = expected_value;
    status = ProSolidDimensionVisit(
        solid, PRO_B_FALSE, dimension_by_owner_value_action, NULL,
        (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    status = ProDimensionIsReldriven(&context.dimension, &relation_driven);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (relation_driven == PRO_B_TRUE)
        return PRO_TK_NO_PERMISSION;
    *dimension = context.dimension;
    return PRO_TK_NO_ERROR;
}

static ProError named_component_action(
    ProFeature *feature,
    ProError filter_status,
    ProAppData app_data)
{
    NamedComponentContext *context = (NamedComponentContext *)app_data;
    ProFeattype type = PRO_FEAT_INVALID;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProMdl component_model = NULL;
    ProMdlName component_name;
    (void)filter_status;
    if (ProFeatureTypeGet(feature, &type) == PRO_TK_NO_ERROR &&
        type == PRO_FEAT_COMPONENT &&
        ProFeatureStatusGet(feature, &feature_status) == PRO_TK_NO_ERROR &&
        feature_status == PRO_FEAT_ACTIVE &&
        ProAsmcompMdlGet((ProAsmcomp *)feature, &component_model) == PRO_TK_NO_ERROR &&
        ProMdlNameGet(component_model, component_name) == PRO_TK_NO_ERROR &&
        _wcsicmp(component_name, context->model_name) == 0)
    {
        context->feature = *feature;
        context->model = component_model;
        ++context->match_count;
    }
    return PRO_TK_NO_ERROR;
}

static ProError named_component_get(
    ProAssembly assembly,
    const wchar_t *model_name,
    ProFeature *feature,
    ProMdl *component_model)
{
    NamedComponentContext context;
    ProError status;
    memset(&context, 0, sizeof(context));
    context.model_name = model_name;
    status = ProSolidFeatVisit(
        (ProSolid)assembly, named_component_action, NULL,
        (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    *feature = context.feature;
    *component_model = context.model;
    return PRO_TK_NO_ERROR;
}

static ProError max_limit_action(
    ProElement elem_tree,
    ProElement elem,
    ProElempath elem_path,
    ProAppData app_data)
{
    MaxLimitContext *context = (MaxLimitContext *)app_data;
    ProElemId id = (ProElemId)0;
    double value = 0.0;
    (void)elem_tree;
    (void)elem_path;
    if (ProElementIdGet(elem, &id) == PRO_TK_NO_ERROR &&
        id == PRO_E_COMPONENT_JAS_MAX_LIMIT_VAL &&
        ProElementDoubleGet(elem, NULL, &value) == PRO_TK_NO_ERROR &&
        near_value(value, context->expected_value))
    {
        context->element = elem;
        context->old_value = value;
        ++context->match_count;
    }
    return PRO_TK_NO_ERROR;
}

static ProError unique_max_limit_get(
    ProElement tree,
    double expected_value,
    ProElement *element,
    double *old_value)
{
    MaxLimitContext context;
    ProError status;
    memset(&context, 0, sizeof(context));
    context.expected_value = expected_value;
    status = ProElemtreeElementVisit(
        tree, NULL, NULL, max_limit_action, (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    *element = context.element;
    *old_value = context.old_value;
    return PRO_TK_NO_ERROR;
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
    if (status == PRO_TK_UNATTACHED_FEATS)
        return PRO_TK_NO_ERROR;
    return status;
}

static int model_family_exists(
    const wchar_t *directory,
    const wchar_t *name,
    const wchar_t *extension)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    _snwprintf_s(
        pattern, sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
        L"%ls\\%ls.%ls*", directory, name, extension);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(handle);
    return 1;
}

static int latest_model_path(
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
    _snwprintf_s(
        pattern, sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
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
            _snwprintf_s(
                saved_path, saved_path_count, _TRUNCATE,
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
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProError status;
    ProError disconnect_status;
    ProMdl current = NULL;
    ProMdl down_model = NULL;
    ProMdl up_model = NULL;
    ProMdl probe = NULL;
    ProMdlType current_type = PRO_MDL_UNUSED;
    ProMdlName current_name;
    ProMdlName check_name;
    ProFeature down_component;
    ProFeature up_component;
    ProFeature down_stroke_feature;
    ProFeature up_stroke_feature;
    ProFeature up_comp_feature;
    ProName down_feature_name;
    ProName up_feature_name;
    ProName comp_feature_name;
    ProDimension down_dimension;
    ProDimension up_dimension;
    ProDimension comp_dimension;
    ProName down_symbol;
    ProName up_symbol;
    ProName comp_symbol;
    ProElement component_tree = NULL;
    ProElement max_limit_element = NULL;
    ProElement readback_tree = NULL;
    ProElement readback_max_element = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist redefine_errors = {NULL, 0};
    ProErrorlist rollback_errors = {NULL, 0};
    ProPath working_directory;
    ProPath down_saved_path;
    ProPath up_saved_path;
    ProPath assembly_saved_path;
    double old_stroke;
    double new_stroke;
    double old_compensation;
    double new_compensation;
    double old_max_limit;
    double new_max_limit;
    double down_old_value = 0.0;
    double up_old_value = 0.0;
    double comp_old_value = 0.0;
    double max_old_value = 0.0;
    double verified_value = 0.0;
    int connected = 0;
    int down_changed = 0;
    int up_changed = 0;
    int comp_changed = 0;
    int max_changed = 0;
    int down_renamed = 0;
    int up_renamed = 0;
    int assembly_renamed = 0;
    int down_regen_attempts = 0;
    int up_regen_attempts = 0;
    int assembly_regen_attempts = 0;
    int window_id = -1;
    int exit_code = 1;

    if (argc != 14)
    {
        fwprintf(stderr,
            L"Usage: creo_gas_spring_variant_bridge <result.json> "
            L"<old_asm> <new_asm> <old_down> <new_down> <old_up> <new_up> "
            L"<old_stroke> <new_stroke> <old_comp> <new_comp> "
            L"<old_max> <new_max>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    if (!parse_value(argv[8], &old_stroke) ||
        !parse_value(argv[9], &new_stroke) ||
        !parse_value(argv[10], &old_compensation) ||
        !parse_value(argv[11], &new_compensation) ||
        !parse_value(argv[12], &old_max_limit) ||
        !parse_value(argv[13], &new_max_limit))
    {
        exit_code = fail(out, "numeric_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    if (wcslen(argv[3]) >= PRO_NAME_SIZE ||
        wcslen(argv[5]) >= PRO_NAME_SIZE ||
        wcslen(argv[7]) >= PRO_NAME_SIZE)
    {
        exit_code = fail(out, "target_name_length", PRO_TK_BAD_INPUTS);
        goto done;
    }

    status = ProEngineerConnect(
        "", "", "", "", PRO_B_TRUE, 20, &random_choice, &process_handle);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "connect", status);
        goto done;
    }
    connected = 1;
    status = ProMdlCurrentGet(&current);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlTypeGet(current, &current_type);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(current, current_name);
    if (status != PRO_TK_NO_ERROR || current_type != PRO_MDL_ASSEMBLY ||
        _wcsicmp(current_name, argv[2]) != 0)
    {
        exit_code = fail(out, "current_assembly_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "working_directory", status);
        goto cleanup;
    }
    if (model_family_exists(working_directory, argv[3], L"asm") ||
        model_family_exists(working_directory, argv[5], L"prt") ||
        model_family_exists(working_directory, argv[7], L"prt"))
    {
        exit_code = fail(out, "target_file_exists", PRO_TK_E_FOUND);
        goto cleanup;
    }
    if (ProMdlnameInit(argv[3], PRO_MDLFILE_ASSEMBLY, &probe) == PRO_TK_NO_ERROR ||
        ProMdlnameInit(argv[5], PRO_MDLFILE_PART, &probe) == PRO_TK_NO_ERROR ||
        ProMdlnameInit(argv[7], PRO_MDLFILE_PART, &probe) == PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "target_model_in_session", PRO_TK_E_FOUND);
        goto cleanup;
    }

    status = named_component_get(
        (ProAssembly)current, argv[4], &down_component, &down_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "down_component_guard", status);
        goto cleanup;
    }
    status = named_component_get(
        (ProAssembly)current, argv[6], &up_component, &up_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "up_component_guard", status);
        goto cleanup;
    }

    status = feature_by_names(
        down_model, L"气弹簧YQLDOWN行程", L"气弹簧DOWN行程",
        &down_stroke_feature, down_feature_name);
    if (status == PRO_TK_NO_ERROR)
        status = unique_dimension_by_owner_value(
            (ProSolid)down_model, down_stroke_feature.id,
            old_stroke, &down_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "down_stroke_guard", status);
        goto cleanup;
    }
    status = feature_by_names(
        up_model, L"气弹簧YQLUP行程", L"气弹簧UP行程",
        &up_stroke_feature, up_feature_name);
    if (status == PRO_TK_NO_ERROR)
        status = unique_dimension_by_owner_value(
            (ProSolid)up_model, up_stroke_feature.id,
            old_stroke, &up_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "up_stroke_guard", status);
        goto cleanup;
    }
    status = feature_by_names(
        up_model, L"气弹簧UP补偿距离", NULL,
        &up_comp_feature, comp_feature_name);
    if (status == PRO_TK_NO_ERROR)
        status = unique_dimension_by_owner_value(
            (ProSolid)up_model, up_comp_feature.id,
            old_compensation, &comp_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "up_compensation_guard", status);
        goto cleanup;
    }
    ProDimensionSymbolGet(&down_dimension, down_symbol);
    ProDimensionSymbolGet(&up_dimension, up_symbol);
    ProDimensionSymbolGet(&comp_dimension, comp_symbol);
    ProDimensionValueGet(&down_dimension, &down_old_value);
    ProDimensionValueGet(&up_dimension, &up_old_value);
    ProDimensionValueGet(&comp_dimension, &comp_old_value);

    status = ProFeatureElemtreeExtract(
        &up_component, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &component_tree);
    if (status == PRO_TK_NO_ERROR)
        status = unique_max_limit_get(
            component_tree, old_max_limit,
            &max_limit_element, &max_old_value);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "translation_max_guard", status);
        goto cleanup;
    }
    status = ProArrayAlloc(
        1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&options);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "redefine_options", status);
        goto cleanup;
    }
    /* PTC's component-redefinition sample uses FIX_MODEL_ON_FAIL for an
       assembly component placement tree. DEFINE_MISS_ELEMS is intended for
       geometry creation/redefinition and can make Creo abort while committing
       a previously saved mechanism JAS tree. */
    options[0] = PRO_FEAT_CR_FIX_MODEL_ON_FAIL;

    /* When shortening a gas spring, lower the mechanism limit first so Creo
       never sees a 280 mm limit on geometry that has already become 205 mm. */
    if (!near_value(max_old_value, new_max_limit) &&
        new_max_limit < max_old_value)
    {
        status = ProElementDoubleSet(max_limit_element, new_max_limit);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = fail(out, "translation_max_set", status);
            goto rollback;
        }
        status = ProFeatureWithoptionsRedefine(
            NULL, &up_component, component_tree, options,
            PRO_REGEN_NO_FLAGS, &redefine_errors);
        if (status != PRO_TK_NO_ERROR || redefine_errors.error_number != 0)
        {
            exit_code = fail(out, "translation_max_redefine",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto rollback;
        }
        max_changed = 1;
    }

    status = ProDimensionValueSet(&down_dimension, new_stroke);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "down_stroke_set", status);
        goto rollback;
    }
    down_changed = 1;
    status = ProDimensionValueSet(&up_dimension, new_stroke);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "up_stroke_set", status);
        goto rollback;
    }
    up_changed = 1;
    status = ProDimensionValueSet(&comp_dimension, new_compensation);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "up_compensation_set", status);
        goto rollback;
    }
    comp_changed = 1;
    status = regenerate_solid((ProSolid)down_model, &down_regen_attempts);
    if (status == PRO_TK_NO_ERROR)
        status = regenerate_solid((ProSolid)up_model, &up_regen_attempts);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "part_regenerate", status);
        goto rollback;
    }

    if (!near_value(max_old_value, new_max_limit) && !max_changed)
    {
        status = ProElementDoubleSet(max_limit_element, new_max_limit);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = fail(out, "translation_max_set", status);
            goto rollback;
        }
        status = ProFeatureWithoptionsRedefine(
            NULL, &up_component, component_tree, options,
            PRO_REGEN_NO_FLAGS, &redefine_errors);
        if (status != PRO_TK_NO_ERROR || redefine_errors.error_number != 0)
        {
            exit_code = fail(out, "translation_max_redefine",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto rollback;
        }
        max_changed = 1;
    }
    status = regenerate_solid((ProSolid)current, &assembly_regen_attempts);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "assembly_regenerate", status);
        goto rollback;
    }
    status = ProFeatureElemtreeExtract(
        &up_component, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &readback_tree);
    if (status == PRO_TK_NO_ERROR)
        status = unique_max_limit_get(
            readback_tree, new_max_limit,
            &readback_max_element, &verified_value);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "translation_max_readback", status);
        goto rollback;
    }

    status = ProMdlnameRename(down_model, argv[5]);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "down_rename", status);
        goto rollback;
    }
    down_renamed = 1;
    status = ProMdlnameRename(up_model, argv[7]);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "up_rename", status);
        goto rollback;
    }
    up_renamed = 1;
    status = ProMdlnameRename(current, argv[3]);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "assembly_rename", status);
        goto rollback;
    }
    assembly_renamed = 1;
    status = regenerate_solid((ProSolid)current, &assembly_regen_attempts);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "renamed_assembly_regenerate", status);
        goto rollback;
    }

    status = ProMdlSave(down_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlSave(up_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlSave(current);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "save_models", status);
        goto cleanup;
    }
    if (!latest_model_path(
            working_directory, argv[5], L"prt",
            down_saved_path, sizeof(down_saved_path) / sizeof(down_saved_path[0])) ||
        !latest_model_path(
            working_directory, argv[7], L"prt",
            up_saved_path, sizeof(up_saved_path) / sizeof(up_saved_path[0])) ||
        !latest_model_path(
            working_directory, argv[3], L"asm",
            assembly_saved_path,
            sizeof(assembly_saved_path) / sizeof(assembly_saved_path[0])))
    {
        exit_code = fail(out, "saved_file_guard", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    if (ProMdlNameGet(current, check_name) != PRO_TK_NO_ERROR ||
        _wcsicmp(check_name, argv[3]) != 0)
    {
        exit_code = fail(out, "assembly_name_readback", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }
    ProMdlDisplay(current);
    if (ProMdlWindowGet(current, &window_id) == PRO_TK_NO_ERROR)
    {
        ProWindowCurrentSet(window_id);
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"source_assembly\":", out);
    json_wide(out, argv[2]);
    fputs(",\"assembly\":", out);
    json_wide(out, argv[3]);
    fputs(",\"down_part\":", out);
    json_wide(out, argv[5]);
    fputs(",\"up_part\":", out);
    json_wide(out, argv[7]);
    fputs(",\"changes\":[{\"feature\":", out);
    json_wide(out, down_feature_name);
    fputs(",\"symbol\":", out);
    json_wide(out, down_symbol);
    fprintf(out, ",\"old_value\":%.17g,\"new_value\":%.17g},",
        down_old_value, new_stroke);
    fputs("{\"feature\":", out);
    json_wide(out, up_feature_name);
    fputs(",\"symbol\":", out);
    json_wide(out, up_symbol);
    fprintf(out, ",\"old_value\":%.17g,\"new_value\":%.17g},",
        up_old_value, new_stroke);
    fputs("{\"feature\":", out);
    json_wide(out, comp_feature_name);
    fputs(",\"symbol\":", out);
    json_wide(out, comp_symbol);
    fprintf(out, ",\"old_value\":%.17g,\"new_value\":%.17g},",
        comp_old_value, new_compensation);
    fprintf(out,
        "{\"feature\":\"YQLUP装配滑块\",\"old_value\":%.17g,"
        "\"new_value\":%.17g}],\"regenerate_attempts\":{"
        "\"down\":%d,\"up\":%d,\"assembly\":%d},"
        "\"saved_files\":[",
        max_old_value, new_max_limit,
        down_regen_attempts, up_regen_attempts, assembly_regen_attempts);
    json_wide(out, down_saved_path);
    fputc(',', out);
    json_wide(out, up_saved_path);
    fputc(',', out);
    json_wide(out, assembly_saved_path);
    fprintf(out, "],\"window_id\":%d}\n", window_id);
    exit_code = 0;
    goto cleanup;

rollback:
    if (assembly_renamed)
        ProMdlnameRename(current, argv[2]);
    if (up_renamed)
        ProMdlnameRename(up_model, argv[6]);
    if (down_renamed)
        ProMdlnameRename(down_model, argv[4]);
    if (max_changed && max_limit_element != NULL && component_tree != NULL)
    {
        ProElementDoubleSet(max_limit_element, max_old_value);
        ProFeatureWithoptionsRedefine(
            NULL, &up_component, component_tree, options,
            PRO_REGEN_NO_FLAGS, &rollback_errors);
    }
    if (comp_changed)
        ProDimensionValueSet(&comp_dimension, comp_old_value);
    if (up_changed)
        ProDimensionValueSet(&up_dimension, up_old_value);
    if (down_changed)
        ProDimensionValueSet(&down_dimension, down_old_value);
    if (down_model != NULL)
        ProSolidRegenerate((ProSolid)down_model, PRO_REGEN_NO_FLAGS);
    if (up_model != NULL)
        ProSolidRegenerate((ProSolid)up_model, PRO_REGEN_NO_FLAGS);
    if (current != NULL)
        ProSolidRegenerate((ProSolid)current, PRO_REGEN_NO_FLAGS);

cleanup:
    if (readback_tree != NULL)
        ProFeatureElemtreeFree(&up_component, readback_tree);
    if (component_tree != NULL)
        ProFeatureElemtreeFree(&up_component, component_tree);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (redefine_errors.error_list != NULL)
        ProArrayFree((ProArray *)&redefine_errors.error_list);
    if (rollback_errors.error_list != NULL)
        ProArrayFree((ProArray *)&rollback_errors.error_list);
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
