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
#include <ProFeatForm.h>
#include <ProModelitem.h>
#include <ProSelection.h>
#include <ProReference.h>
#include <ProElement.h>
#include <ProElemId.h>
#include <ProHole.h>
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

static int parse_bounded_double(
    const wchar_t *text,
    double minimum,
    double maximum,
    double *value)
{
    wchar_t *end = NULL;
    double parsed;

    if (text == NULL || *text == L'\0')
        return 0;
    parsed = wcstod(text, &end);
    if (end == text || *end != L'\0' || !_finite(parsed) ||
        parsed < minimum || parsed > maximum)
        return 0;
    *value = parsed;
    return 1;
}

static int is_safe_hole_table_token(const wchar_t *text)
{
    size_t length;
    size_t i;

    if (text == NULL)
        return 0;
    length = wcsnlen_s(text, PRO_NAME_SIZE);
    if (length == 0 || length >= PRO_NAME_SIZE)
        return 0;
    for (i = 0; i < length; ++i)
    {
        wchar_t c = text[i];
        if (!((c >= L'A' && c <= L'Z') ||
              (c >= L'a' && c <= L'z') ||
              (c >= L'0' && c <= L'9') ||
              c == L'_' || c == L'-' || c == L'.'))
            return 0;
    }
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

static ProError add_compound_element(
    ProElement parent,
    ProElemId element_id,
    ProElement *compound)
{
    ProError status = ProElementAlloc(element_id, compound);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProElemtreeElementAdd(parent, NULL, *compound);
}

static ProError named_plane_reference_get(
    ProMdl model,
    const wchar_t *plane_name,
    ProSelection *selection,
    ProReference *reference)
{
    ProModelitem plane_item;
    ProError status;

    status = ProModelitemByNameInit(
        model,
        PRO_SURFACE,
        (wchar_t *)plane_name,
        &plane_item);
    if (status != PRO_TK_NO_ERROR)
        return status;
    status = ProSelectionAlloc(NULL, &plane_item, selection);
    if (status != PRO_TK_NO_ERROR)
        return status;
    return ProSelectionToReference(*selection, reference);
}

static ProError create_linear_thru_hole(
    ProMdl model,
    const wchar_t *primary_plane,
    const wchar_t *reference_plane1,
    const wchar_t *reference_plane2,
    const wchar_t *feature_name,
    double diameter,
    double offset1,
    double offset2,
    int thru_side,
    int hole_mode,
    double hole_depth,
    double counterbore_diameter,
    double counterbore_depth,
    double countersink_diameter,
    double countersink_angle,
    const wchar_t *thread_series,
    const wchar_t *thread_size,
    double thread_depth,
    ProFeature *created_feature,
    int *creation_error_count)
{
    ProError status;
    ProElement feature_tree = NULL;
    ProElement hole_common = NULL;
    ProElement standard_depth = NULL;
    ProElement depth_to = NULL;
    ProElement depth_from = NULL;
    ProElement placement = NULL;
    ProModelitem model_item;
    ProSelection model_selection = NULL;
    ProSelection primary_selection = NULL;
    ProSelection reference_selection1 = NULL;
    ProSelection reference_selection2 = NULL;
    ProReference primary_reference = NULL;
    ProReference reference1 = NULL;
    ProReference reference2 = NULL;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    int active_depth_type = hole_mode == 1
        ? PRO_HLE_STRGHT_BLIND_DEPTH
        : PRO_HLE_STRGHT_THRU_ALL_DEPTH;
    int depth_to_type = thru_side == 1
        ? active_depth_type
        : PRO_HLE_STRGHT_NONE_DEPTH;
    int depth_from_type = thru_side == 2
        ? active_depth_type
        : PRO_HLE_STRGHT_NONE_DEPTH;

    *creation_error_count = 0;

    status = named_plane_reference_get(
        model, primary_plane, &primary_selection, &primary_reference);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = named_plane_reference_get(
        model, reference_plane1, &reference_selection1, &reference1);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = named_plane_reference_get(
        model, reference_plane2, &reference_selection2, &reference2);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

    status = ProElementAlloc(PRO_E_FEATURE_TREE, &feature_tree);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(feature_tree, PRO_E_FEATURE_TYPE, PRO_FEAT_HOLE);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(
        feature_tree, PRO_E_FEATURE_FORM, PRO_HLE_TYPE_STRAIGHT);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_wstring_element(
        feature_tree, PRO_E_STD_FEATURE_NAME, feature_name);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;

    status = add_compound_element(feature_tree, PRO_E_HLE_COM, &hole_common);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    if (hole_mode == 4)
    {
        status = add_integer_element(
            hole_common, PRO_E_HLE_TYPE_NEW, PRO_HLE_NEW_TYPE_STANDARD);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_STAN_TYPE, PRO_HLE_TAPPED_TYPE);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(hole_common, PRO_E_HLE_THRDSERIS, 0);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_FITTYPE, PRO_HLE_CLOSE_FIT);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(hole_common, PRO_E_HLE_SCREWSIZE, 0);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = ProElementHoleThreadSeriesSet(
            feature_tree, model, (wchar_t *)thread_series);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = ProElementHoleScrewSizeSet(
            feature_tree, model, (wchar_t *)thread_size);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_ADD_THREAD, PRO_HLE_ADD_THREAD);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_ADD_CBORE, PRO_HLE_NO_CBORE);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_ADD_CSINK, PRO_HLE_NO_CSINK);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_double_element(hole_common, PRO_E_HLE_HOLEDIAM, diameter);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_double_element(hole_common, PRO_E_HLE_DRILLANGLE, 118.0);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_double_element(
            hole_common, PRO_E_HLE_THRDDEPTH, thread_depth);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_double_element(
            hole_common, PRO_E_HLE_DRILLDEPTH, hole_depth);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_THRD_DEPTH, PRO_HLE_VARIABLE_THREAD);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_DEPTH, PRO_HLE_STD_VAR_DEPTH);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common,
            PRO_E_HLE_DEPTH_DIM_TYPE,
            PRO_HLE_DEP_TIP_DIM_SCHEME);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common,
            PRO_E_HLE_CRDIR_FLIP,
            thru_side == 1 ? PRO_HLE_CR_IN_SIDE_ONE : PRO_HLE_CR_IN_SIDE_TWO);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_ADD_NOTE, PRO_HOLE_NO_NOTE_FLAG);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_TOP_CLEARANCE, PRO_HOLE_GEN_CLRNCE);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
    }
    else if (hole_mode == 2 || hole_mode == 3)
    {
        status = add_integer_element(
            hole_common, PRO_E_HLE_TYPE_NEW, PRO_HLE_CUSTOM_TYPE);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common,
            PRO_E_HLE_ADD_CBORE,
            hole_mode == 2 ? PRO_HLE_ADD_CBORE : PRO_HLE_NO_CBORE);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common,
            PRO_E_HLE_ADD_CSINK,
            hole_mode == 3 ? PRO_HLE_ADD_CSINK : PRO_HLE_NO_CSINK);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_double_element(hole_common, PRO_E_HLE_HOLEDIAM, diameter);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_double_element(hole_common, PRO_E_HLE_DRILLANGLE, 118.0);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        if (hole_mode == 2)
        {
            status = add_double_element(
                hole_common, PRO_E_HLE_CBOREDEPTH, counterbore_depth);
            if (status != PRO_TK_NO_ERROR)
                goto cleanup;
            status = add_double_element(
                hole_common, PRO_E_HLE_CBOREDIAM, counterbore_diameter);
            if (status != PRO_TK_NO_ERROR)
                goto cleanup;
        }
        else
        {
            status = add_double_element(
                hole_common, PRO_E_HLE_CSINKANGLE, countersink_angle);
            if (status != PRO_TK_NO_ERROR)
                goto cleanup;
            status = add_double_element(
                hole_common, PRO_E_HLE_CSINKDIAM, countersink_diameter);
            if (status != PRO_TK_NO_ERROR)
                goto cleanup;
        }
        status = add_double_element(hole_common, PRO_E_HLE_DRILLDEPTH, hole_depth);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_DEPTH, PRO_HLE_STD_VAR_DEPTH);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common,
            PRO_E_HLE_DEPTH_DIM_TYPE,
            PRO_HLE_DEP_TIP_DIM_SCHEME);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common,
            PRO_E_HLE_CRDIR_FLIP,
            thru_side == 1 ? PRO_HLE_CR_IN_SIDE_ONE : PRO_HLE_CR_IN_SIDE_TWO);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_TOP_CLEARANCE, PRO_HOLE_GEN_CLRNCE);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_ADD_PARAMETERS, PRO_HOLE_NO_PARAMETERS_FLAG);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_ADD_NOTE, PRO_HOLE_NO_NOTE_FLAG);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
    }
    else
    {
        status = add_integer_element(
            hole_common, PRO_E_HLE_TYPE_NEW, PRO_HLE_NEW_TYPE_STRAIGHT);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_MAKE_LIGHTWT, PRO_HLE_REGULAR);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_double_element(hole_common, PRO_E_DIAMETER, diameter);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_TOP_CLEARANCE, PRO_HOLE_GEN_CLRNCE);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_ADD_PARAMETERS, PRO_HOLE_NO_PARAMETERS_FLAG);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            hole_common, PRO_E_HLE_ADD_NOTE, PRO_HOLE_NO_NOTE_FLAG);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;

        status = add_compound_element(
            hole_common, PRO_E_HOLE_STD_DEPTH, &standard_depth);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_compound_element(
            standard_depth, PRO_E_HOLE_DEPTH_TO, &depth_to);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            depth_to, PRO_E_HOLE_DEPTH_TO_TYPE, depth_to_type);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        if (hole_mode == 1 && thru_side == 1)
        {
            status = add_double_element(
                depth_to, PRO_E_EXT_DEPTH_TO_VALUE, hole_depth);
            if (status != PRO_TK_NO_ERROR)
                goto cleanup;
        }
        status = add_compound_element(
            standard_depth, PRO_E_HOLE_DEPTH_FROM, &depth_from);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        status = add_integer_element(
            depth_from, PRO_E_HOLE_DEPTH_FROM_TYPE, depth_from_type);
        if (status != PRO_TK_NO_ERROR)
            goto cleanup;
        if (hole_mode == 1 && thru_side == 2)
        {
            status = add_double_element(
                depth_from, PRO_E_EXT_DEPTH_FROM_VALUE, hole_depth);
            if (status != PRO_TK_NO_ERROR)
                goto cleanup;
        }
    }

    status = add_compound_element(
        feature_tree, PRO_E_HLE_PLACEMENT, &placement);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_reference_element(
        placement, PRO_E_HLE_PRIM_REF, primary_reference);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(
        placement, PRO_E_HLE_PL_TYPE, PRO_HLE_PL_TYPE_LIN);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_reference_element(
        placement, PRO_E_HLE_DIM_REF1, reference1);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(
        placement, PRO_E_HLE_PLC_ALIGN_OPT1, PRO_HLE_PLC_NOT_ALIGN);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_double_element(placement, PRO_E_HLE_DIM_DIST1, offset1);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_reference_element(
        placement, PRO_E_HLE_DIM_REF2, reference2);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_integer_element(
        placement, PRO_E_HLE_PLC_ALIGN_OPT2, PRO_HLE_PLC_NOT_ALIGN);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = add_double_element(placement, PRO_E_HLE_DIM_DIST2, offset2);
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
    if (reference2 != NULL)
        ProReferenceFree(reference2);
    if (reference1 != NULL)
        ProReferenceFree(reference1);
    if (primary_reference != NULL)
        ProReferenceFree(primary_reference);
    if (reference_selection2 != NULL)
        ProSelectionFree(&reference_selection2);
    if (reference_selection1 != NULL)
        ProSelectionFree(&reference_selection1);
    if (primary_selection != NULL)
        ProSelectionFree(&primary_selection);
    return status;
}

