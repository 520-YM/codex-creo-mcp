#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProSelection.h>
#include <ProReference.h>
#include <ProElement.h>
#include <ProElemId.h>
#include <ProDtmPnt.h>
#include <ProArray.h>
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
    fputs("{\"ok\":false,\"safe_copy_only\":true,\"stage\":", out);
    write_utf8_json_string(out, stage);
    fprintf(out, ",\"error_code\":%d}\n", error_code);
    return 1;
}

static int output_model_already_exists(
    const wchar_t *directory,
    const wchar_t *copy_name)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;

    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.*",
        directory,
        copy_name);
    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(find_handle);
    return 1;
}

static int find_latest_saved_model(
    const wchar_t *directory,
    const wchar_t *copy_name,
    wchar_t *saved_path,
    size_t saved_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    int found = 0;
    int best_version = -1;

    _snwprintf_s(pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.*",
        directory,
        copy_name);
    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        wchar_t *last_dot = wcsrchr(find_data.cFileName, L'.');
        int version = last_dot == NULL ? 0 : _wtoi(last_dot + 1);
        if (!found || version > best_version)
        {
            _snwprintf_s(saved_path,
                saved_path_count,
                _TRUNCATE,
                L"%ls\\%ls",
                directory,
                find_data.cFileName);
            best_version = version;
            found = 1;
        }
    } while (FindNextFileW(find_handle, &find_data));
    FindClose(find_handle);
    return found;
}

static int parse_coordinate(const wchar_t *text, double *value)
{
    wchar_t *end = NULL;
    double parsed;

    if (text == NULL || *text == L'\0')
        return 0;
    parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) || fabs(parsed) > 1000.0)
        return 0;
    *value = parsed;
    return 1;
}

static ProError count_feature_action(
    ProFeature *feature,
    ProError filter_status,
    ProAppData app_data)
{
    int *count = (int *)app_data;
    (void)feature;
    (void)filter_status;
    ++(*count);
    return PRO_TK_NO_ERROR;
}

static ProError feature_count_get(ProSolid solid, int *count)
{
    *count = 0;
    return ProSolidFeatVisit(
        solid,
        count_feature_action,
        NULL,
        (ProAppData)count);
}

static ProError add_integer_element(
    ProElement parent,
    ProElemId element_id,
    int value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementIntegerSet(element, value);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
}

static ProError add_double_element(
    ProElement parent,
    ProElemId element_id,
    double value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementDecimalsSet(element, 4);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementDoubleSet(element, value);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
}

static ProError add_wstring_element(
    ProElement parent,
    ProElemId element_id,
    const wchar_t *value)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementWstringSet(element, (wchar_t *)value);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
}

static ProError add_reference_element(
    ProElement parent,
    ProElemId element_id,
    ProReference reference)
{
    ProElement element = NULL;
    ProError status = ProElementAlloc(element_id, &element);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProElementReferenceSet(element, reference);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, element);
}

static ProError create_offset_datum_point(
    ProMdl model,
    const wchar_t *reference_csys,
    const wchar_t *feature_name,
    const wchar_t *point_name,
    double x,
    double y,
    double z,
    ProFeature *created_feature,
    int *creation_error_count)
{
    ProError status;
    ProElement feature_tree = NULL;
    ProElement points_array = NULL;
    ProElement point_element = NULL;
    ProModelitem model_item;
    ProModelitem csys_item;
    ProSelection model_selection = NULL;
    ProSelection csys_selection = NULL;
    ProReference csys_reference = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};

    *creation_error_count = 0;

    status = ProModelitemByNameInit(model, PRO_CSYS, (wchar_t *)reference_csys, &csys_item);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProSelectionAlloc(NULL, &csys_item, &csys_selection);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProSelectionToReference(csys_selection, &csys_reference);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

    status = ProElementAlloc(PRO_E_FEATURE_TREE, &feature_tree);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(feature_tree, PRO_E_FEATURE_TYPE, PRO_FEAT_DATUM_POINT);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(feature_tree, PRO_E_DPOINT_TYPE, PRO_DPOINT_TYPE_OFFSET_CSYS);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_wstring_element(feature_tree, PRO_E_STD_FEATURE_NAME, feature_name);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(
        feature_tree,
        PRO_E_DPOINT_OFST_CSYS_TYPE,
        PRO_DTMPNT_OFFCSYS_CARTESIAN);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_reference_element(
        feature_tree,
        PRO_E_DPOINT_OFST_CSYS_REF,
        csys_reference);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(
        feature_tree,
        PRO_E_DPOINT_OFST_CSYS_WITH_DIMS,
        PRO_B_TRUE);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

    status = ProElementAlloc(PRO_E_DPOINT_OFST_CSYS_PNTS_ARRAY, &points_array);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProElemtreeElementAdd(feature_tree, NULL, points_array);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProElementAlloc(PRO_E_DPOINT_OFST_CSYS_PNT, &point_element);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProElemtreeElementAdd(points_array, NULL, point_element);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_wstring_element(
        point_element,
        PRO_E_DPOINT_OFST_CSYS_PNT_NAME,
        point_name);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_double_element(point_element, PRO_E_DPOINT_OFST_CSYS_DIR1_VAL, x);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_double_element(point_element, PRO_E_DPOINT_OFST_CSYS_DIR2_VAL, y);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_double_element(point_element, PRO_E_DPOINT_OFST_CSYS_DIR3_VAL, z);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

    status = ProMdlToModelitem(model, &model_item);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProSelectionAlloc(NULL, &model_item, &model_selection);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProArrayAlloc(
        1,
        sizeof(ProFeatureCreateOptions),
        1,
        (ProArray *)&options);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    options[0] = PRO_FEAT_CR_DEFINE_MISS_ELEMS;

    status = ProFeatureWithoptionsCreate(
        model_selection,
        feature_tree,
        options,
        PRO_REGEN_NO_FLAGS,
        created_feature,
        &errors);
    *creation_error_count = errors.error_number;

