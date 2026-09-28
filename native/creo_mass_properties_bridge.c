#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProAsmcomp.h>
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

static ProMdlfileType model_file_type_from_path(const wchar_t *path)
{
    const wchar_t *cursor;

    if (path == NULL)
        return PRO_MDLFILE_UNUSED;
    for (cursor = path; *cursor != L'\0'; ++cursor)
    {
        if (_wcsnicmp(cursor, L".asm", 4) == 0 &&
            (cursor[4] == L'\0' || cursor[4] == L'.'))
            return PRO_MDLFILE_ASSEMBLY;
        if (_wcsnicmp(cursor, L".prt", 4) == 0 &&
            (cursor[4] == L'\0' || cursor[4] == L'.'))
            return PRO_MDLFILE_PART;
    }
    return PRO_MDLFILE_UNUSED;
}

static ProError calculate_mass_properties(
    ProMdl model,
    ProMdlType model_type,
    ProMassProperty *mass_property,
    const char **method,
    ProError attempts[3])
{
    attempts[0] = PRO_TK_GENERAL_ERROR;
    attempts[1] = PRO_TK_GENERAL_ERROR;
    attempts[2] = PRO_TK_GENERAL_ERROR;

    if (model_type == PRO_MDL_ASSEMBLY)
    {
        attempts[0] = ProAssemblySolidMassPropertyGet(
            (ProSolid)model, NULL, mass_property);
        if (attempts[0] == PRO_TK_NO_ERROR)
        {
            *method = "assembly_mass_property";
            return attempts[0];
        }

        attempts[1] = ProSolidMassPropertyWithDensityGet(
            (ProSolid)model, NULL, PRO_MP_DENS_DEFAULT, 0.0, mass_property);
        if (attempts[1] == PRO_TK_NO_ERROR)
        {
            *method = "solid_mass_property_with_material_density";
            return attempts[1];
        }

        attempts[2] = ProSolidMassPropertyGet(
            (ProSolid)model, NULL, mass_property);
        if (attempts[2] == PRO_TK_NO_ERROR)
        {
            *method = "solid_mass_property";
            return attempts[2];
        }
        return attempts[2];
    }

    attempts[0] = ProSolidMassPropertyWithDensityGet(
        (ProSolid)model, NULL, PRO_MP_DENS_DEFAULT, 0.0, mass_property);
    if (attempts[0] == PRO_TK_NO_ERROR)
    {
        *method = "solid_mass_property_with_material_density";
        return attempts[0];
    }

    attempts[1] = ProSolidMassPropertyGet(
        (ProSolid)model, NULL, mass_property);
    if (attempts[1] == PRO_TK_NO_ERROR)
    {
        *method = "solid_mass_property";
        return attempts[1];
    }
    return attempts[1];
}

#define MASS_DIAGNOSTIC_LIMIT 32

typedef struct ComponentMassAggregate
{
    double volume;
    double surface_area;
    double mass;
    double weighted_cog[3];
    double inertia_at_origin[3][3];
    int leaf_instance_count;
    int skipped_non_solid_count;
    int failed_component_count;
    wchar_t skipped_names[MASS_DIAGNOSTIC_LIMIT][80];
    wchar_t failed_names[MASS_DIAGNOSTIC_LIMIT][80];
    ProError failed_codes[MASS_DIAGNOSTIC_LIMIT];
} ComponentMassAggregate;

typedef struct ComponentMassContext
{
    ComponentMassAggregate *aggregate;
    ProMatrix parent_to_root;
    int depth;
} ComponentMassContext;

static int name_is_skeleton(const wchar_t *name)
{
    wchar_t upper[80];
    size_t i;

    if (name == NULL)
        return 0;
    wcsncpy_s(upper, 80, name, _TRUNCATE);
    for (i = 0; upper[i] != L'\0'; ++i)
        upper[i] = towupper(upper[i]);
    return wcsstr(upper, L"SKEL") != NULL;
}

static void record_component_name(
    wchar_t names[MASS_DIAGNOSTIC_LIMIT][80],
    int index,
    const wchar_t *name)
{
    if (index >= 0 && index < MASS_DIAGNOSTIC_LIMIT)
        wcsncpy_s(names[index], 80, name == NULL ? L"" : name, _TRUNCATE);
}

