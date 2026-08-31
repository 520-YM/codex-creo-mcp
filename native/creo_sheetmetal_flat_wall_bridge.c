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
#include <ProWindows.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProSelection.h>
#include <ProReference.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProElemId.h>
#include <ProArray.h>
#include <ProSheetmetal.h>
#include <ProDimension.h>
#include <ProParameter.h>
#include <ProEdge.h>
#include <ProSurface.h>
#include <ProSmtFlatWall.h>
#include <ProUtil.h>

static void json_utf8(FILE *out, const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    fputc('"', out);
    while (*p)
    {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        if (*p < 0x20) fprintf(out, "\\u%04x", (unsigned int)*p);
        else fputc(*p, out);
        ++p;
    }
    fputc('"', out);
}

static void json_wide(FILE *out, const wchar_t *value)
{
    int size;
    char *utf8;
    if (value == NULL) { fputs("null", out); return; }
    size = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    if (size <= 0) { fputs("\"\"", out); return; }
    utf8 = (char *)malloc((size_t)size);
    if (utf8 == NULL) { fputs("\"\"", out); return; }
    WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, size, NULL, NULL);
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

static int parse_double(const wchar_t *text, double min, double max, double *value)
{
    wchar_t *end = NULL;
    double parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) ||
        parsed < min || parsed > max) return 0;
    *value = parsed;
    return 1;
}

static int parse_int(const wchar_t *text, int min, int max, int *value)
{
    wchar_t *end = NULL;
    long parsed = wcstol(text, &end, 10);
    if (end == text || *end != L'\0' || parsed < min || parsed > max) return 0;
    *value = (int)parsed;
    return 1;
}

static int near_value(double actual, double expected, double rel)
{
    double tolerance = fabs(expected) * rel;
    if (tolerance < 1.0e-5) tolerance = 1.0e-5;
    return fabs(actual - expected) <= tolerance;
}