cleanup:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (model_selection != NULL)
        ProSelectionFree(&model_selection);
    if (feature_tree != NULL)
        ProElementFree(&feature_tree);
    if (csys_reference != NULL)
        ProReferenceFree(csys_reference);
    if (csys_selection != NULL)
        ProSelectionFree(&csys_selection);
    return status;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError regenerate_status = PRO_TK_NO_ERROR;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl source_model = NULL;
    ProMdl copy_model = NULL;
    ProMdlName source_name;
    ProMdlName expected_name;
    ProMdlName copy_name;
    ProMdlType source_type;
    ProName reference_csys;
    ProName feature_name;
    ProName point_name;
    ProName created_feature_name = L"";
    ProFeature created_feature;
    ProFeattype created_feature_type = -1;
    ProFeatStatus created_feature_status = PRO_FEAT_INVALID;
    ProModelitem point_item;
    ProPath original_directory;
    ProPath output_directory;
    ProPath saved_path;
    double x;
    double y;
    double z;
    int source_feature_count = 0;
    int copy_feature_count = 0;
    int creation_error_count = 0;
    int regenerate_attempts = 0;
    int connected = 0;
    int copy_created = 0;
    int directory_changed = 0;
    int exit_code = 1;

    if (argc != 10)
    {
        fwprintf(stderr,
            L"Usage: creo_datum_point_bridge <result.json> <expected_model> "
            L"<copy_name> <output_dir> <reference_csys> <point_name> "
            L"<x> <y> <z>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
    }
    if (!parse_coordinate(argv[7], &x) ||
        !parse_coordinate(argv[8], &y) ||
        !parse_coordinate(argv[9], &z))
    {
        exit_code = write_error(out, "coordinate_input", PRO_TK_BAD_INPUTS);
        goto done;
    }

    wcsncpy_s(expected_name,
        sizeof(expected_name) / sizeof(expected_name[0]),
        argv[2], _TRUNCATE);
    wcsncpy_s(copy_name,
        sizeof(copy_name) / sizeof(copy_name[0]),
        argv[3], _TRUNCATE);
    wcsncpy_s(output_directory,
        sizeof(output_directory) / sizeof(output_directory[0]),
        argv[4], _TRUNCATE);
    wcsncpy_s(reference_csys,
        sizeof(reference_csys) / sizeof(reference_csys[0]),
        argv[5], _TRUNCATE);
    wcsncpy_s(feature_name,
        sizeof(feature_name) / sizeof(feature_name[0]),
        argv[6], _TRUNCATE);
    wcsncpy_s(point_name,
        sizeof(point_name) / sizeof(point_name[0]),
        argv[6], _TRUNCATE);

    if (GetFileAttributesW(output_directory) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "output_directory", PRO_TK_INVALID_DIR);
        goto done;
    }
    if (output_model_already_exists(output_directory, copy_name))
    {
        exit_code = write_error(out, "refuse_overwrite", PRO_TK_E_FOUND);
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

    status = ProMdlCurrentGet(&source_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto cleanup;
    }
    status = ProMdlNameGet(source_model, source_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_name", status);
        goto cleanup;
    }
    if (_wcsicmp(source_name, expected_name) != 0)
    {
        exit_code = write_error(out, "source_model_guard", PRO_TK_BAD_CONTEXT);
        goto cleanup;
    }
    status = ProMdlTypeGet(source_model, &source_type);
    if (status != PRO_TK_NO_ERROR || source_type != PRO_MDL_PART)
    {
        exit_code = write_error(out, "source_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        source_model,
        PRO_CSYS,
        reference_csys,
        &point_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "reference_csys_guard", status);
        goto cleanup;
    }
    status = feature_count_get((ProSolid)source_model, &source_feature_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_feature_count", status);
        goto cleanup;
    }

    status = ProDirectoryCurrentGet(original_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "get_working_directory", status);
        goto cleanup;
    }
    status = ProDirectoryChange(output_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "change_output_directory", status);
        goto cleanup;
    }
    directory_changed = 1;
    status = ProMdlnameCopy(source_model, copy_name, &copy_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_model", status);
        goto cleanup;
    }
    copy_created = 1;
    ProDirectoryChange(original_directory);
    directory_changed = 0;

    status = create_offset_datum_point(
        copy_model,
        reference_csys,
        feature_name,
        point_name,
        x,
        y,
        z,
        &created_feature,
        &creation_error_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_create", status);
        goto cleanup;
    }

    status = ProFeatureTypeGet(&created_feature, &created_feature_type);
    if (status != PRO_TK_NO_ERROR || created_feature_type != PRO_FEAT_DATUM_POINT)
    {
        exit_code = write_error(out, "created_feature_type_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProFeatureStatusGet(&created_feature, &created_feature_status);
    if (status != PRO_TK_NO_ERROR || created_feature_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "created_feature_status_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProModelitemNameGet((ProModelitem *)&created_feature, created_feature_name);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "created_feature_name_readback", status);
        goto cleanup;
    }
    if (_wcsicmp(created_feature_name, feature_name) != 0)
    {
        ProModelitem named_feature;
        ProError rename_status = ProModelitemNameSet(
            (ProModelitem *)&created_feature,
            feature_name);
        if (rename_status != PRO_TK_NO_ERROR && rename_status != PRO_TK_E_FOUND)
        {
            exit_code = write_error(out, "created_feature_name_set", rename_status);
            goto cleanup;
        }
        status = ProModelitemByNameInit(
            copy_model,
            PRO_FEATURE,
            feature_name,
            &named_feature);
        if (status != PRO_TK_NO_ERROR || named_feature.id != created_feature.id)
        {
            exit_code = write_error(out, "created_feature_name_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto cleanup;
        }
        wcsncpy_s(created_feature_name,
            sizeof(created_feature_name) / sizeof(created_feature_name[0]),
            feature_name,
            _TRUNCATE);
    }
    status = ProModelitemByNameInit(copy_model, PRO_POINT, point_name, &point_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "created_point_name_readback", status);
        goto cleanup;
    }
    status = feature_count_get((ProSolid)copy_model, &copy_feature_count);
    if (status != PRO_TK_NO_ERROR || copy_feature_count <= source_feature_count)
    {
        exit_code = write_error(out, "copy_feature_count_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }

    do
    {
        regenerate_status = ProSolidRegenerate((ProSolid)copy_model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR &&
        regenerate_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "regenerate", regenerate_status);
        goto cleanup;
    }

    status = ProMdlSave(copy_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_copy", status);
        goto cleanup;
    }
    if (!find_latest_saved_model(
            output_directory,
            copy_name,
            saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_file", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    fputs("{\"ok\":true,\"safe_copy_only\":true,\"source_model\":", out);
    write_wide_json_string(out, source_name);
    fputs(",\"copy_model\":", out);
    write_wide_json_string(out, copy_name);
    fputs(",\"reference_csys\":", out);
    write_wide_json_string(out, reference_csys);
    fputs(",\"feature_name\":", out);
    write_wide_json_string(out, created_feature_name);
    fputs(",\"point_name\":", out);
    write_wide_json_string(out, point_name);
    fprintf(out,
        ",\"coordinates\":[%.15g,%.15g,%.15g],"
        "\"created_feature_id\":%d,\"created_feature_type_code\":%d,"
        "\"created_feature_status\":%d,"
        "\"source_feature_count\":%d,\"copy_feature_count\":%d,"
        "\"creation_error_count\":%d,\"regenerate_status\":%d,"
        "\"regenerate_attempts\":%d,\"saved_file\":",
        x, y, z,
        created_feature.id,
        created_feature_type,
        created_feature_status,
        source_feature_count,
        copy_feature_count,
        creation_error_count,
        regenerate_status,
        regenerate_attempts);
    write_wide_json_string(out, saved_path);
    fputs("}\n", out);
    exit_code = 0;

cleanup:
    if (directory_changed)
        ProDirectoryChange(original_directory);
    if (copy_created)
        ProMdlErase(copy_model);
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