static void matrix_identity(ProMatrix matrix)
{
    int row;
    int column;
    for (row = 0; row < 4; ++row)
        for (column = 0; column < 4; ++column)
            matrix[row][column] = row == column ? 1.0 : 0.0;
}

static void matrix_multiply(
    const ProMatrix left,
    const ProMatrix right,
    ProMatrix result)
{
    ProMatrix product;
    int row;
    int column;
    int k;

    for (row = 0; row < 4; ++row)
    {
        for (column = 0; column < 4; ++column)
        {
            product[row][column] = 0.0;
            for (k = 0; k < 4; ++k)
                product[row][column] += left[row][k] * right[k][column];
        }
    }
    memcpy(result, product, sizeof(ProMatrix));
}

static void transform_point(
    const ProMatrix transform,
    const double local[3],
    double root[3])
{
    int column;
    for (column = 0; column < 3; ++column)
    {
        root[column] = transform[3][column];
        root[column] += local[0] * transform[0][column];
        root[column] += local[1] * transform[1][column];
        root[column] += local[2] * transform[2][column];
    }
}

static void rotate_tensor_to_root(
    const ProMatrix transform,
    const double local[3][3],
    double root[3][3])
{
    int row;
    int column;
    int i;
    int j;
    for (row = 0; row < 3; ++row)
    {
        for (column = 0; column < 3; ++column)
        {
            root[row][column] = 0.0;
            for (i = 0; i < 3; ++i)
                for (j = 0; j < 3; ++j)
                    root[row][column] +=
                        transform[i][row] * local[i][j] *
                        transform[j][column];
        }
    }
}

static void add_part_mass(
    ComponentMassAggregate *aggregate,
    const ProMassProperty *part,
    const ProMatrix part_to_root)
{
    double root_cog[3];
    double rotated_cg_tensor[3][3];
    double radius_squared;
    int row;
    int column;

    transform_point(part_to_root, part->center_of_gravity, root_cog);
    rotate_tensor_to_root(
        part_to_root, part->cg_inertia_tensor, rotated_cg_tensor);
    aggregate->volume += part->volume;
    aggregate->surface_area += part->surface_area;
    aggregate->mass += part->mass;
    for (row = 0; row < 3; ++row)
        aggregate->weighted_cog[row] += part->mass * root_cog[row];

    radius_squared = root_cog[0] * root_cog[0] +
        root_cog[1] * root_cog[1] + root_cog[2] * root_cog[2];
    for (row = 0; row < 3; ++row)
    {
        for (column = 0; column < 3; ++column)
        {
            double parallel_axis = -part->mass * root_cog[row] * root_cog[column];
            if (row == column)
                parallel_axis += part->mass * radius_squared;
            aggregate->inertia_at_origin[row][column] +=
                rotated_cg_tensor[row][column] + parallel_axis;
        }
    }
    ++aggregate->leaf_instance_count;
}

static void record_failed_component(
    ComponentMassAggregate *aggregate,
    const wchar_t *name,
    ProError status)
{
    int index = aggregate->failed_component_count;
    record_component_name(aggregate->failed_names, index, name);
    if (index < MASS_DIAGNOSTIC_LIMIT)
        aggregate->failed_codes[index] = status;
    ++aggregate->failed_component_count;
}

static void record_skipped_non_solid(
    ComponentMassAggregate *aggregate,
    const wchar_t *name)
{
    int index = aggregate->skipped_non_solid_count;
    record_component_name(aggregate->skipped_names, index, name);
    ++aggregate->skipped_non_solid_count;
}

