#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProSection.h>
#include <ProSecdim.h>
#include <ProSecerror.h>
#include <ProArray.h>
#include <ProSelection.h>
#include <ProEdge.h>
#include <ProGeomitem.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProElemId.h>

typedef struct
{
    int id;
    double x1, y1, x2, y2;
} LineInfo;

static void json_string(FILE *out, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
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

static int fail(FILE *out, const char *stage, ProError status)
{
    fputs("{\"ok\":false,\"api_only\":true,\"stage\":", out);
    json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", status);
    return 1;
}

static int near_value(double a, double b, double tolerance)
{
    return fabs(a - b) <= tolerance;
}

static ProError project_edge(
    ProMdl model, ProSection section, int edge_id, int *section_entity_id)
{
    ProEdge edge = NULL;
    ProGeomitem geomitem;
    ProSelection selection = NULL;
    ProError status = ProEdgeInit((ProSolid)model, edge_id, &edge);
    if (status == PRO_TK_NO_ERROR)
        status = ProEdgeToGeomitem((ProSolid)model, edge, &geomitem);
    if (status == PRO_TK_NO_ERROR)
        status = ProSelectionAlloc(
            NULL, (ProModelitem *)&geomitem, &selection);
    if (status == PRO_TK_NO_ERROR)
        status = ProSectionEntityUseEdge(
            section, selection, section_entity_id);
    if (selection != NULL) ProSelectionFree(&selection);
    return status;
}

static ProError line_get(ProSection section, int id, LineInfo *line)
{
    Pro2dEntdef *entity = NULL;
    ProError status = ProSectionEntityGet(section, id, &entity);
    if (status != PRO_TK_NO_ERROR) return status;
    if (entity == NULL || entity->type != PRO_2D_LINE)
        return PRO_TK_INVALID_TYPE;
    line->id = id;
    line->x1 = ((Pro2dLinedef *)entity)->end1[0];
    line->y1 = ((Pro2dLinedef *)entity)->end1[1];
    line->x2 = ((Pro2dLinedef *)entity)->end2[0];
    line->y2 = ((Pro2dLinedef *)entity)->end2[1];
    return PRO_TK_NO_ERROR;
}

static ProError dimension_create(
    ProSection section, int first, int second,
    double place_x, double place_y, double value, int *dimension_id)
{
    int entities[2] = {first, second};
    ProSectionPointType points[2] = {PRO_ENT_WHOLE, PRO_ENT_WHOLE};
    Pro2dPnt place = {0.0, 0.0};
    ProError status;
    place[0] = place_x;
    place[1] = place_y;
    status = ProSecdimCreate(
        section, entities, points, 2,
        PRO_TK_DIM_LINE_LINE, place, dimension_id);
    if (status == PRO_TK_NO_ERROR)
        status = ProSecdimValueSet(section, *dimension_id, value);
    return status;
}

static ProError dimension_verify(
    ProSection section, int dimension_id,
    int first, int second, double expected_value)
{
    double value = 0.0;
    ProSecdimType type = PRO_TK_DIM_NONE;
    int *ids = NULL;
    ProSectionPointType *points = NULL;
    int count = 0;
    ProError status = ProSecdimValueGet(section, dimension_id, &value);
    if (status == PRO_TK_NO_ERROR)
        status = ProSecdimTypeGet(section, dimension_id, &type);
    if (status == PRO_TK_NO_ERROR)
        status = ProSecdimReferencesGet(
            section, dimension_id, &ids, &points, &count);
    if (status == PRO_TK_NO_ERROR)
    {
        if (type != PRO_TK_DIM_LINE_LINE || count != 2 ||
            !near_value(value, expected_value, 1.0e-6) ||
            !((ids[0] == first && ids[1] == second) ||
              (ids[0] == second && ids[1] == first)) ||
            points[0] != PRO_ENT_WHOLE || points[1] != PRO_ENT_WHOLE)
            status = PRO_TK_GENERAL_ERROR;
    }
    if (ids != NULL) ProArrayFree((ProArray *)&ids);
    if (points != NULL) ProArrayFree((ProArray *)&points);
    return status;
}

static ProError element_by_ids_get(
    ProElement tree, ProElemId ids[], int count, ProElement *element)
{
    ProElempath path = NULL;
    ProElempathItem items[3];
    ProError status = ProElempathAlloc(&path);
    int i;
    if (count < 1 || count > 3) return PRO_TK_BAD_INPUTS;
    if (status != PRO_TK_NO_ERROR) return status;
    for (i = 0; i < count; ++i)
    {
        items[i].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
        items[i].path_item.elem_id = ids[i];
    }
    status = ProElempathDataSet(path, items, count);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementGet(tree, path, element);
    ProElempathFree(&path);
    return status;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProBoolean random_choice = PRO_B_FALSE;
    ProBoolean sketch_active = PRO_B_FALSE;
    ProProcessHandle process;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProSection section = NULL;
    ProFeature feature;
    ProModelitem feature_item;
    ProElement extracted_tree = NULL;
    ProElement sketch_element = NULL;
    ProElemId sketch_path_ids[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist redefine_errors = {NULL, 0};
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProIntlist entity_ids = NULL;
    ProIntlist dimension_ids = NULL;
    ProIntlist constraint_ids = NULL;
    int entity_count = 0, dimension_count = 0, constraint_count = 0;
    LineInfo inner[4];
    int inner_count = 0;
    int inner_left = -1, inner_right = -1, inner_bottom = -1, inner_top = -1;
    int outer_left = -1, outer_right = -1, outer_bottom = -1, outer_top = -1;
    int projected_ids[4] = {-1, -1, -1, -1};
    int created_dimensions[4] = {-1, -1, -1, -1};
    int old_dimensions[8];
    int old_dimension_count = 0;
    double inspected_dimension_values[32];
    int inspected_dimension_ids[32];
    int inspected_dimension_count = 0;
    int deleted_dimensions = 0, deleted_symmetry = 0;
    int model_edge_ids[4];
    double inner_length, inner_width, inset;
    double inner_min_x = 1.0e100, inner_max_x = -1.0e100;
    double inner_min_y = 1.0e100, inner_max_y = -1.0e100;
    double outer_min_x = 1.0e100, outer_max_x = -1.0e100;
    double outer_min_y = 1.0e100, outer_max_y = -1.0e100;
    const double tol = 1.0e-5;
    ProError status;
    int connected = 0, active_set = 0, exit_code = 1, i;
    int closed_feature_mode = 0, feature_id = -1;
    const wchar_t *feature_name = L"";

    if (argc != 10 && argc != 12)
    {
        fwprintf(stderr,
            L"Usage: active_inset_dims <result.json> <expected_model> "
            L"<inner_length> <inner_width> <inset> "
            L"<edge1> <edge2> <edge3> <edge4> "
            L"[<feature_name> <feature_id>]\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL) return 2;
    inner_length = wcstod(argv[3], NULL);
    inner_width = wcstod(argv[4], NULL);
    inset = wcstod(argv[5], NULL);
    for (i = 0; i < 4; ++i) model_edge_ids[i] = _wtoi(argv[6 + i]);
    if (argc == 12)
    {
        closed_feature_mode = 1;
        feature_name = argv[10];
        feature_id = _wtoi(argv[11]);
    }
    if (!(inner_length > 0.0) || !(inner_width > 0.0) || !(inset > 0.0))
    { exit_code = fail(out, "input", PRO_TK_BAD_INPUTS); goto done; }

    status = ProEngineerConnect("", "", "", "", PRO_B_TRUE, 20,
        &random_choice, &process);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "connect", status); goto done; }
    connected = 1;
    status = ProMdlCurrentGet(&model);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "current_model", status); goto cleanup; }
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(model_name, argv[2]) != 0)
    { exit_code = fail(out, "model_guard",
        status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto cleanup; }
    status = ProSectionIsActive(&sketch_active);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "active_sketch_guard",
        status); goto cleanup; }
    if (sketch_active == PRO_B_TRUE)
    {
        status = ProSectionActiveGet(&section);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "active_section_get", status); goto cleanup; }
    }
    else if (closed_feature_mode)
    {
        status = ProModelitemByNameInit(
            model, PRO_FEATURE, (wchar_t *)feature_name, &feature_item);
        if (status != PRO_TK_NO_ERROR || feature_item.id != feature_id)
        { exit_code = fail(out, "feature_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto cleanup; }
        status = ProFeatureInit((ProSolid)model, feature_id, &feature);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "feature_init", status); goto cleanup; }
        status = ProFeatureElemtreeExtract(
            &feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &extracted_tree);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "feature_tree_extract", status); goto cleanup; }
        status = element_by_ids_get(
            extracted_tree, sketch_path_ids, 2, &sketch_element);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "sketch_element_get", status); goto cleanup; }
        status = ProElementSpecialvalueGet(
            sketch_element, NULL, (ProAppData *)&section);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "section_handle_get", status); goto cleanup; }
    }
    else
    { exit_code = fail(out, "active_sketch_guard", PRO_TK_BAD_CONTEXT); goto cleanup; }
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "intent_manager", status); goto cleanup; }
    status = ProSectionIntentManagerModeSet(section, PRO_B_FALSE);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "explicit_dimension_mode", status); goto cleanup; }

    status = ProSectionEntityIdsGet(section, &entity_ids, &entity_count);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "entity_ids", status); goto cleanup; }
    for (i = 0; i < entity_count && inner_count < 4; ++i)
    {
        ProBoolean is_projection = PRO_B_FALSE;
        Pro2dEntdef *entity = NULL;
        if (ProSectionEntityIsProjection(
                section, entity_ids[i], &is_projection) != PRO_TK_NO_ERROR ||
            is_projection == PRO_B_TRUE)
            continue;
        if (ProSectionEntityGet(section, entity_ids[i], &entity) != PRO_TK_NO_ERROR ||
            entity == NULL || entity->type != PRO_2D_LINE)
            continue;
        line_get(section, entity_ids[i], &inner[inner_count]);
        ++inner_count;
    }
    if (inner_count != 4)
    { exit_code = fail(out, "inner_rectangle_line_guard", PRO_TK_BAD_CONTEXT); goto cleanup; }
    for (i = 0; i < inner_count; ++i)
    {
        LineInfo *line = &inner[i];
        if (near_value(line->x1, line->x2, tol))
        {
            double x = (line->x1 + line->x2) / 2.0;
            if (x < inner_min_x) { inner_min_x = x; inner_left = line->id; }
            if (x > inner_max_x) { inner_max_x = x; inner_right = line->id; }
        }
        else if (near_value(line->y1, line->y2, tol))
        {
            double y = (line->y1 + line->y2) / 2.0;
            if (y < inner_min_y) { inner_min_y = y; inner_bottom = line->id; }
            if (y > inner_max_y) { inner_max_y = y; inner_top = line->id; }
        }
    }
    if (inner_left < 0 || inner_right < 0 || inner_bottom < 0 || inner_top < 0 ||
        !near_value(inner_max_x - inner_min_x, inner_length, tol) ||
        !near_value(inner_max_y - inner_min_y, inner_width, tol))
    { exit_code = fail(out, "inner_rectangle_geometry_guard", PRO_TK_BAD_CONTEXT); goto cleanup; }

    status = ProSecdimIdsGet(section, &dimension_ids, &dimension_count);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
    { exit_code = fail(out, "dimension_ids", status); goto cleanup; }
    for (i = 0; i < dimension_count; ++i)
    {
        double value = 0.0;
        if (ProSecdimValueGet(section, dimension_ids[i], &value) == PRO_TK_NO_ERROR)
        {
            if (inspected_dimension_count < 32)
            {
                inspected_dimension_ids[inspected_dimension_count] = dimension_ids[i];
                inspected_dimension_values[inspected_dimension_count] = value;
                ++inspected_dimension_count;
            }
        }
        if ((near_value(fabs(value), inner_length, tol) ||
             near_value(fabs(value), inner_width, tol)) &&
            inspected_dimension_count > 0)
            if (old_dimension_count < 8)
                old_dimensions[old_dimension_count++] = dimension_ids[i];
    }
    if (dimension_count == 4 && inspected_dimension_count == 4 &&
        old_dimension_count == 0)
    {
        int already_satisfied = 1;
        for (i = 0; i < dimension_count; ++i)
        {
            double value = 0.0;
            ProSecdimType type = PRO_TK_DIM_NONE;
            int *refs = NULL;
            ProSectionPointType *points = NULL;
            int ref_count = 0;
            ProBoolean first_projection = PRO_B_FALSE;
            ProBoolean second_projection = PRO_B_FALSE;
            status = ProSecdimValueGet(section, dimension_ids[i], &value);
            if (status == PRO_TK_NO_ERROR)
                status = ProSecdimTypeGet(section, dimension_ids[i], &type);
            if (status == PRO_TK_NO_ERROR)
                status = ProSecdimReferencesGet(
                    section, dimension_ids[i], &refs, &points, &ref_count);
            if (status == PRO_TK_NO_ERROR && ref_count == 2)
                status = ProSectionEntityIsProjection(
                    section, refs[0], &first_projection);
            if (status == PRO_TK_NO_ERROR && ref_count == 2)
                status = ProSectionEntityIsProjection(
                    section, refs[1], &second_projection);
            if (status != PRO_TK_NO_ERROR ||
                !near_value(value, inset, tol) ||
                type != PRO_TK_DIM_LINE_LINE || ref_count != 2 ||
                first_projection == second_projection ||
                points[0] != PRO_ENT_WHOLE || points[1] != PRO_ENT_WHOLE)
                already_satisfied = 0;
            if (refs != NULL) ProArrayFree((ProArray *)&refs);
            if (points != NULL) ProArrayFree((ProArray *)&points);
        }
        if (already_satisfied)
        {
            fprintf(out,
                "{\"ok\":true,\"api_only\":true,\"model\":\"%ls\","
                "\"already_satisfied\":true,\"saved\":false,"
                "\"dimension_count\":4,\"dimension_values\":["
                "%.15g,%.15g,%.15g,%.15g],"
                "\"dimension_type\":\"line_to_line\","
                "\"reference_pattern\":\"projected_outer_edge_to_inner_rectangle_edge\"}\n",
                model_name,
                inspected_dimension_values[0], inspected_dimension_values[1],
                inspected_dimension_values[2], inspected_dimension_values[3]);
            exit_code = 0;
            goto cleanup;
        }
    }
    for (i = 0; i < old_dimension_count; ++i)
    {
        status = ProSecdimDelete(section, old_dimensions[i]);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "old_dimension_delete", status); goto cleanup; }
        ++deleted_dimensions;
    }
    if (deleted_dimensions != 2)
    {
        fputs("{\"ok\":false,\"api_only\":true,"
            "\"stage\":\"old_dimension_count_guard\",\"error_code\":-8,"
            "\"section_dimension_count\":", out);
        fprintf(out, "%d,\"matched_dimension_count\":%d,\"dimensions\":[",
            dimension_count, old_dimension_count);
        for (i = 0; i < inspected_dimension_count; ++i)
        {
            if (i > 0) fputc(',', out);
            fprintf(out, "{\"id\":%d,\"value\":%.17g}",
                inspected_dimension_ids[i], inspected_dimension_values[i]);
        }
        fputs("]}\n", out);
        exit_code = 1;
        goto cleanup;
    }

    status = ProSectionConstraintsIdsGet(section, &constraint_ids, &constraint_count);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
    { exit_code = fail(out, "constraint_ids", status); goto cleanup; }
    for (i = 0; i < constraint_count; ++i)
    {
        ProConstraintType type = PRO_CONSTRAINT_TYPE_UNKNOWN;
        ProConstraintStatus constraint_status;
        int count = 0;
        int *ids = NULL;
        ProSectionPointType *senses = NULL;
        if (ProSectionConstraintsGet(section, constraint_ids[i],
                &type, &constraint_status, &count, &ids, &senses) == PRO_TK_NO_ERROR &&
            type == PRO_CONSTRAINT_SYMMETRY)
        {
            status = ProSectionConstraintDelete(section, constraint_ids[i]);
            if (status != PRO_TK_NO_ERROR)
            { if (ids) ProArrayFree((ProArray *)&ids); if (senses) ProArrayFree((ProArray *)&senses);
              exit_code = fail(out, "symmetry_constraint_delete", status); goto cleanup; }
            ++deleted_symmetry;
        }
        if (ids != NULL) ProArrayFree((ProArray *)&ids);
        if (senses != NULL) ProArrayFree((ProArray *)&senses);
    }

    for (i = 0; i < 4; ++i)
    {
        status = project_edge(model, section, model_edge_ids[i], &projected_ids[i]);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "edge_projection", status); goto cleanup; }
    }
    for (i = 0; i < 4; ++i)
    {
        LineInfo line;
        status = line_get(section, projected_ids[i], &line);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "projected_line_readback", status); goto cleanup; }
        if (near_value(line.x1, line.x2, tol))
        {
            double x = (line.x1 + line.x2) / 2.0;
            if (x < outer_min_x) { outer_min_x = x; outer_left = line.id; }
            if (x > outer_max_x) { outer_max_x = x; outer_right = line.id; }
        }
        else if (near_value(line.y1, line.y2, tol))
        {
            double y = (line.y1 + line.y2) / 2.0;
            if (y < outer_min_y) { outer_min_y = y; outer_bottom = line.id; }
            if (y > outer_max_y) { outer_max_y = y; outer_top = line.id; }
        }
    }
    if (outer_left < 0 || outer_right < 0 || outer_bottom < 0 || outer_top < 0 ||
        !near_value(inner_min_x - outer_min_x, inset, tol) ||
        !near_value(outer_max_x - inner_max_x, inset, tol) ||
        !near_value(inner_min_y - outer_min_y, inset, tol) ||
        !near_value(outer_max_y - inner_max_y, inset, tol))
    { exit_code = fail(out, "projected_outer_geometry_guard", PRO_TK_BAD_CONTEXT); goto cleanup; }

    status = dimension_create(section, outer_left, inner_left,
        outer_min_x - 20.0, 0.0, inset, &created_dimensions[0]);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "left_dimension_create", status); goto cleanup; }
    status = dimension_create(section, inner_right, outer_right,
        outer_max_x + 20.0, 0.0, inset, &created_dimensions[1]);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "right_dimension_create", status); goto cleanup; }
    status = dimension_create(section, outer_bottom, inner_bottom,
        0.0, outer_min_y - 20.0, inset, &created_dimensions[2]);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "bottom_dimension_create", status); goto cleanup; }
    status = dimension_create(section, inner_top, outer_top,
        0.0, outer_max_y + 20.0, inset, &created_dimensions[3]);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "top_dimension_create", status); goto cleanup; }

    status = dimension_verify(section, created_dimensions[0], outer_left, inner_left, inset);
    if (status == PRO_TK_NO_ERROR)
        status = dimension_verify(section, created_dimensions[1], inner_right, outer_right, inset);
    if (status == PRO_TK_NO_ERROR)
        status = dimension_verify(section, created_dimensions[2], outer_bottom, inner_bottom, inset);
    if (status == PRO_TK_NO_ERROR)
        status = dimension_verify(section, created_dimensions[3], inner_top, outer_top, inset);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "dimension_readback", status); goto cleanup; }

    {
        ProWSecerror errors = NULL;
        int error_count = 0;
        status = ProSecerrorAlloc(&errors);
        if (status == PRO_TK_NO_ERROR)
            status = ProSectionRegenerate(section, &errors);
        if (errors != NULL) ProSecerrorCount(&errors, &error_count);
        if (errors != NULL) ProSecerrorFree(&errors);
        if (status != PRO_TK_NO_ERROR || error_count != 0)
        { exit_code = fail(out, "section_regenerate",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status); goto cleanup; }
    }
    if (sketch_active == PRO_B_TRUE)
    {
        status = ProSectionActiveSet(section);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "active_section_set", status); goto cleanup; }
        active_set = 1;
    }
    else
    {
        status = ProArrayAlloc(
            1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&options);
        if (status == PRO_TK_NO_ERROR)
        {
            options[0] = PRO_FEAT_CR_DEFINE_MISS_ELEMS;
            status = ProFeatureWithoptionsRedefine(
                NULL, &feature, extracted_tree, options,
                PRO_REGEN_NO_FLAGS, &redefine_errors);
        }
        if (status != PRO_TK_NO_ERROR || redefine_errors.error_number != 0)
        { exit_code = fail(out, "feature_redefine",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status); goto cleanup; }
        status = ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
        if (status == PRO_TK_REGEN_AGAIN)
            status = ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
        if (status == PRO_TK_NO_ERROR)
            status = ProFeatureStatusGet(&feature, &feature_status);
        if (status != PRO_TK_NO_ERROR || feature_status != PRO_FEAT_ACTIVE)
        { exit_code = fail(out, "feature_regenerate_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status); goto cleanup; }
        status = ProMdlSave(model);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "model_save", status); goto cleanup; }
    }
    fprintf(out,
        "{\"ok\":true,\"api_only\":true,\"model\":\"%ls\","
        "\"active_sketch_preserved\":%s,\"saved\":%s,"
        "\"old_dimensions_deleted\":%d,\"symmetry_constraints_deleted\":%d,"
        "\"projected_model_edges\":[%d,%d,%d,%d],"
        "\"projected_section_entities\":[%d,%d,%d,%d],"
        "\"inset_dimensions\":["
        "{\"side\":\"left\",\"id\":%d,\"value\":%.15g},"
        "{\"side\":\"right\",\"id\":%d,\"value\":%.15g},"
        "{\"side\":\"bottom\",\"id\":%d,\"value\":%.15g},"
        "{\"side\":\"top\",\"id\":%d,\"value\":%.15g}]}\n",
        model_name,
        sketch_active == PRO_B_TRUE ? "true" : "false",
        sketch_active == PRO_B_TRUE ? "false" : "true",
        deleted_dimensions, deleted_symmetry,
        model_edge_ids[0], model_edge_ids[1], model_edge_ids[2], model_edge_ids[3],
        projected_ids[0], projected_ids[1], projected_ids[2], projected_ids[3],
        created_dimensions[0], inset, created_dimensions[1], inset,
        created_dimensions[2], inset, created_dimensions[3], inset);
    exit_code = 0;

cleanup:
    if (redefine_errors.error_list != NULL)
        ProArrayFree((ProArray *)&redefine_errors.error_list);
    if (options != NULL) ProArrayFree((ProArray *)&options);
    if (constraint_ids != NULL) ProArrayFree((ProArray *)&constraint_ids);
    if (dimension_ids != NULL) ProArrayFree((ProArray *)&dimension_ids);
    if (entity_ids != NULL) ProArrayFree((ProArray *)&entity_ids);
    if (section != NULL && sketch_active == PRO_B_TRUE) ProSectionFree(&section);
    if (extracted_tree != NULL)
        ProFeatureElemtreeFree(&feature, extracted_tree);
    if (connected) ProEngineerDisconnect(&process, 10);
done:
    (void)active_set;
    fclose(out);
    return exit_code;
}
