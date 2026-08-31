#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSection.h>
#include <ProArray.h>
#include <ProSelection.h>

typedef struct
{
    int id;
    Pro2dEntType type;
    double x1, y1, x2, y2;
} LineInfo;

static void json_utf8(FILE *out, const char *text)
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
    json_utf8(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", status);
    return 1;
}

static int near_value(double a, double b, double tolerance)
{
    return fabs(a - b) <= tolerance;
}

static ProError selection_for_entity(
    ProSection section, int entity_id, ProSelection *selection)
{
    return ProSectionEntityGetSelected(
        section, entity_id, PRO_ENT_WHOLE, NULL,
        PRO_VALUE_UNUSED, selection);
}

static ProError symmetry_create(
    ProSection section, int first, int second, int axis,
    int *constraint_id, int *accepted_order)
{
    ProSelection selections[3] = {NULL, NULL, NULL};
    const int orders[3][3] = {
        {2, 0, 1}, /* symmetry axis, first entity, second entity */
        {0, 2, 1}, /* first entity, symmetry axis, second entity */
        {0, 1, 2}  /* first entity, second entity, symmetry axis */
    };
    int entities[3] = {first, second, axis};
    ProError status = PRO_TK_INVALID_TYPE;
    int i, order;
    *accepted_order = -1;
    for (order = 0; order < 3; ++order)
    {
        for (i = 0; i < 3; ++i) selections[i] = NULL;
        status = selection_for_entity(
            section, entities[orders[order][0]], &selections[0]);
        if (status == PRO_TK_NO_ERROR)
            status = selection_for_entity(
                section, entities[orders[order][1]], &selections[1]);
        if (status == PRO_TK_NO_ERROR)
            status = selection_for_entity(
                section, entities[orders[order][2]], &selections[2]);
        if (status == PRO_TK_NO_ERROR)
            status = ProSectionConstraintCreate(
                section, selections, 3, PRO_CONSTRAINT_SYMMETRY,
                constraint_id);
        for (i = 0; i < 3; ++i)
            if (selections[i] != NULL) ProSelectionFree(&selections[i]);
        if (status == PRO_TK_NO_ERROR || status == PRO_TK_E_FOUND)
        {
            *accepted_order = order;
            break;
        }
    }
    return status;
}

