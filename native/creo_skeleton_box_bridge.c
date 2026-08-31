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
#include <ProSkeleton.h>
#include <ProWindows.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProSelection.h>
#include <ProReference.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProElemId.h>
#include <ProExtrude.h>
#include <ProSurface.h>
#include <ProGeomitem.h>
#include <ProStdSection.h>
#include <ProSection.h>
#include <ProArray.h>

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

static ProError count_feature_action(
    ProFeature *feature,
    ProError filter_status,
    ProAppData app_data)
{
    int *count = (int *)app_data;
    (void)feature;
    (void)filter_status;
    ++(*count);
    return PRO_TK_NO_ERROR;
}

static ProError feature_count_get(ProSolid solid, int *count)
{
    *count = 0;
    return ProSolidFeatVisit(
        solid,
        count_feature_action,
        NULL,
        (ProAppData)count);
}

static ProError add_integer_element(
    ProElement parent,
    ProElemId element_id,
    int value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementIntegerSet(element, value);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
}

static ProError add_double_element(
    ProElement parent,
    ProElemId element_id,
    double value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementDecimalsSet(element, 4);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementDoubleSet(element, value);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
}

static ProError add_wstring_element(
    ProElement parent,
    ProElemId element_id,
    const wchar_t *value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementWstringSet(element, (wchar_t *)value);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
}

static ProError add_reference_element(
    ProElement parent,
    ProElemId element_id,
    ProReference reference)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementReferenceSet(element, reference);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
}

static ProError add_compound_element(
    ProElement parent,
    ProElemId element_id,
    ProElement *compound)
{
    ProError status = ProElementAlloc(element_id, compound);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, *compound);
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
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElempathDataSet(path, items, count);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementGet(tree, path, element);
    ProElempathFree(&path);
    return status;
}

static ProError rectangle_section_build(
    ProSection section,
    double length,
    double width,
    const char **failure_stage)
{
    Pro2dLinedef line;
    ProError status;
    int entity_id;
    double half_length = length / 2.0;
    double half_width = width / 2.0;

    line.type = PRO_2D_LINE;
    line.end1[0] = -half_length;
    line.end1[1] = -half_width;
    line.end2[0] = half_length;
    line.end2[1] = -half_width;
    *failure_stage = "rectangle_line_1_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;

    line.end1[0] = half_length;
    line.end1[1] = -half_width;
    line.end2[0] = half_length;
    line.end2[1] = half_width;
    *failure_stage = "rectangle_line_2_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;

    line.end1[0] = half_length;
    line.end1[1] = half_width;
    line.end2[0] = -half_length;
    line.end2[1] = half_width;
    *failure_stage = "rectangle_line_3_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;

    line.end1[0] = -half_length;
    line.end1[1] = half_width;
    line.end2[0] = -half_length;
    line.end2[1] = -half_width;
    *failure_stage = "rectangle_line_4_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;

    *failure_stage = "section_intent_manager_enable";
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
        return status;
    *failure_stage = "section_intent_manager_disable";
    return ProSectionIntentManagerModeSet(section, PRO_B_FALSE);
}

