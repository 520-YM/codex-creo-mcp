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
#include <ProAsmcomp.h>
#include <ProRelSet.h>
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
    int feature_id;
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

static int nearly_equal(double actual, double expected)
{
    double tolerance = fabs(expected) * 1.0e-8;
    if (tolerance < 1.0e-7)
        tolerance = 1.0e-7;
    return fabs(actual - expected) <= tolerance;
}

static int parse_positive_dimension(const wchar_t *text, double *value)
{
    wchar_t *end = NULL;
    double parsed;
    if (text == NULL || *text == L'\0')
        return 0;
    parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) ||
        parsed < 0.1 || parsed > 100000.0)
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
        context->feature_id = feature->id;
        ++context->match_count;
    }
    return PRO_TK_NO_ERROR;
}

static ProError find_component_feature_id(
    ProAssembly assembly,
    const wchar_t *expected_name,
    int *feature_id)
{
    ComponentFindContext context;
    ProError status;
    context.expected_name = expected_name;
    context.feature_id = PRO_VALUE_UNUSED;
    context.match_count = 0;
    status = ProSolidFeatVisit(
        (ProSolid)assembly, component_find_action, NULL, (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    *feature_id = context.feature_id;
    return PRO_TK_NO_ERROR;
}

static ProError regenerate_solid(ProSolid solid, int allow_unattached, int *attempts)
{
    ProError status;
    *attempts = 0;
    do
    {
        status = ProSolidRegenerate(solid, PRO_REGEN_NO_FLAGS);
        ++(*attempts);
    } while (status == PRO_TK_REGEN_AGAIN && *attempts < 3);
    if (allow_unattached && status == PRO_TK_UNATTACHED_FEATS)
        return PRO_TK_NO_ERROR;
    return status;
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
        sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
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

static int outline_matches(
    ProSolid solid,
    double length,
    double width,
    double thickness,
    Pro3dPnt outline)
{
    Pro3dPnt local_outline[2];
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
    double actual[3];
    double expected[3];
    int i;
    int j;
    if (ProSolidOutlineCompute(
            solid, matrix, excludes,
            (int)(sizeof(excludes) / sizeof(excludes[0])),
            local_outline) != PRO_TK_NO_ERROR)
        return 0;
    actual[0] = fabs(local_outline[1][0] - local_outline[0][0]);
    actual[1] = fabs(local_outline[1][1] - local_outline[0][1]);
    actual[2] = fabs(local_outline[1][2] - local_outline[0][2]);
    expected[0] = length;
    expected[1] = width;
    expected[2] = thickness;
    for (i = 0; i < 2; ++i)
    {
        for (j = i + 1; j < 3; ++j)
        {
            if (actual[i] > actual[j])
            {
                double temp = actual[i];
                actual[i] = actual[j];
                actual[j] = temp;
            }
            if (expected[i] > expected[j])
            {
                double temp = expected[i];
                expected[i] = expected[j];
                expected[j] = temp;
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
    if (outline != NULL)
    {
        outline[0] = actual[0];
        outline[1] = actual[1];
        outline[2] = actual[2];
    }
    return 1;
}

static int relation_line_state(
    ProWstring *lines,
    int count,
    const wchar_t *lhs,
    const wchar_t *full_line)
{
    int i;
    for (i = 0; i < count; ++i)
    {
        if (_wcsicmp(lines[i], full_line) == 0)
            return 1;
        if (wcsstr(lines[i], lhs) != NULL)
            return -1;
    }
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl current_model = NULL;
    ProMdl assembly_model = NULL;
    ProMdl skeleton_model = NULL;
    ProMdl sheet_model = NULL;
    ProMdl component_model = NULL;
    ProMdlName current_name;
    ProMdlName assembly_name;
    ProMdlName skeleton_name;
    ProMdlName sheet_name;
    ProMdlName readback_name;
    ProName skeleton_feature_name;
    ProName sheet_profile_feature_name;
    ProName skeleton_length_symbol;
    ProName skeleton_width_symbol;
    ProName sheet_length_symbol;
    ProName sheet_width_symbol;
    ProDimension skeleton_length_dimension;
    ProDimension skeleton_width_dimension;
    ProDimension sheet_length_dimension;
    ProDimension sheet_width_dimension;
    ProFeature owner_feature;
    ProFeature component_feature;
    ProModelitem skeleton_feature_item;
    ProModelitem sheet_profile_feature_item;
    ProModelitem assembly_item;
    ProRelset relset;
    ProWstring *old_lines = NULL;
    ProWstring *new_lines = NULL;
    ProLine length_relation;
    ProLine width_relation;
    ProLine length_lhs;
    ProLine width_lhs;
    ProWstring line_pointer;
    ProPath working_directory;
    ProPath assembly_saved_path;
    ProPath skeleton_saved_path;
    ProPath sheet_saved_path;
    ProBoolean is_skeleton = PRO_B_FALSE;
    ProBoolean length_relation_driven = PRO_B_FALSE;
    ProBoolean width_relation_driven = PRO_B_FALSE;
    ProMdlfileType current_type;
    double expected_length;
    double expected_width;
    double thickness;
    double test_length;
    double test_width;
    double value = 0.0;
    double final_outline[3] = {0.0, 0.0, 0.0};
    int sheet_component_feature_id;
    int skeleton_component_feature_id = PRO_VALUE_UNUSED;
    int sheet_profile_feature_id;
    int skeleton_id = PRO_VALUE_UNUSED;
    int sheet_id = PRO_VALUE_UNUSED;
    int old_count = 0;
    int new_count = 0;
    int length_state = 0;
    int width_state = 0;
    int relation_set_created = 0;
    int relation_modified = 0;
    int skeleton_changed = 0;
    int connected = 0;
    int regen_attempts = 0;
    int skeleton_test_regen_attempts = 0;
    int assembly_test_regen_attempts = 0;
    int skeleton_restore_regen_attempts = 0;
    int assembly_restore_regen_attempts = 0;
    int window_id = -1;
    int sheet_window_id = -1;
    int assembly_window_id = -1;
    int exit_code = 1;

    if (argc != 16)
    {
        fwprintf(stderr,
            L"Usage: creo_sheetmetal_skeleton_link_bridge <result.json> "
            L"<assembly> <skeleton> <sheetmetal> <sheet_component_feature_id> "
            L"<skeleton_feature> <sheet_profile_feature> <sheet_profile_feature_id> "
            L"<skeleton_length_symbol> <skeleton_width_symbol> "
            L"<sheet_length_symbol> <sheet_width_symbol> "
            L"<expected_length> <expected_width> <thickness>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;

    wcsncpy_s(assembly_name, _countof(assembly_name), argv[2], _TRUNCATE);
    wcsncpy_s(skeleton_name, _countof(skeleton_name), argv[3], _TRUNCATE);
    wcsncpy_s(sheet_name, _countof(sheet_name), argv[4], _TRUNCATE);
    wcsncpy_s(skeleton_feature_name, _countof(skeleton_feature_name), argv[6], _TRUNCATE);
    wcsncpy_s(sheet_profile_feature_name, _countof(sheet_profile_feature_name), argv[7], _TRUNCATE);
    wcsncpy_s(skeleton_length_symbol, _countof(skeleton_length_symbol), argv[9], _TRUNCATE);
    wcsncpy_s(skeleton_width_symbol, _countof(skeleton_width_symbol), argv[10], _TRUNCATE);
    wcsncpy_s(sheet_length_symbol, _countof(sheet_length_symbol), argv[11], _TRUNCATE);
    wcsncpy_s(sheet_width_symbol, _countof(sheet_width_symbol), argv[12], _TRUNCATE);
    if (!parse_positive_int(argv[5], &sheet_component_feature_id) ||
        !parse_positive_int(argv[8], &sheet_profile_feature_id) ||
        !parse_positive_dimension(argv[13], &expected_length) ||
        !parse_positive_dimension(argv[14], &expected_width) ||
        !parse_positive_dimension(argv[15], &thickness))
    {
        exit_code = write_error(out, "input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    test_length = expected_length + 10.0;
    test_width = expected_width + 1.0;

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
    if (status != PRO_TK_NO_ERROR || _wcsicmp(current_name, sheet_name) != 0)
    {
        exit_code = write_error(out, "active_sheetmetal_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProMdlTypeGet(current_model, &current_type);
    if (status != PRO_TK_NO_ERROR || current_type != PRO_MDLFILE_PART)
    {
        exit_code = write_error(out, "sheetmetal_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }

    status = ProMdlnameInit(assembly_name, PRO_MDLFILE_ASSEMBLY, &assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "assembly_init", status);
        goto cleanup;
    }
    status = ProAsmSkeletonGet((ProAssembly)assembly_model, &skeleton_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(skeleton_model, readback_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(readback_name, skeleton_name) != 0)
    {
        exit_code = write_error(out, "skeleton_guard",
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

    status = ProFeatureInit((ProSolid)assembly_model,
        sheet_component_feature_id, &component_feature);
    if (status == PRO_TK_NO_ERROR)
        status = ProAsmcompMdlGet((ProAsmcomp *)&component_feature, &component_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(component_model, readback_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(readback_name, sheet_name) != 0 ||
        component_model != current_model)
    {
        exit_code = write_error(out, "sheetmetal_component_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    sheet_model = component_model;
    status = ProMdlWindowGet(sheet_model, &sheet_window_id);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "sheetmetal_window_guard", status);
        goto cleanup;
    }
    status = find_component_feature_id(
        (ProAssembly)assembly_model, skeleton_name,
        &skeleton_component_feature_id);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_component_feature", status);
        goto cleanup;
    }

    status = ProModelitemByNameInit(
        skeleton_model, PRO_FEATURE, skeleton_feature_name, &skeleton_feature_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_feature_guard", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        sheet_model, PRO_FEATURE, sheet_profile_feature_name, &sheet_profile_feature_item);
    if (status != PRO_TK_NO_ERROR || sheet_profile_feature_item.id != sheet_profile_feature_id)
    {
        exit_code = write_error(out, "sheet_profile_feature_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }

    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, skeleton_length_symbol, &skeleton_length_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_length_dimension", status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, skeleton_width_symbol, &skeleton_width_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_width_dimension", status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)sheet_model, sheet_length_symbol, &sheet_length_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "sheet_length_dimension", status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)sheet_model, sheet_width_symbol, &sheet_width_dimension);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "sheet_width_dimension", status);
        goto cleanup;
    }

#define CHECK_OWNER(DIMENSION, EXPECTED_ID, STAGE) \
    do { \
        status = ProDimensionOwnerfeatureGet(&(DIMENSION), &owner_feature); \
        if (status != PRO_TK_NO_ERROR || owner_feature.id != (EXPECTED_ID)) { \
            exit_code = write_error(out, (STAGE), \
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); \
            goto cleanup; \
        } \
    } while (0)

    CHECK_OWNER(skeleton_length_dimension, skeleton_feature_item.id, "skeleton_length_owner");
    CHECK_OWNER(skeleton_width_dimension, skeleton_feature_item.id, "skeleton_width_owner");
    CHECK_OWNER(sheet_length_dimension, sheet_profile_feature_id, "sheet_length_owner");
    CHECK_OWNER(sheet_width_dimension, sheet_profile_feature_id, "sheet_width_owner");

#define CHECK_VALUE(DIMENSION, EXPECTED, STAGE) \
    do { \
        status = ProDimensionValueGet(&(DIMENSION), &value); \
        if (status != PRO_TK_NO_ERROR || !nearly_equal(value, (EXPECTED))) { \
            exit_code = write_error(out, (STAGE), \
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); \
            goto cleanup; \
        } \
    } while (0)

    CHECK_VALUE(skeleton_length_dimension, expected_length, "skeleton_length_value_guard");
    CHECK_VALUE(skeleton_width_dimension, expected_width, "skeleton_width_value_guard");
    CHECK_VALUE(sheet_length_dimension, expected_length, "sheet_length_value_guard");
    CHECK_VALUE(sheet_width_dimension, expected_width, "sheet_width_value_guard");
    if (!outline_matches((ProSolid)sheet_model,
            expected_length, expected_width, thickness, NULL))
    {
        exit_code = write_error(out, "sheet_outline_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }

    status = ProMdlIdGet(skeleton_model, &skeleton_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlIdGet(sheet_model, &sheet_id);
    if (status != PRO_TK_NO_ERROR || skeleton_id == PRO_VALUE_UNUSED ||
        sheet_id == PRO_VALUE_UNUSED || skeleton_id == sheet_id)
    {
        exit_code = write_error(out, "model_session_ids",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    /* ProMdlIdGet includes the model-kind tag in the high bits. Creo
       relation syntax uses the user-visible Session ID in the low 20 bits. */
    skeleton_id &= 0xFFFFF;
    sheet_id &= 0xFFFFF;
    if (skeleton_id < 1 || sheet_id < 1 || skeleton_id == sheet_id)
    {
        exit_code = write_error(out, "relation_session_ids", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }

    _snwprintf_s(length_lhs, _countof(length_lhs), _TRUNCATE,
        L"%ls:cid_%d", sheet_length_symbol, sheet_component_feature_id);
    _snwprintf_s(width_lhs, _countof(width_lhs), _TRUNCATE,
        L"%ls:cid_%d", sheet_width_symbol, sheet_component_feature_id);
    _snwprintf_s(length_relation, _countof(length_relation), _TRUNCATE,
        L"%ls = %ls:cid_%d", length_lhs,
        skeleton_length_symbol, skeleton_component_feature_id);
    _snwprintf_s(width_relation, _countof(width_relation), _TRUNCATE,
        L"%ls = %ls:cid_%d", width_lhs,
        skeleton_width_symbol, skeleton_component_feature_id);

    /* Assembly relations that reference component Session IDs must be parsed
       while the top-level assembly is the current Creo model. */
    status = ProMdlWindowGet(assembly_model, &assembly_window_id);
    if (status != PRO_TK_NO_ERROR)
        status = ProObjectwindowMdlnameCreate(
            assembly_name, PRO_ASSEMBLY, &assembly_window_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProWindowCurrentSet(assembly_window_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProWindowActivate(assembly_window_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlCurrentGet(&component_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(component_model, readback_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(readback_name, assembly_name) != 0)
    {
        exit_code = write_error(out, "assembly_current_context",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }

    status = ProMdlToModelitem(assembly_model, &assembly_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "assembly_modelitem", status);
        goto cleanup;
    }
    status = ProArrayAlloc(0, sizeof(ProWstring), 1, (ProArray *)&old_lines);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "old_relations_array", status);
        goto cleanup;
    }
    status = ProModelitemToRelset(&assembly_item, &relset);
    if (status == PRO_TK_NO_ERROR)
    {
        status = ProRelsetRelationsGet(&relset, &old_lines);
        if (status != PRO_TK_NO_ERROR && status != PRO_TK_BAD_INPUTS)
        {
            exit_code = write_error(out, "relations_get", status);
            goto cleanup;
        }
        status = ProArraySizeGet((ProArray)old_lines, &old_count);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "relations_count", status);
            goto cleanup;
        }
    }
    else
    {
        status = ProRelsetCreate(&assembly_item, &relset);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "relations_create", status);
            goto cleanup;
        }
        relation_set_created = 1;
        old_count = 0;
    }

    length_state = relation_line_state(old_lines, old_count, length_lhs, length_relation);
    width_state = relation_line_state(old_lines, old_count, width_lhs, width_relation);
    if (length_state < 0 || width_state < 0)
    {
        exit_code = write_error(out, "existing_relation_conflict", PRO_TK_E_FOUND);
        goto cleanup;
    }

    status = ProArrayAlloc(0, sizeof(ProWstring), 1, (ProArray *)&new_lines);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "new_relations_array", status);
        goto cleanup;
    }
    for (int index = 0; index < old_count; ++index)
    {
        line_pointer = old_lines[index];
        status = ProArrayObjectAdd(
            (ProArray *)&new_lines, PRO_VALUE_UNUSED, 1, &line_pointer);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "copy_old_relation", status);
            goto cleanup;
        }
    }
    if (length_state == 0)
    {
        line_pointer = length_relation;
        status = ProArrayObjectAdd(
            (ProArray *)&new_lines, PRO_VALUE_UNUSED, 1, &line_pointer);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "add_length_relation", status);
            goto cleanup;
        }
        relation_modified = 1;
    }
    if (width_state == 0)
    {
        line_pointer = width_relation;
        status = ProArrayObjectAdd(
            (ProArray *)&new_lines, PRO_VALUE_UNUSED, 1, &line_pointer);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "add_width_relation", status);
            goto cleanup;
        }
        relation_modified = 1;
    }
    status = ProArraySizeGet((ProArray)new_lines, &new_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "new_relations_count", status);
        goto cleanup;
    }
    if (relation_modified)
    {
        status = ProRelsetRelationsSet(&relset, new_lines, new_count);
        if (status != PRO_TK_NO_ERROR)
        {
            fputs("{\"ok\":false,\"api_only\":true,\"transaction_rolled_back\":true,"
                "\"stage\":\"relations_set\",\"error_code\":", out);
            fprintf(out, "%d,\"sheetmetal_session_id\":%d,"
                "\"skeleton_session_id\":%d,\"length_relation\":",
                status, sheet_id, skeleton_id);
            write_wide_json_string(out, length_relation);
            fputs(",\"width_relation\":", out);
            write_wide_json_string(out, width_relation);
            fprintf(out, ",\"old_relation_count\":%d,"
                "\"new_relation_count\":%d}\n", old_count, new_count);
            exit_code = 1;
            goto cleanup;
        }
        status = ProRelsetRegenerate(&relset);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "relations_regenerate", status);
            goto cleanup;
        }
    }
    status = regenerate_solid((ProSolid)assembly_model, 0, &regen_attempts);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "initial_assembly_regenerate", status);
        goto cleanup;
    }

    status = ProDimensionValueSet(&skeleton_length_dimension, test_length);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueSet(&skeleton_width_dimension, test_width);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "test_skeleton_dimension_set", status);
        goto cleanup;
    }
    skeleton_changed = 1;
    status = regenerate_solid(
        (ProSolid)skeleton_model, 1, &skeleton_test_regen_attempts);
    if (status == PRO_TK_NO_ERROR)
        status = regenerate_solid(
            (ProSolid)assembly_model, 0, &assembly_test_regen_attempts);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "test_regenerate", status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)sheet_model, sheet_length_symbol, &sheet_length_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&sheet_length_dimension, &value);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(value, test_length))
    {
        exit_code = write_error(out, "test_length_dependency",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)sheet_model, sheet_width_symbol, &sheet_width_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&sheet_width_dimension, &value);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(value, test_width))
    {
        exit_code = write_error(out, "test_width_dependency",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    if (!outline_matches((ProSolid)sheet_model,
            test_length, test_width, thickness, NULL))
    {
        exit_code = write_error(out, "test_geometry_dependency", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }

    status = find_dimension_by_symbol(
        (ProSolid)skeleton_model, skeleton_length_symbol, &skeleton_length_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueSet(&skeleton_length_dimension, expected_length);
    if (status == PRO_TK_NO_ERROR)
        status = find_dimension_by_symbol(
            (ProSolid)skeleton_model, skeleton_width_symbol, &skeleton_width_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueSet(&skeleton_width_dimension, expected_width);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "restore_skeleton_dimensions", status);
        goto cleanup;
    }
    skeleton_changed = 0;
    status = regenerate_solid(
        (ProSolid)skeleton_model, 1, &skeleton_restore_regen_attempts);
    if (status == PRO_TK_NO_ERROR)
        status = regenerate_solid(
            (ProSolid)assembly_model, 0, &assembly_restore_regen_attempts);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "restore_regenerate", status);
        goto cleanup;
    }

    status = find_dimension_by_symbol(
        (ProSolid)sheet_model, sheet_length_symbol, &sheet_length_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&sheet_length_dimension, &value);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(value, expected_length))
    {
        exit_code = write_error(out, "restored_length_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = find_dimension_by_symbol(
        (ProSolid)sheet_model, sheet_width_symbol, &sheet_width_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&sheet_width_dimension, &value);
    if (status != PRO_TK_NO_ERROR || !nearly_equal(value, expected_width))
    {
        exit_code = write_error(out, "restored_width_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    if (!outline_matches((ProSolid)sheet_model,
            expected_length, expected_width, thickness, final_outline))
    {
        exit_code = write_error(out, "restored_geometry_guard", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }
    ProDimensionIsReldriven(&sheet_length_dimension, &length_relation_driven);
    ProDimensionIsReldriven(&sheet_width_dimension, &width_relation_driven);

    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    status = ProMdlSave(skeleton_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlSave(sheet_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_models", status);
        goto cleanup;
    }
    if (!find_latest_saved_model(working_directory, skeleton_name, L"prt",
            skeleton_saved_path, _countof(skeleton_saved_path)) ||
        !find_latest_saved_model(working_directory, sheet_name, L"prt",
            sheet_saved_path, _countof(sheet_saved_path)) ||
        !find_latest_saved_model(working_directory, assembly_name, L"asm",
            assembly_saved_path, _countof(assembly_saved_path)))
    {
        exit_code = write_error(out, "saved_files_guard", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    ProMdlDisplay(sheet_model);
    status = ProWindowCurrentSet(sheet_window_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProWindowActivate(sheet_window_id);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "restore_sheetmetal_window", status);
        goto cleanup;
    }
    window_id = sheet_window_id;
    if (window_id >= 0)
    {
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"association\":\"assembly_relation\",", out);
    fputs("\"dependency_verified\":true,\"active_model\":", out);
    write_wide_json_string(out, sheet_name);
    fputs(",\"assembly\":", out);
    write_wide_json_string(out, assembly_name);
    fputs(",\"skeleton\":", out);
    write_wide_json_string(out, skeleton_name);
    fputs(",\"relations\":[", out);
    write_wide_json_string(out, length_relation);
    fputc(',', out);
    write_wide_json_string(out, width_relation);
    fprintf(out,
        "],\"session_ids\":{\"sheetmetal\":%d,\"skeleton\":%d},"
        "\"component_ids\":{\"sheetmetal\":%d,\"skeleton\":%d},"
        "\"dimensions\":{\"restored\":[%.15g,%.15g,%.15g],"
        "\"verification_test\":[%.15g,%.15g,%.15g],"
        "\"sheet_relation_driven_flags\":[%s,%s]},"
        "\"final_sorted_extents\":[%.15g,%.15g,%.15g],"
        "\"regenerate_attempts\":{\"initial_assembly\":%d,"
        "\"test_skeleton\":%d,\"test_assembly\":%d,"
        "\"restore_skeleton\":%d,\"restore_assembly\":%d},"
        "\"skeleton_saved_file\":",
        sheet_id, skeleton_id,
        sheet_component_feature_id, skeleton_component_feature_id,
        expected_length, expected_width, thickness,
        test_length, test_width, thickness,
        length_relation_driven == PRO_B_TRUE ? "true" : "false",
        width_relation_driven == PRO_B_TRUE ? "true" : "false",
        final_outline[0], final_outline[1], final_outline[2],
        regen_attempts,
        skeleton_test_regen_attempts, assembly_test_regen_attempts,
        skeleton_restore_regen_attempts, assembly_restore_regen_attempts);
    write_wide_json_string(out, skeleton_saved_path);
    fputs(",\"sheetmetal_saved_file\":", out);
    write_wide_json_string(out, sheet_saved_path);
    fputs(",\"assembly_saved_file\":", out);
    write_wide_json_string(out, assembly_saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0)
    {
        if (skeleton_changed && skeleton_model != NULL)
        {
            if (find_dimension_by_symbol(
                    (ProSolid)skeleton_model, skeleton_length_symbol,
                    &skeleton_length_dimension) == PRO_TK_NO_ERROR)
                ProDimensionValueSet(&skeleton_length_dimension, expected_length);
            if (find_dimension_by_symbol(
                    (ProSolid)skeleton_model, skeleton_width_symbol,
                    &skeleton_width_dimension) == PRO_TK_NO_ERROR)
                ProDimensionValueSet(&skeleton_width_dimension, expected_width);
        }
        if (relation_modified)
        {
            if (relation_set_created)
                ProRelsetDelete(&relset);
            else
            {
                ProRelsetRelationsSet(&relset, old_lines, old_count);
                ProRelsetRegenerate(&relset);
            }
        }
        if (skeleton_model != NULL)
            ProSolidRegenerate((ProSolid)skeleton_model, PRO_REGEN_NO_FLAGS);
        if (assembly_model != NULL)
            ProSolidRegenerate((ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
    }
    if (old_lines != NULL)
        ProArrayFree((ProArray *)&old_lines);
    if (new_lines != NULL)
        ProArrayFree((ProArray *)&new_lines);
    if (sheet_window_id >= 0)
    {
        ProWindowCurrentSet(sheet_window_id);
        ProWindowActivate(sheet_window_id);
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
