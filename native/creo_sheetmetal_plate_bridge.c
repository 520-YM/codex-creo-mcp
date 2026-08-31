#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#include <io.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProSolidBody.h>
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
#include <ProFlatSrf.h>
#include <ProDtmCrv.h>
#include <ProStdSection.h>
#include <ProSection.h>
#include <ProSecerror.h>
#include <ProArray.h>
#include <ProSheetmetal.h>
#include <ProDimension.h>
#include <ProUtil.h>

static wchar_t trace_path[PRO_PATH_SIZE * 2] = L"";
static wchar_t diagnostic_tree_path[PRO_PATH_SIZE * 2] = L"";

static void trace_stage_write(const char *stage)
{
    FILE *trace = NULL;
    if (trace_path[0] == L'\0')
        return;
    if (_wfopen_s(&trace, trace_path, L"wb") == 0 && trace != NULL)
    {
        fputs(stage, trace);
        fputc('\n', trace);
        fclose(trace);
    }
}

static void trace_errorlist_write(
    const char *stage,
    ProError status,
    ProErrorlist *errors)
{
    FILE *trace = NULL;
    int index;
    if (trace_path[0] == L'\0')
        return;
    if (_wfopen_s(&trace, trace_path, L"wb") != 0 || trace == NULL)
        return;
    fprintf(trace,
        "%s\nstatus=%d\nerror_number=%d\n",
        stage,
        status,
        errors == NULL ? 0 : errors->error_number);
    if (errors != NULL)
    {
        for (index = 0; index < errors->error_number; ++index)
        {
            fprintf(trace,
                "error[%d].item_id=%d item_type=%d error=%d\n",
                index,
                errors->error_list[index].err_item_id,
                errors->error_list[index].err_item_type,
                errors->error_list[index].error);
        }
    }
    fclose(trace);
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
        parsed <= 0.0 || parsed > maximum)
        return 0;
    *value = parsed;
    return 1;
}