static int latest_model(
    const wchar_t *directory, const wchar_t *name,
    wchar_t *saved_path, size_t saved_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    int found = 0;
    int best = -1;
    _snwprintf_s(pattern, sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
        L"%ls\\%ls.prt*", directory, name);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    do
    {
        wchar_t *dot = wcsrchr(data.cFileName, L'.');
        int version = dot == NULL ? 0 : _wtoi(dot + 1);
        if (!found || version > best)
        {
            _snwprintf_s(saved_path, saved_count, _TRUNCATE,
                L"%ls\\%ls", directory, data.cFileName);
            best = version;
            found = 1;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return found;
}

static ProError add_int(ProElement parent, ProElemId id, int value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(id, &element);
    if (status == PRO_TK_NO_ERROR) status = ProElementIntegerSet(element, value);
    if (status == PRO_TK_NO_ERROR) status = ProElemtreeElementAdd(parent, NULL, element);
    if (status != PRO_TK_NO_ERROR && element != NULL) ProElementFree(&element);
    return status;
}

static ProError add_double(ProElement parent, ProElemId id, double value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(id, &element);
    if (status == PRO_TK_NO_ERROR) status = ProElementDecimalsSet(element, 6);
    if (status == PRO_TK_NO_ERROR) status = ProElementDoubleSet(element, value);
    if (status == PRO_TK_NO_ERROR) status = ProElemtreeElementAdd(parent, NULL, element);
    if (status != PRO_TK_NO_ERROR && element != NULL) ProElementFree(&element);
    return status;
}

static ProError add_name(ProElement parent, ProElemId id, const wchar_t *value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(id, &element);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementWstringSet(element, (wchar_t *)value);
    if (status == PRO_TK_NO_ERROR) status = ProElemtreeElementAdd(parent, NULL, element);
    if (status != PRO_TK_NO_ERROR && element != NULL) ProElementFree(&element);
    return status;
}

static ProError add_ref(ProElement parent, ProElemId id, ProReference reference)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(id, &element);
    if (status == PRO_TK_NO_ERROR) status = ProElementReferenceSet(element, reference);
    if (status == PRO_TK_NO_ERROR) status = ProElemtreeElementAdd(parent, NULL, element);
    if (status != PRO_TK_NO_ERROR && element != NULL) ProElementFree(&element);
    return status;
}

static ProError add_refs(
    ProElement parent, ProElemId id, ProReference *references, int count)
{
    ProElement element = NULL;
    ProReference *reference_array = NULL;
    ProError status;
    int index;
    status = ProElementAlloc(id, &element);
    if (status == PRO_TK_NO_ERROR)
        status = ProArrayAlloc(0, sizeof(ProReference), count,
            (ProArray *)&reference_array);
    for (index = 0; status == PRO_TK_NO_ERROR && index < count; ++index)
        status = ProArrayObjectAdd((ProArray *)&reference_array,
            PRO_VALUE_UNUSED, 1, &references[index]);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementReferencesSet(element, reference_array);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementAdd(parent, NULL, element);
    if (reference_array != NULL) ProArrayFree((ProArray *)&reference_array);
    if (status != PRO_TK_NO_ERROR && element != NULL) ProElementFree(&element);
    return status;
}

static ProError add_compound(ProElement parent, ProElemId id, ProElement *element)
{
    ProError status;
    *element = NULL;
    status = ProElementAlloc(id, element);
    if (status == PRO_TK_NO_ERROR) status = ProElemtreeElementAdd(parent, NULL, *element);
    if (status != PRO_TK_NO_ERROR && *element != NULL) ProElementFree(element);
    return status;
}

static ProError add_section(ProElement parent, ProSection section)
{
    ProElement sketcher = NULL;
    ProError status = ProElementAlloc(PRO_E_SKETCHER, &sketcher);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementSpecialvalueSet(sketcher, section);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementAdd(parent, NULL, sketcher);
    if (status != PRO_TK_NO_ERROR && sketcher != NULL) ProElementFree(&sketcher);
    return status;
}

typedef struct
{
    int feature_id;
    double expected_value;
    ProDimension dimension;
    int match_count;
} DimensionValueContext;

static ProError dimension_value_action(
    ProDimension *dimension, ProError filter_status, ProAppData app_data)
{
    DimensionValueContext *context = (DimensionValueContext *)app_data;
    ProFeature owner;
    double value = 0.0;
    (void)filter_status;
    if (ProDimensionOwnerfeatureGet(dimension, &owner) == PRO_TK_NO_ERROR &&
        owner.id == context->feature_id &&
        ProDimensionValueGet(dimension, &value) == PRO_TK_NO_ERROR &&
        near_value(value, context->expected_value, 1.0e-8))
    {
        context->dimension = *dimension;
        ++context->match_count;
    }
    return PRO_TK_NO_ERROR;
}

static ProError set_unique_feature_dimension_by_value(
    ProSolid solid, int feature_id, double expected_value, double new_value,
    int *dimension_id)
{
    DimensionValueContext context;
    ProError status;
    memset(&context, 0, sizeof(context));
    context.feature_id = feature_id;
    context.expected_value = expected_value;
    status = ProSolidDimensionVisit(solid, PRO_B_FALSE,
        dimension_value_action, NULL, (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND) return status;
    if (context.match_count != 1) return PRO_TK_BAD_CONTEXT;
    status = ProDimensionValueSet(&context.dimension, new_value);
    if (status == PRO_TK_NO_ERROR && dimension_id != NULL)
        *dimension_id = context.dimension.id;
    return status;
}

static ProError add_overlap_corner_children(ProElement corners, int corner_count)
{
    ProElement *corner_elements = NULL;
    ProError status = PRO_TK_NO_ERROR;
    int index;
    status = ProArrayAlloc(0, sizeof(ProElement), corner_count,
        (ProArray *)&corner_elements);
    for (index = 0; status == PRO_TK_NO_ERROR && index < corner_count; ++index)
    {
        ProElement corner = NULL, rip = NULL, dim1 = NULL, dim2 = NULL;
        status = ProElementAlloc(PRO_E_SMT_CORNER, &corner);
        if (status == PRO_TK_NO_ERROR)
            status = add_int(corner, PRO_E_WALL_CORNER_TREATMENT,
                PRO_WALL_CORNER_SEAM);
        if (status == PRO_TK_NO_ERROR)
            status = add_compound(corner, PRO_E_SMT_EDGE_RIP, &rip);
        if (status == PRO_TK_NO_ERROR)
            status = add_int(rip, PRO_E_SMT_EDGE_RIP_TYPE,
                PRO_EDGE_RIP_OVERLAP);
        if (status == PRO_TK_NO_ERROR)
            status = add_int(rip, PRO_E_SMT_EDGE_RIP_ADD_GAP, PRO_B_TRUE);
        if (status == PRO_TK_NO_ERROR)
            status = add_compound(rip, PRO_E_SMT_EDGE_RIP_DIM_1, &dim1);
        if (status == PRO_TK_NO_ERROR)
            status = add_int(dim1, PRO_E_SMT_EDGE_RIP_DIM_1_TYPE,
                PRO_EDGE_RIP_DIM_TYPE_PARAM);
        if (status == PRO_TK_NO_ERROR)
            status = add_compound(rip, PRO_E_SMT_EDGE_RIP_DIM_2, &dim2);
        if (status == PRO_TK_NO_ERROR)
            status = add_int(dim2, PRO_E_SMT_EDGE_RIP_DIM_2_TYPE,
                PRO_EDGE_RIP_DIM_TYPE_PARAM);
        if (status == PRO_TK_NO_ERROR)
            status = add_int(rip, PRO_E_SMT_EDGE_RIP_FLIP, PRO_B_FALSE);
        if (status == PRO_TK_NO_ERROR)
            status = ProArrayObjectAdd((ProArray *)&corner_elements,
                PRO_VALUE_UNUSED, 1, &corner);
    }
    if (status == PRO_TK_NO_ERROR)
        status = ProElementArraySet(corners, NULL, corner_elements);
    if (corner_elements != NULL) ProArrayFree((ProArray *)&corner_elements);
    return status;
}

static ProError add_overlap_corners(ProElement tree, int corner_count)
{
    ProElement corners = NULL;
    ProError status = add_compound(tree, PRO_E_SMT_CORNERS_ARR, &corners);
    if (status == PRO_TK_NO_ERROR)
        status = add_overlap_corner_children(corners, corner_count);
    return status;
}

static ProError tree_element(ProElement tree, const ProElemId *ids, int count, ProElement *element)
{
    ProElempath path = NULL;
    ProElempathItem items[4];
    ProError status;
    int i;
    if (count < 1 || count > 4) return PRO_TK_BAD_INPUTS;
    for (i = 0; i < count; ++i)
    {
        items[i].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
        items[i].path_item.elem_id = ids[i];
    }
    status = ProElempathAlloc(&path);
    if (status == PRO_TK_NO_ERROR) status = ProElempathDataSet(path, items, count);
    if (status == PRO_TK_NO_ERROR) status = ProElemtreeElementGet(tree, path, element);
    if (path != NULL) ProElempathFree(&path);
    return status;
}

static int sheetmetal_face_edge(ProMdl model, ProEdge edge)
{
    ProEdge neighbor_edges[2];
    ProSurface surfaces[2];
    ProSmtSurfType types[2];
    if (ProEdgeNeighborsGet(edge, &neighbor_edges[0], &neighbor_edges[1],
            &surfaces[0], &surfaces[1]) != PRO_TK_NO_ERROR) return 0;
    if (ProSmtSurfaceTypeGet(model, surfaces[0], &types[0]) != PRO_TK_NO_ERROR)
        types[0] = PRO_SMT_SURF_NON_SMT;
    if (ProSmtSurfaceTypeGet(model, surfaces[1], &types[1]) != PRO_TK_NO_ERROR)
        types[1] = PRO_SMT_SURF_NON_SMT;
    return types[0] == PRO_SMT_SURF_FACE || types[0] == PRO_SMT_SURF_OFFSET ||
           types[1] == PRO_SMT_SURF_FACE || types[1] == PRO_SMT_SURF_OFFSET;
}

int wmain(int argc, wchar_t **argv)
{
#define WALL_EDGE_COUNT 4
    FILE *out = stdout;
    ProError status;
    ProErrorlist errors = {NULL, 0};
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process;
    ProMdl model = NULL;
    ProMdlName expected_name, actual_name;
    ProName feature_name;
    ProPath project_directory, section_path, saved_path;
    ProMdlType model_type;
    ProMdlsubtype subtype;
    ProModelitem duplicate, edge_items[WALL_EDGE_COUNT], model_item;
    ProParameter edge_treatment_parameter;
    ProParamvalue edge_treatment_value;
    ProName edge_treatment_before;
    ProEdge edges[WALL_EDGE_COUNT] = {NULL, NULL, NULL, NULL};
    ProEnttype edge_type;
    ProSelection edge_selections[WALL_EDGE_COUNT] = {NULL, NULL, NULL, NULL};
    ProSelection model_selection = NULL;
    ProReference edge_references[WALL_EDGE_COUNT] = {NULL, NULL, NULL, NULL};
    ProReference *readback_references = NULL;
    ProSection section = NULL;
    ProElement tree = NULL, angle_tree = NULL, standard_section = NULL;
    ProElement fillets = NULL, height_tree = NULL, relief = NULL;
    ProElement relief1 = NULL, relief2 = NULL, devlen = NULL, yfactor = NULL;
    ProElement extracted = NULL, read_element = NULL;
    ProElement *read_corner_elements = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProFeature feature;
    ProFeattype feature_type;
    ProFeatStatus feature_status;
    ProMassProperty before_mass, after_mass;
    Pro3dPnt outline[2];
    ProMatrix matrix = {
        {1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}
    };
    ProSolidOutlExclTypes excludes[] = {
        PRO_OUTL_EXC_DATUM_PLANE, PRO_OUTL_EXC_DATUM_POINT,
        PRO_OUTL_EXC_DATUM_CSYS, PRO_OUTL_EXC_DATUM_AXES,
        PRO_OUTL_EXC_ALL_CRVS
    };
    ProElemId wall_type_path[] = {PRO_E_SMT_WALL_TYPE};
    ProElemId angle_path[] = {PRO_E_SMT_FLAT_WALL_ANGLE,
        PRO_E_SMT_FLAT_WALL_ANGLE_VAL};
    ProElemId edge_path[] = {PRO_E_SMT_FLAT_WALL_ATT_EDGE};
    ProElemId corners_path[] = {PRO_E_SMT_CORNERS_ARR};
    ProElemId corner_treatment_path[] = {PRO_E_WALL_CORNER_TREATMENT};
    ProElemId corner_rip_path[] = {PRO_E_SMT_EDGE_RIP, PRO_E_SMT_EDGE_RIP_TYPE};
    double expected_edge_lengths[WALL_EDGE_COUNT];
    double actual_edge_lengths[WALL_EDGE_COUNT];
    double angle, height, radius, thickness;
    double read_angle = 0.0;
    double extents[3];
    int edge_ids[WALL_EDGE_COUNT], read_wall_type = -1;
    int read_reference_count = 0, read_corner_count = 0;
    int height_dimension_id = -1;
    int index, check_index, wall_edge_count;
    int connected = 0, created = 0, saved = 0, window_id = -1;
    int exit_code = 1;
    const char *stage = "not_started";

#define STEP(label, expression) do { stage = label; status = (expression); \
    if (status != PRO_TK_NO_ERROR) goto failure; } while (0)

    feature.id = -1;
    if (argc == 11) wall_edge_count = 1;
    else if (argc == 13) wall_edge_count = 2;
    else if (argc == 17) wall_edge_count = 4;
    else
    {
        fwprintf(stderr, L"Usage: flat_wall <result> <expected_model> <feature_name> "
            L"<project_dir> <edge> <length> [<edge> <length> x3] "
            L"<angle_deg> <height> <none|overlap> "
            L"<rect_flat_wall.sec>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL) return 2;
    SetHandleInformation((HANDLE)_get_osfhandle(_fileno(out)), HANDLE_FLAG_INHERIT, 0);
    for (index = 0; index < wall_edge_count; ++index)
    {
        if (!parse_int(argv[5 + index * 2], 1, 100000000, &edge_ids[index]) ||
            !parse_double(argv[6 + index * 2], 0.01, 100000.0,
                &expected_edge_lengths[index]))
        {
            exit_code = fail(out, "input_edge", PRO_TK_BAD_INPUTS);
            goto done;
        }
    }
    if (!parse_double(argv[5 + wall_edge_count * 2], 1.0, 179.0, &angle) ||
        !parse_double(argv[6 + wall_edge_count * 2], 0.01, 100000.0, &height) ||
        ((wall_edge_count < 4 &&
          _wcsicmp(argv[7 + wall_edge_count * 2], L"none") != 0) ||
         (wall_edge_count == 4 &&
          _wcsicmp(argv[7 + wall_edge_count * 2], L"overlap") != 0)))
    {
        exit_code = fail(out, "input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    wcsncpy_s(expected_name, sizeof(expected_name)/sizeof(expected_name[0]), argv[2], _TRUNCATE);
    wcsncpy_s(feature_name, sizeof(feature_name)/sizeof(feature_name[0]), argv[3], _TRUNCATE);
    wcsncpy_s(project_directory, sizeof(project_directory)/sizeof(project_directory[0]), argv[4], _TRUNCATE);
    wcsncpy_s(section_path, sizeof(section_path)/sizeof(section_path[0]),
        argv[8 + wall_edge_count * 2], _TRUNCATE);
    if (GetFileAttributesW(project_directory) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(section_path) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = fail(out, "file_guard", PRO_TK_E_NOT_FOUND);
        goto done;
    }

    status = ProEngineerConnect("", "", "", "", PRO_B_TRUE, 20,
        &random_choice, &process);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = fail(out, "connect", status);
        goto done;
    }
    connected = 1;
    STEP("current_model", ProMdlCurrentGet(&model));
    STEP("model_name", ProMdlNameGet(model, actual_name));
    if (_wcsicmp(actual_name, expected_name) != 0)
    {
        status = PRO_TK_BAD_CONTEXT; stage = "model_name_guard"; goto failure;
    }
    STEP("model_type", ProMdlTypeGet(model, &model_type));
    STEP("model_subtype", ProMdlSubtypeGet(model, &subtype));
    if (model_type != PRO_MDL_PART || subtype != PROMDLSTYPE_PART_SHEETMETAL)
    {
        status = PRO_TK_INVALID_TYPE; stage = "sheetmetal_guard"; goto failure;
    }
    STEP("working_directory", ProDirectoryChange(project_directory));
    STEP("model_item_for_parameters", ProMdlToModelitem(model, &model_item));
    STEP("edge_treatment_parameter", ProParameterInit(
        &model_item, L"SMT_DFLT_EDGE_TREA_TYPE", &edge_treatment_parameter));
    edge_treatment_value.type = PRO_PARAM_VOID;
    STEP("edge_treatment_value", ProParameterValueWithUnitsGet(
        &edge_treatment_parameter, &edge_treatment_value, NULL));
    if (edge_treatment_value.type != PRO_PARAM_STRING)
    {
        status = PRO_TK_INVALID_TYPE;
        stage = "edge_treatment_parameter_type";
        goto failure;
    }
    wcsncpy_s(edge_treatment_before,
        sizeof(edge_treatment_before) / sizeof(edge_treatment_before[0]),
        edge_treatment_value.value.s_val, _TRUNCATE);
    status = ProModelitemByNameInit(model, PRO_FEATURE, feature_name, &duplicate);
    if (status == PRO_TK_NO_ERROR)
    {
        status = PRO_TK_E_FOUND; stage = "feature_name_guard"; goto failure;
    }
    if (status != PRO_TK_E_NOT_FOUND) { stage = "feature_name_guard"; goto failure; }

    for (index = 0; index < wall_edge_count; ++index)
    {
        for (check_index = 0; check_index < index; ++check_index)
        {
            if (edge_ids[index] == edge_ids[check_index])
            {
                status = PRO_TK_BAD_INPUTS;
                stage = "duplicate_edge_guard";
                goto failure;
            }
        }
        STEP("edge_init", ProModelitemInit(
            model, edge_ids[index], PRO_EDGE, &edge_items[index]));
        STEP("edge_handle", ProGeomitemToEdge(
            (ProGeomitem *)&edge_items[index], &edges[index]));
        STEP("edge_type", ProEdgeTypeGet(edges[index], &edge_type));
        STEP("edge_length", ProEdgeLengthEval(
            edges[index], &actual_edge_lengths[index]));
        if (edge_type != PRO_ENT_LINE ||
            !near_value(actual_edge_lengths[index],
                expected_edge_lengths[index], 1.0e-8) ||
            !sheetmetal_face_edge(model, edges[index]))
        {
            status = PRO_TK_BAD_CONTEXT;
            stage = "attachment_edge_guard";
            goto failure;
        }
        STEP("edge_selection", ProSelectionAlloc(
            NULL, &edge_items[index], &edge_selections[index]));
        STEP("edge_reference", ProSelectionToReference(
            edge_selections[index], &edge_references[index]));
    }
    {
        ProDimension thickness_dimension;
        STEP("thickness_dimension", ProSmtPartThicknessGet(
            (ProPart)model, &thickness_dimension));
        STEP("thickness_value", ProDimensionValueGet(
            &thickness_dimension, &thickness));
        if (!(thickness > 0.0))
        {
            status = PRO_TK_BAD_CONTEXT;
            stage = "thickness_guard";
            goto failure;
        }
        radius = thickness * 0.5;
    }
    STEP("section_load", ProMdlFiletypeLoad(
        section_path, PRO_MDLFILE_UNUSED, PRO_B_FALSE, (ProMdl *)&section));
    STEP("mass_before", ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &before_mass));

    STEP("tree", ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    STEP("feature_type_element", add_int(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_WALL));
    STEP("wall_type", add_int(tree, PRO_E_SMT_WALL_TYPE, PRO_SMT_WALL_TYPE_FLAT));
    STEP("feature_name_element", add_name(tree, PRO_E_STD_FEATURE_NAME, feature_name));
    STEP("attachment_edges", add_refs(tree, PRO_E_SMT_FLAT_WALL_ATT_EDGE,
        edge_references, wall_edge_count));
    STEP("angle_tree", add_compound(tree, PRO_E_SMT_FLAT_WALL_ANGLE, &angle_tree));
    STEP("angle_type", add_int(angle_tree, PRO_E_SMT_FLAT_WALL_ANGLE_TYPE, PRO_BND_ANGLE_VALUE));
    STEP("angle_value", add_double(angle_tree, PRO_E_SMT_FLAT_WALL_ANGLE_VAL, angle));
    STEP("angle_flip", add_int(angle_tree, PRO_E_SMT_FLAT_WALL_ANGLE_FLIP, PRO_B_FALSE));
    STEP("standard_section", add_compound(tree, PRO_E_STD_SECTION, &standard_section));
    STEP("section_value", add_section(standard_section, section));
    STEP("fillets", add_compound(tree, PRO_E_SMT_FILLETS, &fillets));
    STEP("fillets_use_radius", add_int(fillets, PRO_E_SMT_FILLETS_USE_RAD, PRO_B_TRUE));
    STEP("fillets_side", add_int(fillets, PRO_E_SMT_FILLETS_SIDE, PRO_BEND_RAD_INSIDE));
    STEP("fillets_radius", add_double(fillets, PRO_E_SMT_FILLETS_VALUE, radius));
    STEP("height", add_compound(tree, PRO_E_SMT_WALL_HEIGHT, &height_tree));
    STEP("height_type", add_int(height_tree, PRO_E_SMT_WALL_HEIGHT_TYPE, PRO_SMT_WALL_HEIGHT_NONE));
    STEP("relief", add_compound(tree, PRO_E_SMT_BEND_RELIEF, &relief));
    STEP("relief_side1", add_compound(relief, PRO_E_SMT_BEND_RELIEF_SIDE1, &relief1));
    STEP("relief_type1", add_int(relief1, PRO_E_BEND_RELIEF_TYPE, PRO_BEND_RLF_RIP));
    STEP("relief_side2", add_compound(relief, PRO_E_SMT_BEND_RELIEF_SIDE2, &relief2));
    STEP("relief_type2", add_int(relief2, PRO_E_BEND_RELIEF_TYPE, PRO_BEND_RLF_RIP));
    STEP("thickness_flip", add_int(tree, PRO_E_SMT_WALL_THICKNESS_FLIP, PRO_B_FALSE));
    STEP("devlen", add_compound(tree, PRO_E_SMT_DEV_LEN_CALCULATION, &devlen));
    STEP("devlen_source", add_int(devlen, PRO_E_SMT_DEV_LEN_SOURCE, PRO_DVL_SRC_FEAT_YF_ONLY));
    STEP("yfactor", add_compound(devlen, PRO_E_SMT_DEV_LEN_Y_FACTOR, &yfactor));
    STEP("yfactor_type", add_int(yfactor, PRO_E_SMT_DEV_LEN_Y_FACTOR_TYPE, PRO_FACTOR_Y));
    STEP("yfactor_value", add_double(yfactor, PRO_E_SMT_DEV_LEN_Y_FACTOR_VALUE, 0.32));
    if (wall_edge_count == 4)
        STEP("overlap_corners", add_overlap_corners(tree, wall_edge_count));

    STEP("model_item", ProMdlToModelitem(model, &model_item));
    STEP("model_selection", ProSelectionAlloc(NULL, &model_item, &model_selection));
    STEP("options", ProArrayAlloc(1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&options));
    options[0] = PRO_FEAT_CR_NO_OPTS;
    status = ProFeatureWithoptionsCreate(model_selection, tree, options,
        PRO_REGEN_NO_FLAGS, &feature, &errors);
    if (status != PRO_TK_NO_ERROR) { stage = "flat_wall_create"; goto failure; }
    created = 1;
    if (!near_value(height, 5.0, 1.0e-9))
        STEP("wall_height_dimension", set_unique_feature_dimension_by_value(
            (ProSolid)model, feature.id, 5.0, height, &height_dimension_id));
    STEP("regenerate", ProSolidRegenerate((ProSolid)model, PRO_REGEN_FORCE_REGEN));
    if (wall_edge_count == 4)
    {
        STEP("tree_extract_for_corner_redefine", ProFeatureElemtreeExtract(
            &feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &extracted));
        STEP("corners_get_for_redefine", tree_element(
            extracted, corners_path, 1, &read_element));
        STEP("overlap_corners_for_redefine", add_overlap_corner_children(
            read_element, wall_edge_count));
        if (errors.error_list != NULL)
        {
            ProArrayFree((ProArray *)&errors.error_list);
            errors.error_list = NULL;
            errors.error_number = 0;
        }
        status = ProFeatureWithoptionsRedefine(NULL, &feature, extracted, options,
            PRO_REGEN_NO_FLAGS, &errors);
        if (status != PRO_TK_NO_ERROR)
        {
            stage = "corner_overlap_redefine";
            goto failure;
        }
        STEP("tree_free_after_corner_redefine",
            ProFeatureElemtreeFree(&feature, extracted));
        extracted = NULL;
        STEP("regenerate_after_corner_redefine",
            ProSolidRegenerate((ProSolid)model, PRO_REGEN_FORCE_REGEN));
    }
    STEP("feature_type_readback", ProFeatureTypeGet(&feature, &feature_type));
    STEP("feature_status", ProFeatureStatusGet(&feature, &feature_status));
    if (feature_type != PRO_FEAT_WALL || feature_status != PRO_FEAT_ACTIVE)
    {
        status = PRO_TK_GENERAL_ERROR; stage = "feature_status_guard"; goto failure;
    }
    STEP("tree_extract", ProFeatureElemtreeExtract(
        &feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &extracted));
    STEP("wall_type_element_get", tree_element(extracted, wall_type_path, 1, &read_element));
    STEP("wall_type_readback", ProElementIntegerGet(read_element, NULL, &read_wall_type));
    STEP("angle_element_get", tree_element(extracted, angle_path, 2, &read_element));
    STEP("angle_readback", ProElementDoubleGet(read_element, NULL, &read_angle));
    STEP("edge_element_get", tree_element(extracted, edge_path, 1, &read_element));
    STEP("edge_references_readback", ProElementReferencesGet(
        read_element, NULL, &readback_references));
    STEP("edge_reference_count", ProArraySizeGet(
        readback_references, &read_reference_count));
    if (read_reference_count != wall_edge_count)
    {
        status = PRO_TK_GENERAL_ERROR;
        stage = "attachment_edge_count_guard";
        goto failure;
    }
    for (index = 0; index < wall_edge_count; ++index)
    {
        int read_id = -1, found = 0;
        STEP("edge_id_readback", ProReferenceIdGet(
            readback_references[index], &read_id));
        for (check_index = 0; check_index < wall_edge_count; ++check_index)
            if (read_id == edge_ids[check_index]) found = 1;
        if (!found)
        {
            status = PRO_TK_GENERAL_ERROR;
            stage = "attachment_edge_id_guard";
            goto failure;
        }
    }
    if (wall_edge_count == 4)
    {
        STEP("corners_element_get", tree_element(
            extracted, corners_path, 1, &read_element));
        STEP("corner_count", ProElementArrayCount(
            read_element, NULL, &read_corner_count));
        if (read_corner_count != wall_edge_count)
        {
            status = PRO_TK_GENERAL_ERROR;
            stage = "corner_count_guard";
            goto failure;
        }
        STEP("corner_array_alloc", ProArrayAlloc(0, sizeof(ProElement),
            wall_edge_count, (ProArray *)&read_corner_elements));
        STEP("corner_array_get", ProElementArrayGet(
            read_element, NULL, &read_corner_elements));
        for (index = 0; index < read_corner_count; ++index)
        {
            int treatment = -1, rip_type = -1;
            STEP("corner_treatment_element", tree_element(
                read_corner_elements[index], corner_treatment_path, 1, &read_element));
            STEP("corner_treatment_read", ProElementIntegerGet(
                read_element, NULL, &treatment));
            STEP("corner_rip_element", tree_element(
                read_corner_elements[index], corner_rip_path, 2, &read_element));
            STEP("corner_rip_read", ProElementIntegerGet(
                read_element, NULL, &rip_type));
            if (treatment != PRO_WALL_CORNER_SEAM ||
                rip_type != PRO_EDGE_RIP_OVERLAP)
            {
                status = PRO_TK_GENERAL_ERROR;
                stage = "corner_overlap_guard";
                goto failure;
            }
        }
    }
    if (read_wall_type != PRO_SMT_WALL_TYPE_FLAT ||
        !near_value(read_angle, angle, 1.0e-9))
    {
        status = PRO_TK_GENERAL_ERROR; stage = "feature_element_guard"; goto failure;
    }
    STEP("mass_after", ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &after_mass));
    if (!(after_mass.volume > before_mass.volume + 1.0))
    {
        status = PRO_TK_GENERAL_ERROR; stage = "volume_guard"; goto failure;
    }
    STEP("outline", ProSolidOutlineCompute((ProSolid)model, matrix, excludes,
        (int)(sizeof(excludes)/sizeof(excludes[0])), outline));
    extents[0] = outline[1][0] - outline[0][0];
    extents[1] = outline[1][1] - outline[0][1];
    extents[2] = outline[1][2] - outline[0][2];
    STEP("save", ProMdlSave(model));
    saved = 1;
    if (!latest_model(project_directory, actual_name, saved_path,
            sizeof(saved_path)/sizeof(saved_path[0])))
    {
        status = PRO_TK_E_NOT_FOUND; stage = "saved_file_guard"; goto failure;
    }
    if (ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
    {
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"model\":", out);
    json_wide(out, actual_name);
    fputs(",\"feature_name\":", out); json_wide(out, feature_name);
    fprintf(out, ",\"feature_id\":%d,\"feature_type_code\":%d,"
        "\"feature_status\":%d,\"wall_type\":%d,\"attachment_edge_count\":%d,"
        "\"attachment_edge_ids\":[",
        feature.id, feature_type, feature_status, read_wall_type, wall_edge_count);
    for (index = 0; index < wall_edge_count; ++index)
        fprintf(out, "%s%d", index == 0 ? "" : ",", edge_ids[index]);
    fputs("],\"attachment_edge_lengths_mm\":[", out);
    for (index = 0; index < wall_edge_count; ++index)
        fprintf(out, "%s%.15g", index == 0 ? "" : ",",
            actual_edge_lengths[index]);
    fprintf(out, "],\"corner_count\":%d,\"corner_type\":%s,"
        "\"angle_deg\":%.15g,\"wall_height_mm\":%.15g,"
        "\"wall_height_dimension_id\":%d,\"sheet_thickness_mm\":%.15g,"
        "\"bend_radius_rule\":\"thickness_x_0.5\",\"bend_radius_mm\":%.15g,"
        "\"volume_before_mm3\":%.15g,"
        "\"volume_after_mm3\":%.15g,\"volume_added_mm3\":%.15g,"
        "\"extents_mm\":[%.15g,%.15g,%.15g],\"creation_error_count\":%d,"
        "\"saved_file\":",
        read_corner_count, wall_edge_count == 4 ? "\"overlap\"" : "null",
        read_angle, height, height_dimension_id,
        thickness, radius, before_mass.volume,
        after_mass.volume, after_mass.volume - before_mass.volume,
        extents[0], extents[1], extents[2], errors.error_number);
    json_wide(out, saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;
    goto cleanup;

failure:
    exit_code = fail(out, stage, status);
cleanup:
    if (exit_code != 0 && created && !saved && model != NULL)
    {
        int id = feature.id;
        ProFeatureDeleteOptions option = PRO_FEAT_DELETE_NO_OPTS;
        ProFeatureDelete((ProSolid)model, &id, 1, &option, 1);
        ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
    }
    if (read_corner_elements != NULL) ProArrayFree((ProArray *)&read_corner_elements);
    if (readback_references != NULL) ProReferencearrayFree(readback_references);
    if (extracted != NULL && feature.id > 0) ProFeatureElemtreeFree(&feature, extracted);
    if (errors.error_list != NULL) ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL) ProArrayFree((ProArray *)&options);
    if (tree != NULL) ProElementFree(&tree);
    if (model_selection != NULL) ProSelectionFree(&model_selection);
    if (section != NULL) ProMdlErase(section);
    for (index = 0; index < WALL_EDGE_COUNT; ++index)
    {
        if (edge_references[index] != NULL) ProReferenceFree(edge_references[index]);
        if (edge_selections[index] != NULL) ProSelectionFree(&edge_selections[index]);
    }
    if (connected) ProEngineerDisconnect(&process, 10);
done:
    fclose(out);
    return exit_code;
#undef STEP
#undef WALL_EDGE_COUNT
}
