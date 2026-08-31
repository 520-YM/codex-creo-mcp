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
#include <ProFeatForm.h>
#include <ProModelitem.h>
#include <ProGeomitem.h>
#include <ProSelection.h>
#include <ProReference.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProArray.h>
#include <ProExtrude.h>
#include <ProStdSection.h>
#include <ProSection.h>
#include <ProSurface.h>
#include <ProSurfacedata.h>
#include <ProSheetmetal.h>
#include <ProDimension.h>

typedef struct
{
    int surface_count;
    int cylindrical_count;
    double cylindrical_area;
} FeatureSurfaceContext;

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

static int near_value(double actual, double expected, double tolerance)
{
    return fabs(actual - expected) <= tolerance;
}

static int latest_model(
    const wchar_t *directory, const wchar_t *name,
    wchar_t *saved_path, size_t saved_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    int found = 0, best = -1;
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

static ProError add_compound(ProElement parent, ProElemId id, ProElement *element)
{
    ProError status;
    *element = NULL;
    status = ProElementAlloc(id, element);
    if (status == PRO_TK_NO_ERROR) status = ProElemtreeElementAdd(parent, NULL, *element);
    if (status != PRO_TK_NO_ERROR && *element != NULL) ProElementFree(element);
    return status;
}

static ProError tree_element(
    ProElement tree, const ProElemId *ids, int count, ProElement *element)
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

static ProError three_circle_section_build(
    ProSection section, double diameter, double spacing,
    double center_u, double center_v, int entity_ids[3])
{
    Pro2dCircledef circle;
    ProError status = PRO_TK_NO_ERROR;
    int index;
    memset(&circle, 0, sizeof(circle));
    circle.type = PRO_2D_CIRCLE;
    circle.radius = diameter / 2.0;
    for (index = 0; index < 3; ++index)
    {
        circle.center[0] = center_u + (index - 1) * spacing;
        circle.center[1] = center_v;
        status = ProSectionEntityAdd(
            section, (Pro2dEntdef *)&circle, &entity_ids[index]);
        if (status != PRO_TK_NO_ERROR) return status;
    }
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status == PRO_TK_NO_ERROR)
        status = ProSectionIntentManagerModeSet(section, PRO_B_FALSE);
    return status;
}