static ProError symmetry_verify(
    ProSection section, int constraint_id,
    int first, int second, int axis)
{
    ProConstraintType type = PRO_CONSTRAINT_TYPE_UNKNOWN;
    ProConstraintStatus constraint_status = PRO_TK_CONSTRAINT_DENIED;
    int count = 0;
    int *ids = NULL;
    ProSectionPointType *senses = NULL;
    ProError status = ProSectionConstraintsGet(
        section, constraint_id, &type, &constraint_status,
        &count, &ids, &senses);
    if (status == PRO_TK_NO_ERROR)
    {
        int found_first = 0, found_second = 0, found_axis = 0, i;
        for (i = 0; i < count; ++i)
        {
            if (ids[i] == first) found_first = 1;
            if (ids[i] == second) found_second = 1;
            if (ids[i] == axis) found_axis = 1;
        }
        if (type != PRO_CONSTRAINT_SYMMETRY ||
            constraint_status != PRO_TK_CONSTRAINT_ENABLED || count != 3 ||
            !found_first || !found_second || !found_axis)
            status = PRO_TK_GENERAL_ERROR;
    }
    if (ids != NULL) ProArrayFree((ProArray *)&ids);
    if (senses != NULL) ProArrayFree((ProArray *)&senses);
    return status;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProBoolean sketch_active = PRO_B_FALSE;
    ProProcessHandle process;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProSection section = NULL;
    ProIntlist ids = NULL;
    int id_count = 0;
    LineInfo lines[32];
    int line_count = 0;
    int left = -1, right = -1, bottom = -1, top = -1;
    int x_axis = -1, y_axis = -1;
    int x_symmetry = -1, y_symmetry = -1;
    int x_order = -1, y_order = -1;
    int i;
    double expected_length, expected_width;
    double min_x = 1.0e100, max_x = -1.0e100;
    double min_y = 1.0e100, max_y = -1.0e100;
    const double tol = 1.0e-5;
    int connected = 0;
    int active_set = 0;
    int exit_code = 1;

    if (argc != 5)
    {
        fwprintf(stderr,
            L"Usage: active_sketch_symmetry <result.json> <expected_model> "
            L"<expected_length> <expected_width>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL) return 2;
    expected_length = wcstod(argv[3], NULL);
    expected_width = wcstod(argv[4], NULL);
    if (!(expected_length > 0.0) || !(expected_width > 0.0))
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
    { exit_code = fail(out, "model_name_guard",
        status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto cleanup; }
    status = ProSectionIsActive(&sketch_active);
    if (status != PRO_TK_NO_ERROR || sketch_active != PRO_B_TRUE)
    { exit_code = fail(out, "active_sketch_guard",
        status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto cleanup; }
    status = ProSectionActiveGet(&section);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "active_section_get", status); goto cleanup; }
    status = ProSectionIntentManagerModeSet(section, PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "intent_manager", status); goto cleanup; }
    status = ProSectionEntityIdsGet(section, &ids, &id_count);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "entity_ids", status); goto cleanup; }
    for (i = 0; i < id_count && line_count < 32; ++i)
    {
        Pro2dEntdef *entity = NULL;
        status = ProSectionEntityGet(section, ids[i], &entity);
        if (status != PRO_TK_NO_ERROR || entity == NULL) continue;
        if (entity->type == PRO_2D_LINE || entity->type == PRO_2D_CENTER_LINE)
        {
            Pro2dLinedef *line = (Pro2dLinedef *)entity;
            lines[line_count].id = ids[i];
            lines[line_count].type = entity->type;
            lines[line_count].x1 = line->end1[0];
            lines[line_count].y1 = line->end1[1];
            lines[line_count].x2 = line->end2[0];
            lines[line_count].y2 = line->end2[1];
            ++line_count;
        }
    }
    for (i = 0; i < line_count; ++i)
    {
        LineInfo *line = &lines[i];
        double mx = (line->x1 + line->x2) / 2.0;
        double my = (line->y1 + line->y2) / 2.0;
        if (line->type == PRO_2D_CENTER_LINE)
        {
            if (near_value(line->x1, 0.0, tol) && near_value(line->x2, 0.0, tol))
                y_axis = line->id;
            if (near_value(line->y1, 0.0, tol) && near_value(line->y2, 0.0, tol))
                x_axis = line->id;
            continue;
        }
        if (near_value(line->x1, line->x2, tol))
        {
            if (mx < min_x) { min_x = mx; left = line->id; }
            if (mx > max_x) { max_x = mx; right = line->id; }
        }
        else if (near_value(line->y1, line->y2, tol))
        {
            if (my < min_y) { min_y = my; bottom = line->id; }
            if (my > max_y) { max_y = my; top = line->id; }
        }
    }
    if (left < 0 || right < 0 || bottom < 0 || top < 0 ||
        !near_value(max_x - min_x, expected_length, tol) ||
        !near_value(max_y - min_y, expected_width, tol) ||
        !near_value(min_x + max_x, 0.0, tol) ||
        !near_value(min_y + max_y, 0.0, tol))
    { exit_code = fail(out, "rectangle_geometry_guard", PRO_TK_BAD_CONTEXT); goto cleanup; }
    if (x_axis < 0)
    {
        Pro2dClinedef axis;
        axis.type = PRO_2D_CENTER_LINE;
        axis.end1[0] = -expected_length;
        axis.end1[1] = 0.0;
        axis.end2[0] = expected_length;
        axis.end2[1] = 0.0;
        status = ProSectionEntityAdd(section, (Pro2dEntdef *)&axis, &x_axis);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "x_axis_create", status); goto cleanup; }
    }
    if (y_axis < 0)
    {
        Pro2dClinedef axis;
        axis.type = PRO_2D_CENTER_LINE;
        axis.end1[0] = 0.0;
        axis.end1[1] = -expected_width;
        axis.end2[0] = 0.0;
        axis.end2[1] = expected_width;
        status = ProSectionEntityAdd(section, (Pro2dEntdef *)&axis, &y_axis);
        if (status != PRO_TK_NO_ERROR)
        { exit_code = fail(out, "y_axis_create", status); goto cleanup; }
    }
    status = symmetry_create(
        section, left, right, y_axis, &y_symmetry, &y_order);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_FOUND)
    { exit_code = fail(out, "left_right_y_symmetry", status); goto cleanup; }
    if (status == PRO_TK_E_FOUND)
    { exit_code = fail(out, "left_right_symmetry_already_exists", status); goto cleanup; }
    status = symmetry_verify(section, y_symmetry, left, right, y_axis);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "left_right_symmetry_verify", status); goto cleanup; }
    status = symmetry_create(
        section, bottom, top, x_axis, &x_symmetry, &x_order);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_FOUND)
    { exit_code = fail(out, "bottom_top_x_symmetry", status); goto cleanup; }
    if (status == PRO_TK_E_FOUND)
    { exit_code = fail(out, "bottom_top_symmetry_already_exists", status); goto cleanup; }
    status = symmetry_verify(section, x_symmetry, bottom, top, x_axis);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "bottom_top_symmetry_verify", status); goto cleanup; }
    status = ProSectionActiveSet(section);
    if (status != PRO_TK_NO_ERROR)
    { exit_code = fail(out, "active_section_set", status); goto cleanup; }
    active_set = 1;
    fputs("{\"ok\":true,\"api_only\":true,\"active_sketch_preserved\":true,", out);
    fprintf(out,
        "\"model\":\"%ls\",\"rectangle\":{\"left_line_id\":%d,"
        "\"right_line_id\":%d,\"bottom_line_id\":%d,\"top_line_id\":%d,"
        "\"length\":%.15g,\"width\":%.15g},"
        "\"axes\":{\"x_axis_entity_id\":%d,\"y_axis_entity_id\":%d},"
        "\"constraints\":[{\"id\":%d,\"type\":\"symmetry\","
        "\"entities\":[%d,%d,%d],\"axis\":\"Y\",\"api_order\":%d},"
        "{\"id\":%d,\"type\":\"symmetry\","
        "\"entities\":[%d,%d,%d],\"axis\":\"X\",\"api_order\":%d}],"
        "\"saved\":false}\n",
        model_name, left, right, bottom, top,
        max_x - min_x, max_y - min_y, x_axis, y_axis,
        y_symmetry, left, right, y_axis, y_order,
        x_symmetry, bottom, top, x_axis, x_order);
    exit_code = 0;

cleanup:
    if (ids != NULL) ProArrayFree((ProArray *)&ids);
    if (section != NULL) ProSectionFree(&section);
    if (connected) ProEngineerDisconnect(&process, 10);
done:
    (void)active_set;
    fclose(out);
    return exit_code;
}
