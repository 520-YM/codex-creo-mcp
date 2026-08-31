#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProWindows.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProSelection.h>
#include <ProReference.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProElemId.h>
#include <ProDtmCrv.h>
#include <ProStdSection.h>
#include <ProSection.h>
#include <ProArray.h>

#define MAX_SKETCH_ENTITIES 32
#define PI_VALUE 3.14159265358979323846

typedef enum sketch_geometry_kind
{
    SKETCH_GEOMETRY_LINE = 1,
    SKETCH_GEOMETRY_CIRCLE = 2,
    SKETCH_GEOMETRY_ARC = 3
} SketchGeometryKind;

typedef struct sketch_geometry
{
    SketchGeometryKind kind;
    double values[5];
    int entity_id;
} SketchGeometry;

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

static int parse_finite_value(
    const wchar_t *text,
    double minimum,
    double maximum,
    double *value)
{
    wchar_t *end = NULL;
    double parsed;
    if (text == NULL || *text == L'\0')
        return 0;
    parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) ||
        parsed < minimum || parsed > maximum)
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
    ProElempathItem items[4];
    int index;
    if (count < 1 || count > 4)
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

static ProError add_sketch_entity(
    ProSection section,
    SketchGeometry *geometry)
{
    if (geometry->kind == SKETCH_GEOMETRY_LINE)
    {
        Pro2dLinedef line;
        line.type = PRO_2D_LINE;
        line.end1[0] = geometry->values[0];
        line.end1[1] = geometry->values[1];
        line.end2[0] = geometry->values[2];
        line.end2[1] = geometry->values[3];
        return ProSectionEntityAdd(
            section, (Pro2dEntdef *)&line, &geometry->entity_id);
    }
    if (geometry->kind == SKETCH_GEOMETRY_CIRCLE)
    {
        Pro2dCircledef circle;
        circle.type = PRO_2D_CIRCLE;
        circle.center[0] = geometry->values[0];
        circle.center[1] = geometry->values[1];
        circle.radius = geometry->values[2];
        return ProSectionEntityAdd(
            section, (Pro2dEntdef *)&circle, &geometry->entity_id);
    }
    if (geometry->kind == SKETCH_GEOMETRY_ARC)
    {
        Pro2dArcdef arc;
        arc.type = PRO_2D_ARC;
        arc.center[0] = geometry->values[0];
        arc.center[1] = geometry->values[1];
        arc.radius = geometry->values[2];
        arc.start_angle = geometry->values[3] * PI_VALUE / 180.0;
        arc.end_angle = geometry->values[4] * PI_VALUE / 180.0;
        return ProSectionEntityAdd(
            section, (Pro2dEntdef *)&arc, &geometry->entity_id);
    }
    return PRO_TK_INVALID_TYPE;
}