static ProError verify_thread_metadata(
    ProMdl model,
    ProFeature *feature,
    const wchar_t *expected_series,
    const wchar_t *expected_size)
{
    ProError status;
    ProElement feature_tree = NULL;
    wchar_t *actual_series = NULL;
    wchar_t *actual_size = NULL;

    status = ProFeatureElemtreeExtract(
        feature, NULL, PRO_FEAT_EXTRACT_NO_OPTS, &feature_tree);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProElementHoleThreadSeriesGet(
        feature_tree, model, &actual_series);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    status = ProElementHoleScrewSizeGet(
        feature_tree, model, &actual_size);
    if (status != PRO_TK_NO_ERROR)
        goto cleanup;
    if (actual_series == NULL || actual_size == NULL ||
        _wcsicmp(actual_series, expected_series) != 0 ||
        _wcsicmp(actual_size, expected_size) != 0)
        status = PRO_TK_GENERAL_ERROR;

cleanup:
    if (actual_size != NULL)
        ProWstringFree(actual_size);
    if (actual_series != NULL)
        ProWstringFree(actual_series);
    if (feature_tree != NULL)
        ProFeatureElemtreeFree(feature, feature_tree);
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
    ProName primary_plane;
    ProName reference_plane1;
    ProName reference_plane2;
    ProName feature_name;
    ProName thread_series = L"";
    ProName thread_size = L"";
    ProName created_feature_name = L"";
    ProFeature created_feature;
    ProFeattype created_feature_type = -1;
    ProFeatStatus created_feature_status = PRO_FEAT_INVALID;
    ProModelitem guard_item;
    ProPath original_directory;
    ProPath output_directory;
    ProPath saved_path;
    ProMassProperty source_mass_property;
    ProMassProperty copy_mass_property;
    double diameter;
    double offset1;
    double offset2;
    double hole_depth = 0.0;
    double counterbore_diameter = 0.0;
    double counterbore_depth = 0.0;
    double countersink_diameter = 0.0;
    double countersink_angle = 0.0;
    double countersink_depth = 0.0;
    double thread_depth = 0.0;
    double volume_delta;
    double minimum_volume_delta;
    double maximum_hole_volume;
    int thru_side;
    int hole_mode = 0;
    int source_feature_count = 0;
    int copy_feature_count = 0;
    int creation_error_count = 0;
    int regenerate_attempts = 0;
    int connected = 0;
    int copy_created = 0;
    int directory_changed = 0;
    int exit_code = 1;

    if (argc != 13 && argc != 15 && argc != 17 && argc != 18)
    {
        fwprintf(stderr,
            L"Usage: creo_hole_bridge <result.json> <expected_model> "
            L"<copy_name> <output_dir> <primary_plane> <reference_plane1> "
            L"<reference_plane2> <hole_name> <diameter> <offset1> "
            L"<offset2> <direction_side_1_or_2> "
            L"[BLIND <depth> | COUNTERBORE <depth> <cbore_diameter> <cbore_depth> | "
            L"COUNTERSINK <depth> <csink_diameter> <csink_angle> | "
            L"THREAD <depth> <thread_depth> <thread_series> <thread_size>]\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
    {
        fwprintf(stderr, L"Unable to open result file: %ls\n", argv[1]);
        return 2;
    }
    if (!parse_bounded_double(argv[9], 0.1, 100.0, &diameter) ||
        !parse_bounded_double(argv[10], -1000.0, 1000.0, &offset1) ||
        !parse_bounded_double(argv[11], -1000.0, 1000.0, &offset2))
    {
        exit_code = write_error(out, "numeric_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    thru_side = _wtoi(argv[12]);
    if ((thru_side != 1 && thru_side != 2) ||
        (thru_side == 1 && wcscmp(argv[12], L"1") != 0) ||
        (thru_side == 2 && wcscmp(argv[12], L"2") != 0))
    {
        exit_code = write_error(out, "thru_side_input", PRO_TK_BAD_INPUTS);
        goto done;
    }
    if (argc == 15)
    {
        if (wcscmp(argv[13], L"BLIND") != 0 ||
            !parse_bounded_double(argv[14], 0.1, 500.0, &hole_depth))
        {
            exit_code = write_error(out, "blind_depth_input", PRO_TK_BAD_INPUTS);
            goto done;
        }
        hole_mode = 1;
    }
    else if (argc == 17)
    {
        if (wcscmp(argv[13], L"COUNTERBORE") == 0)
        {
            if (!parse_bounded_double(argv[14], 0.1, 500.0, &hole_depth) ||
                !parse_bounded_double(argv[15], 0.2, 200.0, &counterbore_diameter) ||
                !parse_bounded_double(argv[16], 0.1, 500.0, &counterbore_depth) ||
                counterbore_diameter <= diameter ||
                counterbore_depth >= hole_depth)
            {
                exit_code = write_error(out, "counterbore_input", PRO_TK_BAD_INPUTS);
                goto done;
            }
            hole_mode = 2;
        }
        else if (wcscmp(argv[13], L"COUNTERSINK") == 0)
        {
            if (!parse_bounded_double(argv[14], 0.1, 500.0, &hole_depth) ||
                !parse_bounded_double(argv[15], 0.2, 200.0, &countersink_diameter) ||
                !parse_bounded_double(argv[16], 30.0, 150.0, &countersink_angle) ||
                countersink_diameter <= diameter)
            {
                exit_code = write_error(out, "countersink_input", PRO_TK_BAD_INPUTS);
                goto done;
            }
            countersink_depth =
                (countersink_diameter - diameter) * 0.5 /
                tan(countersink_angle * 3.14159265358979323846 / 360.0);
            if (!_finite(countersink_depth) || countersink_depth >= hole_depth)
            {
                exit_code = write_error(
                    out, "countersink_depth_guard", PRO_TK_BAD_INPUTS);
                goto done;
            }
            hole_mode = 3;
        }
        else
        {
            exit_code = write_error(out, "advanced_hole_mode", PRO_TK_BAD_INPUTS);
            goto done;
        }
    }
    else if (argc == 18)
    {
        if (wcscmp(argv[13], L"THREAD") != 0 ||
            !parse_bounded_double(argv[14], 0.1, 500.0, &hole_depth) ||
            !parse_bounded_double(argv[15], 0.1, 500.0, &thread_depth) ||
            thread_depth >= hole_depth ||
            !is_safe_hole_table_token(argv[16]) ||
            !is_safe_hole_table_token(argv[17]))
        {
            exit_code = write_error(out, "thread_input", PRO_TK_BAD_INPUTS);
            goto done;
        }
        wcsncpy_s(thread_series,
            sizeof(thread_series) / sizeof(thread_series[0]), argv[16], _TRUNCATE);
        wcsncpy_s(thread_size,
            sizeof(thread_size) / sizeof(thread_size[0]), argv[17], _TRUNCATE);
        hole_mode = 4;
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
    wcsncpy_s(primary_plane,
        sizeof(primary_plane) / sizeof(primary_plane[0]),
        argv[5], _TRUNCATE);
    wcsncpy_s(reference_plane1,
        sizeof(reference_plane1) / sizeof(reference_plane1[0]),
        argv[6], _TRUNCATE);
    wcsncpy_s(reference_plane2,
        sizeof(reference_plane2) / sizeof(reference_plane2[0]),
        argv[7], _TRUNCATE);
    wcsncpy_s(feature_name,
        sizeof(feature_name) / sizeof(feature_name[0]),
        argv[8], _TRUNCATE);

    if (_wcsicmp(primary_plane, reference_plane1) == 0 ||
        _wcsicmp(primary_plane, reference_plane2) == 0 ||
        _wcsicmp(reference_plane1, reference_plane2) == 0)
    {
        exit_code = write_error(out, "distinct_plane_guard", PRO_TK_BAD_INPUTS);
        goto done;
    }
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
        source_model, PRO_SURFACE, primary_plane, &guard_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "primary_plane_guard", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        source_model, PRO_SURFACE, reference_plane1, &guard_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "reference_plane1_guard", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        source_model, PRO_SURFACE, reference_plane2, &guard_item);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "reference_plane2_guard", status);
        goto cleanup;
    }
    status = ProModelitemByNameInit(
        source_model, PRO_FEATURE, feature_name, &guard_item);
    if (status == PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_name_guard", PRO_TK_E_FOUND);
        goto cleanup;
    }
    status = feature_count_get((ProSolid)source_model, &source_feature_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_feature_count", status);
        goto cleanup;
    }
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)source_model,
        NULL,
        PRO_MP_DENS_USE_ALWAYS,
        1.0,
        &source_mass_property);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "source_mass_properties", status);
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

    status = create_linear_thru_hole(
        copy_model,
        primary_plane,
        reference_plane1,
        reference_plane2,
        feature_name,
        diameter,
        offset1,
        offset2,
        thru_side,
        hole_mode,
        hole_depth,
        counterbore_diameter,
        counterbore_depth,
        countersink_diameter,
        countersink_angle,
        thread_series,
        thread_size,
        thread_depth,
        &created_feature,
        &creation_error_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "feature_create", status);
        goto cleanup;
    }

    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)copy_model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR &&
        regenerate_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "regenerate", regenerate_status);
        goto cleanup;
    }

    status = ProFeatureTypeGet(&created_feature, &created_feature_type);
    if (status != PRO_TK_NO_ERROR || created_feature_type != PRO_FEAT_HOLE)
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
    status = ProModelitemNameGet(
        (ProModelitem *)&created_feature, created_feature_name);
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
            copy_model, PRO_FEATURE, feature_name, &named_feature);
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
    if (hole_mode == 4)
    {
        status = verify_thread_metadata(
            copy_model, &created_feature, thread_series, thread_size);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "thread_metadata_readback", status);
            goto cleanup;
        }
    }
    status = feature_count_get((ProSolid)copy_model, &copy_feature_count);
    if (status != PRO_TK_NO_ERROR || copy_feature_count <= source_feature_count)
    {
        exit_code = write_error(out, "copy_feature_count_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProSolidMassPropertyWithDensityGet(
        (ProSolid)copy_model,
        NULL,
        PRO_MP_DENS_USE_ALWAYS,
        1.0,
        &copy_mass_property);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_mass_properties", status);
        goto cleanup;
    }
    volume_delta = source_mass_property.volume - copy_mass_property.volume;
    minimum_volume_delta = fabs(source_mass_property.volume) * 1.0e-9;
    if (minimum_volume_delta < 1.0e-6)
        minimum_volume_delta = 1.0e-6;
    if (!_finite(volume_delta) || volume_delta <= minimum_volume_delta)
    {
        exit_code = write_error(out, "volume_reduction_guard", PRO_TK_GENERAL_ERROR);
        goto cleanup;
    }
    maximum_hole_volume =
        3.14159265358979323846 * 0.25 *
        (diameter * diameter * hole_depth +
         (counterbore_diameter * counterbore_diameter - diameter * diameter) *
             counterbore_depth +
         (countersink_diameter * countersink_diameter - diameter * diameter) *
             countersink_depth);
    if (hole_mode != 0 &&
        volume_delta > maximum_hole_volume + minimum_volume_delta)
    {
        exit_code = write_error(
            out, "maximum_hole_volume_guard", PRO_TK_GENERAL_ERROR);
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
    fputs(",\"primary_plane\":", out);
    write_wide_json_string(out, primary_plane);
    fputs(",\"reference_planes\":[", out);
    write_wide_json_string(out, reference_plane1);
    fputc(',', out);
    write_wide_json_string(out, reference_plane2);
    fputs("],\"feature_name\":", out);
    write_wide_json_string(out, created_feature_name);
    fputs(",\"thread_series\":", out);
    write_wide_json_string(out, thread_series);
    fputs(",\"thread_size\":", out);
    write_wide_json_string(out, thread_size);
    fprintf(out,
        ",\"hole_mode\":\"%s\",\"diameter\":%.15g,"
        "\"depth\":%.15g,\"counterbore_diameter\":%.15g,"
        "\"counterbore_depth\":%.15g,"
        "\"countersink_diameter\":%.15g,\"countersink_angle\":%.15g,"
        "\"thread_depth\":%.15g,\"thread_metadata_verified\":%s,"
        "\"offsets\":[%.15g,%.15g],\"direction_side\":%d,"
        "\"created_feature_id\":%d,\"created_feature_type_code\":%d,"
        "\"created_feature_status\":%d,"
        "\"source_feature_count\":%d,\"copy_feature_count\":%d,"
        "\"source_volume\":%.17g,\"copy_volume\":%.17g,"
        "\"removed_volume\":%.17g,"
        "\"creation_error_count\":%d,\"regenerate_status\":%d,"
        "\"regenerate_attempts\":%d,\"saved_file\":",
        hole_mode == 4 ? "threaded" :
            (hole_mode == 3 ? "countersink" :
                (hole_mode == 2 ? "counterbore" :
                    (hole_mode == 1 ? "blind" : "thru_all"))),
        diameter,
        hole_depth,
        counterbore_diameter,
        counterbore_depth,
        countersink_diameter,
        countersink_angle,
        thread_depth,
        hole_mode == 4 ? "true" : "false",
        offset1,
        offset2,
        thru_side,
        created_feature.id,
        created_feature_type,
        created_feature_status,
        source_feature_count,
        copy_feature_count,
        source_mass_property.volume,
        copy_mass_property.volume,
        volume_delta,
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
