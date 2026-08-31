#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
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
    fputs("{\"ok\":false,\"readonly\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProMdlType model_type;
    ProMassProperty mass_property;
    int connected = 0;
    int loaded_from_file = 0;
    int exit_code = 1;

    if (argc != 2 && argc != 3)
    {
        fwprintf(stderr, L"Usage: creo_mass_properties_bridge <result.json> [model_file]\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
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

    if (argc == 3)
    {
        ProPath model_path;
        wcsncpy_s(model_path,
            sizeof(model_path) / sizeof(model_path[0]),
            argv[2], _TRUNCATE);
        status = ProMdlFiletypeLoad(model_path, PRO_MDLFILE_PART, PRO_B_FALSE, &model);
        if (status == PRO_TK_NO_ERROR)
            loaded_from_file = 1;
    }
    else
    {
        status = ProMdlCurrentGet(&model);
    }
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out,
            argc == 3 ? "load_model_file" : "current_model",
            status);
        goto cleanup;
    }
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_name", status);
        goto cleanup;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR ||
        (model_type != PRO_MDL_PART && model_type != PRO_MDL_ASSEMBLY))
    {
        exit_code = write_error(out, "model_type",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)model,
        NULL,
        PRO_MP_DENS_USE_ALWAYS,
        1.0,
        &mass_property);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "mass_properties", status);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"readonly\":true,\"loaded_from_file\":", out);
    fputs(loaded_from_file ? "true" : "false", out);
    fputs(",\"model\":", out);
    write_wide_json_string(out, model_name);
    fprintf(out,
        ",\"model_type_code\":%d,\"volume\":%.17g,"
        "\"surface_area\":%.17g,\"density\":%.17g,\"mass\":%.17g,"
        "\"center_of_gravity\":[%.17g,%.17g,%.17g],"
        "\"principal_moments\":[%.17g,%.17g,%.17g]}\n",
        model_type,
        mass_property.volume,
        mass_property.surface_area,
        mass_property.density,
        mass_property.mass,
        mass_property.center_of_gravity[0],
        mass_property.center_of_gravity[1],
        mass_property.center_of_gravity[2],
        mass_property.principal_moments[0],
        mass_property.principal_moments[1],
        mass_property.principal_moments[2]);
    exit_code = 0;

cleanup:
    if (loaded_from_file)
        ProMdlErase(model);
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