static ProError component_mass_action(
    ProFeature *feature,
    ProError filter_status,
    ProAppData app_data)
{
    ComponentMassContext *context = (ComponentMassContext *)app_data;
    ProFeattype feature_type = PRO_FEAT_INVALID;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProMdl component_model = NULL;
    ProMdlType component_type = PRO_MDL_UNUSED;
    ProMdlName component_name = L"";
    ProMatrix local_to_parent;
    ProMatrix local_to_root;
    ProMassProperty part_mass;
    ProMassProperty geometry_mass;
    ProError status;

    (void)filter_status;
    if (ProFeatureTypeGet(feature, &feature_type) != PRO_TK_NO_ERROR ||
        feature_type != PRO_FEAT_COMPONENT ||
        ProFeatureStatusGet(feature, &feature_status) != PRO_TK_NO_ERROR ||
        feature_status != PRO_FEAT_ACTIVE)
        return PRO_TK_NO_ERROR;

    status = ProAsmcompMdlGet((ProAsmcomp *)feature, &component_model);
    if (status != PRO_TK_NO_ERROR || component_model == NULL)
    {
        record_failed_component(
            context->aggregate, L"<unresolved component>", status);
        return PRO_TK_NO_ERROR;
    }
    ProMdlNameGet(component_model, component_name);
    status = ProAsmcompPositionGet((ProAsmcomp *)feature, local_to_parent);
    if (status != PRO_TK_NO_ERROR)
    {
        record_failed_component(context->aggregate, component_name, status);
        return PRO_TK_NO_ERROR;
    }
    matrix_multiply(local_to_parent, context->parent_to_root, local_to_root);
    status = ProMdlTypeGet(component_model, &component_type);
    if (status != PRO_TK_NO_ERROR)
    {
        record_failed_component(context->aggregate, component_name, status);
        return PRO_TK_NO_ERROR;
    }

    if (component_type == PRO_MDL_ASSEMBLY)
    {
        ComponentMassContext child_context;
        if (context->depth >= 48)
        {
            record_failed_component(
                context->aggregate, component_name, PRO_TK_GENERAL_ERROR);
            return PRO_TK_NO_ERROR;
        }
        memset(&child_context, 0, sizeof(child_context));
        child_context.aggregate = context->aggregate;
        memcpy(
            child_context.parent_to_root, local_to_root, sizeof(ProMatrix));
        child_context.depth = context->depth + 1;
        status = ProSolidFeatVisit(
            (ProSolid)component_model,
            component_mass_action,
            NULL,
            (ProAppData)&child_context);
        if (status != PRO_TK_NO_ERROR)
            record_failed_component(context->aggregate, component_name, status);
        return PRO_TK_NO_ERROR;
    }

    if (component_type != PRO_MDL_PART)
        return PRO_TK_NO_ERROR;

    /* Creo skeletons can contain reference solids, but they are design aids
       rather than physical BOM items and must never contribute product mass. */
    if (name_is_skeleton(component_name))
    {
        record_skipped_non_solid(context->aggregate, component_name);
        return PRO_TK_NO_ERROR;
    }

    memset(&part_mass, 0, sizeof(part_mass));
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)component_model,
        NULL,
        PRO_MP_DENS_DEFAULT,
        0.0,
        &part_mass);
    if (status != PRO_TK_NO_ERROR)
        status = ProSolidMassPropertyGet(
            (ProSolid)component_model, NULL, &part_mass);
    if (status == PRO_TK_NO_ERROR)
    {
        add_part_mass(context->aggregate, &part_mass, local_to_root);
        return PRO_TK_NO_ERROR;
    }

    memset(&geometry_mass, 0, sizeof(geometry_mass));
    if (ProSolidMassPropertyWithDensityGet(
            (ProSolid)component_model,
            NULL,
            PRO_MP_DENS_USE_ALWAYS,
            1.0,
            &geometry_mass) == PRO_TK_NO_ERROR &&
        geometry_mass.volume > 1.0e-9)
    {
        record_failed_component(context->aggregate, component_name, status);
    }
    else
    {
        record_failed_component(context->aggregate, component_name, status);
    }
    return PRO_TK_NO_ERROR;
}