static int nearly_equal(double actual, double expected, double relative_tolerance)
{
    double tolerance = fabs(expected) * relative_tolerance;
    if (tolerance < 1.0e-5)
        tolerance = 1.0e-5;
    return fabs(actual - expected) <= tolerance;
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

static ProError add_integer_element(
    ProElement parent,
    ProElemId element_id,
    int value)
{
    ProError status;
    ProElement element = NULL;
    status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementIntegerSet(element, value);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementAdd(parent, NULL, element);
    if (status != PRO_TK_NO_ERROR)
        ProElementFree(&element);
    return status;
}

static ProError add_double_element(
    ProElement parent,
    ProElemId element_id,
    double value)
{
    ProError status;
    ProElement element = NULL;
    status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementDecimalsSet(element, 6);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementDoubleSet(element, value);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementAdd(parent, NULL, element);
    if (status != PRO_TK_NO_ERROR)
        ProElementFree(&element);
    return status;
}

static ProError add_wstring_element(
    ProElement parent,
    ProElemId element_id,
    const wchar_t *value)
{
    ProError status;
    ProElement element = NULL;
    status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementWstringSet(element, (wchar_t *)value);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementAdd(parent, NULL, element);
    if (status != PRO_TK_NO_ERROR)
        ProElementFree(&element);
    return status;
}

static ProError add_reference_element(
    ProElement parent,
    ProElemId element_id,
    ProReference reference)
{
    ProError status;
    ProElement element = NULL;
    status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementReferenceSet(element, reference);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementAdd(parent, NULL, element);
    if (status != PRO_TK_NO_ERROR)
        ProElementFree(&element);
    return status;
}

static ProError add_compound_element(
    ProElement parent,
    ProElemId element_id,
    ProElement *element)
{
    ProError status;
    *element = NULL;
    status = ProElementAlloc(element_id, element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElemtreeElementAdd(parent, NULL, *element);
    if (status != PRO_TK_NO_ERROR)
    {
        ProElementFree(element);
        *element = NULL;
    }
    return status;
}

static ProError element_by_ids_get(
    ProElement tree,
    const ProElemId *ids,
    int count,
    ProElement *element)
{
    ProError status;
    ProElempath path = NULL;
    ProElempathItem items[3];
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
    ProSection *section,
    double length,
    double width,
    const char **failure_stage)
{
    Pro2dLinedef line;
    ProError status;
    ProWSecerror autodim_errors = NULL;
    ProWSecerror regenerate_errors = NULL;
    int entity_id;
    double half_length = length / 2.0;
    double half_width = width / 2.0;

    *failure_stage = "section_2d_alloc";
    status = ProSection2DAlloc(section);
    if (status != PRO_TK_NO_ERROR) return status;
    status = ProSecerrorAlloc(&autodim_errors);
    if (status != PRO_TK_NO_ERROR) return status;
    status = ProSecerrorAlloc(&regenerate_errors);
    if (status != PRO_TK_NO_ERROR)
    {
        ProSecerrorFree(&autodim_errors);
        return status;
    }

    line.type = PRO_2D_LINE;
    line.end1[0] = -half_length;
    line.end1[1] = -half_width;
    line.end2[0] = half_length;
    line.end2[1] = -half_width;
    *failure_stage = "rectangle_line_1_add";
    status = ProSectionEntityAdd(*section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR) goto cleanup;

    line.end1[0] = half_length;
    line.end1[1] = -half_width;
    line.end2[0] = half_length;
    line.end2[1] = half_width;
    *failure_stage = "rectangle_line_2_add";
    status = ProSectionEntityAdd(*section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR) goto cleanup;

    line.end1[0] = half_length;
    line.end1[1] = half_width;
    line.end2[0] = -half_length;
    line.end2[1] = half_width;
    *failure_stage = "rectangle_line_3_add";
    status = ProSectionEntityAdd(*section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR) goto cleanup;

    line.end1[0] = -half_length;
    line.end1[1] = half_width;
    line.end2[0] = -half_length;
    line.end2[1] = -half_width;
    *failure_stage = "rectangle_line_4_add";
    status = ProSectionEntityAdd(*section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR) goto cleanup;

    *failure_stage = "section_epsilon_set";
    status = ProSectionEpsilonSet(*section, 0.1);
    if (status != PRO_TK_NO_ERROR) goto cleanup;
    *failure_stage = "section_autodim";
    status = ProSectionAutodim(*section, &autodim_errors);
    if (status != PRO_TK_NO_ERROR) goto cleanup;
    *failure_stage = "section_regenerate";
    status = ProSectionRegenerate(*section, &regenerate_errors);

cleanup:
    ProSecerrorFree(&regenerate_errors);
    ProSecerrorFree(&autodim_errors);
    return status;
}

static ProError closed_rectangle_section_build(
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
    status = ProSectionEntityAdd(
        section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;

    line.end1[0] = half_length;
    line.end1[1] = -half_width;
    line.end2[0] = half_length;
    line.end2[1] = half_width;
    *failure_stage = "rectangle_line_2_add";
    status = ProSectionEntityAdd(
        section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;

    line.end1[0] = half_length;
    line.end1[1] = half_width;
    line.end2[0] = -half_length;
    line.end2[1] = half_width;
    *failure_stage = "rectangle_line_3_add";
    status = ProSectionEntityAdd(
        section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;

    line.end1[0] = -half_length;
    line.end1[1] = half_width;
    line.end2[0] = -half_length;
    line.end2[1] = -half_width;
    *failure_stage = "rectangle_line_4_add";
    status = ProSectionEntityAdd(
        section, (Pro2dEntdef *)&line, &entity_id);
    if (status != PRO_TK_NO_ERROR)
        return status;

    *failure_stage = "rectangle_intent_manager_enable";
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
        return status;
    *failure_stage = "rectangle_intent_manager_disable";
    return ProSectionIntentManagerModeSet(section, PRO_B_FALSE);
}

static ProError create_profile_sketch(
    ProMdl model,
    const wchar_t *profile_name,
    double length,
    double width,
    ProFeature *created_profile,
    int *creation_error_count,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement feature_tree = NULL;
    ProElement standard_section = NULL;
    ProElement setup_plane = NULL;
    ProElement extracted_tree = NULL;
    ProElement sketch_element = NULL;
    ProElemId sketch_path_ids[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProModelitem front_item;
    ProModelitem top_item;
    ProModelitem model_item;
    ProSelection front_selection = NULL;
    ProSelection top_selection = NULL;
    ProSelection model_selection = NULL;
    ProReference front_reference = NULL;
    ProReference top_reference = NULL;
    ProSection section = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    int incomplete_profile_created = 0;

#define RUN_PROFILE_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        trace_stage_write(label); \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    created_profile->id = -1;
    *creation_error_count = 0;
    RUN_PROFILE_STAGE("profile_front_plane_lookup",
        ProModelitemByNameInit(model, PRO_SURFACE, L"FRONT", &front_item));
    RUN_PROFILE_STAGE("profile_top_plane_lookup",
        ProModelitemByNameInit(model, PRO_SURFACE, L"TOP", &top_item));
    RUN_PROFILE_STAGE("profile_front_plane_selection",
        ProSelectionAlloc(NULL, &front_item, &front_selection));
    RUN_PROFILE_STAGE("profile_top_plane_selection",
        ProSelectionAlloc(NULL, &top_item, &top_selection));
    RUN_PROFILE_STAGE("profile_front_plane_reference",
        ProSelectionToReference(front_selection, &front_reference));
    RUN_PROFILE_STAGE("profile_top_plane_reference",
        ProSelectionToReference(top_selection, &top_reference));
    RUN_PROFILE_STAGE("profile_feature_tree_alloc",
        ProElementAlloc(PRO_E_FEATURE_TREE, &feature_tree));
    RUN_PROFILE_STAGE("profile_feature_type",
        add_integer_element(feature_tree, PRO_E_FEATURE_TYPE, PRO_FEAT_CURVE));
    RUN_PROFILE_STAGE("profile_curve_type",
        add_integer_element(
            feature_tree, PRO_E_CURVE_TYPE, PRO_CURVE_TYPE_SKETCHED));
    RUN_PROFILE_STAGE("profile_feature_name",
        add_wstring_element(
            feature_tree, PRO_E_STD_FEATURE_NAME, profile_name));
    RUN_PROFILE_STAGE("profile_standard_section",
        add_compound_element(
            feature_tree, PRO_E_STD_SECTION, &standard_section));
    RUN_PROFILE_STAGE("profile_setup_plane",
        add_compound_element(
            standard_section, PRO_E_STD_SEC_SETUP_PLANE, &setup_plane));
    RUN_PROFILE_STAGE("profile_section_plane",
        add_reference_element(
            setup_plane, PRO_E_STD_SEC_PLANE, front_reference));
    RUN_PROFILE_STAGE("profile_section_view_direction",
        add_integer_element(
            setup_plane,
            PRO_E_STD_SEC_PLANE_VIEW_DIR,
            PRO_SEC_VIEW_DIR_SIDE_ONE));
    RUN_PROFILE_STAGE("profile_section_orientation_direction",
        add_integer_element(
            setup_plane,
            PRO_E_STD_SEC_PLANE_ORIENT_DIR,
            PRO_SEC_ORIENT_DIR_UP));
    RUN_PROFILE_STAGE("profile_section_orientation_reference",
        add_reference_element(
            setup_plane,
            PRO_E_STD_SEC_PLANE_ORIENT_REF,
            top_reference));
    RUN_PROFILE_STAGE("profile_model_item",
        ProMdlToModelitem(model, &model_item));
    RUN_PROFILE_STAGE("profile_model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_PROFILE_STAGE("profile_incomplete_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_INCOMPLETE_FEAT;
    *failure_stage = "profile_incomplete_feature_create";
    trace_stage_write(*failure_stage);
    status = ProFeatureWithoptionsCreate(
        model_selection,
        feature_tree,
        options,
        PRO_REGEN_NO_FLAGS,
        created_profile,
        &errors);
    *creation_error_count = errors.error_number;
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    incomplete_profile_created = 1;
    ProArrayFree((ProArray *)&options);
    options = NULL;
    if (errors.error_list != NULL)
    {
        ProArrayFree((ProArray *)&errors.error_list);
        errors.error_list = NULL;
        errors.error_number = 0;
    }
    RUN_PROFILE_STAGE("profile_feature_tree_extract",
        ProFeatureElemtreeExtract(
            created_profile,
            NULL,
            PRO_FEAT_EXTRACT_NO_OPTS,
            &extracted_tree));
    RUN_PROFILE_STAGE("profile_sketch_element_get",
        element_by_ids_get(
            extracted_tree, sketch_path_ids, 2, &sketch_element));
    RUN_PROFILE_STAGE("profile_section_handle_get",
        ProElementSpecialvalueGet(
            sketch_element, NULL, (ProAppData *)&section));
    RUN_PROFILE_STAGE("profile_closed_rectangle_build",
        closed_rectangle_section_build(
            section, length, width, failure_stage));
    RUN_PROFILE_STAGE("profile_redefine_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_DEFINE_MISS_ELEMS;
    *failure_stage = "profile_feature_redefine";
    trace_stage_write(*failure_stage);
    status = ProFeatureWithoptionsRedefine(
        NULL,
        created_profile,
        extracted_tree,
        options,
        PRO_REGEN_NO_FLAGS,
        &errors);
    *creation_error_count += errors.error_number;
    trace_errorlist_write(*failure_stage, status, &errors);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    *failure_stage = "profile_regenerate";
    trace_stage_write(*failure_stage);
    status = ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (extracted_tree != NULL && incomplete_profile_created)
        ProFeatureElemtreeFree(created_profile, extracted_tree);
    if (feature_tree != NULL)
        ProElementFree(&feature_tree);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
    if (top_reference != NULL)
        ProReferenceFree(top_reference);
    if (front_reference != NULL)
        ProReferenceFree(front_reference);
    if (top_selection != NULL)
        ProSelectionFree(&top_selection);
    if (front_selection != NULL)
        ProSelectionFree(&front_selection);
    if (status != PRO_TK_NO_ERROR && incomplete_profile_created)
    {
        int profile_id = created_profile->id;
        ProFeatureDeleteOptions delete_option = PRO_FEAT_DELETE_NO_OPTS;
        ProFeatureDelete(
            (ProSolid)model,
            &profile_id,
            1,
            &delete_option,
            1);
        ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
        created_profile->id = -1;
    }
#undef RUN_PROFILE_STAGE
    return status;
}

static ProError create_unattached_wall(
    ProMdl model,
    const wchar_t *profile_name,
    const wchar_t *feature_name,
    double length,
    double width,
    double thickness,
    ProFeature *created_profile,
    ProFeature *created_feature,
    int *profile_created,
    int *creation_error_count,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement feature_tree = NULL;
    ProElement standard_section = NULL;
    ProModelitem model_item;
    ProSelection model_selection = NULL;
    ProSelection profile_selection = NULL;
    ProSelection *sketch_array = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    int profile_creation_errors = 0;

#define RUN_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        trace_stage_write(label); \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    *profile_created = 0;
    created_profile->id = -1;
    created_feature->id = -1;
    *failure_stage = "initialize";
    status = create_profile_sketch(
        model,
        profile_name,
        length,
        width,
        created_profile,
        &profile_creation_errors,
        failure_stage);
    *creation_error_count += profile_creation_errors;
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    *profile_created = 1;
    RUN_STAGE("feature_tree_alloc",
        ProElementAlloc(PRO_E_FEATURE_TREE, &feature_tree));
    RUN_STAGE("feature_type",
        add_integer_element(
            feature_tree, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM_SURF));
    RUN_STAGE("feature_form",
        add_integer_element(feature_tree, PRO_E_FEATURE_FORM, PRO_FLAT));
    RUN_STAGE("feature_name",
        add_wstring_element(feature_tree, PRO_E_STD_FEATURE_NAME, feature_name));
    RUN_STAGE("standard_section",
        add_compound_element(feature_tree, PRO_E_STD_SECTION, &standard_section));
    RUN_STAGE("is_unattached_wall",
        add_integer_element(
            feature_tree, PRO_E_IS_UNATTACHED_WALL, PRO_B_TRUE));
    RUN_STAGE("direction",
        add_integer_element(
            feature_tree, PRO_E_STD_DIRECTION, PRO_EXT_CR_IN_SIDE_TWO));
    RUN_STAGE("sheetmetal_thickness",
        add_double_element(feature_tree, PRO_E_STD_SMT_THICKNESS, thickness));
    RUN_STAGE("sheetmetal_swap_driving_side",
        add_integer_element(
            feature_tree, PRO_E_STD_SMT_SWAP_DRV_SIDE, PRO_B_FALSE));

    RUN_STAGE("model_item", ProMdlToModelitem(model, &model_item));
    RUN_STAGE("model_selection",
        ProSelectionAlloc(NULL, &model_item, &model_selection));
    RUN_STAGE("profile_selection",
        ProSelectionAlloc(
            NULL, (ProModelitem *)created_profile, &profile_selection));
    RUN_STAGE("sketch_array_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProSelection),
            1,
            (ProArray *)&sketch_array));
    RUN_STAGE("profile_selection_copy",
        ProSelectionCopy(profile_selection, &sketch_array[0]));
    RUN_STAGE("wall_options_alloc",
        ProArrayAlloc(
            1,
            sizeof(ProFeatureCreateOptions),
            1,
            (ProArray *)&options));
    options[0] = PRO_FEAT_CR_NO_OPTS;
    *failure_stage = "planar_wall_create";
    trace_stage_write(*failure_stage);
    status = ProFeatureSketchedWithOptionsCreate(
        model_selection,
        feature_tree,
        options,
        sketch_array,
        PRO_REGEN_NO_FLAGS,
        created_feature,
        &errors);
    *creation_error_count += errors.error_number;
    trace_errorlist_write(*failure_stage, status, &errors);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    *failure_stage = "planar_wall_regenerate";
    trace_stage_write(*failure_stage);
    status = ProSolidRegenerate((ProSolid)model, PRO_REGEN_FORCE_REGEN);

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (sketch_array != NULL)
    {
        if (sketch_array[0] != NULL)
            ProSelectionFree(&sketch_array[0]);
        ProArrayFree((ProArray *)&sketch_array);
    }
    if (profile_selection != NULL)
        ProSelectionFree(&profile_selection);
    if (feature_tree != NULL)
        ProElementFree(&feature_tree);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
    if (status != PRO_TK_NO_ERROR && created_feature->id > 0)
    {
        int feature_id = created_feature->id;
        ProFeatureDeleteOptions delete_option = PRO_FEAT_DELETE_NO_OPTS;
        ProFeatureDelete(
            (ProSolid)model,
            &feature_id,
            1,
            &delete_option,
            1);
        created_feature->id = -1;
    }
    if (status != PRO_TK_NO_ERROR && *profile_created)
    {
        int profile_id = created_profile->id;
        ProFeatureDeleteOptions delete_option = PRO_FEAT_DELETE_NO_OPTS;
        ProFeatureDelete(
            (ProSolid)model,
            &profile_id,
            1,
            &delete_option,
            1);
        ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
        created_profile->id = -1;
        *profile_created = 0;
    }
#undef RUN_STAGE
    return status;
}

static void sort3(double values[3])
{
    double temporary;
    if (values[0] > values[1])
    {
        temporary = values[0]; values[0] = values[1]; values[1] = temporary;
    }
    if (values[1] > values[2])
    {
        temporary = values[1]; values[1] = values[2]; values[2] = temporary;
    }
    if (values[0] > values[1])
    {
        temporary = values[0]; values[0] = values[1]; values[1] = temporary;
    }
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError regenerate_status = PRO_TK_NO_ERROR;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProMdl template_model = NULL;
    ProMdlName expected_name;
    ProMdlName actual_name;
    ProName feature_name;
    ProName profile_name;
    ProMdlType model_type;
    ProMdlsubtype model_subtype;
    ProModelitem feature_guard;
    ProModelitem profile_guard;
    ProFeature created_profile;
    ProFeature created_feature;
    ProFeattype feature_type = -1;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProDimension thickness_dimension;
    double old_thickness = 0.0;
    double actual_thickness = 0.0;
    double length;
    double width;
    double thickness;
    ProMassProperty mass_property;
    ProPath working_directory;
    ProPath saved_path;
    ProPath startup_model_path;
    ProPath startup_directory;
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
    double extents[3];
    int creation_error_count = 0;
    int regenerate_attempts = 0;
    int connected = 0;
    int started_new_session = 0;
    int created_new_model = 0;
    int profile_created = 0;
    int feature_created = 0;
    int thickness_changed = 0;
    int saved = 0;
    int window_id = -1;
    int exit_code = 1;
    const char *failure_stage = "not_started";

    if (argc != 7 && argc != 8 && argc != 9)
    {
        fwprintf(stderr,
            L"Usage: creo_sheetmetal_plate_bridge <result.json> <expected_model> "
            L"<feature_name> <length_mm> <width_mm> <thickness_mm> "
            L"[model_file_to_load_in_new_session | "
            L"new_model_directory sheetmetal_template]\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    SetHandleInformation(
        (HANDLE)_get_osfhandle(_fileno(out)),
        HANDLE_FLAG_INHERIT,
        0);
    _snwprintf_s(
        trace_path,
        sizeof(trace_path) / sizeof(trace_path[0]),
        _TRUNCATE,
        L"%ls.stage",
        argv[1]);
    _snwprintf_s(
        diagnostic_tree_path,
        sizeof(diagnostic_tree_path) / sizeof(diagnostic_tree_path[0]),
        _TRUNCATE,
        L"%ls.tree.xml",
        argv[1]);
    if (!parse_positive_dimension(argv[4], 100000.0, &length) ||
        !parse_positive_dimension(argv[5], 100000.0, &width) ||
        !parse_positive_dimension(argv[6], 1000.0, &thickness))
    {
        exit_code = write_error(out, "dimension_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    wcsncpy_s(expected_name,
        sizeof(expected_name) / sizeof(expected_name[0]), argv[2], _TRUNCATE);
    wcsncpy_s(feature_name,
        sizeof(feature_name) / sizeof(feature_name[0]), argv[3], _TRUNCATE);
    _snwprintf_s(
        profile_name,
        sizeof(profile_name) / sizeof(profile_name[0]),
        _TRUNCATE,
        L"%.28ls_P",
        feature_name);

    status = ProEngineerConnect(
        "", "", "", "", PRO_B_TRUE, 20,
        &random_choice, &process_handle);
    if (status != PRO_TK_NO_ERROR)
    {
        if (argc != 8 && argc != 9)
        {
            exit_code = write_error(out, "connect", status);
            goto done;
        }
        const char *parametric_bat = getenv("CREO_PARAMETRIC_BAT");
        if (parametric_bat == NULL || parametric_bat[0] == '\0')
        {
            exit_code = write_error(out, "missing_CREO_PARAMETRIC_BAT", PRO_TK_BAD_INPUTS);
            goto done;
        }
        status = ProEngineerConnectionStart(parametric_bat, "", &process_handle);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "start_new_creo_session", status);
            goto done;
        }
        started_new_session = 1;
    }
    connected = 1;
    if (started_new_session || argc == 9)
    {
        if (argc == 8)
        {
            wchar_t *separator;
            wcsncpy_s(
                startup_model_path,
                sizeof(startup_model_path) / sizeof(startup_model_path[0]),
                argv[7],
                _TRUNCATE);
            if (GetFileAttributesW(startup_model_path) == INVALID_FILE_ATTRIBUTES)
            {
                exit_code = write_error(
                    out, "startup_model_file", PRO_TK_E_NOT_FOUND);
                goto cleanup;
            }
            wcsncpy_s(
                startup_directory,
                sizeof(startup_directory) / sizeof(startup_directory[0]),
                startup_model_path,
                _TRUNCATE);
            separator = wcsrchr(startup_directory, L'\\');
            if (separator == NULL)
            {
                exit_code = write_error(
                    out, "startup_model_directory", PRO_TK_BAD_INPUTS);
                goto cleanup;
            }
            *separator = L'\0';
        }
        else
        {
            wcsncpy_s(
                startup_directory,
                sizeof(startup_directory) / sizeof(startup_directory[0]),
                argv[7],
                _TRUNCATE);
            wcsncpy_s(
                startup_model_path,
                sizeof(startup_model_path) / sizeof(startup_model_path[0]),
                argv[8],
                _TRUNCATE);
            if (GetFileAttributesW(startup_directory) == INVALID_FILE_ATTRIBUTES)
            {
                exit_code = write_error(
                    out, "new_model_directory", PRO_TK_INVALID_DIR);
                goto cleanup;
            }
            if (GetFileAttributesW(startup_model_path) == INVALID_FILE_ATTRIBUTES)
            {
                exit_code = write_error(
                    out, "sheetmetal_template", PRO_TK_E_NOT_FOUND);
                goto cleanup;
            }
            if (find_latest_saved_model(
                    startup_directory,
                    expected_name,
                    saved_path,
                    sizeof(saved_path) / sizeof(saved_path[0])))
            {
                exit_code = write_error(out, "refuse_overwrite", PRO_TK_E_FOUND);
                goto cleanup;
            }
        }
        status = ProDirectoryChange(startup_directory);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "startup_working_directory", status);
            goto cleanup;
        }
        if (argc == 8)
        {
            status = ProMdlFiletypeLoad(
                startup_model_path, PRO_MDLFILE_PART, PRO_B_FALSE, &model);
            if (status != PRO_TK_NO_ERROR)
            {
                exit_code = write_error(out, "startup_model_load", status);
                goto cleanup;
            }
            status = ProMdlDisplay(model);
            if (status != PRO_TK_NO_ERROR)
            {
                exit_code = write_error(out, "startup_model_display", status);
                goto cleanup;
            }
        }
        else
        {
            status = ProMdlFiletypeLoad(
                startup_model_path,
                PRO_MDLFILE_PART,
                PRO_B_FALSE,
                &template_model);
            if (status != PRO_TK_NO_ERROR)
            {
                exit_code = write_error(out, "sheetmetal_template_load", status);
                goto cleanup;
            }
            status = ProMdlnameCopy(template_model, expected_name, &model);
            if (status != PRO_TK_NO_ERROR)
            {
                exit_code = write_error(out, "sheetmetal_model_create", status);
                goto cleanup;
            }
            created_new_model = 1;
            ProMdlErase(template_model);
            template_model = NULL;
        }
    }
    else
    {
        status = ProMdlCurrentGet(&model);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "current_model", status);
            goto cleanup;
        }
    }
    status = ProMdlNameGet(model, actual_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_name, expected_name) != 0)
    {
        exit_code = write_error(out, "model_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART)
    {
        exit_code = write_error(out, "part_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProMdlSubtypeGet(model, &model_subtype);
    if (status != PRO_TK_NO_ERROR || model_subtype != PROMDLSTYPE_PART_SHEETMETAL)
    {
        exit_code = write_error(out, "sheetmetal_subtype_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(model, PRO_FEATURE, feature_name, &feature_guard);
    if (status == PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_name_guard", PRO_TK_E_FOUND);
        goto cleanup;
    }
    if (status != PRO_TK_E_NOT_FOUND)
    {
        exit_code = write_error(out, "feature_name_guard", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        model, PRO_FEATURE, profile_name, &profile_guard);
    if (status == PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "profile_name_guard", PRO_TK_E_FOUND);
        goto cleanup;
    }
    if (status != PRO_TK_E_NOT_FOUND)
    {
        exit_code = write_error(out, "profile_name_guard", status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    status = ProSmtPartThicknessGet((ProPart)model, &thickness_dimension);
    if (status == PRO_TK_NO_ERROR)
    {
        status = ProDimensionValueGet(&thickness_dimension, &old_thickness);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "sheetmetal_thickness_value_get", status);
            goto cleanup;
        }
        if (!nearly_equal(old_thickness, thickness, 1.0e-9))
        {
            status = ProDimensionValueSet(&thickness_dimension, thickness);
            if (status != PRO_TK_NO_ERROR)
            {
                exit_code = write_error(out, "sheetmetal_thickness_value_set", status);
                goto cleanup;
            }
            thickness_changed = 1;
            status = ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
            if (status != PRO_TK_NO_ERROR && status != PRO_TK_REGEN_AGAIN)
            {
                exit_code = write_error(out, "thickness_regenerate", status);
                goto cleanup;
            }
        }
    }
    else if (status == PRO_TK_E_NOT_FOUND)
    {
        /* An empty sheetmetal template has no thickness dimension yet.
           The first unattached wall creates it from PRO_E_STD_SMT_THICKNESS. */
        old_thickness = thickness;
    }
    else
    {
        exit_code = write_error(out, "sheetmetal_thickness_get", status);
        goto cleanup;
    }

    status = create_unattached_wall(
        model,
        profile_name,
        feature_name,
        length,
        width,
        thickness,
        &created_profile,
        &created_feature,
        &profile_created,
        &creation_error_count,
        &failure_stage);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, failure_stage, status);
        goto cleanup;
    }
    feature_created = 1;
    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)model, PRO_REGEN_FORCE_REGEN);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "regenerate", regenerate_status);
        goto cleanup;
    }
    status = ProFeatureTypeGet(&created_feature, &feature_type);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_type_get", status);
        goto cleanup;
    }
    if (feature_type != PRO_FEAT_DATUM_SURF)
    {
        exit_code = write_error(out, "feature_type_guard", PRO_TK_INVALID_TYPE);
        goto cleanup;
    }
    status = ProFeatureStatusGet(&created_feature, &feature_status);
    if (status != PRO_TK_NO_ERROR || feature_status != PRO_FEAT_ACTIVE)
    {
        trace_errorlist_write(
            "feature_status_guard",
            status == PRO_TK_NO_ERROR ? (ProError)feature_status : status,
            NULL);
        exit_code = write_error(out, "feature_status_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(model, PRO_FEATURE, feature_name, &feature_guard);
    if (status != PRO_TK_NO_ERROR || feature_guard.id != created_feature.id)
    {
        exit_code = write_error(out, "feature_name_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProMdlSubtypeGet(model, &model_subtype);
    if (status != PRO_TK_NO_ERROR || model_subtype != PROMDLSTYPE_PART_SHEETMETAL)
    {
        exit_code = write_error(out, "sheetmetal_subtype_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProSmtPartThicknessGet((ProPart)model, &thickness_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&thickness_dimension, &actual_thickness);
    if (status != PRO_TK_NO_ERROR ||
        !nearly_equal(actual_thickness, thickness, 1.0e-8))
    {
        exit_code = write_error(out, "sheetmetal_thickness_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &mass_property);
    if (status != PRO_TK_NO_ERROR ||
        !nearly_equal(mass_property.volume, length * width * thickness, 1.0e-5))
    {
        exit_code = write_error(out, "volume_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProSolidOutlineCompute(
        (ProSolid)model,
        outline_matrix,
        outline_excludes,
        (int)(sizeof(outline_excludes) / sizeof(outline_excludes[0])),
        outline);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "outline_compute", status);
        goto cleanup;
    }
    extents[0] = outline[1][0] - outline[0][0];
    extents[1] = outline[1][1] - outline[0][1];
    extents[2] = outline[1][2] - outline[0][2];
    sort3(extents);
    {
        double expected_extents[3] = {thickness, width, length};
        sort3(expected_extents);
        if (!nearly_equal(extents[0], expected_extents[0], 1.0e-6) ||
            !nearly_equal(extents[1], expected_extents[1], 1.0e-6) ||
            !nearly_equal(extents[2], expected_extents[2], 1.0e-6))
        {
            exit_code = write_error(out, "outline_guard", PRO_TK_GENERAL_ERROR);
            goto cleanup;
        }
    }

    status = ProMdlSave(model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save", status);
        goto cleanup;
    }
    saved = 1;
    if (!find_latest_saved_model(
            working_directory,
            actual_name,
            saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    {
        exit_code = write_error(out, "saved_file_guard", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    if (created_new_model)
        ProMdlDisplay(model);
    if (ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
    {
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"created_new_model\":", out);
    fputs(created_new_model ? "true" : "false", out);
    fputs(",\"model\":", out);
    write_wide_json_string(out, actual_name);
    fputs(",\"model_subtype\":\"sheetmetal\",\"profile_name\":", out);
    write_wide_json_string(out, profile_name);
    fprintf(out, ",\"profile_id\":%d,\"feature_name\":", created_profile.id);
    write_wide_json_string(out, feature_name);
    fprintf(out,
        ",\"feature_id\":%d,\"feature_type_code\":%d,"
        "\"feature_status\":%d,\"length_mm\":%.15g,"
        "\"width_mm\":%.15g,\"thickness_mm\":%.15g,"
        "\"volume_mm3\":%.15g,\"sorted_extents_mm\":[%.15g,%.15g,%.15g],"
        "\"creation_error_count\":%d,\"regenerate_status\":%d,"
        "\"saved_file\":",
        created_feature.id,
        feature_type,
        feature_status,
        length,
        width,
        actual_thickness,
        mass_property.volume,
        extents[0], extents[1], extents[2],
        creation_error_count,
        regenerate_status);
    write_wide_json_string(out, saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0 && feature_created && !saved && model != NULL)
    {
        int feature_id = created_feature.id;
        ProFeatureDeleteOptions delete_option = PRO_FEAT_DELETE_NO_OPTS;
        ProFeatureDelete(
            (ProSolid)model,
            &feature_id,
            1,
            &delete_option,
            1);
        ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
    }
    if (exit_code != 0 && profile_created && !saved && model != NULL)
    {
        int profile_id = created_profile.id;
        ProFeatureDeleteOptions delete_option = PRO_FEAT_DELETE_NO_OPTS;
        ProFeatureDelete(
            (ProSolid)model,
            &profile_id,
            1,
            &delete_option,
            1);
        ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
    }
    if (exit_code != 0 && thickness_changed && !saved && model != NULL)
    {
        if (ProSmtPartThicknessGet((ProPart)model, &thickness_dimension) ==
            PRO_TK_NO_ERROR)
        {
            ProDimensionValueSet(&thickness_dimension, old_thickness);
            ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
        }
    }
    if (exit_code != 0 && created_new_model && !saved && model != NULL)
    {
        ProMdlErase(model);
        model = NULL;
    }
    if (template_model != NULL)
        ProMdlErase(template_model);
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