static ProError feature_surface_action(
    ProGeomitem *item, ProError filter_status, ProAppData app_data)
{
    FeatureSurfaceContext *context = (FeatureSurfaceContext *)app_data;
    ProSurface surface = NULL;
    ProSrftype type;
    double area = 0.0;
    (void)filter_status;
    ++context->surface_count;
    if (ProGeomitemToSurface(item, &surface) == PRO_TK_NO_ERROR &&
        ProSurfaceTypeGet(surface, &type) == PRO_TK_NO_ERROR &&
        type == PRO_SRF_CYL)
    {
        ++context->cylindrical_count;
        if (ProSurfaceAreaEval(surface, &area) == PRO_TK_NO_ERROR)
            context->cylindrical_area += area;
    }
    return PRO_TK_NO_ERROR;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProErrorlist errors = {NULL, 0};
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process;
    ProMdl model = NULL;
    ProMdlName expected_name, actual_name;
    ProName feature_name, owner_feature_name, actual_owner_name;
    ProPath project_directory, saved_path;
    ProMdlType model_type;
    ProMdlsubtype subtype;
    ProModelitem duplicate, target_surface_item, orientation_item, model_item;
    ProSurface target_surface = NULL;
    ProSrftype target_surface_type;
    ProFeature owner_feature, feature;
    ProFeattype feature_type;
    ProFeatStatus feature_status;
    ProSelection target_selection = NULL, orientation_selection = NULL;
    ProSelection model_selection = NULL;
    ProReference target_reference = NULL, orientation_reference = NULL;
    ProElement tree = NULL, standard_section = NULL, setup_plane = NULL;
    ProElement depth_tree = NULL, depth_from = NULL, depth_to = NULL;
    ProElement extracted = NULL, sketch_element = NULL, read_element = NULL;
    ProSection section = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProMassProperty before_mass, after_mass;
    ProDimension thickness_dimension;
    FeatureSurfaceContext feature_surfaces;
    ProVector direction, min_point, max_point;
    Pro3dPnt outline[2];
    ProMatrix matrix = {
        {1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}
    };
    ProSolidOutlExclTypes excludes[] = {
        PRO_OUTL_EXC_DATUM_PLANE, PRO_OUTL_EXC_DATUM_POINT,
        PRO_OUTL_EXC_DATUM_CSYS, PRO_OUTL_EXC_DATUM_AXES,
        PRO_OUTL_EXC_ALL_CRVS
    };
    ProElemId sketch_path[] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProElemId direction_path[] = {PRO_E_STD_DIRECTION};
    ProElemId material_path[] = {PRO_E_STD_MATRLSIDE};
    int surface_id, owner_feature_id, entity_ids[3] = {-1, -1, -1};
    int connected = 0, created = 0, saved = 0, window_id = -1;
    int exit_code = 1, axis;
    double expected_area, diameter, spacing, center_u, center_v;
    double actual_area, thickness, expected_removed, actual_removed;
    double expected_cylindrical_area;
    double surface_min[3], surface_max[3], extents[3];
    const char *stage = "not_started";

#define STEP(label, expression) do { stage = label; status = (expression); \
    if (status != PRO_TK_NO_ERROR) goto failure; } while (0)

    feature.id = -1;
    if (argc != 14)
    {
        fwprintf(stderr, L"Usage: three_circle_cut <result> <expected_model> "
            L"<feature_name> <project_dir> <surface_id> <expected_area> "
            L"<owner_feature_id> <owner_feature_name> <orientation_plane> "
            L"<diameter> <spacing> <center_u> <center_v>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL) return 2;
    SetHandleInformation((HANDLE)_get_osfhandle(_fileno(out)), HANDLE_FLAG_INHERIT, 0);
    if (!parse_int(argv[5], 1, 100000000, &surface_id) ||
        !parse_double(argv[6], 0.01, 100000000.0, &expected_area) ||
        !parse_int(argv[7], 1, 100000000, &owner_feature_id) ||
        !parse_double(argv[10], 0.01, 1000.0, &diameter) ||
        !parse_double(argv[11], 0.01, 100000.0, &spacing) ||
        !parse_double(argv[12], -100000.0, 100000.0, &center_u) ||
        !parse_double(argv[13], -100000.0, 100000.0, &center_v))
    {
        exit_code = fail(out, "input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    wcsncpy_s(expected_name, sizeof(expected_name)/sizeof(expected_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(feature_name, sizeof(feature_name)/sizeof(feature_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(project_directory,
        sizeof(project_directory)/sizeof(project_directory[0]), argv[4], _TRUNCATE);
    wcsncpy_s(owner_feature_name,
        sizeof(owner_feature_name)/sizeof(owner_feature_name[0]), argv[8], _TRUNCATE);

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
    status = ProModelitemByNameInit(model, PRO_FEATURE, feature_name, &duplicate);
    if (status == PRO_TK_NO_ERROR)
    {
        status = PRO_TK_E_FOUND; stage = "feature_name_guard"; goto failure;
    }
    if (status != PRO_TK_E_NOT_FOUND) { stage = "feature_name_guard"; goto failure; }

    STEP("surface_init", ProModelitemInit(
        model, surface_id, PRO_SURFACE, &target_surface_item));
    STEP("surface_handle", ProGeomitemToSurface(
        (ProGeomitem *)&target_surface_item, &target_surface));
    STEP("surface_type", ProSurfaceTypeGet(target_surface, &target_surface_type));
    STEP("surface_area", ProSurfaceAreaEval(target_surface, &actual_area));
    if (target_surface_type != PRO_SRF_PLANE ||
        !near_value(actual_area, expected_area, fmax(0.001, expected_area * 1.0e-8)))
    {
        status = PRO_TK_BAD_CONTEXT; stage = "target_surface_guard"; goto failure;
    }
    STEP("surface_owner", ProGeomitemFeatureGet(
        (ProGeomitem *)&target_surface_item, &owner_feature));
    STEP("surface_owner_name", ProModelitemNameGet(
        (ProModelitem *)&owner_feature, actual_owner_name));
    if (owner_feature.id != owner_feature_id ||
        _wcsicmp(actual_owner_name, owner_feature_name) != 0)
    {
        status = PRO_TK_BAD_CONTEXT; stage = "surface_owner_guard"; goto failure;
    }
    for (axis = 0; axis < 3; ++axis)
    {
        direction[0] = direction[1] = direction[2] = 0.0;
        direction[axis] = 1.0;
        STEP("surface_extremes", ProSurfaceExtremesEval(
            target_surface, direction, min_point, max_point));
        surface_min[axis] = min_point[axis];
        surface_max[axis] = max_point[axis];
    }
    if (surface_max[2] - surface_min[2] > 1.0e-6 ||
        center_u - spacing - diameter / 2.0 < surface_min[0] ||
        center_u + spacing + diameter / 2.0 > surface_max[0] ||
        center_v - diameter / 2.0 < surface_min[1] ||
        center_v + diameter / 2.0 > surface_max[1])
    {
        status = PRO_TK_BAD_CONTEXT; stage = "hole_layout_guard"; goto failure;
    }
    STEP("thickness_dimension", ProSmtPartThicknessGet(
        (ProPart)model, &thickness_dimension));
    STEP("thickness_value", ProDimensionValueGet(&thickness_dimension, &thickness));
    STEP("orientation_lookup", ProModelitemByNameInit(
        model, PRO_SURFACE, argv[9], &orientation_item));
    STEP("surface_selection", ProSelectionAlloc(
        NULL, &target_surface_item, &target_selection));
    STEP("orientation_selection", ProSelectionAlloc(
        NULL, &orientation_item, &orientation_selection));
    STEP("surface_reference", ProSelectionToReference(
        target_selection, &target_reference));
    STEP("orientation_reference", ProSelectionToReference(
        orientation_selection, &orientation_reference));
    STEP("mass_before", ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &before_mass));

    STEP("tree", ProElementAlloc(PRO_E_FEATURE_TREE, &tree));
    STEP("feature_type", add_int(tree, PRO_E_FEATURE_TYPE, PRO_FEAT_CUT));
    STEP("feature_form", add_int(tree, PRO_E_FEATURE_FORM, PRO_EXTRUDE));
    STEP("feature_name", add_name(tree, PRO_E_STD_FEATURE_NAME, feature_name));
    STEP("solid_type", add_int(
        tree, PRO_E_EXT_SURF_CUT_SOLID_TYPE, PRO_EXT_FEAT_TYPE_SOLID));
    STEP("material_remove", add_int(
        tree, PRO_E_REMOVE_MATERIAL, PRO_EXT_MATERIAL_REMOVE));
    STEP("not_thin", add_int(
        tree, PRO_E_FEAT_FORM_IS_THIN, PRO_EXT_FEAT_FORM_NO_THIN));
    STEP("standard_section", add_compound(
        tree, PRO_E_STD_SECTION, &standard_section));
    STEP("setup_plane", add_compound(
        standard_section, PRO_E_STD_SEC_SETUP_PLANE, &setup_plane));
    STEP("section_plane", add_ref(
        setup_plane, PRO_E_STD_SEC_PLANE, target_reference));
    STEP("view_direction", add_int(
        setup_plane, PRO_E_STD_SEC_PLANE_VIEW_DIR, PRO_SEC_VIEW_DIR_SIDE_ONE));
    STEP("orientation_direction", add_int(
        setup_plane, PRO_E_STD_SEC_PLANE_ORIENT_DIR, PRO_SEC_ORIENT_DIR_UP));
    STEP("orientation_reference_element", add_ref(
        setup_plane, PRO_E_STD_SEC_PLANE_ORIENT_REF, orientation_reference));
    STEP("depth", add_compound(tree, PRO_E_STD_EXT_DEPTH, &depth_tree));
    STEP("depth_from", add_compound(
        depth_tree, PRO_E_EXT_DEPTH_FROM, &depth_from));
    STEP("depth_from_all", add_int(
        depth_from, PRO_E_EXT_DEPTH_FROM_TYPE, PRO_EXT_DEPTH_FROM_ALL));
    STEP("depth_to", add_compound(depth_tree, PRO_E_EXT_DEPTH_TO, &depth_to));
    STEP("depth_to_all", add_int(
        depth_to, PRO_E_EXT_DEPTH_TO_TYPE, PRO_EXT_DEPTH_TO_ALL));
    STEP("model_item", ProMdlToModelitem(model, &model_item));
    STEP("model_selection", ProSelectionAlloc(NULL, &model_item, &model_selection));
    STEP("options", ProArrayAlloc(
        1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&options));
    options[0] = PRO_FEAT_CR_INCOMPLETE_FEAT;
    status = ProFeatureWithoptionsCreate(model_selection, tree, options,
        PRO_REGEN_NO_FLAGS, &feature, &errors);
    if (status != PRO_TK_NO_ERROR) { stage = "incomplete_feature_create"; goto failure; }
    created = 1;
    ProArrayFree((ProArray *)&options); options = NULL;
    if (errors.error_list != NULL)
    {
        ProArrayFree((ProArray *)&errors.error_list);
        errors.error_list = NULL; errors.error_number = 0;
    }
    STEP("tree_extract", ProFeatureElemtreeExtract(
        &feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &extracted));
    STEP("sketch_element", tree_element(
        extracted, sketch_path, 2, &sketch_element));
    STEP("section_handle", ProElementSpecialvalueGet(
        sketch_element, NULL, (ProAppData *)&section));
    STEP("three_circles", three_circle_section_build(
        section, diameter, spacing, center_u, center_v, entity_ids));
    status = tree_element(extracted, direction_path, 1, &read_element);
    if (status == PRO_TK_NO_ERROR)
        STEP("direction_set", ProElementIntegerSet(
            read_element, PRO_EXT_CR_IN_SIDE_ONE));
    else
        STEP("direction_add", add_int(
            extracted, PRO_E_STD_DIRECTION, PRO_EXT_CR_IN_SIDE_ONE));
    status = tree_element(extracted, material_path, 1, &read_element);
    if (status == PRO_TK_NO_ERROR)
        STEP("material_side_set", ProElementIntegerSet(
            read_element, PRO_EXT_MATERIAL_SIDE_TWO));
    STEP("redefine_options", ProArrayAlloc(
        1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&options));
    options[0] = PRO_FEAT_CR_DEFINE_MISS_ELEMS;
    status = ProFeatureWithoptionsRedefine(NULL, &feature, extracted, options,
        PRO_REGEN_NO_FLAGS, &errors);
    if (status != PRO_TK_NO_ERROR) { stage = "feature_redefine"; goto failure; }
    STEP("regenerate", ProSolidRegenerate((ProSolid)model, PRO_REGEN_FORCE_REGEN));
    STEP("feature_type_readback", ProFeatureTypeGet(&feature, &feature_type));
    STEP("feature_status", ProFeatureStatusGet(&feature, &feature_status));
    if (feature_type != PRO_FEAT_CUT || feature_status != PRO_FEAT_ACTIVE)
    {
        status = PRO_TK_GENERAL_ERROR; stage = "feature_status_guard"; goto failure;
    }
    memset(&feature_surfaces, 0, sizeof(feature_surfaces));
    status = ProFeatureGeomitemVisit(&feature, PRO_SURFACE,
        feature_surface_action, NULL, (ProAppData)&feature_surfaces);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
    {
        stage = "feature_surface_visit"; goto failure;
    }
    expected_cylindrical_area = 3.0 * 3.14159265358979323846 *
        diameter * thickness;
    if (feature_surfaces.cylindrical_count < 3 ||
        feature_surfaces.cylindrical_count > 6 ||
        !near_value(feature_surfaces.cylindrical_area,
            expected_cylindrical_area,
            fmax(0.02, expected_cylindrical_area * 0.002)))
    {
        ProMassProperty debug_mass;
        ProError debug_mass_status = ProSolidMassPropertyWithDensityGet(
            (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &debug_mass);
        fputs("{\"ok\":false,\"api_only\":true,"
            "\"stage\":\"cylindrical_surface_guard\",", out);
        fprintf(out,
            "\"error_code\":%d,\"feature_surface_count\":%d,"
            "\"cylindrical_surface_count\":%d,"
            "\"cylindrical_area_mm2\":%.15g,\"mass_status\":%d,"
            "\"volume_before_mm3\":%.15g,\"volume_current_mm3\":%.15g}\n",
            PRO_TK_GENERAL_ERROR, feature_surfaces.surface_count,
            feature_surfaces.cylindrical_count,
            feature_surfaces.cylindrical_area, debug_mass_status,
            before_mass.volume,
            debug_mass_status == PRO_TK_NO_ERROR ? debug_mass.volume : -1.0);
        exit_code = 1;
        goto cleanup;
    }
    STEP("mass_after", ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0, &after_mass));
    actual_removed = before_mass.volume - after_mass.volume;
    expected_removed = 3.0 * 3.14159265358979323846 *
        (diameter / 2.0) * (diameter / 2.0) * thickness;
    if (!(actual_removed > 0.0) ||
        !near_value(actual_removed, expected_removed, fmax(0.05, expected_removed * 0.002)))
    {
        status = PRO_TK_GENERAL_ERROR; stage = "removed_volume_guard"; goto failure;
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
    fprintf(out,
        ",\"feature_id\":%d,\"feature_type_code\":%d,\"feature_status\":%d,"
        "\"target_surface_id\":%d,\"target_surface_area_mm2\":%.15g,"
        "\"target_owner_feature_id\":%d,\"target_owner_feature_name\":",
        feature.id, feature_type, feature_status, surface_id, actual_area,
        owner_feature.id);
    json_wide(out, actual_owner_name);
    fprintf(out,
        ",\"operation\":\"extrude_cut_through_all\",\"hole_count\":3,"
        "\"diameter_mm\":%.15g,\"center_spacing_mm\":%.15g,"
        "\"sketch_centers\":[[%.15g,%.15g],[%.15g,%.15g],[%.15g,%.15g]],"
        "\"section_entity_ids\":[%d,%d,%d],\"cylindrical_surface_count\":%d,"
        "\"sheet_thickness_mm\":%.15g,\"expected_removed_volume_mm3\":%.15g,"
        "\"actual_removed_volume_mm3\":%.15g,\"volume_after_mm3\":%.15g,"
        "\"extents_mm\":[%.15g,%.15g,%.15g],\"creation_error_count\":%d,"
        "\"saved_file\":",
        diameter, spacing,
        center_u - spacing, center_v, center_u, center_v,
        center_u + spacing, center_v,
        entity_ids[0], entity_ids[1], entity_ids[2],
        feature_surfaces.cylindrical_count, thickness, expected_removed,
        actual_removed, after_mass.volume, extents[0], extents[1], extents[2],
        errors.error_number);
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
    if (errors.error_list != NULL) ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL) ProArrayFree((ProArray *)&options);
    if (extracted != NULL && feature.id > 0) ProFeatureElemtreeFree(&feature, extracted);
    if (tree != NULL) ProElementFree(&tree);
    if (model_selection != NULL) ProSelectionFree(&model_selection);
    if (orientation_reference != NULL) ProReferenceFree(orientation_reference);
    if (target_reference != NULL) ProReferenceFree(target_reference);
    if (orientation_selection != NULL) ProSelectionFree(&orientation_selection);
    if (target_selection != NULL) ProSelectionFree(&target_selection);
    if (connected) ProEngineerDisconnect(&process, 10);
done:
    fclose(out);
    return exit_code;
#undef STEP
}