static void symmetric_eigenvalues(
    const double input[3][3],
    double eigenvalues[3])
{
    double matrix[3][3];
    int iteration;
    int row;
    int column;

    memcpy(matrix, input, sizeof(matrix));
    for (iteration = 0; iteration < 40; ++iteration)
    {
        int p = 0;
        int q = 1;
        double maximum = fabs(matrix[0][1]);
        double angle;
        double cosine;
        double sine;

        if (fabs(matrix[0][2]) > maximum)
        {
            p = 0;
            q = 2;
            maximum = fabs(matrix[0][2]);
        }
        if (fabs(matrix[1][2]) > maximum)
        {
            p = 1;
            q = 2;
            maximum = fabs(matrix[1][2]);
        }
        if (maximum < 1.0e-10)
            break;
        angle = 0.5 * atan2(
            2.0 * matrix[p][q], matrix[q][q] - matrix[p][p]);
        cosine = cos(angle);
        sine = sin(angle);
        for (row = 0; row < 3; ++row)
        {
            if (row != p && row != q)
            {
                double old_p = matrix[row][p];
                double old_q = matrix[row][q];
                matrix[row][p] = cosine * old_p - sine * old_q;
                matrix[p][row] = matrix[row][p];
                matrix[row][q] = sine * old_p + cosine * old_q;
                matrix[q][row] = matrix[row][q];
            }
        }
        {
            double app = matrix[p][p];
            double aqq = matrix[q][q];
            double apq = matrix[p][q];
            matrix[p][p] = cosine * cosine * app -
                2.0 * sine * cosine * apq + sine * sine * aqq;
            matrix[q][q] = sine * sine * app +
                2.0 * sine * cosine * apq + cosine * cosine * aqq;
            matrix[p][q] = 0.0;
            matrix[q][p] = 0.0;
        }
    }
    for (row = 0; row < 3; ++row)
        eigenvalues[row] = matrix[row][row];
    for (row = 0; row < 2; ++row)
    {
        for (column = row + 1; column < 3; ++column)
        {
            if (eigenvalues[column] < eigenvalues[row])
            {
                double temporary = eigenvalues[row];
                eigenvalues[row] = eigenvalues[column];
                eigenvalues[column] = temporary;
            }
        }
    }
}