static ProError create_general_sketch(
    ProMdl model,
    const wchar_t *feature_name,
    const wchar_t *sketch_plane_name,
    const wchar_t *orientation_plane_name,
    int view_side,
    int orientation_direction,
    SketchGeometry *geometries,
    int geometry_count,
    ProFeature *created_feature,
    int *creation_error_count,
    int *section_entity_count,
    const char **failure_stage)
{
    ProError status = PRO_TK_NO_ERROR;
    ProElement feature_tree = NULL;
    ProElement standard_section = NULL;
    ProElement setup_plane = NULL;
    ProElement extracted_tree = NULL;
    ProElement sketch_element = NULL;
    ProElemId sketch_path_ids[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
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
    ProIntlist section_ids = NULL;
    int incomplete_feature_created = 0;
    int index;

#define RUN_STAGE(label, expression) \
    do { \
        *failure_stage = label; \
        status = (expression); \
        if (status != PRO_TK_NO_ERROR) goto cleanup; \
    } while (0)

    *creation_error_count = 0;
    *section_entity_count = 0;
    created_feature->id = -1;
    RUN_STAGE("sketch_plane_lookup",
        ProModelitemByNameInit(
            model, PRO_SURFACE, (wchar_t *)sketch_plane_name,
            &sketch_plane_item));
    RUN_STAGE("orientation_plane_lookup",
        ProModelitemByNameInit(
            model, PRO_SURFACE, (wchar_t *)orientation_plane_name,
            &orientation_plane_item));
    if (sketch_plane_item.id == orientation_plane_item.id)
    {
        status = PRO_TK_BAD_CONTEXT;
        *failure_stage = "distinct_plane_guard";
        goto cleanup;
    }
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
        add_integer_element(feature_tree, PRO_E_FEATURE_TYPE, PRO_FEAT_CURVE));
    RUN_STAGE("curve_type",
        add_integer_element(
            feature_tree, PRO_E_CURVE_TYPE, PRO_CURVE_TYPE_SKETCHED));
    RUN_STAGE("feature_name",
        add_wstring_element(
            feature_tree, PRO_E_STD_FEATURE_NAME, feature_name));
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
            setup_plane,
            PRO_E_STD_SEC_PLANE_VIEW_DIR,
            view_side == 1 ? PRO_SEC_VIEW_DIR_SIDE_ONE : PRO_SEC_VIEW_DIR_SIDE_TWO));
    RUN_STAGE("section_orientation_direction",
        add_integer_element(
            setup_plane,
            PRO_E_STD_SEC_PLANE_ORIENT_DIR,
            orientation_direction));
    RUN_STAGE("section_orientation_reference",
        add_reference_element(
            setup_plane,
            PRO_E_STD_SEC_PLANE_ORIENT_REF,
            orientation_plane_reference));

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

    for (index = 0; index < geometry_count; ++index)
    {
        *failure_stage = "section_entity_add";
        status = add_sketch_entity(section, &geometries[index]);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
    }
    RUN_STAGE("section_intent_manager_enable",
        ProSectionIntentManagerModeSet(section, PRO_B_TRUE));
    RUN_STAGE("section_intent_manager_disable",
        ProSectionIntentManagerModeSet(section, PRO_B_FALSE));
    RUN_STAGE("section_entity_ids_get",
        ProSectionEntityIdsGet(
            section, &section_ids, section_entity_count));
    if (*section_entity_count < geometry_count)
    {
        status = PRO_TK_GENERAL_ERROR;
        *failure_stage = "section_entity_count_guard";
        goto cleanup;
    }
    for (index = 0; index < geometry_count; ++index)
    {
        Pro2dEntdef *entity = NULL;
        Pro2dEntType expected_type = PRO_2D_LINE;
        if (geometries[index].kind == SKETCH_GEOMETRY_CIRCLE)
            expected_type = PRO_2D_CIRCLE;
        else if (geometries[index].kind == SKETCH_GEOMETRY_ARC)
            expected_type = PRO_2D_ARC;
        status = ProSectionEntityGet(
            section, geometries[index].entity_id, &entity);
        if (status != PRO_TK_NO_ERROR || entity == NULL ||
            entity->type != expected_type)
        {
            if (status == PRO_TK_NO_ERROR)
                status = PRO_TK_INVALID_TYPE;
            *failure_stage = "section_entity_type_guard";
            goto cleanup;
        }
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
    if (section_ids != NULL)
        ProArrayFree((ProArray *)&section_ids);
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

static const char *geometry_kind_name(SketchGeometryKind kind)
{
    if (kind == SKETCH_GEOMETRY_LINE)
        return "line";
    if (kind == SKETCH_GEOMETRY_CIRCLE)
        return "circle";
    if (kind == SKETCH_GEOMETRY_ARC)
        return "arc";
    return "unknown";
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
    ProMdlName expected_model_name;
    ProMdlName actual_model_name;
    ProName feature_name;
    ProName sketch_plane_name;
    ProName orientation_plane_name;
    ProMdlType model_type;
    ProModelitem existing_feature;
    ProFeature created_feature;
    ProFeattype feature_type = -1;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProPath working_directory;
    ProPath saved_path;
    ProMassProperty source_mass;
    ProMassProperty final_mass;
    SketchGeometry geometries[MAX_SKETCH_ENTITIES];
    const char *failure_stage = "input";
    int view_side;
    int orientation_direction;
    int geometry_count;
    int cursor;
    int index;
    int line_count = 0;
    int circle_count = 0;
    int arc_count = 0;
    int connected = 0;
    int feature_created = 0;
    int saved = 0;
    int creation_error_count = 0;
    int section_entity_count = 0;
    int regenerate_attempts = 0;
    int source_mass_status = PRO_TK_E_NOT_FOUND;
    int final_mass_status = PRO_TK_E_NOT_FOUND;
    int window_id = -1;
    int exit_code = 1;

    ZeroMemory(&created_feature, sizeof(created_feature));
    created_feature.id = -1;
    ZeroMemory(geometries, sizeof(geometries));
    if (argc < 13)
    {
        fwprintf(stderr,
            L"Usage: creo_general_sketch_bridge <result.json> "
            L"<expected_model> <feature_name> <sketch_plane> "
            L"<orientation_plane> <view_side> <orientation_direction> "
            L"<entity_count> (<line x1 y1 x2 y2>|"
            L"<circle cx cy radius>|<arc cx cy radius start_deg end_deg>)+\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(expected_model_name,
        sizeof(expected_model_name) / sizeof(expected_model_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(feature_name,
        sizeof(feature_name) / sizeof(feature_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(sketch_plane_name,
        sizeof(sketch_plane_name) / sizeof(sketch_plane_name[0]),
        argv[4], _TRUNCATE);
    wcsncpy_s(orientation_plane_name,
        sizeof(orientation_plane_name) / sizeof(orientation_plane_name[0]),
        argv[5], _TRUNCATE);
    view_side = _wtoi(argv[6]);
    orientation_direction = _wtoi(argv[7]);
    geometry_count = _wtoi(argv[8]);
    if ((view_side != 1 && view_side != 2) ||
        orientation_direction < PRO_SEC_ORIENT_DIR_UP ||
        orientation_direction > PRO_SEC_ORIENT_DIR_RIGHT ||
        geometry_count < 1 || geometry_count > MAX_SKETCH_ENTITIES)
    {
        exit_code = write_error(out, "sketch_option_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    cursor = 9;
    for (index = 0; index < geometry_count; ++index)
    {
        if (cursor >= argc)
        {
            exit_code = write_error(out, "entity_input_missing", PRO_TK_BAD_INPUTS);
            goto done;
        }
        if (_wcsicmp(argv[cursor], L"line") == 0)
        {
            double dx;
            double dy;
            if (cursor + 4 >= argc)
            {
                exit_code = write_error(out, "line_input_missing", PRO_TK_BAD_INPUTS);
                goto done;
            }
            geometries[index].kind = SKETCH_GEOMETRY_LINE;
            if (!parse_finite_value(argv[cursor + 1], -1000000.0, 1000000.0, &geometries[index].values[0]) ||
                !parse_finite_value(argv[cursor + 2], -1000000.0, 1000000.0, &geometries[index].values[1]) ||
                !parse_finite_value(argv[cursor + 3], -1000000.0, 1000000.0, &geometries[index].values[2]) ||
                !parse_finite_value(argv[cursor + 4], -1000000.0, 1000000.0, &geometries[index].values[3]))
            {
                exit_code = write_error(out, "line_value_input", PRO_TK_BAD_INPUTS);
                goto done;
            }
            dx = geometries[index].values[2] - geometries[index].values[0];
            dy = geometries[index].values[3] - geometries[index].values[1];
            if (dx * dx + dy * dy < 1.0e-12)
            {
                exit_code = write_error(out, "line_length_guard", PRO_TK_BAD_INPUTS);
                goto done;
            }
            cursor += 5;
            ++line_count;
        }
        else if (_wcsicmp(argv[cursor], L"circle") == 0)
        {
            if (cursor + 3 >= argc)
            {
                exit_code = write_error(out, "circle_input_missing", PRO_TK_BAD_INPUTS);
                goto done;
            }
            geometries[index].kind = SKETCH_GEOMETRY_CIRCLE;
            if (!parse_finite_value(argv[cursor + 1], -1000000.0, 1000000.0, &geometries[index].values[0]) ||
                !parse_finite_value(argv[cursor + 2], -1000000.0, 1000000.0, &geometries[index].values[1]) ||
                !parse_finite_value(argv[cursor + 3], 0.000001, 1000000.0, &geometries[index].values[2]))
            {
                exit_code = write_error(out, "circle_value_input", PRO_TK_BAD_INPUTS);
                goto done;
            }
            cursor += 4;
            ++circle_count;
        }
        else if (_wcsicmp(argv[cursor], L"arc") == 0)
        {
            double span;
            if (cursor + 5 >= argc)
            {
                exit_code = write_error(out, "arc_input_missing", PRO_TK_BAD_INPUTS);
                goto done;
            }
            geometries[index].kind = SKETCH_GEOMETRY_ARC;
            if (!parse_finite_value(argv[cursor + 1], -1000000.0, 1000000.0, &geometries[index].values[0]) ||
                !parse_finite_value(argv[cursor + 2], -1000000.0, 1000000.0, &geometries[index].values[1]) ||
                !parse_finite_value(argv[cursor + 3], 0.000001, 1000000.0, &geometries[index].values[2]) ||
                !parse_finite_value(argv[cursor + 4], -360.0, 360.0, &geometries[index].values[3]) ||
                !parse_finite_value(argv[cursor + 5], -360.0, 360.0, &geometries[index].values[4]))
            {
                exit_code = write_error(out, "arc_value_input", PRO_TK_BAD_INPUTS);
                goto done;
            }
            span = fabs(geometries[index].values[4] - geometries[index].values[3]);
            if (span < 0.001 || span >= 359.999)
            {
                exit_code = write_error(out, "arc_span_guard", PRO_TK_BAD_INPUTS);
                goto done;
            }
            cursor += 6;
            ++arc_count;
        }
        else
        {
            exit_code = write_error(out, "entity_kind_input", PRO_TK_INVALID_TYPE);
            goto done;
        }
    }
    if (cursor != argc)
    {
        exit_code = write_error(out, "unexpected_entity_input", PRO_TK_BAD_INPUTS);
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
    status = ProMdlCurrentGet(&model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto cleanup;
    }
    status = ProMdlNameGet(model, actual_model_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(actual_model_name, expected_model_name) != 0)
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
    status = ProDirectoryCurrentGet(working_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "working_directory", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        model, PRO_FEATURE, feature_name, &existing_feature);
    if (status == PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_name_collision", PRO_TK_E_FOUND);
        goto cleanup;
    }

    source_mass_status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &source_mass);
    status = create_general_sketch(
        model,
        feature_name,
        sketch_plane_name,
        orientation_plane_name,
        view_side,
        orientation_direction,
        geometries,
        geometry_count,
        &created_feature,
        &creation_error_count,
        &section_entity_count,
        &failure_stage);
    if (created_feature.id >= 0)
        feature_created = 1;
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, failure_stage, status);
        goto cleanup;
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
    status = ProFeatureStatusGet(&created_feature, &feature_status);
    if (status != PRO_TK_NO_ERROR || feature_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "feature_status_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProFeatureTypeGet(&created_feature, &feature_type);
    if (status != PRO_TK_NO_ERROR || feature_type != PRO_FEAT_CURVE)
    {
        exit_code = write_error(out, "feature_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    final_mass_status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &final_mass);
    if ((source_mass_status == PRO_TK_NO_ERROR) !=
        (final_mass_status == PRO_TK_NO_ERROR) ||
        (source_mass_status == PRO_TK_NO_ERROR &&
         !nearly_equal(source_mass.volume, final_mass.volume)))
    {
        exit_code = write_error(out, "solid_volume_guard", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }
    status = ProMdlSave(model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_model", status);
        goto cleanup;
    }
    saved = 1;
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
    fputs(",\"sketch_plane\":", out);
    write_wide_json_string(out, sketch_plane_name);
    fputs(",\"orientation_plane\":", out);
    write_wide_json_string(out, orientation_plane_name);
    fprintf(out,
        ",\"view_side\":%d,\"orientation_direction\":%d,"
        "\"feature_id\":%d,\"feature_type_code\":%d,"
        "\"feature_status\":%d,\"section_entity_count\":%d,"
        "\"geometry_summary\":{\"line_count\":%d,"
        "\"circle_count\":%d,\"arc_count\":%d},"
        "\"entities\":[",
        view_side,
        orientation_direction,
        created_feature.id,
        feature_type,
        feature_status,
        section_entity_count,
        line_count,
        circle_count,
        arc_count);
    for (index = 0; index < geometry_count; ++index)
    {
        if (index > 0)
            fputc(',', out);
        fputs("{\"kind\":", out);
        write_utf8_json_string(out, geometry_kind_name(geometries[index].kind));
        fprintf(out, ",\"entity_id\":%d}", geometries[index].entity_id);
    }
    fprintf(out,
        "],\"creation_error_count\":%d,"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"mass\":{\"before_status\":%d,\"after_status\":%d,"
        "\"before_volume\":%.17g,\"after_volume\":%.17g},"
        "\"saved_file\":",
        creation_error_count,
        regenerate_status,
        regenerate_attempts,
        source_mass_status,
        final_mass_status,
        source_mass_status == PRO_TK_NO_ERROR ? source_mass.volume : 0.0,
        final_mass_status == PRO_TK_NO_ERROR ? final_mass.volume : 0.0);
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
