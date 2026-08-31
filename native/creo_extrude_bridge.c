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
#include <ProFeatForm.h>
#include <ProModelitem.h>
#include <ProSelection.h>
#include <ProReference.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProElemId.h>
#include <ProExtrude.h>
#include <ProRevolve.h>
#include <ProSweep.h>
#include <ProRound.h>
#include <ProChamfer.h>
#include <ProShell.h>
#include <ProSolidBody.h>
#include <ProBodyOpts.h>
#include <ProDraft.h>
#include <ProMirror.h>
#include <ProDirection.h>
#include <ProSrfcollection.h>
#include <ProCrvcollection.h>
#include <ProEdge.h>
#include <ProGeomitem.h>
#include <ProSurface.h>
#include <ProContour.h>
#include <ProStdSection.h>
#include <ProSection.h>
#include <ProArray.h>
#include <ProUtil.h>

static void write_utf8_json_string(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    fputc('"', out);
    while (*p)
    {
        switch (*p)
        {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20)
                fprintf(out, "\\u%04x", (unsigned int)*p);
            else
                fputc(*p, out);
        }
        ++p;
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

static int write_error(FILE *out, const char *stage, ProError error_code)
{
    fputs("{\"ok\":false,\"safe_copy_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

static int output_model_already_exists(
    const wchar_t *directory,
    const wchar_t *copy_name)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;

    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.*",
        directory,
        copy_name);
    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(find_handle);
    return 1;
}

static int find_latest_saved_model(
    const wchar_t *directory,
    const wchar_t *copy_name,
    wchar_t *saved_path,
    size_t saved_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    int found = 0;
    int best_version = -1;

    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.*",
        directory,
        copy_name);
    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        wchar_t *last_dot = wcsrchr(find_data.cFileName, L'.');
        int version = last_dot == NULL ? 0 : _wtoi(last_dot + 1);
        if (!found || version > best_version)
        {
            _snwprintf_s(saved_path,
                saved_path_count,
                _TRUNCATE,
                L"%ls\\%ls",
                directory,
                find_data.cFileName);
            best_version = version;
            found = 1;
        }
    } while (FindNextFileW(find_handle, &find_data));
    FindClose(find_handle);
    return found;
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

static ProError add_references_element(
    ProElement parent,
    ProElemId element_id,
    ProReference *references)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementReferencesSet(element, references);
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

static ProError add_collection_element(
    ProElement parent,
    ProElemId element_id,
    ProCollection collection)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementCollectionSet(element, collection);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
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
    double width,
    double height,
    const char **failure_stage)
{
    Pro2dLinedef line;
    ProError status;
    int entity_id;
    double half_width = width / 2.0;
    double half_height = height / 2.0;

    line.type = PRO_2D_LINE;
    line.end1[0] = -half_width;
    line.end1[1] = -half_height;
    line.end2[0] = half_width;
    line.end2[1] = -half_height;
    *failure_stage = "rectangle_line_1_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    line.end1[0] = half_width;
    line.end1[1] = -half_height;
    line.end2[0] = half_width;
    line.end2[1] = half_height;
    *failure_stage = "rectangle_line_2_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    line.end1[0] = half_width;
    line.end1[1] = half_height;
    line.end2[0] = -half_width;
    line.end2[1] = half_height;
    *failure_stage = "rectangle_line_3_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    line.end1[0] = -half_width;
    line.end1[1] = half_height;
    line.end2[0] = -half_width;
    line.end2[1] = -half_height;
    *failure_stage = "rectangle_line_4_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

    *failure_stage = "section_intent_manager_enable";
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    *failure_stage = "section_intent_manager_disable";
    status = ProSectionIntentManagerModeSet(section, PRO_B_FALSE);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

cleanup:
    return status;
}

