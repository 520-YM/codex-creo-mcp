#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProParameter.h>
#include <ProUtil.h>

typedef struct
{
    FILE *out;
    int first;
    int count;
} ParameterContext;

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
        case '\b': fputs("\\b", out); break;
        case '\f': fputs("\\f", out); break;
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

static const char *model_type_name(ProMdlType type)
{
    switch (type)
    {
    case PRO_MDL_PART: return "part";
    case PRO_MDL_ASSEMBLY: return "assembly";
    case PRO_MDL_DRAWING: return "drawing";
    case PRO_MDL_3DSECTION: return "3d_section";
    case PRO_MDL_2DSECTION: return "2d_section";
    case PRO_MDL_LAYOUT: return "layout";
    case PRO_MDL_MFG: return "manufacturing";
    case PRO_MDL_REPORT: return "report";
    case PRO_MDL_MARKUP: return "markup";
    case PRO_MDL_DIAGRAM: return "diagram";
    default: return "unknown";
    }
}

static ProError count_feature_action(
    ProFeature *feature,
    ProError status,
    ProAppData app_data)
{
    int *count = (int *)app_data;
    (void)feature;
    (void)status;
    ++(*count);
    return PRO_TK_NO_ERROR;
}

static ProError write_parameter_action(
    ProParameter *parameter,
    ProError status,
    ProAppData app_data)
{
    ParameterContext *context = (ParameterContext *)app_data;
    ProParamvalue value;
    ProError value_status;

    (void)status;
    value.type = PRO_PARAM_VOID;
    value_status = ProParameterValueWithUnitsGet(parameter, &value, NULL);

    if (!context->first)
        fputc(',', context->out);
    context->first = 0;
    ++context->count;

    fputs("{\"name\":", context->out);
    write_wide_json_string(context->out, parameter->id);

    if (value_status != PRO_TK_NO_ERROR)
    {
        fprintf(context->out,
            ",\"type\":\"unavailable\",\"value\":null,\"error_code\":%d}",
            value_status);
        return PRO_TK_NO_ERROR;
    }

    switch (value.type)
    {
    case PRO_PARAM_DOUBLE:
        fprintf(context->out, ",\"type\":\"double\",\"value\":%.17g}", value.value.d_val);
        break;
    case PRO_PARAM_STRING:
        fputs(",\"type\":\"string\",\"value\":", context->out);
        write_wide_json_string(context->out, value.value.s_val);
        fputc('}', context->out);
        break;
    case PRO_PARAM_INTEGER:
        fprintf(context->out, ",\"type\":\"integer\",\"value\":%d}", value.value.i_val);
        break;
    case PRO_PARAM_BOOLEAN:
        fprintf(context->out, ",\"type\":\"boolean\",\"value\":%s}",
            value.value.l_val ? "true" : "false");
        break;
    default:
        fputs(",\"type\":\"unset\",\"value\":null}", context->out);
        break;
    }

    return PRO_TK_NO_ERROR;
}

static int write_error(FILE *out, const char *stage, ProError error_code)
{
    fputs("{\"ok\":false,\"readonly\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

int main(int argc, char **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model;
    ProMdlName model_name;
    ProMdlType model_type;
    ProModelitem model_item;
    ProError parameter_status;
    ProError feature_status = PRO_TK_E_NOT_FOUND;
    ProError stored_outline_status = PRO_TK_E_NOT_FOUND;
    ProError computed_outline_status = PRO_TK_E_NOT_FOUND;
    Pro3dPnt stored_outline[2] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
    Pro3dPnt computed_outline[2] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
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
    ParameterContext parameter_context;
    int feature_count = 0;
    int ptc_numeric_version = 0;
    int exit_code = 0;

    if (argc > 1)
    {
        if (fopen_s(&out, argv[1], "wb") != 0 || out == NULL)
        {
            fprintf(stderr, "Unable to open output file: %s\n", argv[1]);
            return 2;
        }
    }

    status = ProEngineerConnect(
        "",
        "",
        "",
        "",
        PRO_B_TRUE,
        20,
        &random_choice,
        &process_handle);

    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "connect", status);
        goto done;
    }

    status = ProMdlCurrentGet(&model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto disconnect;
    }

    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_name", status);
        goto disconnect;
    }

    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_type", status);
        goto disconnect;
    }

    ProToolkitMajorVersionGet(&ptc_numeric_version);

    if (model_type == PRO_MDL_PART ||
        model_type == PRO_MDL_ASSEMBLY ||
        model_type == PRO_MDL_MFG)
    {
        feature_status = ProSolidFeatVisit(
            (ProSolid)model,
            count_feature_action,
            NULL,
            (ProAppData)&feature_count);

        stored_outline_status = ProSolidOutlineGet(
            (ProSolid)model,
            stored_outline);
        computed_outline_status = ProSolidOutlineCompute(
            (ProSolid)model,
            outline_matrix,
            outline_excludes,
            (int)(sizeof(outline_excludes) / sizeof(outline_excludes[0])),
            computed_outline);
    }

    status = ProMdlToModelitem(model, &model_item);
    parameter_context.out = out;
    parameter_context.first = 1;
    parameter_context.count = 0;

    fputs("{\"ok\":true,\"readonly\":true,", out);
    fprintf(out, "\"ptc_numeric_version\":%d,", ptc_numeric_version);
    fputs("\"model\":{\"name\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"type\":", out);
    write_utf8_json_string(out, model_type_name(model_type));
    fprintf(out, ",\"type_code\":%d,", (int)model_type);
    fprintf(out, "\"feature_count\":%d,\"feature_status\":%d,",
        feature_count, feature_status);
    fprintf(out,
        "\"outline\":{"
        "\"stored\":{\"status\":%d,\"min\":[%.15g,%.15g,%.15g],"
        "\"max\":[%.15g,%.15g,%.15g],\"size\":[%.15g,%.15g,%.15g]},"
        "\"computed\":{\"status\":%d,\"min\":[%.15g,%.15g,%.15g],"
        "\"max\":[%.15g,%.15g,%.15g],\"size\":[%.15g,%.15g,%.15g]}},",
        stored_outline_status,
        stored_outline[0][0], stored_outline[0][1], stored_outline[0][2],
        stored_outline[1][0], stored_outline[1][1], stored_outline[1][2],
        stored_outline[1][0] - stored_outline[0][0],
        stored_outline[1][1] - stored_outline[0][1],
        stored_outline[1][2] - stored_outline[0][2],
        computed_outline_status,
        computed_outline[0][0], computed_outline[0][1], computed_outline[0][2],
        computed_outline[1][0], computed_outline[1][1], computed_outline[1][2],
        computed_outline[1][0] - computed_outline[0][0],
        computed_outline[1][1] - computed_outline[0][1],
        computed_outline[1][2] - computed_outline[0][2]);
    fputs("\"parameters\":[", out);

    if (status == PRO_TK_NO_ERROR)
    {
        parameter_status = ProParameterVisit(
            &model_item,
            NULL,
            write_parameter_action,
            (ProAppData)&parameter_context);
    }
    else
    {
        parameter_status = status;
    }

    fprintf(out,
        "],\"parameter_count\":%d,\"parameter_status\":%d}}\n",
        parameter_context.count,
        parameter_status);

disconnect:
    disconnect_status = ProEngineerDisconnect(&process_handle, 10);
    if (disconnect_status != PRO_TK_NO_ERROR && exit_code == 0)
        exit_code = 3;

done:
    if (out != stdout)
        fclose(out);
    return exit_code;
}