static ProError calculate_assembly_by_component_rollup(
    ProMdl assembly,
    ProMassProperty *mass_property,
    ComponentMassAggregate *aggregate)
{
    ComponentMassContext context;
    ProError status;
    double cog_radius_squared;
    int row;
    int column;

    memset(aggregate, 0, sizeof(*aggregate));
    memset(&context, 0, sizeof(context));
    context.aggregate = aggregate;
    matrix_identity(context.parent_to_root);
    status = ProSolidFeatVisit(
        (ProSolid)assembly,
        component_mass_action,
        NULL,
        (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR)
        return status;
    if (aggregate->failed_component_count > 0 ||
        aggregate->leaf_instance_count < 1 || aggregate->mass <= 0.0)
        return PRO_TK_GENERAL_ERROR;

    memset(mass_property, 0, sizeof(*mass_property));
    mass_property->volume = aggregate->volume;
    mass_property->surface_area = aggregate->surface_area;
    mass_property->mass = aggregate->mass;
    mass_property->density = aggregate->volume > 0.0
        ? aggregate->mass / aggregate->volume : 0.0;
    for (row = 0; row < 3; ++row)
        mass_property->center_of_gravity[row] =
            aggregate->weighted_cog[row] / aggregate->mass;
    cog_radius_squared =
        mass_property->center_of_gravity[0] *
            mass_property->center_of_gravity[0] +
        mass_property->center_of_gravity[1] *
            mass_property->center_of_gravity[1] +
        mass_property->center_of_gravity[2] *
            mass_property->center_of_gravity[2];
    for (row = 0; row < 3; ++row)
    {
        for (column = 0; column < 3; ++column)
        {
            double shift = -aggregate->mass *
                mass_property->center_of_gravity[row] *
                mass_property->center_of_gravity[column];
            if (row == column)
                shift += aggregate->mass * cog_radius_squared;
            mass_property->coor_sys_inertia_tensor[row][column] =
                aggregate->inertia_at_origin[row][column];
            mass_property->cg_inertia_tensor[row][column] =
                aggregate->inertia_at_origin[row][column] - shift;
        }
    }
    symmetric_eigenvalues(
        mass_property->cg_inertia_tensor,
        mass_property->principal_moments);
    return PRO_TK_NO_ERROR;
}

static void write_component_diagnostics(
    FILE *out,
    const ComponentMassAggregate *aggregate)
{
    int index;
    int displayed;

    fprintf(out,
        ",\"component_rollup\":{\"leaf_instances\":%d,"
        "\"skipped_non_solid_count\":%d,\"failed_component_count\":%d,"
        "\"skipped_non_solid\":[",
        aggregate->leaf_instance_count,
        aggregate->skipped_non_solid_count,
        aggregate->failed_component_count);
    displayed = aggregate->skipped_non_solid_count < MASS_DIAGNOSTIC_LIMIT
        ? aggregate->skipped_non_solid_count : MASS_DIAGNOSTIC_LIMIT;
    for (index = 0; index < displayed; ++index)
    {
        if (index > 0)
            fputc(',', out);
        write_wide_json_string(out, aggregate->skipped_names[index]);
    }
    fputs("],\"failed_components\":[", out);
    displayed = aggregate->failed_component_count < MASS_DIAGNOSTIC_LIMIT
        ? aggregate->failed_component_count : MASS_DIAGNOSTIC_LIMIT;
    for (index = 0; index < displayed; ++index)
    {
        if (index > 0)
            fputc(',', out);
        fputs("{\"name\":", out);
        write_wide_json_string(out, aggregate->failed_names[index]);
        fprintf(out, ",\"error_code\":%d}", aggregate->failed_codes[index]);
    }
    fputs("]}", out);
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
    ProMdlfileType model_file_type;
    ProMassProperty mass_property;
    ProError mass_attempts[3];
    ComponentMassAggregate component_aggregate;
    const char *mass_method = NULL;
    int component_rollup_used = 0;
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
        model_file_type = model_file_type_from_path(argv[2]);
        if (model_file_type != PRO_MDLFILE_PART &&
            model_file_type != PRO_MDLFILE_ASSEMBLY)
        {
            exit_code = write_error(out, "model_file_type", PRO_TK_INVALID_TYPE);
            goto cleanup;
        }
        wcsncpy_s(model_path,
            sizeof(model_path) / sizeof(model_path[0]),
            argv[2], _TRUNCATE);
        status = ProMdlFiletypeLoad(
            model_path, model_file_type, PRO_B_FALSE, &model);
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
    status = calculate_mass_properties(
        model, model_type, &mass_property, &mass_method, mass_attempts);
    if (status != PRO_TK_NO_ERROR && model_type == PRO_MDL_ASSEMBLY)
    {
        status = calculate_assembly_by_component_rollup(
            model, &mass_property, &component_aggregate);
        if (status == PRO_TK_NO_ERROR)
        {
            mass_method = "component_instance_rollup";
            component_rollup_used = 1;
        }
    }
    if (status != PRO_TK_NO_ERROR)
    {
        fputs("{\"ok\":false,\"readonly\":true,"
              "\"stage\":\"mass_properties\",\"error_code\":", out);
        fprintf(out, "%d,\"model_type_code\":%d,", status, model_type);
        if (model_type == PRO_MDL_ASSEMBLY)
        {
            fprintf(out,
                "\"attempts\":{\"assembly\":%d,"
                "\"with_material_density\":%d,\"solid\":%d}",
                mass_attempts[0], mass_attempts[1], mass_attempts[2]);
            write_component_diagnostics(out, &component_aggregate);
            fputs("}\n", out);
        }
        else
        {
            fprintf(out,
                "\"attempts\":{\"with_material_density\":%d,"
                "\"solid\":%d}}\n",
                mass_attempts[0], mass_attempts[1]);
        }
        exit_code = 1;
        goto cleanup;
    }

    fputs("{\"ok\":true,\"readonly\":true,\"loaded_from_file\":", out);
    fputs(loaded_from_file ? "true" : "false", out);
    fputs(",\"model\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"calculation_method\":", out);
    write_utf8_json_string(out, mass_method);
    fprintf(out,
        ",\"model_type_code\":%d,\"volume\":%.17g,"
        "\"surface_area\":%.17g,\"density\":%.17g,\"mass\":%.17g,"
        "\"center_of_gravity\":[%.17g,%.17g,%.17g],"
        "\"principal_moments\":[%.17g,%.17g,%.17g]",
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
    if (component_rollup_used)
        write_component_diagnostics(out, &component_aggregate);
    fputs("}\n", out);
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
