#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProSkeleton.h>
#include <ProWindows.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProElemId.h>
#include <ProExtrude.h>
#include <ProArray.h>

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
    const wchar_t *text, double maximum, double *value)
{
    wchar_t *end = NULL;
    double parsed = wcstod(text, &end);
    if (text == NULL || *text == L'\0' || end == text || *end != L'\0' ||
        !_finite(parsed) || parsed < 0.1 || parsed > maximum)
        return 0;
    *value = parsed;
    return 1;
}

static int find_latest_saved_model(
    const wchar_t *directory, const wchar_t *name, const wchar_t *extension,
    wchar_t *saved_path, size_t saved_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    int found = 0;
    int best_version = -1;
    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]), _TRUNCATE,
        L"%ls\\%ls.%ls*", directory, name, extension);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        wchar_t *dot = wcsrchr(data.cFileName, L'.');
        int version = dot == NULL ? 0 : _wtoi(dot + 1);
        if (!found || version > best_version)
        {
            _snwprintf_s(saved_path, saved_path_count, _TRUNCATE,
                L"%ls\\%ls", directory, data.cFileName);
            best_version = version;
            found = 1;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return found;
}

static ProError element_by_ids_get(
    ProElement tree, const ProElemId *ids, int count, ProElement *element)
{
    ProElempath path = NULL;
    ProElempathItem items[3];
    ProError status;
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

static int near_value(double actual, double expected, double tolerance)
{
    return fabs(actual - expected) <= tolerance;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl current_model = NULL;
    ProMdl assembly_model = NULL;
    ProMdl skeleton_model = NULL;
    ProMdl current_readback = NULL;
    ProMdlName current_name;
    ProMdlName actual_skeleton_name;
    ProMdlName current_readback_name;
    ProMdlType current_type;
    ProBoolean is_skeleton = PRO_B_FALSE;
    ProModelitem named_feature;
    ProFeature feature;
    ProFeattype feature_type = PRO_FEAT_INVALID;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProElement tree = NULL;
    ProElement direction_element = NULL;
    ProElement readback_tree = NULL;
    ProElement readback_direction_element = NULL;
    ProElemId direction_path_ids[1] = {PRO_E_STD_DIRECTION};
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    ProMassProperty mass_property;
    Pro3dPnt outline[2] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
    ProMatrix identity = {
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
    ProPath working_directory;
    ProPath skeleton_saved_path;
    ProPath assembly_saved_path;
    double length;
    double width;
    double height;
    double expected_volume;
    double tolerance = 1.0e-5;
    int requested_side;
    int old_direction_value = 0;
    int requested_direction_value;
    int readback_direction_value = 0;
    int connected = 0;
    int redefined = 0;
    int saved = 0;
    int regenerate_attempts = 0;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    int window_id = -1;
    int exit_code = 1;

    if (argc != 9)
        return 2;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    if (!parse_positive_dimension(argv[5], 100000.0, &length) ||
        !parse_positive_dimension(argv[6], 100000.0, &width) ||
        !parse_positive_dimension(argv[7], 100000.0, &height) ||
        (wcscmp(argv[8], L"1") != 0 && wcscmp(argv[8], L"2") != 0))
    {
        exit_code = write_error(out, "input_guard", PRO_TK_BAD_INPUTS);
        goto done;
    }
    requested_side = _wtoi(argv[8]);
    requested_direction_value = requested_side == 1
        ? PRO_EXT_CR_IN_SIDE_ONE : PRO_EXT_CR_IN_SIDE_TWO;

    status = ProEngineerConnect(
        "", "", "", "", PRO_B_TRUE, 20,
        &random_choice, &process_handle);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "connect", status);
        goto done;
    }
    connected = 1;
    status = ProMdlCurrentGet(&current_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(current_model, current_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto cleanup;
    }
    if (_wcsicmp(current_name, argv[2]) == 0)
    {
        status = ProMdlTypeGet(current_model, &current_type);
        if (status != PRO_TK_NO_ERROR || current_type != PRO_MDL_ASSEMBLY)
        {
            exit_code = write_error(out, "assembly_type_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
            goto cleanup;
        }
        assembly_model = current_model;
    }
    else if (_wcsicmp(current_name, argv[3]) == 0)
    {
        status = ProMdlnameInit(argv[2], PRO_MDLFILE_ASSEMBLY, &assembly_model);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "assembly_init", status);
            goto cleanup;
        }
    }
    else
    {
        exit_code = write_error(out, "assembly_or_skeleton_name_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProAsmSkeletonGet((ProAssembly)assembly_model, &skeleton_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(skeleton_model, actual_skeleton_name);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlIsSkeleton(skeleton_model, &is_skeleton);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(actual_skeleton_name, argv[3]) != 0 || is_skeleton != PRO_B_TRUE)
    {
        exit_code = write_error(out, "skeleton_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        skeleton_model, PRO_FEATURE, argv[4], &named_feature);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureInit(
            (ProSolid)skeleton_model, named_feature.id, &feature);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureTypeGet(&feature, &feature_type);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureStatusGet(&feature, &feature_status);
    if (status != PRO_TK_NO_ERROR || feature_type != PRO_FEAT_PROTRUSION ||
        feature_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "feature_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }
    status = ProFeatureElemtreeExtract(
        &feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &tree);
    if (status == PRO_TK_NO_ERROR)
        status = element_by_ids_get(
            tree, direction_path_ids, 1, &direction_element);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementIntegerGet(
            direction_element, NULL, &old_direction_value);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "direction_read", status);
        goto cleanup;
    }
    if (old_direction_value == requested_direction_value)
    {
        exit_code = write_error(out, "direction_already_matches", PRO_TK_E_FOUND);
        goto cleanup;
    }
    status = ProElementIntegerSet(direction_element, requested_direction_value);
    if (status == PRO_TK_NO_ERROR)
        status = ProArrayAlloc(
            1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&options);
    if (status == PRO_TK_NO_ERROR)
    {
        options[0] = PRO_FEAT_CR_DEFINE_MISS_ELEMS;
        status = ProFeatureWithoptionsRedefine(
            NULL, &feature, tree, options, PRO_REGEN_NO_FLAGS, &errors);
    }
    if (status != PRO_TK_NO_ERROR || errors.error_number != 0)
    {
        exit_code = write_error(out, "feature_direction_redefine",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    redefined = 1;
    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)skeleton_model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR &&
        regenerate_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "regenerate_skeleton", regenerate_status);
        goto cleanup;
    }
    status = ProFeatureElemtreeExtract(
        &feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &readback_tree);
    if (status == PRO_TK_NO_ERROR)
        status = element_by_ids_get(
            readback_tree, direction_path_ids, 1, &readback_direction_element);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementIntegerGet(
            readback_direction_element, NULL, &readback_direction_value);
    if (status != PRO_TK_NO_ERROR ||
        readback_direction_value != requested_direction_value)
    {
        exit_code = write_error(out, "direction_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)skeleton_model, NULL, PRO_MP_DENS_USE_ALWAYS, 1.0,
        &mass_property);
    expected_volume = length * width * height;
    if (status != PRO_TK_NO_ERROR ||
        fabs(mass_property.volume - expected_volume) > expected_volume * 1.0e-7)
    {
        exit_code = write_error(out, "volume_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProSolidOutlineCompute(
        (ProSolid)skeleton_model, identity, outline_excludes,
        (int)(sizeof(outline_excludes) / sizeof(outline_excludes[0])), outline);
    if (status != PRO_TK_NO_ERROR ||
        !near_value(outline[1][0] - outline[0][0], length, tolerance) ||
        !near_value(outline[1][1] - outline[0][1], width, tolerance) ||
        !near_value(outline[1][2] - outline[0][2], height, tolerance) ||
        (requested_side == 2 &&
            (!near_value(outline[0][2], 0.0, tolerance) ||
             !near_value(outline[1][2], height, tolerance))) ||
        (requested_side == 1 &&
            (!near_value(outline[0][2], -height, tolerance) ||
             !near_value(outline[1][2], 0.0, tolerance))))
    {
        exit_code = write_error(out, "positive_z_outline_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProSolidRegenerate((ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "regenerate_assembly", status);
        goto cleanup;
    }
    status = ProDirectoryCurrentGet(working_directory);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlSave(skeleton_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_models", status);
        goto cleanup;
    }
    saved = 1;
    if (!find_latest_saved_model(
            working_directory, actual_skeleton_name, L"prt",
            skeleton_saved_path,
            sizeof(skeleton_saved_path) / sizeof(skeleton_saved_path[0])) ||
        !find_latest_saved_model(
            working_directory, argv[2], L"asm",
            assembly_saved_path,
            sizeof(assembly_saved_path) / sizeof(assembly_saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_models", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }
    status = ProMdlDisplay(skeleton_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlCurrentGet(&current_readback);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(current_readback, current_readback_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(current_readback_name, actual_skeleton_name) != 0)
    {
        status = ProObjectwindowMdlnameCreate(
            actual_skeleton_name, PRO_PART, &window_id);
        if (status == PRO_TK_NO_ERROR)
            status = ProWindowCurrentSet(window_id);
        if (status == PRO_TK_NO_ERROR)
            status = ProWindowActivate(window_id);
    }
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "display_skeleton", status);
        goto cleanup;
    }
    if (window_id < 0 && ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
        ProWindowActivate(window_id);
    if (window_id >= 0)
    {
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"assembly\":", out);
    write_wide_json_string(out, argv[2]);
    fputs(",\"skeleton\":", out);
    write_wide_json_string(out, actual_skeleton_name);
    fputs(",\"feature_name\":", out);
    write_wide_json_string(out, argv[4]);
    fprintf(out,
        ",\"feature_id\":%d,\"old_direction_value\":%d,"
        "\"new_direction_value\":%d,\"direction_side\":%d,"
        "\"axis_direction\":\"+Z\",\"length\":%.15g,"
        "\"width\":%.15g,\"height\":%.15g,\"volume\":%.17g,"
        "\"outline\":{\"min\":[%.15g,%.15g,%.15g],"
        "\"max\":[%.15g,%.15g,%.15g],"
        "\"size\":[%.15g,%.15g,%.15g]},"
        "\"regenerate_status\":%d,\"regenerate_attempts\":%d,"
        "\"skeleton_saved_file\":",
        feature.id, old_direction_value, readback_direction_value,
        requested_side, length, width, height, mass_property.volume,
        outline[0][0], outline[0][1], outline[0][2],
        outline[1][0], outline[1][1], outline[1][2],
        outline[1][0] - outline[0][0],
        outline[1][1] - outline[0][1],
        outline[1][2] - outline[0][2],
        regenerate_status, regenerate_attempts);
    write_wide_json_string(out, skeleton_saved_path);
    fputs(",\"assembly_saved_file\":", out);
    write_wide_json_string(out, assembly_saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (exit_code != 0 && redefined && !saved &&
        direction_element != NULL && tree != NULL)
    {
        ProErrorlist rollback_errors = {NULL, 0};
        ProElementIntegerSet(direction_element, old_direction_value);
        ProFeatureWithoptionsRedefine(
            NULL, &feature, tree, options, PRO_REGEN_NO_FLAGS,
            &rollback_errors);
        ProSolidRegenerate((ProSolid)skeleton_model, PRO_REGEN_NO_FLAGS);
        if (rollback_errors.error_list != NULL)
            ProArrayFree((ProArray *)&rollback_errors.error_list);
    }
    if (readback_tree != NULL)
        ProFeatureElemtreeFree(&feature, readback_tree);
    if (tree != NULL)
        ProFeatureElemtreeFree(&feature, tree);
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
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
