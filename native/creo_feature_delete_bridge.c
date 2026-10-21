#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProArray.h>

typedef struct FeatureCountContext { int count; } FeatureCountContext;

static int fail(FILE *out, const char *stage, ProError error)
{
    fprintf(out, "{\"ok\":false,\"api_only\":true,\"stage\":\"%s\",\"error_code\":%d}\n", stage, error);
    return 1;
}

static ProError count_action(ProFeature *feature, ProError filter_status, ProAppData app_data)
{
    FeatureCountContext *context = (FeatureCountContext *)app_data;
    (void)feature;
    (void)filter_status;
    ++context->count;
    return PRO_TK_NO_ERROR;
}

static ProError feature_count_get(ProSolid solid, int *count)
{
    FeatureCountContext context;
    ProError status;
    context.count = 0;
    status = ProSolidFeatVisit(solid, count_action, NULL, (ProAppData)&context);
    if (status == PRO_TK_NO_ERROR)
        *count = context.count;
    return status;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = NULL;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl current = NULL;
    ProMdlType model_type = PRO_MDL_UNUSED;
    ProMdlName current_name;
    ProModelitem item, probe;
    ProFeature feature;
    ProFeattype feature_type = PRO_FEAT_INVALID;
    ProFeatStatus feature_status = PRO_FEAT_INVALID;
    ProFeatureDeleteOptions delete_option = PRO_FEAT_DELETE_NO_OPTS;
    int *children = NULL;
    int child_count = 0;
    int feature_id, expected_type, before_count = 0, after_count = 0;
    ProError status, disconnect_status;
    int connected = 0, exit_code = 1;

    if (argc != 6) {
        fwprintf(stderr, L"Usage: creo_feature_delete_bridge <result.json> <expected_model> <feature_name> <feature_id> <expected_type>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    feature_id = _wtoi(argv[4]);
    expected_type = _wtoi(argv[5]);
    if (!*argv[2] || !*argv[3] || feature_id < 8 || expected_type < 1) {
        exit_code = fail(out, "input", PRO_TK_BAD_INPUTS); goto done;
    }

    status = ProEngineerConnect("", "", "", "", PRO_B_TRUE, 10, &random_choice, &process_handle);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "connect", status); goto done; }
    connected = 1;
    status = ProMdlCurrentGet(&current);
    if (status == PRO_TK_NO_ERROR) status = ProMdlTypeGet(current, &model_type);
    if (status == PRO_TK_NO_ERROR) status = ProMdlNameGet(current, current_name);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART || _wcsicmp(current_name, argv[2]) != 0) {
        exit_code = fail(out, "current_part_guard", status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto cleanup;
    }
    status = ProModelitemByNameInit(current, PRO_FEATURE, argv[3], &item);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "feature_name_guard", status); goto cleanup; }
    feature = *(ProFeature *)&item;
    if (feature.id != feature_id) { exit_code = fail(out, "feature_id_guard", PRO_TK_BAD_CONTEXT); goto cleanup; }
    status = ProFeatureTypeGet(&feature, &feature_type);
    if (status != PRO_TK_NO_ERROR || (int)feature_type != expected_type) {
        exit_code = fail(out, "feature_type_guard", status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto cleanup;
    }
    status = ProFeatureStatusGet(&feature, &feature_status);
    if (status != PRO_TK_NO_ERROR || feature_status != PRO_FEAT_ACTIVE) {
        exit_code = fail(out, "feature_status_guard", status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status); goto cleanup;
    }
    status = ProFeatureChildrenGet(&feature, &children, &child_count);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "children_readback", status); goto cleanup; }
    if (child_count != 0) { exit_code = fail(out, "dependent_children_guard", PRO_TK_BAD_CONTEXT); goto cleanup; }
    status = feature_count_get((ProSolid)current, &before_count);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "before_count", status); goto cleanup; }

    status = ProFeatureDelete((ProSolid)current, &feature_id, 1, &delete_option, 1);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "delete", status); goto cleanup; }
    status = ProSolidRegenerate((ProSolid)current, PRO_REGEN_NO_FLAGS);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "regenerate", status); goto cleanup; }
    status = ProModelitemByNameInit(current, PRO_FEATURE, argv[3], &probe);
    if (status == PRO_TK_NO_ERROR) { exit_code = fail(out, "deleted_feature_still_present", PRO_TK_GENERAL_ERROR); goto cleanup; }
    status = feature_count_get((ProSolid)current, &after_count);
    if (status != PRO_TK_NO_ERROR || after_count >= before_count) {
        exit_code = fail(out, "feature_count_readback", status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status); goto cleanup;
    }
    status = ProMdlSave(current);
    if (status != PRO_TK_NO_ERROR) { exit_code = fail(out, "save", status); goto cleanup; }

    fprintf(out, "{\"ok\":true,\"api_only\":true,\"model\":\"%ls\",\"deleted_feature\":\"%ls\",\"feature_id\":%d,\"type_code\":%d,\"before_feature_count\":%d,\"after_feature_count\":%d,\"removed_feature_count\":%d,\"regenerated\":true,\"saved\":true,\"previous_version_preserved\":true}\n",
        argv[2], argv[3], feature_id, expected_type, before_count, after_count,
        before_count - after_count);
    exit_code = 0;

cleanup:
    if (children != NULL) ProArrayFree((ProArray *)&children);
    if (connected) {
        disconnect_status = ProEngineerDisconnect(&process_handle, 10);
        (void)disconnect_status;
    }
done:
    fclose(out);
    return exit_code;
}