static ProError create_rectangle_extrusion(
    ProMdl model,
    const wchar_t *sketch_plane,
    int sketch_surface_id,
    const wchar_t *orientation_plane,
    const wchar_t *feature_name,
    double length,
    double width,
    double height,
    int direction_side,
    ProFeature *created_feature,
    int *creation_error_count,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement feature_tree = NULL;
    ProElement standard_section = NULL;
    ProElement setup_plane = NULL;
    ProElement standard_depth = NULL;
    ProElement depth_from = NULL;
    ProElement depth_to = NULL;
    ProElement extracted_tree = NULL;
    ProElement sketch_element = NULL;
    ProElement direction_element = NULL;
    ProElement material_side_element = NULL;
    ProElemId sketch_path_ids[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProElemId direction_path_ids[1] = {PRO_E_STD_DIRECTION};
    ProElemId material_path_ids[1] = {PRO_E_STD_MATRLSIDE};
    ProModelitem sketch_plane_item;
    ProModelitem orientation_plane_item;
    ProSurface sketch_surface = NULL;
    ProSrftype sketch_surface_type;
    ProGeomitem sketch_surface_geomitem;
    ProModelitem model_item;
    ProSelection sketch_plane_selection = NULL;
    ProSelection orientation_plane_selection = NULL;
    ProSelection model_selection = NULL;
    ProReference sketch_plane_reference = NULL;
    ProReference orientation_plane_reference = NULL;
    ProSection section = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    int incomplete_feature_created = 0;

#define RUN_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    *failure_stage = "initialize";
    if (sketch_surface_id > 0)
    {
        RUN_STAGE("sketch_surface_lookup",
            ProSurfaceInit((ProSolid)model, sketch_surface_id, &sketch_surface));
        RUN_STAGE("sketch_surface_type",
            ProSurfaceTypeGet(sketch_surface, &sketch_surface_type));
        if (sketch_surface_type != PRO_SRF_PLANE)
        {
            status = PRO_TK_INVALID_TYPE;
            *failure_stage = "sketch_surface_planar_guard";
            goto cleanup;
        }
        RUN_STAGE("sketch_surface_to_geomitem",
            ProSurfaceToGeomitem(
                (ProSolid)model, sketch_surface, &sketch_surface_geomitem));
        memcpy(&sketch_plane_item, &sketch_surface_geomitem,
            sizeof(sketch_plane_item));
    }
    else
    {
        RUN_STAGE("sketch_plane_lookup",
            ProModelitemByNameInit(
                model, PRO_SURFACE, (wchar_t *)sketch_plane,
                &sketch_plane_item));
    }
    RUN_STAGE("orientation_plane_lookup",
        ProModelitemByNameInit(
            model, PRO_SURFACE, (wchar_t *)orientation_plane,
            &orientation_plane_item));
    RUN_STAGE("sketch_plane_selection",
        ProSelectionAlloc(NULL, &sketch_plane_item, &sketch_plane_selection));
    RUN_STAGE("orientation_plane_selection",
        ProSelectionAlloc(
            NULL, &orientation_plane_item, &orientation_plane_selection));
    RUN_STAGE("sketch_plane_reference",
        ProSelectionToReference(sketch_plane_selection, &sketch_plane_reference));
    RUN_STAGE("orientation_plane_reference",
        ProSelectionToReference(
            orientation_plane_selection, &orientation_plane_reference));

    RUN_STAGE("feature_tree_alloc",
        ProElementAlloc(PRO_E_FEATURE_TREE, &feature_tree));
    RUN_STAGE("feature_type",
        add_integer_element(
            feature_tree, PRO_E_FEATURE_TYPE, PRO_FEAT_PROTRUSION));
    RUN_STAGE("feature_form",
        add_integer_element(feature_tree, PRO_E_FEATURE_FORM, PRO_EXTRUDE));
    RUN_STAGE("feature_name",
        add_wstring_element(feature_tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_STAGE("solid_type",
        add_integer_element(
            feature_tree, PRO_E_EXT_SURF_CUT_SOLID_TYPE, PRO_EXT_FEAT_TYPE_SOLID));
    RUN_STAGE("material_add",
        add_integer_element(
            feature_tree, PRO_E_REMOVE_MATERIAL, PRO_EXT_MATERIAL_ADD));
    RUN_STAGE("not_thin",
        add_integer_element(
            feature_tree, PRO_E_FEAT_FORM_IS_THIN, PRO_EXT_FEAT_FORM_NO_THIN));

    RUN_STAGE("standard_section",
        add_compound_element(feature_tree, PRO_E_STD_SECTION, &standard_section));
    RUN_STAGE("setup_plane",
        add_compound_element(
            standard_section, PRO_E_STD_SEC_SETUP_PLANE, &setup_plane));
    RUN_STAGE("section_plane",
        add_reference_element(
            setup_plane, PRO_E_STD_SEC_PLANE, sketch_plane_reference));
    RUN_STAGE("section_view_direction",
        add_integer_element(
            setup_plane, PRO_E_STD_SEC_PLANE_VIEW_DIR, PRO_SEC_VIEW_DIR_SIDE_ONE));
    RUN_STAGE("section_orientation_direction",
        add_integer_element(
            setup_plane, PRO_E_STD_SEC_PLANE_ORIENT_DIR, PRO_SEC_ORIENT_DIR_UP));
    RUN_STAGE("section_orientation_reference",
        add_reference_element(
            setup_plane,
            PRO_E_STD_SEC_PLANE_ORIENT_REF,
            orientation_plane_reference));

    RUN_STAGE("standard_depth",
        add_compound_element(feature_tree, PRO_E_STD_EXT_DEPTH, &standard_depth));
    RUN_STAGE("depth_from",
        add_compound_element(standard_depth, PRO_E_EXT_DEPTH_FROM, &depth_from));
    RUN_STAGE("depth_from_type",
        add_integer_element(
            depth_from, PRO_E_EXT_DEPTH_FROM_TYPE, PRO_EXT_DEPTH_FROM_NONE));
    RUN_STAGE("depth_to",
        add_compound_element(standard_depth, PRO_E_EXT_DEPTH_TO, &depth_to));
    RUN_STAGE("depth_to_type",
        add_integer_element(
            depth_to, PRO_E_EXT_DEPTH_TO_TYPE, PRO_EXT_DEPTH_TO_BLIND));
    RUN_STAGE("depth_to_value",
        add_double_element(depth_to, PRO_E_EXT_DEPTH_TO_VALUE, height));

    RUN_STAGE("model_item", ProMdlToModelitem(model, &model_item));
    RUN_STAGE("model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_STAGE("incomplete_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_INCOMPLETE_FEAT;
    *failure_stage = "incomplete_feature_create";
    status = ProFeatureWithoptionsCreate(
        model_selection,
        feature_tree,
        options,
        PRO_REGEN_NO_FLAGS,
        created_feature,
        &errors);
    *creation_error_count = errors.error_number;
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    incomplete_feature_created = 1;
    ProArrayFree((ProArray *)&options);
    options = NULL;
    if (errors.error_list != NULL)
    {
        ProArrayFree((ProArray *)&errors.error_list);
        errors.error_list = NULL;
        errors.error_number = 0;
    }

    RUN_STAGE("feature_tree_extract",
        ProFeatureElemtreeExtract(
            created_feature,
            NULL,
            PRO_FEAT_EXTRACT_NO_OPTS,
            &extracted_tree));
    RUN_STAGE("sketch_element_get",
        element_by_ids_get(extracted_tree, sketch_path_ids, 2, &sketch_element));
    RUN_STAGE("section_handle_get",
        ProElementSpecialvalueGet(
            sketch_element, NULL, (ProAppData *)&section));
    RUN_STAGE("rectangle_section_build",
        rectangle_section_build(section, length, width, failure_stage));

    status = element_by_ids_get(
        extracted_tree, direction_path_ids, 1, &direction_element);
    if (status == PRO_TK_NO_ERROR)
    {
        RUN_STAGE("direction_set",
            ProElementIntegerSet(
                direction_element,
                direction_side == 1
                    ? PRO_EXT_CR_IN_SIDE_ONE
                    : PRO_EXT_CR_IN_SIDE_TWO));
    }
    else
    {
        RUN_STAGE("direction_add",
            add_integer_element(
                extracted_tree,
                PRO_E_STD_DIRECTION,
                direction_side == 1
                    ? PRO_EXT_CR_IN_SIDE_ONE
                    : PRO_EXT_CR_IN_SIDE_TWO));
    }
    status = element_by_ids_get(
        extracted_tree, material_path_ids, 1, &material_side_element);
    if (status == PRO_TK_NO_ERROR)
    {
        RUN_STAGE("material_side_set",
            ProElementIntegerSet(
                material_side_element, PRO_EXT_MATERIAL_SIDE_ONE));
    }

    RUN_STAGE("redefine_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_DEFINE_MISS_ELEMS;
    *failure_stage = "feature_redefine";
    status = ProFeatureWithoptionsRedefine(
        NULL,
        created_feature,
        extracted_tree,
        options,
        PRO_REGEN_NO_FLAGS,
        &errors);
    *creation_error_count += errors.error_number;

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (extracted_tree != NULL && incomplete_feature_created)
        ProFeatureElemtreeFree(created_feature, extracted_tree);
    if (feature_tree != NULL)
        ProElementFree(&feature_tree);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
    if (orientation_plane_reference != NULL)
        ProReferenceFree(orientation_plane_reference);
    if (sketch_plane_reference != NULL)
        ProReferenceFree(sketch_plane_reference);
    if (orientation_plane_selection != NULL)
        ProSelectionFree(&orientation_plane_selection);
    if (sketch_plane_selection != NULL)
        ProSelectionFree(&sketch_plane_selection);
#undef RUN_STAGE
    return status;
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
    ProMdl current_readback = NULL;
    ProMdlType current_type;
    ProMdlName current_name;
    ProMdlName expected_assembly_name;
    ProMdlName expected_skeleton_name;
    ProMdlName actual_skeleton_name;
    ProMdlName current_readback_name;
    ProName sketch_plane;
    ProName orientation_plane;
    ProName feature_name;
    ProModelitem guard_item;
    ProModelitem named_feature;
    ProFeature created_feature;
    ProFeattype created_feature_type = -1;
    ProFeatStatus created_feature_status = PRO_FEAT_INVALID;
    ProSurface guarded_surface = NULL;
    ProSrftype guarded_surface_type;
    double guarded_surface_area = 0.0;
    ProVector surface_x_direction = {1.0, 0.0, 0.0};
    ProVector surface_y_direction = {0.0, 1.0, 0.0};
    ProVector surface_z_direction = {0.0, 0.0, 1.0};
    Pro3dPnt surface_x_min = {0.0, 0.0, 0.0};
    Pro3dPnt surface_x_max = {0.0, 0.0, 0.0};
    Pro3dPnt surface_y_min = {0.0, 0.0, 0.0};
    Pro3dPnt surface_y_max = {0.0, 0.0, 0.0};
    Pro3dPnt surface_z_min = {0.0, 0.0, 0.0};
    Pro3dPnt surface_z_max = {0.0, 0.0, 0.0};
    ProBoolean is_skeleton = PRO_B_FALSE;
    ProPath working_directory;
    ProPath skeleton_saved_path;
    ProPath assembly_saved_path;
    ProMassProperty mass_property;
    ProMassProperty source_mass_property;
    Pro3dPnt source_outline[2] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
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
    const char *feature_failure_stage = "not_started";
    double length;
    double width;
    double height;
    double expected_volume;
    double volume_tolerance;
    double inset = 0.0;
    double surface_length = 0.0;
    double surface_width = 0.0;
    double surface_center_x = 0.0;
    double surface_center_y = 0.0;
    int connected = 0;
    int feature_created = 0;
    int source_feature_count = 0;
    int final_feature_count = 0;
    int creation_error_count = 0;
    int regenerate_attempts = 0;
    int direction_side;
    int sketch_surface_id = 0;
    int surface_inset_mode = 0;
    int surface_outset_mode = 0;
    int window_id = -1;
    int exit_code = 1;

    if (argc != 11 && argc != 13 && argc != 14)
    {
        fwprintf(stderr,
            L"Usage: creo_skeleton_box_bridge <result.json> "
            L"<expected_assembly> <expected_skeleton> <feature_name> "
            L"<length> <width> <height> <sketch_plane> "
            L"<orientation_plane> <direction_side> "
            L"[<sketch_surface_id> <offset> [INSET|OUTSET]]\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(expected_assembly_name,
        sizeof(expected_assembly_name) / sizeof(expected_assembly_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(expected_skeleton_name,
        sizeof(expected_skeleton_name) / sizeof(expected_skeleton_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(feature_name,
        sizeof(feature_name) / sizeof(feature_name[0]),
        argv[4], _TRUNCATE);
    wcsncpy_s(sketch_plane,
        sizeof(sketch_plane) / sizeof(sketch_plane[0]),
        argv[8], _TRUNCATE);
    wcsncpy_s(orientation_plane,
        sizeof(orientation_plane) / sizeof(orientation_plane[0]),
        argv[9], _TRUNCATE);
    if (!parse_positive_dimension(argv[5], 100000.0, &length) ||
        !parse_positive_dimension(argv[6], 100000.0, &width) ||
        !parse_positive_dimension(argv[7], 100000.0, &height) ||
        (wcscmp(argv[10], L"1") != 0 && wcscmp(argv[10], L"2") != 0))
    {
        exit_code = write_error(out, "dimension_or_direction_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    direction_side = _wtoi(argv[10]);
    if (argc == 13 || argc == 14)
    {
        surface_inset_mode = 1;
        sketch_surface_id = _wtoi(argv[11]);
        if (sketch_surface_id < 1 ||
            !parse_positive_dimension(argv[12], 100000.0, &inset))
        {
            exit_code = write_error(
                out, "surface_id_or_inset_input", PRO_TK_BAD_INPUTS);
            goto done;
        }
        if (argc == 14)
        {
            if (_wcsicmp(argv[13], L"OUTSET") == 0)
                surface_outset_mode = 1;
            else if (_wcsicmp(argv[13], L"INSET") != 0)
            {
                exit_code = write_error(
                    out, "surface_offset_mode_input", PRO_TK_BAD_INPUTS);
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
    status = ProMdlNameGet(current_model, current_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model_name", status);
        goto cleanup;
    }
    if (_wcsicmp(current_name, expected_assembly_name) == 0)
    {
        status = ProMdlTypeGet(current_model, &current_type);
        if (status != PRO_TK_NO_ERROR || current_type != PRO_MDL_ASSEMBLY)
        {
            exit_code = write_error(out, "assembly_type_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
            goto cleanup;
        }
        assembly_model = current_model;
    }
    else if (_wcsicmp(current_name, expected_skeleton_name) == 0)
    {
        status = ProMdlnameInit(
            expected_assembly_name, PRO_MDLFILE_ASSEMBLY, &assembly_model);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "assembly_init", status);
            goto cleanup;
        }
    }
    else
    {
        exit_code = write_error(out, "assembly_or_skeleton_name_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProAsmSkeletonGet((ProAssembly)assembly_model, &skeleton_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_get", status);
        goto cleanup;
    }
    status = ProMdlNameGet(skeleton_model, actual_skeleton_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(actual_skeleton_name, expected_skeleton_name) != 0)
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
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    status = surface_inset_mode
        ? ProSurfaceInit(
            (ProSolid)skeleton_model, sketch_surface_id, &guarded_surface)
        : ProModelitemByNameInit(
            skeleton_model, PRO_SURFACE, sketch_plane, &guard_item);
    if (status == PRO_TK_NO_ERROR && surface_inset_mode)
        status = ProSurfaceTypeGet(guarded_surface, &guarded_surface_type);
    if (status == PRO_TK_NO_ERROR && surface_inset_mode &&
        guarded_surface_type != PRO_SRF_PLANE)
        status = PRO_TK_INVALID_TYPE;
    if (status == PRO_TK_NO_ERROR && surface_inset_mode)
        status = ProSurfaceAreaEval(guarded_surface, &guarded_surface_area);
    if (status == PRO_TK_NO_ERROR && surface_inset_mode)
        status = ProSurfaceExtremesEval(
            guarded_surface, surface_x_direction, surface_x_min, surface_x_max);
    if (status == PRO_TK_NO_ERROR && surface_inset_mode)
        status = ProSurfaceExtremesEval(
            guarded_surface, surface_y_direction, surface_y_min, surface_y_max);
    if (status == PRO_TK_NO_ERROR && surface_inset_mode)
        status = ProSurfaceExtremesEval(
            guarded_surface, surface_z_direction, surface_z_min, surface_z_max);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out,
            surface_inset_mode ? "sketch_surface_guard" : "front_plane_guard",
            status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        skeleton_model, PRO_SURFACE, orientation_plane, &guard_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "top_plane_guard", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        skeleton_model, PRO_FEATURE, feature_name, &guard_item);
    if (status == PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_name_guard", PRO_TK_E_FOUND);
        goto cleanup;
    }
    status = feature_count_get((ProSolid)skeleton_model, &source_feature_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_feature_count", status);
        goto cleanup;
    }
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)skeleton_model,
        NULL,
        PRO_MP_DENS_USE_ALWAYS,
        1.0,
        &source_mass_property);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_mass_properties", status);
        goto cleanup;
    }
    status = ProSolidOutlineCompute(
        (ProSolid)skeleton_model,
        outline_matrix,
        outline_excludes,
        (int)(sizeof(outline_excludes) / sizeof(outline_excludes[0])),
        source_outline);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_outline", status);
        goto cleanup;
    }
    if (surface_inset_mode)
    {
        double expected_surface_area;
        double expected_length;
        double expected_width;
        surface_length = fabs(surface_x_max[0] - surface_x_min[0]);
        surface_width = fabs(surface_y_max[1] - surface_y_min[1]);
        surface_center_x = 0.5 * (surface_x_max[0] + surface_x_min[0]);
        surface_center_y = 0.5 * (surface_y_max[1] + surface_y_min[1]);
        expected_surface_area = surface_length * surface_width;
        expected_length = surface_outset_mode
            ? surface_length + 2.0 * inset
            : surface_length - 2.0 * inset;
        expected_width = surface_outset_mode
            ? surface_width + 2.0 * inset
            : surface_width - 2.0 * inset;
        double area_tolerance = expected_surface_area * 1.0e-7;
        if (expected_length <= 0.0 || expected_width <= 0.0 ||
            fabs(length - expected_length) > 1.0e-6 ||
            fabs(width - expected_width) > 1.0e-6 ||
            fabs(guarded_surface_area - expected_surface_area) > area_tolerance ||
            fabs(surface_center_x) > 1.0e-6 ||
            fabs(surface_center_y) > 1.0e-6 ||
            fabs(surface_z_min[2] - source_outline[1][2]) > 1.0e-6 ||
            fabs(surface_z_max[2] - source_outline[1][2]) > 1.0e-6)
        {
            exit_code = write_error(
                out, "surface_offset_geometry_guard", PRO_TK_BAD_CONTEXT);
            goto cleanup;
        }
    }

    status = create_rectangle_extrusion(
        skeleton_model,
        sketch_plane,
        sketch_surface_id,
        orientation_plane,
        feature_name,
        length,
        width,
        height,
        direction_side,
        &created_feature,
        &creation_error_count,
        &feature_failure_stage);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, feature_failure_stage, status);
        goto cleanup;
    }
    feature_created = 1;
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
    status = ProFeatureTypeGet(&created_feature, &created_feature_type);
    if (status != PRO_TK_NO_ERROR || created_feature_type != PRO_FEAT_PROTRUSION)
    {
        exit_code = write_error(out, "feature_type_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProFeatureStatusGet(&created_feature, &created_feature_status);
    if (status != PRO_TK_NO_ERROR || created_feature_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "feature_status_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        skeleton_model, PRO_FEATURE, feature_name, &named_feature);
    if (status != PRO_TK_NO_ERROR || named_feature.id != created_feature.id)
    {
        exit_code = write_error(out, "feature_name_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = feature_count_get((ProSolid)skeleton_model, &final_feature_count);
    if (status != PRO_TK_NO_ERROR ||
        final_feature_count <= source_feature_count)
    {
        exit_code = write_error(out, "feature_count_readback",
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
    expected_volume = surface_inset_mode
        ? source_mass_property.volume + length * width * height
        : length * width * height;
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
    if (status == PRO_TK_NO_ERROR && surface_inset_mode)
    {
        double tol = 1.0e-6;
        double target_min_x = surface_center_x - 0.5 * length;
        double target_max_x = surface_center_x + 0.5 * length;
        double target_min_y = surface_center_y - 0.5 * width;
        double target_max_y = surface_center_y + 0.5 * width;
        double expected_min_x = source_outline[0][0] < target_min_x
            ? source_outline[0][0] : target_min_x;
        double expected_max_x = source_outline[1][0] > target_max_x
            ? source_outline[1][0] : target_max_x;
        double expected_min_y = source_outline[0][1] < target_min_y
            ? source_outline[0][1] : target_min_y;
        double expected_max_y = source_outline[1][1] > target_max_y
            ? source_outline[1][1] : target_max_y;
        if (fabs(outline[0][0] - expected_min_x) > tol ||
            fabs(outline[1][0] - expected_max_x) > tol ||
            fabs(outline[0][1] - expected_min_y) > tol ||
            fabs(outline[1][1] - expected_max_y) > tol ||
            fabs(outline[0][2] - source_outline[0][2]) > tol ||
            fabs(outline[1][2] - (source_outline[1][2] + height)) > tol)
            status = PRO_TK_GENERAL_ERROR;
    }
    if (status != PRO_TK_NO_ERROR ||
        (!surface_inset_mode &&
         !dimensions_match(outline, length, width, height)))
    {
        exit_code = write_error(out, "outline_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProSolidRegenerate(
        (ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "regenerate_assembly", status);
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
            actual_skeleton_name,
            L"prt",
            skeleton_saved_path,
            sizeof(skeleton_saved_path) / sizeof(skeleton_saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_skeleton", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            working_directory,
            expected_assembly_name,
            L"asm",
            assembly_saved_path,
            sizeof(assembly_saved_path) / sizeof(assembly_saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_assembly", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    status = ProMdlDisplay(skeleton_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "display_skeleton", status);
        goto cleanup;
    }
    status = ProMdlCurrentGet(&current_readback);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(current_readback, current_readback_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(current_readback_name, actual_skeleton_name) != 0)
    {
        status = ProObjectwindowMdlnameCreate(
            actual_skeleton_name, PRO_PART, &window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "create_skeleton_window", status);
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
    if (window_id < 0 && ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
        ProWindowActivate(window_id);
    if (window_id >= 0)
    {
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"assembly\":", out);
    write_wide_json_string(out, expected_assembly_name);
    fputs(",\"skeleton\":", out);
    write_wide_json_string(out, actual_skeleton_name);
    fputs(",\"is_skeleton\":true,\"standard_subtype\":true", out);
    fputs(",\"sketch_plane\":", out);
    write_wide_json_string(out, sketch_plane);
    if (surface_inset_mode)
        fprintf(out,
            ",\"sketch_surface_id\":%d,\"offset_mode\":\"%s\","
            "\"offset\":%.15g,\"sketch_surface_length\":%.15g,"
            "\"sketch_surface_width\":%.15g,"
            "\"sketch_surface_area\":%.17g,\"source_volume\":%.17g",
            sketch_surface_id, surface_outset_mode ? "outset" : "inset",
            inset, surface_length, surface_width, guarded_surface_area,
            source_mass_property.volume);
    fputs(",\"orientation_plane\":", out);
    write_wide_json_string(out, orientation_plane);
    fputs(",\"feature_name\":", out);
    write_wide_json_string(out, feature_name);
    fprintf(out,
        ",\"length\":%.15g,\"width\":%.15g,\"height\":%.15g,"
        "\"direction_side\":%d,"
        "\"feature_id\":%d,\"feature_type_code\":%d,"
        "\"feature_status\":%d,\"source_feature_count\":%d,"
        "\"final_feature_count\":%d,\"volume\":%.17g,"
        "\"outline\":{\"min\":[%.15g,%.15g,%.15g],"
        "\"max\":[%.15g,%.15g,%.15g],"
        "\"size\":[%.15g,%.15g,%.15g]},"
        "\"creation_error_count\":%d,\"regenerate_status\":%d,"
        "\"skeleton_saved_file\":",
        length,
        width,
        height,
        direction_side,
        created_feature.id,
        created_feature_type,
        created_feature_status,
        source_feature_count,
        final_feature_count,
        mass_property.volume,
        outline[0][0], outline[0][1], outline[0][2],
        outline[1][0], outline[1][1], outline[1][2],
        outline[1][0] - outline[0][0],
        outline[1][1] - outline[0][1],
        outline[1][2] - outline[0][2],
        creation_error_count,
        regenerate_status);
    write_wide_json_string(out, skeleton_saved_path);
    fputs(",\"assembly_saved_file\":", out);
    write_wide_json_string(out, assembly_saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0 && feature_created && skeleton_model != NULL)
    {
        int feature_id = created_feature.id;
        ProFeatureDelete((ProSolid)skeleton_model, &feature_id, 1, NULL, 0);
        ProSolidRegenerate((ProSolid)skeleton_model, PRO_REGEN_NO_FLAGS);
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
