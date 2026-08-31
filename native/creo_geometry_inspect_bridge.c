#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProSurface.h>
#include <ProContour.h>
#include <ProEdge.h>

typedef struct EdgeRecord
{
    int id;
    int type;
    double length;
    double center[3];
} EdgeRecord;

typedef struct SurfaceRecord
{
    int id;
    int type;
    double area;
    double xyz_min[3];
    double xyz_max[3];
} SurfaceRecord;

typedef struct InspectContext
{
    ProSurface current_surface;
    EdgeRecord edges[256];
    int edge_count;
    SurfaceRecord surfaces[128];
    int surface_count;
} InspectContext;

static void write_wide_json_string(FILE *out, const wchar_t *text)
{
    int bytes;
    char *utf8;
    const unsigned char *cursor;
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
    fputc('"', out);
    cursor = (const unsigned char *)utf8;
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
    free(utf8);
}

static ProError edge_action(
    ProEdge edge,
    ProError filter_status,
    ProAppData app_data)
{
    InspectContext *context = (InspectContext *)app_data;
    ProVector point;
    ProVector deriv1;
    ProVector deriv2;
    ProVector tangent;
    ProError status;
    int id;
    int index;
    int sample;
    (void)filter_status;

    status = ProEdgeIdGet(edge, &id);
    if (status != PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;
    for (index = 0; index < context->edge_count; ++index)
        if (context->edges[index].id == id)
            return PRO_TK_NO_ERROR;
    if (context->edge_count >= 256)
        return PRO_TK_NO_ERROR;
    index = context->edge_count++;
    memset(&context->edges[index], 0, sizeof(EdgeRecord));
    context->edges[index].id = id;
    ProEdgeTypeGet(edge, (ProEnttype *)&context->edges[index].type);
    ProEdgeLengthEval(edge, &context->edges[index].length);
    for (sample = 0; sample < 4; ++sample)
    {
        status = ProEdgeXyzdataEval(
            edge,
            (double)sample / 4.0,
            point,
            deriv1,
            deriv2,
            tangent);
        if (status == PRO_TK_NO_ERROR)
        {
            context->edges[index].center[0] += point[0] / 4.0;
            context->edges[index].center[1] += point[1] / 4.0;
            context->edges[index].center[2] += point[2] / 4.0;
        }
    }
    return PRO_TK_NO_ERROR;
}

static ProError contour_action(
    ProContour contour,
    ProError filter_status,
    ProAppData app_data)
{
    InspectContext *context = (InspectContext *)app_data;
    (void)filter_status;
    return ProContourEdgeVisit(
        context->current_surface,
        contour,
        edge_action,
        NULL,
        app_data);
}

static ProError surface_action(
    ProSurface surface,
    ProError filter_status,
    ProAppData app_data)
{
    InspectContext *context = (InspectContext *)app_data;
    SurfaceRecord *record;
    ProVector direction;
    ProVector minimum_point;
    ProVector maximum_point;
    int axis;
    (void)filter_status;
    if (context->surface_count < 128)
    {
        record = &context->surfaces[context->surface_count++];
        memset(record, 0, sizeof(SurfaceRecord));
        ProSurfaceIdGet(surface, &record->id);
        ProSurfaceTypeGet(surface, (ProSrftype *)&record->type);
        ProSurfaceAreaEval(surface, &record->area);
        for (axis = 0; axis < 3; ++axis)
        {
            direction[0] = direction[1] = direction[2] = 0.0;
            direction[axis] = 1.0;
            if (ProSurfaceExtremesEval(
                    surface,
                    direction,
                    minimum_point,
                    maximum_point) == PRO_TK_NO_ERROR)
            {
                record->xyz_min[axis] = minimum_point[axis];
                record->xyz_max[axis] = maximum_point[axis];
            }
        }
    }
    context->current_surface = surface;
    return ProSurfaceContourVisit(
        surface,
        contour_action,
        NULL,
        app_data);
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProPath model_path;
    ProMdlName model_name;
    InspectContext context;
    int connected = 0;
    int index;
    int exit_code = 1;

    if (argc != 3)
        return 2;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(model_path,
        sizeof(model_path) / sizeof(model_path[0]), argv[2], _TRUNCATE);
    status = ProEngineerConnect(
        "", "", "", "", PRO_B_TRUE, 20,
        &random_choice, &process_handle);
    if (status != PRO_TK_NO_ERROR)
    {
        fprintf(out, "{\"ok\":false,\"stage\":\"connect\",\"error_code\":%d}\n", status);
        goto done;
    }
    connected = 1;
    status = ProMdlFiletypeLoad(
        model_path, PRO_MDLFILE_PART, PRO_B_FALSE, &model);
    if (status != PRO_TK_NO_ERROR)
    {
        fprintf(out, "{\"ok\":false,\"stage\":\"load\",\"error_code\":%d}\n", status);
        goto cleanup;
    }
    ProMdlNameGet(model, model_name);
    memset(&context, 0, sizeof(context));
    status = ProSolidSurfaceVisit(
        (ProSolid)model,
        surface_action,
        NULL,
        (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR)
    {
        fprintf(out, "{\"ok\":false,\"stage\":\"visit\",\"error_code\":%d}\n", status);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"readonly\":true,\"model\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"edges\":[", out);
    for (index = 0; index < context.edge_count; ++index)
    {
        EdgeRecord *record = &context.edges[index];
        if (index > 0)
            fputc(',', out);
        fprintf(out,
            "{\"id\":%d,\"type\":%d,\"length\":%.17g,"
            "\"center\":[%.17g,%.17g,%.17g]}",
            record->id,
            record->type,
            record->length,
            record->center[0],
            record->center[1],
            record->center[2]);
    }
    fputs("],\"surfaces\":[", out);
    for (index = 0; index < context.surface_count; ++index)
    {
        SurfaceRecord *record = &context.surfaces[index];
        if (index > 0)
            fputc(',', out);
        fprintf(out,
            "{\"id\":%d,\"type\":%d,\"area\":%.17g,"
            "\"min\":[%.17g,%.17g,%.17g],"
            "\"max\":[%.17g,%.17g,%.17g]}",
            record->id,
            record->type,
            record->area,
            record->xyz_min[0],
            record->xyz_min[1],
            record->xyz_min[2],
            record->xyz_max[0],
            record->xyz_max[1],
            record->xyz_max[2]);
    }
    fprintf(out, "],\"edge_count\":%d,\"surface_count\":%d}\n",
        context.edge_count, context.surface_count);
    exit_code = 0;

cleanup:
    if (connected)
        ProEngineerDisconnect(&process_handle, 10);
done:
    fclose(out);
    return exit_code;
}