static ProError circle_section_build(
    ProSection section,
    double diameter,
    const char **failure_stage)
{
    Pro2dCircledef circle;
    ProError status;
    int entity_id;

    circle.type = PRO_2D_CIRCLE;
    circle.center[0] = 0.0;
    circle.center[1] = 0.0;
    circle.radius = diameter / 2.0;
    *failure_stage = "circle_entity_add";
    status = ProSectionEntityAdd(
        section, (Pro2dEntdef *)&circle, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;
    *failure_stage = "circle_intent_manager_enable";
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
        return status;
    *failure_stage = "circle_intent_manager_disable";
    return ProSectionIntentManagerModeSet(section, PRO_B_FALSE);
}

static ProError revolve_rectangle_section_build(
    ProSection section,
    double axial_width,
    double inner_radius,
    double radial_thickness,
    const char **failure_stage)
{
    Pro2dLinedef line;
    ProError status;
    int entity_id;
    double half_width = axial_width / 2.0;
    double outer_radius = inner_radius + radial_thickness;
    double axis_span = half_width + outer_radius + 10.0;

    line.type = PRO_2D_CENTER_LINE;
    line.end1[0] = -axis_span;
    line.end1[1] = 0.0;
    line.end2[0] = axis_span;
    line.end2[1] = 0.0;
    *failure_stage = "revolve_axis_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    *failure_stage = "revolve_axis_set";
    status = ProSectionEntityConstructionSet(section, entity_id, PRO_B_FALSE);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

    line.type = PRO_2D_LINE;
    line.end1[0] = -half_width;
    line.end1[1] = inner_radius;
    line.end2[0] = half_width;
    line.end2[1] = inner_radius;
    *failure_stage = "revolve_rectangle_line_1_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    line.end1[0] = half_width;
    line.end1[1] = inner_radius;
    line.end2[0] = half_width;
    line.end2[1] = outer_radius;
    *failure_stage = "revolve_rectangle_line_2_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    line.end1[0] = half_width;
    line.end1[1] = outer_radius;
    line.end2[0] = -half_width;
    line.end2[1] = outer_radius;
    *failure_stage = "revolve_rectangle_line_3_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    line.end1[0] = -half_width;
    line.end1[1] = outer_radius;
    line.end2[0] = -half_width;
    line.end2[1] = inner_radius;
    *failure_stage = "revolve_rectangle_line_4_add";
    status = ProSectionEntityAdd(section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

    *failure_stage = "revolve_intent_manager_enable";
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    *failure_stage = "revolve_intent_manager_disable";
    status = ProSectionIntentManagerModeSet(section, PRO_B_FALSE);

cleanup:
    return status;
}

static ProError create_rectangle_revolve(
    ProMdl model,
    const wchar_t *sketch_plane,
    const wchar_t *orientation_plane,
    const wchar_t *feature_name,
    double axial_width,
    double inner_radius,
    double radial_thickness,
    double angle_degrees,
    int direction_side,
    int remove_material,
    ProFeature *created_feature,
    int *creation_error_count,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement feature_tree = NULL;
    ProElement standard_section = NULL;
    ProElement setup_plane = NULL;
    ProElement revolve_angle = NULL;
    ProElement angle_from = NULL;
    ProElement angle_to = NULL;
    ProElement extracted_tree = NULL;
    ProElement sketch_element = NULL;
    ProElement direction_element = NULL;
    ProElement material_side_element = NULL;
    ProElemId sketch_path_ids[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProElemId direction_path_ids[1] = {PRO_E_STD_DIRECTION};
    ProElemId material_path_ids[1] = {PRO_E_STD_MATRLSIDE};
    ProModelitem sketch_plane_item;
    ProModelitem orientation_plane_item;
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

#define RUN_REV_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    *failure_stage = "initialize";
    RUN_REV_STAGE("sketch_plane_lookup",
        ProModelitemByNameInit(
            model, PRO_SURFACE, (wchar_t *)sketch_plane, &sketch_plane_item));
    RUN_REV_STAGE("orientation_plane_lookup",
        ProModelitemByNameInit(
            model, PRO_SURFACE, (wchar_t *)orientation_plane, &orientation_plane_item));
    RUN_REV_STAGE("sketch_plane_selection",
        ProSelectionAlloc(NULL, &sketch_plane_item, &sketch_plane_selection));
    RUN_REV_STAGE("orientation_plane_selection",
        ProSelectionAlloc(NULL, &orientation_plane_item, &orientation_plane_selection));
    RUN_REV_STAGE("sketch_plane_reference",
        ProSelectionToReference(sketch_plane_selection, &sketch_plane_reference));
    RUN_REV_STAGE("orientation_plane_reference",
        ProSelectionToReference(
            orientation_plane_selection, &orientation_plane_reference));

    RUN_REV_STAGE("feature_tree_alloc",
        ProElementAlloc(PRO_E_FEATURE_TREE, &feature_tree));
    RUN_REV_STAGE("feature_type",
        add_integer_element(
            feature_tree,
            PRO_E_FEATURE_TYPE,
            remove_material ? PRO_FEAT_CUT : PRO_FEAT_PROTRUSION));
    RUN_REV_STAGE("feature_form",
        add_integer_element(feature_tree, PRO_E_FEATURE_FORM, PRO_REVOLVE));
    RUN_REV_STAGE("feature_name",
        add_wstring_element(feature_tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_REV_STAGE("solid_type",
        add_integer_element(
            feature_tree,
            PRO_E_EXT_SURF_CUT_SOLID_TYPE,
            PRO_REV_FEAT_TYPE_SOLID));
    RUN_REV_STAGE(remove_material ? "material_remove" : "material_add",
        add_integer_element(
            feature_tree,
            PRO_E_REMOVE_MATERIAL,
            remove_material ? PRO_REV_MATERIAL_REMOVE : PRO_REV_MATERIAL_ADD));
    RUN_REV_STAGE("not_thin",
        add_integer_element(
            feature_tree, PRO_E_FEAT_FORM_IS_THIN, PRO_REV_FEAT_FORM_NO_THIN));

    RUN_REV_STAGE("revolve_angle",
        add_compound_element(feature_tree, PRO_E_REV_ANGLE, &revolve_angle));
    RUN_REV_STAGE("angle_from",
        add_compound_element(revolve_angle, PRO_E_REV_ANGLE_FROM, &angle_from));
    RUN_REV_STAGE("angle_from_type",
        add_integer_element(
            angle_from, PRO_E_REV_ANGLE_FROM_TYPE, PRO_REV_ANG_FROM_NONE));
    RUN_REV_STAGE("angle_to",
        add_compound_element(revolve_angle, PRO_E_REV_ANGLE_TO, &angle_to));
    RUN_REV_STAGE("angle_to_type",
        add_integer_element(
            angle_to, PRO_E_REV_ANGLE_TO_TYPE, PRO_REV_ANG_TO_ANGLE));
    RUN_REV_STAGE("angle_to_value",
        add_double_element(angle_to, PRO_E_REV_ANGLE_TO_VAL, angle_degrees));

    RUN_REV_STAGE("standard_section",
        add_compound_element(feature_tree, PRO_E_STD_SECTION, &standard_section));
    RUN_REV_STAGE("setup_plane",
        add_compound_element(
            standard_section, PRO_E_STD_SEC_SETUP_PLANE, &setup_plane));
    RUN_REV_STAGE("section_plane",
        add_reference_element(
            setup_plane, PRO_E_STD_SEC_PLANE, sketch_plane_reference));
    RUN_REV_STAGE("section_view_direction",
        add_integer_element(
            setup_plane, PRO_E_STD_SEC_PLANE_VIEW_DIR, PRO_SEC_VIEW_DIR_SIDE_ONE));
    RUN_REV_STAGE("section_orientation_direction",
        add_integer_element(
            setup_plane, PRO_E_STD_SEC_PLANE_ORIENT_DIR, PRO_SEC_ORIENT_DIR_UP));
    RUN_REV_STAGE("section_orientation_reference",
        add_reference_element(
            setup_plane,
            PRO_E_STD_SEC_PLANE_ORIENT_REF,
            orientation_plane_reference));

    RUN_REV_STAGE("model_item", ProMdlToModelitem(model, &model_item));
    RUN_REV_STAGE("model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_REV_STAGE("incomplete_options_alloc",
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

    RUN_REV_STAGE("feature_tree_extract",
        ProFeatureElemtreeExtract(
            created_feature,
            NULL,
            PRO_FEAT_EXTRACT_NO_OPTS,
            &extracted_tree));
    RUN_REV_STAGE("sketch_element_get",
        element_by_ids_get(
            extracted_tree, sketch_path_ids, 2, &sketch_element));
    RUN_REV_STAGE("section_handle_get",
        ProElementSpecialvalueGet(
            sketch_element, NULL, (ProAppData *)&section));
    RUN_REV_STAGE("revolve_rectangle_section_build",
        revolve_rectangle_section_build(
            section,
            axial_width,
            inner_radius,
            radial_thickness,
            failure_stage));

    status = element_by_ids_get(
        extracted_tree, direction_path_ids, 1, &direction_element);
    if (status == PRO_TK_NO_ERROR)
    {
        RUN_REV_STAGE("direction_set",
            ProElementIntegerSet(
                direction_element,
                direction_side == 1
                    ? PRO_REV_CR_IN_SIDE_ONE
                    : PRO_REV_CR_IN_SIDE_TWO));
    }
    else
    {
        RUN_REV_STAGE("direction_add",
            add_integer_element(
                extracted_tree,
                PRO_E_STD_DIRECTION,
                direction_side == 1
                    ? PRO_REV_CR_IN_SIDE_ONE
                    : PRO_REV_CR_IN_SIDE_TWO));
    }
    status = element_by_ids_get(
        extracted_tree, material_path_ids, 1, &material_side_element);
    if (status == PRO_TK_NO_ERROR)
    {
        RUN_REV_STAGE("material_side_set",
            ProElementIntegerSet(
                material_side_element,
                remove_material
                    ? PRO_REV_MATERIAL_SIDE_TWO
                    : PRO_REV_MATERIAL_SIDE_ONE));
    }

    RUN_REV_STAGE("redefine_options_alloc",
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
#undef RUN_REV_STAGE
    return status;
}

static ProError circular_sweep_section_build(
    ProSection section,
    double radius,
    const char **failure_stage)
{
    Pro2dCircledef circle;
    ProError status;
    int entity_id;

    circle.type = PRO_2D_CIRCLE;
    circle.center[0] = 0.0;
    circle.center[1] = 0.0;
    circle.radius = radius;
    *failure_stage = "sweep_circle_add";
    status = ProSectionEntityAdd(
        section, (Pro2dEntdef *)&circle, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;
    *failure_stage = "sweep_intent_manager_enable";
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
        return status;
    *failure_stage = "sweep_intent_manager_disable";
    return ProSectionIntentManagerModeSet(section, PRO_B_FALSE);
}

typedef struct SweepEdgeSearch
{
    ProSurface current_surface;
    ProEdge best_edge;
    int best_edge_id;
    ProEnttype best_edge_type;
    double best_edge_length;
    int found;
} SweepEdgeSearch;

static ProError sweep_edge_visit_action(
    ProEdge edge,
    ProError filter_status,
    ProAppData app_data)
{
    SweepEdgeSearch *search = (SweepEdgeSearch *)app_data;
    ProError status;
    ProEnttype edge_type;
    double edge_length;
    int edge_id;
    (void)filter_status;

    status = ProEdgeTypeGet(edge, &edge_type);
    if (status != PRO_TK_NO_ERROR ||
        (edge_type != PRO_ENT_CIRCLE && edge_type != PRO_ENT_ARC))
        return PRO_TK_NO_ERROR;
    status = ProEdgeLengthEval(edge, &edge_length);
    if (status != PRO_TK_NO_ERROR || !_finite(edge_length) ||
        edge_length <= search->best_edge_length)
        return PRO_TK_NO_ERROR;
    status = ProEdgeIdGet(edge, &edge_id);
    if (status != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;
    search->best_edge = edge;
    search->best_edge_id = edge_id;
    search->best_edge_type = edge_type;
    search->best_edge_length = edge_length;
    search->found = 1;
    return PRO_TK_NO_ERROR;
}

static ProError sweep_contour_visit_action(
    ProContour contour,
    ProError filter_status,
    ProAppData app_data)
{
    SweepEdgeSearch *search = (SweepEdgeSearch *)app_data;
    (void)filter_status;
    return ProContourEdgeVisit(
        search->current_surface,
        contour,
        sweep_edge_visit_action,
        NULL,
        app_data);
}

static ProError sweep_surface_visit_action(
    ProSurface surface,
    ProError filter_status,
    ProAppData app_data)
{
    SweepEdgeSearch *search = (SweepEdgeSearch *)app_data;
    (void)filter_status;
    search->current_surface = surface;
    return ProSurfaceContourVisit(
        surface,
        sweep_contour_visit_action,
        NULL,
        app_data);
}

static ProError sweep_edge_find(
    ProSolid solid,
    int requested_edge_id,
    ProEdge *selected_edge,
    int *selected_edge_id,
    ProEnttype *selected_edge_type,
    double *selected_edge_length)
{
    ProError status;
    ProEdge edge;
    ProEnttype edge_type;
    double edge_length;
    SweepEdgeSearch search;

    if (requested_edge_id > 0)
    {
        status = ProEdgeInit(solid, requested_edge_id, &edge);
        if (status != PRO_TK_NO_ERROR)
            return status;
        status = ProEdgeTypeGet(edge, &edge_type);
        if (status != PRO_TK_NO_ERROR)
            return status;
        status = ProEdgeLengthEval(edge, &edge_length);
        if (status != PRO_TK_NO_ERROR || !_finite(edge_length) || edge_length <= 0.0)
            return status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status;
        *selected_edge = edge;
        *selected_edge_id = requested_edge_id;
        *selected_edge_type = edge_type;
        *selected_edge_length = edge_length;
        return PRO_TK_NO_ERROR;
    }

    memset(&search, 0, sizeof(search));
    search.best_edge_type = PRO_ENT_NONE;
    status = ProSolidSurfaceVisit(
        solid,
        sweep_surface_visit_action,
        NULL,
        (ProAppData)&search);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (!search.found)
        return PRO_TK_E_NOT_FOUND;
    *selected_edge = search.best_edge;
    *selected_edge_id = search.best_edge_id;
    *selected_edge_type = search.best_edge_type;
    *selected_edge_length = search.best_edge_length;
    return PRO_TK_NO_ERROR;
}

static ProError create_circular_sweep(
    ProMdl model,
    const wchar_t *feature_name,
    int requested_edge_id,
    double section_radius,
    int remove_material,
    ProFeature *created_feature,
    int *creation_error_count,
    int *selected_edge_id,
    ProEnttype *selected_edge_type,
    double *selected_edge_length,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement feature_tree = NULL;
    ProElement sweep_frame = NULL;
    ProElement optional_trajectories = NULL;
    ProElement origin_trajectory = NULL;
    ProElement selected_trajectory = NULL;
    ProElement frame_setup = NULL;
    ProElement sweep_attributes = NULL;
    ProElement extracted_tree = NULL;
    ProElement sketch_element = NULL;
    ProElement material_side_element = NULL;
    ProElemId sketch_path_ids[3] = {
        PRO_E_SWEEP_PROF_COMP, PRO_E_SWEEP_SECTION, PRO_E_SKETCHER};
    ProElemId material_path_ids[1] = {PRO_E_STD_MATRLSIDE};
    ProModelitem model_item;
    ProGeomitem edge_geomitem;
    ProEdge selected_edge;
    ProSelection model_selection = NULL;
    ProSelection edge_selection = NULL;
    ProReference edge_reference = NULL;
    ProCollection trajectory_collection = NULL;
    ProCrvcollinstr trajectory_instruction = NULL;
    ProSection section = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    int incomplete_feature_created = 0;

    #define WRITE_SWP_STAGE(label) \
        do { \
            char stage_log_path[PRO_PATH_SIZE * 2]; \
            DWORD stage_log_length = GetEnvironmentVariableA( \
                "CREO_SWEEP_STAGE_LOG", \
                stage_log_path, \
                (DWORD)(sizeof(stage_log_path) / sizeof(stage_log_path[0]))); \
            if (stage_log_length > 0 && stage_log_length < sizeof(stage_log_path)) \
            { \
                FILE *stage_log_file = NULL; \
                if (fopen_s(&stage_log_file, stage_log_path, "wb") == 0 && \
                    stage_log_file != NULL) \
                { \
                    fprintf(stage_log_file, "%s\n", label); \
                    fclose(stage_log_file); \
                } \
            } \
        } while (0)

#define RUN_SWP_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        WRITE_SWP_STAGE(label); \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    RUN_SWP_STAGE("trajectory_edge_find",
        sweep_edge_find(
            (ProSolid)model,
            requested_edge_id,
            &selected_edge,
            selected_edge_id,
            selected_edge_type,
            selected_edge_length));
    RUN_SWP_STAGE("trajectory_edge_to_geomitem",
        ProEdgeToGeomitem((ProSolid)model, selected_edge, &edge_geomitem));
    RUN_SWP_STAGE("trajectory_edge_selection",
        ProSelectionAlloc(
            NULL, (ProModelitem *)&edge_geomitem, &edge_selection));
    RUN_SWP_STAGE("trajectory_edge_reference",
        ProSelectionToReference(edge_selection, &edge_reference));
    RUN_SWP_STAGE("trajectory_collection_alloc",
        ProCrvcollectionAlloc(&trajectory_collection));
    RUN_SWP_STAGE("trajectory_instruction_alloc",
        ProCrvcollinstrAlloc(
            PRO_CURVCOLL_ADD_ONE_INSTR, &trajectory_instruction));
    RUN_SWP_STAGE("trajectory_reference_add",
        ProCrvcollinstrReferenceAdd(
            trajectory_instruction, edge_reference));
    RUN_SWP_STAGE("trajectory_instruction_add",
        ProCrvcollectionInstructionAdd(
            trajectory_collection, trajectory_instruction));

    RUN_SWP_STAGE("feature_tree_alloc",
        ProElementAlloc(PRO_E_FEATURE_TREE, &feature_tree));
    RUN_SWP_STAGE("feature_form",
        add_integer_element(feature_tree, PRO_E_FEATURE_FORM, PRO_SWEEP));
    RUN_SWP_STAGE("sweep_type",
        add_integer_element(
            feature_tree, PRO_E_SWEEP_TYPE, PRO_SWEEP_TYPE_MULTI_TRAJ));
    RUN_SWP_STAGE("sweep_frame",
        add_compound_element(
            feature_tree, PRO_E_SWEEP_FRAME_COMP, &sweep_frame));
    RUN_SWP_STAGE("optional_trajectories",
        add_compound_element(
            sweep_frame, PRO_E_FRM_OPT_TRAJ, &optional_trajectories));
    RUN_SWP_STAGE("origin_trajectory",
        add_compound_element(
            optional_trajectories, PRO_E_OPT_TRAJ, &origin_trajectory));
    RUN_SWP_STAGE("trajectory_method",
        add_integer_element(
            origin_trajectory, PRO_E_STD_SEC_METHOD, PRO_SEC_SELECT));
    RUN_SWP_STAGE("trajectory_select_holder",
        add_compound_element(
            origin_trajectory, PRO_E_STD_SEC_SELECT, &selected_trajectory));
    RUN_SWP_STAGE("trajectory_collection_set",
        add_collection_element(
            selected_trajectory,
            PRO_E_STD_CURVE_COLLECTION_APPL,
            trajectory_collection));
    RUN_SWP_STAGE("frame_setup",
        add_compound_element(sweep_frame, PRO_E_FRAME_SETUP, &frame_setup));
    RUN_SWP_STAGE("frame_normal",
        add_integer_element(
            frame_setup, PRO_E_FRM_NORMAL, PRO_FRAME_NORM_ORIGIN));
    RUN_SWP_STAGE("frame_orient",
        add_integer_element(
            frame_setup, PRO_E_FRM_ORIENT, PRO_FRAME_MIN));
    RUN_SWP_STAGE("frame_start_x",
        add_integer_element(
            frame_setup, PRO_E_FRM_USER_X, PRO_FRAME_DEFAULT_START_X));
    RUN_SWP_STAGE("feature_name",
        add_wstring_element(feature_tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_SWP_STAGE("solid_type",
        add_integer_element(
            feature_tree,
            PRO_E_EXT_SURF_CUT_SOLID_TYPE,
            PRO_SWEEP_FEAT_TYPE_SOLID));
    RUN_SWP_STAGE(remove_material ? "material_remove" : "material_add",
        add_integer_element(
            feature_tree,
            PRO_E_REMOVE_MATERIAL,
            remove_material
                ? PRO_SWEEP_MATERIAL_REMOVE
                : PRO_SWEEP_MATERIAL_ADD));
    RUN_SWP_STAGE("not_thin",
        add_integer_element(
            feature_tree,
            PRO_E_FEAT_FORM_IS_THIN,
            PRO_SWEEP_FEAT_FORM_NO_THIN));
    RUN_SWP_STAGE("sweep_attributes",
        add_compound_element(
            feature_tree, PRO_E_SWP_ATTR, &sweep_attributes));

    RUN_SWP_STAGE("model_item", ProMdlToModelitem(model, &model_item));
    RUN_SWP_STAGE("model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_SWP_STAGE("incomplete_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_INCOMPLETE_FEAT;
    *failure_stage = "incomplete_feature_create";
    WRITE_SWP_STAGE("incomplete_feature_create");
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

    RUN_SWP_STAGE("feature_tree_extract",
        ProFeatureElemtreeExtract(
            created_feature,
            NULL,
            PRO_FEAT_EXTRACT_NO_OPTS,
            &extracted_tree));
    RUN_SWP_STAGE("sketch_element_get",
        element_by_ids_get(
            extracted_tree, sketch_path_ids, 3, &sketch_element));
    RUN_SWP_STAGE("section_handle_get",
        ProElementSpecialvalueGet(
            sketch_element, NULL, (ProAppData *)&section));
    RUN_SWP_STAGE("circular_sweep_section_build",
        circular_sweep_section_build(
            section, section_radius, failure_stage));
    RUN_SWP_STAGE("section_handle_set",
        ProElementSpecialvalueSet(sketch_element, (ProAppData)section));

    if (remove_material)
    {
        status = element_by_ids_get(
            extracted_tree,
            material_path_ids,
            1,
            &material_side_element);
        if (status == PRO_TK_NO_ERROR)
        {
            RUN_SWP_STAGE("material_side_set",
                ProElementIntegerSet(
                    material_side_element, PRO_SWEEP_MATERIAL_SIDE_TWO));
        }
        else
        {
            RUN_SWP_STAGE("material_side_add",
                add_integer_element(
                    extracted_tree,
                    PRO_E_STD_MATRLSIDE,
                    PRO_SWEEP_MATERIAL_SIDE_TWO));
        }
    }

    RUN_SWP_STAGE("redefine_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    /*
     * Creo's modern sweep workflow keeps the incomplete-feature option for
     * the redefine that supplies PRO_E_SKETCHER.  This is the sequence used
     * by the Creo 10 UgCreoSweepCreate sample; NO_OPTS rejects the otherwise
     * valid extracted sweep tree with PRO_TK_GENERAL_ERROR.
     */
    options[0] = PRO_FEAT_CR_INCOMPLETE_FEAT;
    *failure_stage = "feature_redefine";
    WRITE_SWP_STAGE("feature_redefine");
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
    if (trajectory_collection != NULL)
        ProCollectionFree(&trajectory_collection);
    if (edge_reference != NULL)
        ProReferenceFree(edge_reference);
    if (edge_selection != NULL)
        ProSelectionFree(&edge_selection);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
#undef RUN_SWP_STAGE
#undef WRITE_SWP_STAGE
    return status;
}

typedef struct RoundEdgeSearch
{
    ProSurface current_surface;
    ProEdge best_edge;
    int best_edge_id;
    ProEnttype best_edge_type;
    double best_edge_length;
    int found;
} RoundEdgeSearch;

static ProError round_edge_visit_action(
    ProEdge edge,
    ProError filter_status,
    ProAppData app_data)
{
    RoundEdgeSearch *search = (RoundEdgeSearch *)app_data;
    ProError status;
    ProEnttype edge_type;
    double edge_length;
    int edge_id;
    (void)filter_status;

    status = ProEdgeTypeGet(edge, &edge_type);
    if (status != PRO_TK_NO_ERROR ||
        (edge_type != PRO_ENT_LINE &&
         edge_type != PRO_ENT_CIRCLE &&
         edge_type != PRO_ENT_ARC))
        return PRO_TK_NO_ERROR;
    status = ProEdgeLengthEval(edge, &edge_length);
    if (status != PRO_TK_NO_ERROR || !_finite(edge_length) ||
        edge_length <= search->best_edge_length)
        return PRO_TK_NO_ERROR;
    status = ProEdgeIdGet(edge, &edge_id);
    if (status != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;
    search->best_edge = edge;
    search->best_edge_id = edge_id;
    search->best_edge_type = edge_type;
    search->best_edge_length = edge_length;
    search->found = 1;
    return PRO_TK_NO_ERROR;
}

static ProError round_contour_visit_action(
    ProContour contour,
    ProError filter_status,
    ProAppData app_data)
{
    RoundEdgeSearch *search = (RoundEdgeSearch *)app_data;
    (void)filter_status;
    return ProContourEdgeVisit(
        search->current_surface,
        contour,
        round_edge_visit_action,
        NULL,
        app_data);
}

static ProError round_surface_visit_action(
    ProSurface surface,
    ProError filter_status,
    ProAppData app_data)
{
    RoundEdgeSearch *search = (RoundEdgeSearch *)app_data;
    (void)filter_status;
    search->current_surface = surface;
    return ProSurfaceContourVisit(
        surface,
        round_contour_visit_action,
        NULL,
        app_data);
}

static ProError round_edge_find(
    ProSolid solid,
    int requested_edge_id,
    ProEdge *selected_edge,
    int *selected_edge_id,
    ProEnttype *selected_edge_type,
    double *selected_edge_length)
{
    ProError status;
    ProEdge edge;
    ProEnttype edge_type;
    double edge_length;
    RoundEdgeSearch search;

    if (requested_edge_id > 0)
    {
        status = ProEdgeInit(solid, requested_edge_id, &edge);
        if (status != PRO_TK_NO_ERROR)
            return status;
        status = ProEdgeTypeGet(edge, &edge_type);
        if (status != PRO_TK_NO_ERROR)
            return status;
        status = ProEdgeLengthEval(edge, &edge_length);
        if (status != PRO_TK_NO_ERROR || !_finite(edge_length) ||
            edge_length <= 0.0)
            return status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status;
        *selected_edge = edge;
        *selected_edge_id = requested_edge_id;
        *selected_edge_type = edge_type;
        *selected_edge_length = edge_length;
        return PRO_TK_NO_ERROR;
    }

    memset(&search, 0, sizeof(search));
    search.best_edge_type = PRO_ENT_NONE;
    status = ProSolidSurfaceVisit(
        solid,
        round_surface_visit_action,
        NULL,
        (ProAppData)&search);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (!search.found)
        return PRO_TK_E_NOT_FOUND;
    *selected_edge = search.best_edge;
    *selected_edge_id = search.best_edge_id;
    *selected_edge_type = search.best_edge_type;
    *selected_edge_length = search.best_edge_length;
    return PRO_TK_NO_ERROR;
}

static ProError create_constant_round(
    ProMdl model,
    const wchar_t *feature_name,
    int requested_edge_id,
    double radius,
    ProFeature *created_feature,
    int *creation_error_count,
    int *selected_edge_id,
    ProEnttype *selected_edge_type,
    double *selected_edge_length,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement tree = NULL;
    ProElement sets = NULL;
    ProElement set = NULL;
    ProElement conic = NULL;
    ProElement references = NULL;
    ProElement spine = NULL;
    ProElement radii = NULL;
    ProElement radius_element = NULL;
    ProElement leg1 = NULL;
    ProElement transitions = NULL;
    ProModelitem model_item;
    ProGeomitem edge_geomitem;
    ProEdge selected_edge;
    ProSelection model_selection = NULL;
    ProSelection edge_selection = NULL;
    ProReference edge_reference = NULL;
    ProCollection collection = NULL;
    ProCrvcollinstr instruction = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};

#define RUN_RND_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    RUN_RND_STAGE("round_edge_find",
        round_edge_find(
            (ProSolid)model,
            requested_edge_id,
            &selected_edge,
            selected_edge_id,
            selected_edge_type,
            selected_edge_length));
    RUN_RND_STAGE("round_edge_to_geomitem",
        ProEdgeToGeomitem((ProSolid)model, selected_edge, &edge_geomitem));
    RUN_RND_STAGE("round_edge_selection",
        ProSelectionAlloc(NULL, (ProModelitem *)&edge_geomitem, &edge_selection));
    RUN_RND_STAGE("round_edge_reference",
        ProSelectionToReference(edge_selection, &edge_reference));
    RUN_RND_STAGE("round_collection_alloc", ProCrvcollectionAlloc(&collection));
    RUN_RND_STAGE("round_instruction_alloc",
        ProCrvcollinstrAlloc(PRO_CURVCOLL_ADD_ONE_INSTR, &instruction));
    RUN_RND_STAGE("round_reference_add",
        ProCrvcollinstrReferenceAdd(instruction, edge_reference));
    RUN_RND_STAGE("round_instruction_add",
        ProCrvcollectionInstructionAdd(collection, instruction));

    RUN_RND_STAGE("round_tree_alloc", ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    RUN_RND_STAGE("round_feature_type",
        add_integer_element(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_ROUND));
    RUN_RND_STAGE("round_feature_name",
        add_wstring_element(tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_RND_STAGE("round_sets",
        add_compound_element(tree, PRO_E_RNDCH_SETS, &sets));
    RUN_RND_STAGE("round_set",
        add_compound_element(sets, PRO_E_RNDCH_SET, &set));
    RUN_RND_STAGE("round_shape",
        add_integer_element(set, PRO_E_RNDCH_SHAPE_OPTIONS, PRO_ROUND_TYPE_CONSTANT));
    RUN_RND_STAGE("round_variable_radius",
        add_integer_element(set, PRO_E_RNDCH_VARIABLE_RADIUS, 0));
    RUN_RND_STAGE("round_conic",
        add_compound_element(set, PRO_E_RNDCH_COMPOUND_CONIC, &conic));
    RUN_RND_STAGE("round_conic_type",
        add_integer_element(conic, PRO_E_RNDCH_CONIC_TYPE, PRO_ROUND_CONIC_DISABLE));
    RUN_RND_STAGE("round_references",
        add_compound_element(set, PRO_E_RNDCH_REFERENCES, &references));
    RUN_RND_STAGE("round_reference_type",
        add_integer_element(references, PRO_E_RNDCH_REFERENCE_TYPE, PRO_ROUND_REF_EDGE));
    RUN_RND_STAGE("round_collection_set",
        add_collection_element(references, PRO_E_STD_CURVE_COLLECTION_APPL, collection));
    RUN_RND_STAGE("round_spine",
        add_compound_element(set, PRO_E_RNDCH_COMPOUND_SPINE, &spine));
    RUN_RND_STAGE("round_ball_spine",
        add_integer_element(spine, PRO_E_RNDCH_BALL_SPINE, PRO_ROUND_ROLLING_BALL));
    RUN_RND_STAGE("round_auto_continue",
        add_integer_element(set, PRO_E_RNDCH_AUTO_CONTINUE, PRO_ROUND_AUTO_CONT_ENABLE));
    RUN_RND_STAGE("round_radii",
        add_compound_element(set, PRO_E_RNDCH_RADII, &radii));
    RUN_RND_STAGE("round_radius",
        add_compound_element(radii, PRO_E_RNDCH_RADIUS, &radius_element));
    RUN_RND_STAGE("round_leg1",
        add_compound_element(radius_element, PRO_E_RNDCH_LEG1, &leg1));
    RUN_RND_STAGE("round_leg_type",
        add_integer_element(leg1, PRO_E_RNDCH_LEG_TYPE, PRO_ROUND_RADIUS_TYPE_VALUE));
    RUN_RND_STAGE("round_leg_value",
        add_double_element(leg1, PRO_E_RNDCH_LEG_VALUE, radius));
    RUN_RND_STAGE("round_attach_type",
        add_integer_element(tree, PRO_E_RNDCH_ATTACH_TYPE, PRO_ROUND_ATTACHED));
    RUN_RND_STAGE("round_transitions",
        add_compound_element(tree, PRO_E_RNDCH_TRANSITIONS, &transitions));

    RUN_RND_STAGE("round_model_item", ProMdlToModelitem(model, &model_item));
    RUN_RND_STAGE("round_model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_RND_STAGE("round_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_NO_OPTS;
    *failure_stage = "round_feature_create";
    status = ProFeatureWithoptionsCreate(
        model_selection,
        tree,
        options,
        PRO_REGEN_NO_FLAGS,
        created_feature,
        &errors);
    *creation_error_count = errors.error_number;

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (tree != NULL)
        ProElementFree(&tree);
    if (collection != NULL)
        ProCollectionFree(&collection);
    if (edge_reference != NULL)
        ProReferenceFree(edge_reference);
    if (edge_selection != NULL)
        ProSelectionFree(&edge_selection);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
#undef RUN_RND_STAGE
    return status;
}

static ProError create_equal_distance_chamfer(
    ProMdl model,
    const wchar_t *feature_name,
    int requested_edge_id,
    double distance,
    ProFeature *created_feature,
    int *creation_error_count,
    int *selected_edge_id,
    ProEnttype *selected_edge_type,
    double *selected_edge_length,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement tree = NULL;
    ProElement sets = NULL;
    ProElement set = NULL;
    ProElement references = NULL;
    ProElement radii = NULL;
    ProElement radius_element = NULL;
    ProElement leg1 = NULL;
    ProElement transitions = NULL;
    ProModelitem model_item;
    ProGeomitem edge_geomitem;
    ProEdge selected_edge;
    ProSelection model_selection = NULL;
    ProSelection edge_selection = NULL;
    ProReference edge_reference = NULL;
    ProCollection collection = NULL;
    ProCrvcollinstr instruction = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};

#define RUN_CHM_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    RUN_CHM_STAGE("chamfer_edge_find",
        round_edge_find(
            (ProSolid)model,
            requested_edge_id,
            &selected_edge,
            selected_edge_id,
            selected_edge_type,
            selected_edge_length));
    RUN_CHM_STAGE("chamfer_edge_to_geomitem",
        ProEdgeToGeomitem((ProSolid)model, selected_edge, &edge_geomitem));
    RUN_CHM_STAGE("chamfer_edge_selection",
        ProSelectionAlloc(NULL, (ProModelitem *)&edge_geomitem, &edge_selection));
    RUN_CHM_STAGE("chamfer_edge_reference",
        ProSelectionToReference(edge_selection, &edge_reference));
    RUN_CHM_STAGE("chamfer_collection_alloc", ProCrvcollectionAlloc(&collection));
    RUN_CHM_STAGE("chamfer_instruction_alloc",
        ProCrvcollinstrAlloc(PRO_CURVCOLL_ADD_ONE_INSTR, &instruction));
    RUN_CHM_STAGE("chamfer_reference_add",
        ProCrvcollinstrReferenceAdd(instruction, edge_reference));
    RUN_CHM_STAGE("chamfer_instruction_add",
        ProCrvcollectionInstructionAdd(collection, instruction));

    RUN_CHM_STAGE("chamfer_tree_alloc", ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    RUN_CHM_STAGE("chamfer_feature_type",
        add_integer_element(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_CHAMFER));
    RUN_CHM_STAGE("chamfer_feature_name",
        add_wstring_element(tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_CHM_STAGE("chamfer_sets",
        add_compound_element(tree, PRO_E_RNDCH_SETS, &sets));
    RUN_CHM_STAGE("chamfer_set",
        add_compound_element(sets, PRO_E_RNDCH_SET, &set));
    RUN_CHM_STAGE("chamfer_schema",
        add_integer_element(set, PRO_E_RNDCH_DIMENSIONAL_SCHEMA, PRO_CHM_D_X_D));
    RUN_CHM_STAGE("chamfer_shape",
        add_integer_element(
            set, PRO_E_RNDCH_CHAMFER_SHAPE, PRO_CHM_OFFSET_SURFACE));
    RUN_CHM_STAGE("chamfer_references",
        add_compound_element(set, PRO_E_RNDCH_REFERENCES, &references));
    RUN_CHM_STAGE("chamfer_reference_type",
        add_integer_element(references, PRO_E_RNDCH_REFERENCE_TYPE, PRO_ROUND_REF_EDGE));
    RUN_CHM_STAGE("chamfer_collection_set",
        add_collection_element(references, PRO_E_STD_CURVE_COLLECTION_APPL, collection));
    RUN_CHM_STAGE("chamfer_radii",
        add_compound_element(set, PRO_E_RNDCH_RADII, &radii));
    RUN_CHM_STAGE("chamfer_radius",
        add_compound_element(radii, PRO_E_RNDCH_RADIUS, &radius_element));
    RUN_CHM_STAGE("chamfer_leg1",
        add_compound_element(radius_element, PRO_E_RNDCH_LEG1, &leg1));
    RUN_CHM_STAGE("chamfer_leg_type",
        add_integer_element(leg1, PRO_E_RNDCH_LEG_TYPE, PRO_ROUND_RADIUS_TYPE_VALUE));
    RUN_CHM_STAGE("chamfer_leg_value",
        add_double_element(leg1, PRO_E_RNDCH_LEG_VALUE, distance));
    RUN_CHM_STAGE("chamfer_attach_type",
        add_integer_element(tree, PRO_E_RNDCH_ATTACH_TYPE, PRO_ROUND_ATTACHED));
    RUN_CHM_STAGE("chamfer_transitions",
        add_compound_element(tree, PRO_E_RNDCH_TRANSITIONS, &transitions));

    RUN_CHM_STAGE("chamfer_model_item", ProMdlToModelitem(model, &model_item));
    RUN_CHM_STAGE("chamfer_model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_CHM_STAGE("chamfer_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_NO_OPTS;
    *failure_stage = "chamfer_feature_create";
    status = ProFeatureWithoptionsCreate(
        model_selection,
        tree,
        options,
        PRO_REGEN_NO_FLAGS,
        created_feature,
        &errors);
    *creation_error_count = errors.error_number;

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (tree != NULL)
        ProElementFree(&tree);
    if (collection != NULL)
        ProCollectionFree(&collection);
    if (edge_reference != NULL)
        ProReferenceFree(edge_reference);
    if (edge_selection != NULL)
        ProSelectionFree(&edge_selection);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
#undef RUN_CHM_STAGE
    return status;
}

typedef struct ShellSurfaceSearch
{
    ProSurface best_surface;
    int best_surface_id;
    ProSrftype best_surface_type;
    double best_surface_area;
    int found;
} ShellSurfaceSearch;

static ProError shell_surface_visit_action(
    ProSurface surface,
    ProError filter_status,
    ProAppData app_data)
{
    ShellSurfaceSearch *search = (ShellSurfaceSearch *)app_data;
    ProError status;
    ProSrftype surface_type;
    double surface_area;
    int surface_id;
    (void)filter_status;

    status = ProSurfaceTypeGet(surface, &surface_type);
    if (status != PRO_TK_NO_ERROR || surface_type != PRO_SRF_PLANE)
        return PRO_TK_NO_ERROR;
    status = ProSurfaceAreaEval(surface, &surface_area);
    if (status != PRO_TK_NO_ERROR || !_finite(surface_area) ||
        surface_area <= search->best_surface_area)
        return PRO_TK_NO_ERROR;
    status = ProSurfaceIdGet(surface, &surface_id);
    if (status != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;
    search->best_surface = surface;
    search->best_surface_id = surface_id;
    search->best_surface_type = surface_type;
    search->best_surface_area = surface_area;
    search->found = 1;
    return PRO_TK_NO_ERROR;
}

static ProError shell_surface_find(
    ProSolid solid,
    int requested_surface_id,
    ProSurface *selected_surface,
    int *selected_surface_id,
    ProSrftype *selected_surface_type,
    double *selected_surface_area)
{
    ProError status;
    ProSurface surface;
    ProSrftype surface_type;
    double surface_area;
    ShellSurfaceSearch search;

    if (requested_surface_id > 0)
    {
        status = ProSurfaceInit((ProMdl)solid, requested_surface_id, &surface);
        if (status != PRO_TK_NO_ERROR)
            return status;
        status = ProSurfaceTypeGet(surface, &surface_type);
        if (status != PRO_TK_NO_ERROR)
            return status;
        status = ProSurfaceAreaEval(surface, &surface_area);
        if (status != PRO_TK_NO_ERROR || !_finite(surface_area) ||
            surface_area <= 0.0)
            return status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status;
        *selected_surface = surface;
        *selected_surface_id = requested_surface_id;
        *selected_surface_type = surface_type;
        *selected_surface_area = surface_area;
        return PRO_TK_NO_ERROR;
    }

    memset(&search, 0, sizeof(search));
    search.best_surface_type = PRO_SRF_NONE;
    status = ProSolidSurfaceVisit(
        solid,
        shell_surface_visit_action,
        NULL,
        (ProAppData)&search);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (!search.found)
        return PRO_TK_E_NOT_FOUND;
    *selected_surface = search.best_surface;
    *selected_surface_id = search.best_surface_id;
    *selected_surface_type = search.best_surface_type;
    *selected_surface_area = search.best_surface_area;
    return PRO_TK_NO_ERROR;
}

static ProError create_inside_shell(
    ProMdl model,
    const wchar_t *feature_name,
    int requested_surface_id,
    double thickness,
    ProFeature *created_feature,
    int *creation_error_count,
    int *selected_surface_id,
    ProSrftype *selected_surface_type,
    double *selected_surface_area,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement tree = NULL;
    ProElement body_options = NULL;
    ProModelitem model_item;
    ProSolidBody default_body;
    ProSurface selected_surface;
    ProGeomitem surface_geomitem;
    ProSelection model_selection = NULL;
    ProSelection body_selection = NULL;
    ProSelection surface_selection = NULL;
    ProReference body_reference = NULL;
    ProReference surface_reference = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};

#define RUN_SHL_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    RUN_SHL_STAGE("shell_surface_find",
        shell_surface_find(
            (ProSolid)model,
            requested_surface_id,
            &selected_surface,
            selected_surface_id,
            selected_surface_type,
            selected_surface_area));
    RUN_SHL_STAGE("shell_surface_to_geomitem",
        ProSurfaceToGeomitem(
            (ProSolid)model, selected_surface, &surface_geomitem));
    RUN_SHL_STAGE("shell_surface_selection",
        ProSelectionAlloc(
            NULL, (ProModelitem *)&surface_geomitem, &surface_selection));
    RUN_SHL_STAGE("shell_surface_reference",
        ProSelectionToReference(surface_selection, &surface_reference));

    RUN_SHL_STAGE("shell_default_body",
        ProSolidDefaultBodyGet((ProSolid)model, &default_body));
    RUN_SHL_STAGE("shell_body_selection",
        ProSelectionAlloc(
            NULL, (ProModelitem *)&default_body, &body_selection));
    RUN_SHL_STAGE("shell_body_reference",
        ProSelectionToReference(body_selection, &body_reference));

    RUN_SHL_STAGE("shell_tree_alloc", ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    RUN_SHL_STAGE("shell_feature_type",
        add_integer_element(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_SHELL));
    RUN_SHL_STAGE("shell_feature_name",
        add_wstring_element(tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_SHL_STAGE("shell_body_options",
        add_compound_element(tree, PRO_E_BODY, &body_options));
    RUN_SHL_STAGE("shell_body_use",
        add_integer_element(body_options, PRO_E_BODY_USE, PRO_BODY_USE_SELECTED));
    RUN_SHL_STAGE("shell_body_select",
        add_reference_element(body_options, PRO_E_BODY_SELECT, body_reference));
    RUN_SHL_STAGE("shell_remove_surface",
        add_reference_element(tree, PRO_E_SHELL_SRF, surface_reference));
    RUN_SHL_STAGE("shell_thickness",
        add_double_element(tree, PRO_E_SHELL_THICK, thickness));
    RUN_SHL_STAGE("shell_side",
        add_integer_element(tree, PRO_E_SHELL_FLIP, PRO_SHELL_INSIDE));
    RUN_SHL_STAGE("shell_lace",
        add_integer_element(tree, PRO_E_SHELL_LACE_BNDRY, PRO_SHELL_DONT_LACE));
    RUN_SHL_STAGE("shell_alt_cut",
        add_integer_element(
            tree, PRO_E_SHELL_ALT_CUT_METHOD, PRO_SHELL_ALT_CUT_METHOD_NO));

    RUN_SHL_STAGE("shell_model_item", ProMdlToModelitem(model, &model_item));
    RUN_SHL_STAGE("shell_model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_SHL_STAGE("shell_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_NO_OPTS;
    *failure_stage = "shell_feature_create";
    status = ProFeatureWithoptionsCreate(
        model_selection,
        tree,
        options,
        PRO_REGEN_NO_FLAGS,
        created_feature,
        &errors);
    *creation_error_count = errors.error_number;

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (tree != NULL)
        ProElementFree(&tree);
    if (surface_reference != NULL)
        ProReferenceFree(surface_reference);
    if (body_reference != NULL)
        ProReferenceFree(body_reference);
    if (surface_selection != NULL)
        ProSelectionFree(&surface_selection);
    if (body_selection != NULL)
        ProSelectionFree(&body_selection);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
#undef RUN_SHL_STAGE
    return status;
}

typedef struct DraftSurfaceSearch
{
    ProSurface best_surface;
    int best_surface_id;
    ProSrftype best_surface_type;
    double best_surface_area;
    int found;
} DraftSurfaceSearch;

static ProError draft_surface_visit_action(
    ProSurface surface,
    ProError filter_status,
    ProAppData app_data)
{
    DraftSurfaceSearch *search = (DraftSurfaceSearch *)app_data;
    ProError status;
    ProSrftype surface_type;
    double surface_area;
    int surface_id;
    (void)filter_status;

    status = ProSurfaceTypeGet(surface, &surface_type);
    if (status != PRO_TK_NO_ERROR || surface_type != PRO_SRF_CYL)
        return PRO_TK_NO_ERROR;
    status = ProSurfaceAreaEval(surface, &surface_area);
    if (status != PRO_TK_NO_ERROR || !_finite(surface_area) ||
        surface_area <= search->best_surface_area)
        return PRO_TK_NO_ERROR;
    status = ProSurfaceIdGet(surface, &surface_id);
    if (status != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;
    search->best_surface = surface;
    search->best_surface_id = surface_id;
    search->best_surface_type = surface_type;
    search->best_surface_area = surface_area;
    search->found = 1;
    return PRO_TK_NO_ERROR;
}

static ProError draft_surface_find(
    ProSolid solid,
    int requested_surface_id,
    ProSurface *selected_surface,
    int *selected_surface_id,
    ProSrftype *selected_surface_type,
    double *selected_surface_area)
{
    ProError status;
    ProSurface surface;
    ProSrftype surface_type;
    double surface_area;
    DraftSurfaceSearch search;

    if (requested_surface_id > 0)
    {
        status = ProSurfaceInit((ProMdl)solid, requested_surface_id, &surface);
        if (status != PRO_TK_NO_ERROR)
            return status;
        status = ProSurfaceTypeGet(surface, &surface_type);
        if (status != PRO_TK_NO_ERROR)
            return status;
        status = ProSurfaceAreaEval(surface, &surface_area);
        if (status != PRO_TK_NO_ERROR || !_finite(surface_area) ||
            surface_area <= 0.0)
            return status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status;
        *selected_surface = surface;
        *selected_surface_id = requested_surface_id;
        *selected_surface_type = surface_type;
        *selected_surface_area = surface_area;
        return PRO_TK_NO_ERROR;
    }

    memset(&search, 0, sizeof(search));
    search.best_surface_type = PRO_SRF_NONE;
    status = ProSolidSurfaceVisit(
        solid,
        draft_surface_visit_action,
        NULL,
        (ProAppData)&search);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (!search.found)
        return PRO_TK_E_NOT_FOUND;
    *selected_surface = search.best_surface;
    *selected_surface_id = search.best_surface_id;
    *selected_surface_type = search.best_surface_type;
    *selected_surface_area = search.best_surface_area;
    return PRO_TK_NO_ERROR;
}

static ProError create_constant_plane_draft(
    ProMdl model,
    const wchar_t *feature_name,
    const wchar_t *hinge_plane_name,
    int requested_surface_id,
    double angle_degrees,
    int direction_side,
    ProFeature *created_feature,
    int *creation_error_count,
    int *selected_surface_id,
    ProSrftype *selected_surface_type,
    double *selected_surface_area,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement tree = NULL;
    ProElement direction = NULL;
    ProElement side1 = NULL;
    ProElement side1_angles = NULL;
    ProElement side2 = NULL;
    ProElement side2_angles = NULL;
    ProModelitem model_item;
    ProModelitem plane_item;
    ProSurface selected_surface;
    ProGeomitem surface_geomitem;
    ProSelection model_selection = NULL;
    ProSelection plane_selection = NULL;
    ProSelection surface_selection = NULL;
    ProReference plane_reference = NULL;
    ProReference surface_reference = NULL;
    ProCollection surface_collection = NULL;
    ProSrfcollinstr surface_instruction = NULL;
    ProSrfcollref surface_collection_reference = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};

#define RUN_DRF_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    RUN_DRF_STAGE("draft_surface_find",
        draft_surface_find(
            (ProSolid)model,
            requested_surface_id,
            &selected_surface,
            selected_surface_id,
            selected_surface_type,
            selected_surface_area));
    RUN_DRF_STAGE("draft_surface_to_geomitem",
        ProSurfaceToGeomitem(
            (ProSolid)model, selected_surface, &surface_geomitem));
    RUN_DRF_STAGE("draft_surface_selection",
        ProSelectionAlloc(
            NULL, (ProModelitem *)&surface_geomitem, &surface_selection));
    RUN_DRF_STAGE("draft_surface_reference",
        ProSelectionToReference(surface_selection, &surface_reference));
    RUN_DRF_STAGE("draft_plane_item",
        ProModelitemByNameInit(
            model, PRO_SURFACE, (wchar_t *)hinge_plane_name, &plane_item));
    RUN_DRF_STAGE("draft_plane_selection",
        ProSelectionAlloc(NULL, &plane_item, &plane_selection));
    RUN_DRF_STAGE("draft_plane_reference",
        ProSelectionToReference(plane_selection, &plane_reference));

    RUN_DRF_STAGE("draft_surface_collection",
        ProSrfcollectionAlloc(&surface_collection));
    RUN_DRF_STAGE("draft_surface_instruction",
        ProSrfcollinstrAlloc(
            PRO_SURFCOLL_SINGLE_SURF, PRO_B_TRUE, &surface_instruction));
    RUN_DRF_STAGE("draft_surface_include",
        ProSrfcollinstrIncludeSet(surface_instruction, PRO_B_TRUE));
    RUN_DRF_STAGE("draft_surface_collection_reference",
        ProSrfcollrefAlloc(
            PRO_SURFCOLL_REF_SINGLE,
            surface_reference,
            &surface_collection_reference));
    RUN_DRF_STAGE("draft_surface_reference_add",
        ProSrfcollinstrReferenceAdd(
            surface_instruction, surface_collection_reference));
    RUN_DRF_STAGE("draft_surface_instruction_add",
        ProSrfcollectionInstructionAdd(
            surface_collection, surface_instruction));

    RUN_DRF_STAGE("draft_tree_alloc", ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    RUN_DRF_STAGE("draft_feature_type",
        add_integer_element(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_DRAFT));
    RUN_DRF_STAGE("draft_feature_name",
        add_wstring_element(tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_DRF_STAGE("draft_tweak",
        add_integer_element(
            tree, PRO_E_DRAFT_TWEAK_OR_INTERSEC, PRO_DRAFT_UI_TWEAK));
    RUN_DRF_STAGE("draft_extend",
        add_integer_element(tree, PRO_E_DRAFT_EXTEND, PRO_DRAFT_UI_NO_EXTEND));
    RUN_DRF_STAGE("draft_split",
        add_integer_element(tree, PRO_E_DRAFT_SPLIT, PRO_DRAFT_UI_SPLIT_NONE));
    RUN_DRF_STAGE("draft_surfaces",
        add_collection_element(
            tree, PRO_E_STD_SURF_COLLECTION_APPL, surface_collection));
    RUN_DRF_STAGE("draft_direction",
        add_compound_element(tree, PRO_E_DIRECTION_COMPOUND, &direction));
    RUN_DRF_STAGE("draft_direction_reference",
        add_reference_element(
            direction, PRO_E_DIRECTION_REFERENCE, plane_reference));
    RUN_DRF_STAGE("draft_direction_flip",
        add_integer_element(
            direction,
            PRO_E_DIRECTION_FLIP,
            direction_side == 1
                ? PRO_DIRECTION_FLIP_ALONG
                : PRO_DIRECTION_FLIP_AGAINST));
    RUN_DRF_STAGE("draft_constant",
        add_integer_element(
            tree, PRO_E_DRAFT_CONSTANT_OR_VARIABLE, PRO_DRAFT_UI_CONSTANT));
    RUN_DRF_STAGE("draft_include_tangent",
        add_integer_element(
            tree, PRO_E_DRAFT_INCLUDE_TANGENT, PRO_DRAFT_UI_NOT_INC_TANG));
    RUN_DRF_STAGE("draft_side1",
        add_compound_element(tree, PRO_E_DRAFT_SIDE_1, &side1));
    RUN_DRF_STAGE("draft_neutral_type",
        add_integer_element(
            side1, PRO_E_DRAFT_NEUTRAL_OBJECT_TYPE_1, PRO_DRAFT_UI_PLANE));
    RUN_DRF_STAGE("draft_neutral_plane",
        add_reference_element(
            side1, PRO_E_DRAFT_NEUTRAL_PLANE_1, plane_reference));
    RUN_DRF_STAGE("draft_dependent",
        add_integer_element(
            side1, PRO_E_DRAFT_DEPENDENT_1, PRO_DRAFT_UI_INDEPENDENT));
    RUN_DRF_STAGE("draft_angle",
        add_double_element(side1, PRO_E_DRAFT_ANGLE_1, angle_degrees));
    RUN_DRF_STAGE("draft_side1_angles",
        add_compound_element(side1, PRO_E_DRAFT_ANGLES, &side1_angles));
    RUN_DRF_STAGE("draft_side2",
        add_compound_element(tree, PRO_E_DRAFT_SIDE_2, &side2));
    RUN_DRF_STAGE("draft_side2_neutral_type",
        add_integer_element(
            side2, PRO_E_DRAFT_NEUTRAL_OBJECT_TYPE_2, PRO_DRAFT_UI_NO_NEUT));
    RUN_DRF_STAGE("draft_side2_dependent",
        add_integer_element(
            side2, PRO_E_DRAFT_DEPENDENT_2, PRO_DRAFT_UI_INDEPENDENT));
    RUN_DRF_STAGE("draft_side2_angle",
        add_double_element(side2, PRO_E_DRAFT_ANGLE_2, angle_degrees));
    RUN_DRF_STAGE("draft_side2_angles",
        add_compound_element(side2, PRO_E_DRAFT_ANGLES, &side2_angles));

    RUN_DRF_STAGE("draft_model_item", ProMdlToModelitem(model, &model_item));
    RUN_DRF_STAGE("draft_model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_DRF_STAGE("draft_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_NO_OPTS;
    *failure_stage = "draft_feature_create";
    status = ProFeatureWithoptionsCreate(
        model_selection,
        tree,
        options,
        PRO_REGEN_NO_FLAGS,
        created_feature,
        &errors);
    *creation_error_count = errors.error_number;

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (tree != NULL)
        ProElementFree(&tree);
    if (surface_collection != NULL)
        ProCollectionFree(&surface_collection);
    if (surface_reference != NULL)
        ProReferenceFree(surface_reference);
    if (plane_reference != NULL)
        ProReferenceFree(plane_reference);
    if (surface_selection != NULL)
        ProSelectionFree(&surface_selection);
    if (plane_selection != NULL)
        ProSelectionFree(&plane_selection);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
#undef RUN_DRF_STAGE
    return status;
}

static ProError create_feature_mirror(
    ProMdl model,
    const wchar_t *feature_name,
    const wchar_t *source_feature_name,
    const wchar_t *mirror_plane_name,
    ProFeature *created_feature,
    int *creation_error_count,
    int *source_feature_id,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement tree = NULL;
    ProModelitem model_item;
    ProModelitem mirror_plane_item;
    ProSelection model_selection = NULL;
    ProSelection mirror_plane_selection = NULL;
    ProReference source_feature_reference = NULL;
    ProReference mirror_plane_reference = NULL;
    ProReference *source_references = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};

#define RUN_MIR_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    (void)source_feature_name;
    *source_feature_id = -1;
    RUN_MIR_STAGE("mirror_model_item", ProMdlToModelitem(model, &model_item));
    RUN_MIR_STAGE("mirror_model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_MIR_STAGE("mirror_plane",
        ProModelitemByNameInit(
            model,
            PRO_SURFACE,
            (wchar_t *)mirror_plane_name,
            &mirror_plane_item));
    RUN_MIR_STAGE("mirror_source_reference",
        ProSelectionToReference(model_selection, &source_feature_reference));
    RUN_MIR_STAGE("mirror_plane_selection",
        ProSelectionAlloc(NULL, &mirror_plane_item, &mirror_plane_selection));
    RUN_MIR_STAGE("mirror_plane_reference",
        ProSelectionToReference(mirror_plane_selection, &mirror_plane_reference));
    RUN_MIR_STAGE("mirror_references_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProReference),
            1,
            (ProArray *)&source_references));
    source_references[0] = source_feature_reference;

    RUN_MIR_STAGE("mirror_tree_alloc",
        ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    RUN_MIR_STAGE("mirror_feature_type",
        add_integer_element(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_GEN_MERGE));
    RUN_MIR_STAGE("mirror_transform_type",
        add_integer_element(
            tree, PRO_E_SRF_TRANS_TYPE, PRO_SURF_TRANS_TYPE_PART_MIRROR));
    RUN_MIR_STAGE("mirror_feature_name",
        add_wstring_element(tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_MIR_STAGE("mirror_source_items",
        add_references_element(
            tree, PRO_E_MIRROR_REF_ITEMS, source_references));
    RUN_MIR_STAGE("mirror_plane_item",
        add_reference_element(
            tree, PRO_E_MIRROR_REF_PLANE, mirror_plane_reference));
    RUN_MIR_STAGE("mirror_keep_original",
        add_integer_element(
            tree, PRO_E_COPY_NO_COPY, PRO_MIRROR_KEEP_ORIGINAL));

    RUN_MIR_STAGE("mirror_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_NO_OPTS;
    *failure_stage = "mirror_feature_create";
    status = ProFeatureWithoptionsCreate(
        model_selection,
        tree,
        options,
        PRO_REGEN_NO_FLAGS,
        created_feature,
        &errors);
    *creation_error_count = errors.error_number;

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (tree != NULL)
        ProElementFree(&tree);
    if (source_references != NULL)
        ProArrayFree((ProArray *)&source_references);
    if (source_feature_reference != NULL)
        ProReferenceFree(source_feature_reference);
    if (mirror_plane_reference != NULL)
        ProReferenceFree(mirror_plane_reference);
    if (mirror_plane_selection != NULL)
        ProSelectionFree(&mirror_plane_selection);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
#undef RUN_MIR_STAGE
    return status;
}

static ProError create_rectangle_extrusion(
    ProMdl model,
    const wchar_t *sketch_plane,
    const wchar_t *orientation_plane,
    const wchar_t *feature_name,
    double width,
    double height,
    double depth,
    int direction_side,
    int remove_material,
    int circle_profile,
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
    RUN_STAGE("sketch_plane_lookup",
        ProModelitemByNameInit(
            model, PRO_SURFACE, (wchar_t *)sketch_plane, &sketch_plane_item));
    RUN_STAGE("orientation_plane_lookup",
        ProModelitemByNameInit(
            model, PRO_SURFACE, (wchar_t *)orientation_plane, &orientation_plane_item));
    RUN_STAGE("sketch_plane_selection",
        ProSelectionAlloc(NULL, &sketch_plane_item, &sketch_plane_selection));
    RUN_STAGE("orientation_plane_selection",
        ProSelectionAlloc(NULL, &orientation_plane_item, &orientation_plane_selection));
    RUN_STAGE("sketch_plane_reference",
        ProSelectionToReference(sketch_plane_selection, &sketch_plane_reference));
    RUN_STAGE("orientation_plane_reference",
        ProSelectionToReference(
            orientation_plane_selection, &orientation_plane_reference));

    RUN_STAGE("feature_tree_alloc",
        ProElementAlloc(PRO_E_FEATURE_TREE, &feature_tree));
    RUN_STAGE("feature_type",
        add_integer_element(
            feature_tree,
            PRO_E_FEATURE_TYPE,
            remove_material ? PRO_FEAT_CUT : PRO_FEAT_PROTRUSION));
    RUN_STAGE("feature_form",
        add_integer_element(feature_tree, PRO_E_FEATURE_FORM, PRO_EXTRUDE));
    RUN_STAGE("feature_name",
        add_wstring_element(feature_tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_STAGE("solid_type",
        add_integer_element(
            feature_tree, PRO_E_EXT_SURF_CUT_SOLID_TYPE, PRO_EXT_FEAT_TYPE_SOLID));
    RUN_STAGE(remove_material ? "material_remove" : "material_add",
        add_integer_element(
            feature_tree,
            PRO_E_REMOVE_MATERIAL,
            remove_material ? PRO_EXT_MATERIAL_REMOVE : PRO_EXT_MATERIAL_ADD));
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
        add_double_element(depth_to, PRO_E_EXT_DEPTH_TO_VALUE, depth));

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
        element_by_ids_get(
            extracted_tree, sketch_path_ids, 2, &sketch_element));
    RUN_STAGE("section_handle_get",
        ProElementSpecialvalueGet(
            sketch_element, NULL, (ProAppData *)&section));
    if (circle_profile)
    {
        RUN_STAGE("circle_section_build",
            circle_section_build(section, width, failure_stage));
    }
    else
    {
        RUN_STAGE("rectangle_section_build",
            rectangle_section_build(section, width, height, failure_stage));
    }

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
                material_side_element,
                remove_material
                    ? PRO_EXT_MATERIAL_SIDE_TWO
                    : PRO_EXT_MATERIAL_SIDE_ONE));
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

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError regenerate_status = PRO_TK_NO_ERROR;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl source_model = NULL;
    ProMdl copy_model = NULL;
    ProMdlName source_name;
    ProMdlName expected_name;
    ProMdlName copy_name;
    ProMdlType source_type;
    ProName sketch_plane;
    ProName orientation_plane;
    ProName feature_name;
    ProName created_feature_name = L"";
    ProFeature created_feature;
    ProFeattype created_feature_type = -1;
    ProFeatStatus created_feature_status = PRO_FEAT_INVALID;
    ProModelitem guard_item;
    ProPath original_directory;
    ProPath output_directory;
    ProPath saved_path;
    ProMassProperty source_mass_property;
    ProMassProperty copy_mass_property;
    double width;
    double height;
    double depth;
    double volume_delta;
    double minimum_volume_delta;
    double maximum_cut_volume;
    int direction_side;
    int remove_material = 0;
    int revolve_mode = 0;
    int sweep_mode = 0;
    int round_mode = 0;
    int chamfer_mode = 0;
    int shell_mode = 0;
    int draft_mode = 0;
    int mirror_mode = 0;
    int circle_mode = 0;
    int mirrored_source_feature_id = -1;
    int selected_edge_id = 0;
    ProEnttype selected_edge_type = PRO_ENT_NONE;
    double selected_edge_length = 0.0;
    int selected_surface_id = 0;
    ProSrftype selected_surface_type = PRO_SRF_NONE;
    double selected_surface_area = 0.0;
    int source_feature_count = 0;
    int copy_feature_count = 0;
    int creation_error_count = 0;
    int regenerate_attempts = 0;
    int connected = 0;
    int copy_created = 0;
    int directory_changed = 0;
    int exit_code = 1;
    const char *feature_failure_stage = "feature_create";

    if (argc != 13)
    {
        fwprintf(stderr,
            L"Usage: creo_extrude_bridge <result.json> <expected_model> "
            L"<copy_name> <output_dir> <sketch_plane> <orientation_plane> "
            L"<feature_name> <width> <height> <depth> <direction_side_1_or_2> "
            L"<profile_mode: RECTANGLE, RECTANGLE_CUT, CIRCLE, CIRCLE_CUT, "
            L"REVOLVE_RECTANGLE, REVOLVE_RECTANGLE_CUT, "
            L"SWEEP_CIRCLE, SWEEP_CIRCLE_CUT, ROUND, CHAMFER, SHELL, DRAFT, "
            L"MIRROR or CIRCLE>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
    }
    if (!parse_positive_dimension(
            argv[8],
            (wcscmp(argv[12], L"ROUND") == 0 ||
             wcscmp(argv[12], L"CHAMFER") == 0 ||
             wcscmp(argv[12], L"SHELL") == 0 ||
             wcscmp(argv[12], L"DRAFT") == 0 ||
             wcscmp(argv[12], L"MIRROR") == 0 ||
             wcscmp(argv[12], L"SWEEP_CIRCLE") == 0 ||
             wcscmp(argv[12], L"SWEEP_CIRCLE_CUT") == 0)
                ? 2147483647.0
                : 500.0,
            &width) ||
        !parse_positive_dimension(argv[9], 500.0, &height) ||
        !parse_positive_dimension(argv[10], 500.0, &depth))
    {
        exit_code = write_error(out, "dimension_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    direction_side = _wtoi(argv[11]);
    if ((direction_side != 1 && direction_side != 2) ||
        (direction_side == 1 && wcscmp(argv[11], L"1") != 0) ||
        (direction_side == 2 && wcscmp(argv[11], L"2") != 0))
    {
        exit_code = write_error(out, "direction_side_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    if (wcscmp(argv[12], L"RECTANGLE") == 0)
        remove_material = 0;
    else if (wcscmp(argv[12], L"CIRCLE") == 0)
    {
        remove_material = 0;
        circle_mode = 1;
    }
    else if (wcscmp(argv[12], L"CIRCLE_CUT") == 0)
    {
        remove_material = 1;
        circle_mode = 1;
    }
    else if (wcscmp(argv[12], L"RECTANGLE_CUT") == 0)
        remove_material = 1;
    else if (wcscmp(argv[12], L"REVOLVE_RECTANGLE") == 0)
    {
        remove_material = 0;
        revolve_mode = 1;
    }
    else if (wcscmp(argv[12], L"REVOLVE_RECTANGLE_CUT") == 0)
    {
        remove_material = 1;
        revolve_mode = 1;
    }
    else if (wcscmp(argv[12], L"SWEEP_CIRCLE") == 0)
    {
        remove_material = 0;
        sweep_mode = 1;
    }
    else if (wcscmp(argv[12], L"SWEEP_CIRCLE_CUT") == 0)
    {
        remove_material = 1;
        sweep_mode = 1;
    }
    else if (wcscmp(argv[12], L"ROUND") == 0)
    {
        remove_material = 0;
        round_mode = 1;
    }
    else if (wcscmp(argv[12], L"CHAMFER") == 0)
    {
        remove_material = 0;
        chamfer_mode = 1;
    }
    else if (wcscmp(argv[12], L"SHELL") == 0)
    {
        remove_material = 0;
        shell_mode = 1;
    }
    else if (wcscmp(argv[12], L"DRAFT") == 0)
    {
        remove_material = 0;
        draft_mode = 1;
    }
    else if (wcscmp(argv[12], L"MIRROR") == 0)
    {
        remove_material = 0;
        mirror_mode = 1;
    }
    else
    {
        exit_code = write_error(out, "profile_guard", PRO_TK_BAD_INPUTS);
        goto done;
    }

    wcsncpy_s(expected_name,
        sizeof(expected_name) / sizeof(expected_name[0]), argv[2], _TRUNCATE);
    wcsncpy_s(copy_name,
        sizeof(copy_name) / sizeof(copy_name[0]), argv[3], _TRUNCATE);
    wcsncpy_s(output_directory,
        sizeof(output_directory) / sizeof(output_directory[0]), argv[4], _TRUNCATE);
    wcsncpy_s(sketch_plane,
        sizeof(sketch_plane) / sizeof(sketch_plane[0]), argv[5], _TRUNCATE);
    wcsncpy_s(orientation_plane,
        sizeof(orientation_plane) / sizeof(orientation_plane[0]), argv[6], _TRUNCATE);
    wcsncpy_s(feature_name,
        sizeof(feature_name) / sizeof(feature_name[0]), argv[7], _TRUNCATE);

    if (!mirror_mode && _wcsicmp(sketch_plane, orientation_plane) == 0)
    {
        exit_code = write_error(out, "distinct_plane_guard", PRO_TK_BAD_INPUTS);
        goto done;
    }
    if (GetFileAttributesW(output_directory) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "output_directory", PRO_TK_INVALID_DIR);
        goto done;
    }
    if (output_model_already_exists(output_directory, copy_name))
    {
        exit_code = write_error(out, "refuse_overwrite", PRO_TK_E_FOUND);
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
    status = ProMdlCurrentGet(&source_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto cleanup;
    }
    status = ProMdlNameGet(source_model, source_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_name", status);
        goto cleanup;
    }
    if (_wcsicmp(source_name, expected_name) != 0)
    {
        exit_code = write_error(out, "source_model_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProMdlTypeGet(source_model, &source_type);
    if (status != PRO_TK_NO_ERROR || source_type != PRO_MDL_PART)
    {
        exit_code = write_error(out, "source_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        source_model, PRO_SURFACE, sketch_plane, &guard_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "sketch_plane_guard", status);
        goto cleanup;
    }
    status = mirror_mode
        ? PRO_TK_NO_ERROR
        : ProModelitemByNameInit(
            source_model,
            PRO_SURFACE,
            orientation_plane,
            &guard_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "orientation_plane_guard", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        source_model, PRO_FEATURE, feature_name, &guard_item);
    if (status == PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_name_guard", PRO_TK_E_FOUND);
        goto cleanup;
    }
    status = feature_count_get((ProSolid)source_model, &source_feature_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_feature_count", status);
        goto cleanup;
    }
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)source_model,
        NULL,
        PRO_MP_DENS_USE_ALWAYS,
        1.0,
        &source_mass_property);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_mass_properties", status);
        goto cleanup;
    }

    status = ProDirectoryCurrentGet(original_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "get_working_directory", status);
        goto cleanup;
    }
    status = ProDirectoryChange(output_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "change_output_directory", status);
        goto cleanup;
    }
    directory_changed = 1;
    status = ProMdlnameCopy(source_model, copy_name, &copy_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_model", status);
        goto cleanup;
    }
    copy_created = 1;
    ProDirectoryChange(original_directory);
    directory_changed = 0;

    if (mirror_mode)
    {
        status = create_feature_mirror(
            copy_model,
            feature_name,
            orientation_plane,
            sketch_plane,
            &created_feature,
            &creation_error_count,
            &mirrored_source_feature_id,
            &feature_failure_stage);
    }
    else if (draft_mode)
    {
        int requested_surface_id = width < 1.0 ? 0 : (int)width;
        if (width >= 1.0 && fabs(width - (double)requested_surface_id) > 1.0e-9)
        {
            exit_code = write_error(out, "draft_surface_id_input", PRO_TK_BAD_INPUTS);
            goto cleanup;
        }
        status = create_constant_plane_draft(
            copy_model,
            feature_name,
            sketch_plane,
            requested_surface_id,
            height,
            direction_side,
            &created_feature,
            &creation_error_count,
            &selected_surface_id,
            &selected_surface_type,
            &selected_surface_area,
            &feature_failure_stage);
    }
    else if (shell_mode)
    {
        int requested_surface_id = width < 1.0 ? 0 : (int)width;
        if (width >= 1.0 && fabs(width - (double)requested_surface_id) > 1.0e-9)
        {
            exit_code = write_error(out, "shell_surface_id_input", PRO_TK_BAD_INPUTS);
            goto cleanup;
        }
        status = create_inside_shell(
            copy_model,
            feature_name,
            requested_surface_id,
            height,
            &created_feature,
            &creation_error_count,
            &selected_surface_id,
            &selected_surface_type,
            &selected_surface_area,
            &feature_failure_stage);
    }
    else if (chamfer_mode)
    {
        int requested_edge_id = width < 1.0 ? 0 : (int)width;
        if (width >= 1.0 && fabs(width - (double)requested_edge_id) > 1.0e-9)
        {
            exit_code = write_error(out, "chamfer_edge_id_input", PRO_TK_BAD_INPUTS);
            goto cleanup;
        }
        status = create_equal_distance_chamfer(
            copy_model,
            feature_name,
            requested_edge_id,
            height,
            &created_feature,
            &creation_error_count,
            &selected_edge_id,
            &selected_edge_type,
            &selected_edge_length,
            &feature_failure_stage);
    }
    else if (round_mode)
    {
        int requested_edge_id = width < 1.0 ? 0 : (int)width;
        if (width >= 1.0 && fabs(width - (double)requested_edge_id) > 1.0e-9)
        {
            exit_code = write_error(out, "round_edge_id_input", PRO_TK_BAD_INPUTS);
            goto cleanup;
        }
        status = create_constant_round(
            copy_model,
            feature_name,
            requested_edge_id,
            height,
            &created_feature,
            &creation_error_count,
            &selected_edge_id,
            &selected_edge_type,
            &selected_edge_length,
            &feature_failure_stage);
    }
    else if (sweep_mode)
    {
        int requested_edge_id = width < 1.0 ? 0 : (int)width;
        if (width >= 1.0 && fabs(width - (double)requested_edge_id) > 1.0e-9)
        {
            exit_code = write_error(out, "trajectory_edge_id_input", PRO_TK_BAD_INPUTS);
            goto cleanup;
        }
        status = create_circular_sweep(
            copy_model,
            feature_name,
            requested_edge_id,
            height,
            remove_material,
            &created_feature,
            &creation_error_count,
            &selected_edge_id,
            &selected_edge_type,
            &selected_edge_length,
            &feature_failure_stage);
    }
    else if (revolve_mode)
    {
        status = create_rectangle_revolve(
            copy_model,
            sketch_plane,
            orientation_plane,
            feature_name,
            width,
            height,
            depth,
            360.0,
            direction_side,
            remove_material,
            &created_feature,
            &creation_error_count,
            &feature_failure_stage);
    }
    else
    {
        status = create_rectangle_extrusion(
            copy_model,
            sketch_plane,
            orientation_plane,
            feature_name,
            width,
            height,
            depth,
            direction_side,
            remove_material,
            circle_mode,
            &created_feature,
            &creation_error_count,
            &feature_failure_stage);
    }
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, feature_failure_stage, status);
        goto cleanup;
    }
    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)copy_model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR &&
        regenerate_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "regenerate", regenerate_status);
        goto cleanup;
    }
    status = ProFeatureTypeGet(&created_feature, &created_feature_type);
    if (status != PRO_TK_NO_ERROR ||
        created_feature_type !=
            (mirror_mode
                ? PRO_FEAT_GEN_MERGE
            : (draft_mode
                ? PRO_FEAT_DRAFT
                : (shell_mode
                    ? PRO_FEAT_SHELL
                : (chamfer_mode
                    ? PRO_FEAT_CHAMFER
                : (round_mode
                    ? PRO_FEAT_ROUND
                    : (remove_material ? PRO_FEAT_CUT : PRO_FEAT_PROTRUSION)))))))
    {
        exit_code = write_error(out, "created_feature_type_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProFeatureStatusGet(&created_feature, &created_feature_status);
    if (status != PRO_TK_NO_ERROR || created_feature_status != PRO_FEAT_ACTIVE)
    {
        if (draft_mode)
        {
            fprintf(stderr,
                "draft_failure surface_id=%d surface_type=%d area=%.17g "
                "feature_status=%d status_get=%d\n",
                selected_surface_id,
                selected_surface_type,
                selected_surface_area,
                created_feature_status,
                status);
        }
        exit_code = write_error(out, "created_feature_status_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProModelitemNameGet(
        (ProModelitem *)&created_feature, created_feature_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "created_feature_name_readback", status);
        goto cleanup;
    }
    if (_wcsicmp(created_feature_name, feature_name) != 0)
    {
        ProModelitem named_feature;
        ProError rename_status = ProModelitemNameSet(
            (ProModelitem *)&created_feature, feature_name);
        if (rename_status != PRO_TK_NO_ERROR && rename_status != PRO_TK_E_FOUND)
        {
            exit_code = write_error(out, "created_feature_name_set", rename_status);
            goto cleanup;
        }
        status = ProModelitemByNameInit(
            copy_model, PRO_FEATURE, feature_name, &named_feature);
        if (status != PRO_TK_NO_ERROR || named_feature.id != created_feature.id)
        {
            exit_code = write_error(out, "created_feature_name_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto cleanup;
        }
        wcsncpy_s(created_feature_name,
            sizeof(created_feature_name) / sizeof(created_feature_name[0]),
            feature_name,
            _TRUNCATE);
    }
    status = feature_count_get((ProSolid)copy_model, &copy_feature_count);
    if (status != PRO_TK_NO_ERROR || copy_feature_count <= source_feature_count)
    {
        exit_code = write_error(out, "copy_feature_count_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)copy_model,
        NULL,
        PRO_MP_DENS_USE_ALWAYS,
        1.0,
        &copy_mass_property);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_mass_properties", status);
        goto cleanup;
    }
    volume_delta = (draft_mode || mirror_mode)
        ? fabs(copy_mass_property.volume - source_mass_property.volume)
        : (shell_mode
            ? source_mass_property.volume - copy_mass_property.volume
        : ((round_mode || chamfer_mode)
            ? fabs(copy_mass_property.volume - source_mass_property.volume)
            : (remove_material
                ? source_mass_property.volume - copy_mass_property.volume
                : copy_mass_property.volume - source_mass_property.volume)));
    minimum_volume_delta = fabs(source_mass_property.volume) * 1.0e-9;
    if (minimum_volume_delta < 1.0e-6)
        minimum_volume_delta = 1.0e-6;
    if (!_finite(volume_delta) || volume_delta <= minimum_volume_delta)
    {
        exit_code = write_error(
            out,
            mirror_mode
                ? "mirror_geometry_change_guard"
                : (draft_mode
                ? "draft_geometry_change_guard"
                : (shell_mode
                    ? "shell_volume_decrease_guard"
                : ((round_mode || chamfer_mode)
                ? (chamfer_mode
                    ? "chamfer_geometry_change_guard"
                    : "round_geometry_change_guard")
                : (remove_material ? "volume_decrease_guard" : "volume_increase_guard")))),
            PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }
    maximum_cut_volume = sweep_mode
        ? 3.14159265358979323846 * height * height * selected_edge_length
        : (revolve_mode
            ? 3.14159265358979323846 *
                (((height + depth) * (height + depth)) - (height * height)) *
                width
            : width * height * depth);
    if (remove_material &&
        volume_delta > maximum_cut_volume + minimum_volume_delta)
    {
        exit_code = write_error(out, "maximum_cut_volume_guard", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }

    status = ProMdlSave(copy_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_copy", status);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            output_directory,
            copy_name,
            saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_file", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"safe_copy_only\":true,\"source_model\":", out);
    write_wide_json_string(out, source_name);
    fputs(",\"copy_model\":", out);
    write_wide_json_string(out, copy_name);
    fputs(",\"sketch_plane\":", out);
    write_wide_json_string(out, sketch_plane);
    fputs(",\"orientation_plane\":", out);
    write_wide_json_string(out, orientation_plane);
    fputs(",\"feature_name\":", out);
    write_wide_json_string(out, created_feature_name);
    if (mirror_mode)
    {
        fputs(",\"mirror_plane\":", out);
        write_wide_json_string(out, sketch_plane);
        fputs(",\"mirror_scope\":\"whole_part\"", out);
    }
    else if (draft_mode)
    {
        fprintf(out,
            ",\"drafted_surface_id\":%d,\"surface_type_code\":%d,"
            "\"drafted_surface_area\":%.17g,\"angle_degrees\":%.15g",
            selected_surface_id,
            selected_surface_type,
            selected_surface_area,
            height);
    }
    else if (shell_mode)
    {
        fprintf(out,
            ",\"removed_surface_id\":%d,\"surface_type_code\":%d,"
            "\"removed_surface_area\":%.17g,\"thickness\":%.15g,"
            "\"shell_side\":\"inside\"",
            selected_surface_id,
            selected_surface_type,
            selected_surface_area,
            height);
    }
    else if (chamfer_mode)
    {
        fprintf(out,
            ",\"edge_id\":%d,\"edge_type_code\":%d,"
            "\"edge_length\":%.17g,\"distance\":%.15g",
            selected_edge_id,
            selected_edge_type,
            selected_edge_length,
            height);
    }
    else if (round_mode)
    {
        fprintf(out,
            ",\"edge_id\":%d,\"edge_type_code\":%d,"
            "\"edge_length\":%.17g,\"radius\":%.15g",
            selected_edge_id,
            selected_edge_type,
            selected_edge_length,
            height);
    }
    else if (sweep_mode)
    {
        fprintf(out,
            ",\"trajectory_edge_id\":%d,\"trajectory_edge_type_code\":%d,"
            "\"trajectory_edge_length\":%.17g,\"section_radius\":%.15g",
            selected_edge_id,
            selected_edge_type,
            selected_edge_length,
            height);
    }
    fprintf(out,
        ",\"operation\":\"%s\",\"profile\":\"%s\","
        "\"width\":%.15g,\"height\":%.15g,\"depth\":%.15g,"
        "\"direction_side\":%d,\"created_feature_id\":%d,"
        "\"created_feature_type_code\":%d,\"created_feature_status\":%d,"
        "\"source_feature_count\":%d,\"copy_feature_count\":%d,"
        "\"source_volume\":%.17g,\"copy_volume\":%.17g,\"%s\":%.17g,"
        "\"creation_error_count\":%d,\"regenerate_status\":%d,"
        "\"regenerate_attempts\":%d,\"saved_file\":",
        mirror_mode
            ? "part_mirror"
            : (draft_mode
            ? "draft"
            : (shell_mode
                ? "shell"
            : (chamfer_mode
                ? "chamfer"
            : (round_mode
                ? "round"
                : (sweep_mode
                ? (remove_material ? "sweep_cut" : "sweep_add")
            : (revolve_mode
                ? (remove_material ? "revolve_cut" : "revolve_add")
                : (remove_material ? "cut" : "protrusion"))))))),
        mirror_mode
            ? "whole_part_mirror"
            : (draft_mode
            ? "constant_angle_plane_hinge_draft"
            : (shell_mode
                ? "inside_shell_remove_surface"
            : (chamfer_mode
                ? "equal_distance_edge_chamfer"
            : (round_mode
                ? "constant_radius_edge_round"
                : (sweep_mode
                ? "origin_centered_circle"
            : (revolve_mode
                ? "centered_axial_rectangle"
                : (circle_mode ? "centered_circle" : "centered_rectangle"))))))),
        width,
        height,
        depth,
        direction_side,
        created_feature.id,
        created_feature_type,
        created_feature_status,
        source_feature_count,
        copy_feature_count,
        source_mass_property.volume,
        copy_mass_property.volume,
        (draft_mode || mirror_mode)
            ? "volume_change"
            : (shell_mode
                ? "removed_volume"
            : ((round_mode || chamfer_mode)
            ? "volume_change"
            : (remove_material ? "removed_volume" : "added_volume"))),
        volume_delta,
        creation_error_count,
        regenerate_status,
        regenerate_attempts);
    write_wide_json_string(out, saved_path);
    fputs("}\n", out);
    exit_code = 0;

cleanup:
    if (directory_changed)
        ProDirectoryChange(original_directory);
    if (copy_created)
        ProMdlErase(copy_model);
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
