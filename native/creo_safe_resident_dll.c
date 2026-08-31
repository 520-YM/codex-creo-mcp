#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <ProToolkit.h>
#include <ProCore.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProAsmcomp.h>
#include <ProPattern.h>
#include <ProElement.h>
#include <ProElempath.h>
#include <ProModelitem.h>
#include <ProArray.h>
#include <ProUIDialog.h>

#define PIPE_NAME L"\\\\.\\pipe\\creo_safe_flat_wall_v1"
#define MAX_REQUEST 4096
#define MAX_RESPONSE 1024
#define MAX_COMMAND_ARGS 32
#define MAX_COMMAND_LINE 2048

static volatile LONG resident_running = 0;
static volatile LONG resident_request_pending = 0;
static CRITICAL_SECTION resident_lock;
static HANDLE resident_stop_event = NULL;
static HANDLE resident_response_event = NULL;

static void resident_startup_log(const wchar_t *stage, int status)
{
    wchar_t path[MAX_PATH];
    wchar_t line[256];
    DWORD length;
    DWORD written = 0;
    HANDLE file;
    if (GetTempPathW(MAX_PATH, path) == 0)
        return;
    if (wcscat_s(path, MAX_PATH, L"creo_safe_resident_startup.log") != 0)
        return;
    file = CreateFileW(
        path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    swprintf_s(line, sizeof(line) / sizeof(line[0]),
               L"%s status=%d pid=%lu\r\n", stage, status,
               (unsigned long)GetCurrentProcessId());
    length = (DWORD)(wcslen(line) * sizeof(wchar_t));
    WriteFile(file, line, length, &written, NULL);
    CloseHandle(file);
}
static HANDLE resident_pipe_thread = NULL;
static ProUITimerID resident_timer = NULL;
static char resident_request[MAX_REQUEST];
static char resident_response[MAX_RESPONSE];

static ProError resident_fake_connect(
    char *session_id, char *display, char *user, char *textpath,
    ProBoolean allow_random, unsigned int timeout,
    ProBoolean *random_choice, ProProcessHandle *process)
{
    (void)session_id;
    (void)display;
    (void)user;
    (void)textpath;
    (void)allow_random;
    (void)timeout;
    if (InterlockedCompareExchange(&resident_running, 0, 0) == 0)
        return PRO_TK_COMM_ERROR;
    if (random_choice != NULL)
        *random_choice = PRO_B_FALSE;
    if (process != NULL)
        memset(process, 0, sizeof(*process));
    return PRO_TK_NO_ERROR;
}

static ProError resident_fake_disconnect(
    ProProcessHandle *process, unsigned int timeout)
{
    (void)process;
    (void)timeout;
    return PRO_TK_NO_ERROR;
}

#define ProEngineerConnect resident_fake_connect
#define ProEngineerDisconnect resident_fake_disconnect
#define wmain resident_flat_wall_wmain
#include "creo_sheetmetal_flat_wall_bridge.c"
#undef wmain
#undef ProEngineerDisconnect
#undef ProEngineerConnect

#define ProEngineerConnect resident_fake_connect
#define ProEngineerDisconnect resident_fake_disconnect
#define wmain resident_dimension_modify_wmain
#include "creo_dimension_modify_bridge.c"
#undef wmain
#undef ProEngineerDisconnect
#undef ProEngineerConnect

typedef struct resident_feature_context
{
    FILE *out;
    int count;
} ResidentFeatureContext;

typedef struct resident_dimension_change
{
    int owner_feature_id;
    ProName symbol;
    double expected_value;
    double new_value;
    double original_value;
    double verified_value;
    int dimension_id;
    int changed;
} ResidentDimensionChange;

typedef struct resident_component_switch_context
{
    const wchar_t *suppress_model_name;
    const wchar_t *resume_model_name;
    int suppress_feature_id;
    int resume_feature_id;
    ProFeatStatus suppress_status;
    ProFeatStatus resume_status;
    int suppress_match_count;
    int resume_match_count;
} ResidentComponentSwitchContext;

typedef struct resident_pattern_dimension_context
{
    int owner_feature_id;
    int match_count;
    ProDimension dimension;
    ProName symbol;
} ResidentPatternDimensionContext;

static int resident_utf8_to_wide(
    const char *source, wchar_t *target, size_t target_count)
{
    if (source == NULL || target == NULL || target_count == 0)
        return 0;
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
        target, (int)target_count) > 0;
}

static const char *resident_model_type_name(ProMdlType type)
{
    switch (type)
    {
        case PRO_MDL_PART: return "part";
        case PRO_MDL_ASSEMBLY: return "assembly";
        case PRO_MDL_DRAWING: return "drawing";
        case PRO_MDL_MFG: return "manufacturing";
        case PRO_MDL_LAYOUT: return "layout";
        case PRO_MDL_REPORT: return "report";
        case PRO_MDL_MARKUP: return "markup";
        case PRO_MDL_DIAGRAM: return "diagram";
        default: return "other";
    }
}

static int resident_basic_snapshot(const wchar_t *output_path)
{
    FILE *out = NULL;
    ProPath working_directory;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProMdlType model_type = PRO_MDL_UNUSED;
    ProError directory_status;
    ProError current_status;
    ProError name_status;
    ProError type_status;
    ProError outline_status = PRO_TK_INVALID_TYPE;
    Pro3dPnt outline[2] = {{0}};
    ProMatrix matrix = {
        {1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}
    };
    ProSolidOutlExclTypes excludes[] = {
        PRO_OUTL_EXC_DATUM_PLANE, PRO_OUTL_EXC_DATUM_POINT,
        PRO_OUTL_EXC_DATUM_CSYS, PRO_OUTL_EXC_DATUM_AXES,
        PRO_OUTL_EXC_ALL_CRVS
    };

    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;
    directory_status = ProDirectoryCurrentGet(working_directory);
    if (directory_status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            "{\"ok\":false,\"readonly\":true,\"stage\":\"working_directory\","
            "\"error_code\":%d}\n", directory_status);
        fclose(out);
        return 1;
    }
    current_status = ProMdlCurrentGet(&model);
    fputs("{\"ok\":true,\"readonly\":true,\"persistent\":true,"
          "\"session_bound\":true,\"working_directory\":", out);
    json_wide(out, working_directory);
    if (current_status == PRO_TK_BAD_CONTEXT)
    {
        fputs(",\"model_open\":false,\"current_model\":null,"
              "\"scan_scope\":[\"working_directory\"]}\n", out);
        fclose(out);
        return 0;
    }
    if (current_status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            ",\"ok\":false,\"model_open\":false,\"current_model\":null,"
            "\"stage\":\"current_model\",\"error_code\":%d}\n",
            current_status);
        fclose(out);
        return 1;
    }
    name_status = ProMdlNameGet(model, model_name);
    type_status = ProMdlTypeGet(model, &model_type);
    if (name_status != PRO_TK_NO_ERROR || type_status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            ",\"ok\":false,\"model_open\":true,\"current_model\":null,"
            "\"stage\":\"model_identity\",\"name_status\":%d,"
            "\"type_status\":%d}\n", name_status, type_status);
        fclose(out);
        return 1;
    }
    if (model_type == PRO_MDL_PART ||
        model_type == PRO_MDL_ASSEMBLY || model_type == PRO_MDL_MFG)
    {
        outline_status = ProSolidOutlineCompute(
            (ProSolid)model, matrix, excludes,
            (int)(sizeof(excludes) / sizeof(excludes[0])), outline);
    }
    fputs(",\"model_open\":true,\"current_model\":{\"name\":", out);
    json_wide(out, model_name);
    fprintf(out,
        ",\"model_type\":\"%s\",\"model_type_code\":%d,"
        "\"outline_status\":%d,\"outline\":",
        resident_model_type_name(model_type), model_type, outline_status);
    if (outline_status == PRO_TK_NO_ERROR)
    {
        fprintf(out,
            "{\"min\":[%.17g,%.17g,%.17g],"
            "\"max\":[%.17g,%.17g,%.17g],"
            "\"size\":[%.17g,%.17g,%.17g]}",
            outline[0][0], outline[0][1], outline[0][2],
            outline[1][0], outline[1][1], outline[1][2],
            outline[1][0] - outline[0][0],
            outline[1][1] - outline[0][1],
            outline[1][2] - outline[0][2]);
    }
    else
        fputs("null", out);
    fputs("},\"scan_scope\":[\"working_directory\",\"model_identity\","
          "\"model_type\",\"outline\"]}\n", out);
    fclose(out);
    return 0;
}

static const char *resident_feature_type_name(ProFeattype type)
{
    switch (type)
    {
        case PRO_FEAT_PROTRUSION: return "protrusion";
        case PRO_FEAT_CUT: return "cut";
        case PRO_FEAT_HOLE: return "hole";
        case PRO_FEAT_ROUND: return "round";
        case PRO_FEAT_CHAMFER: return "chamfer";
        case PRO_FEAT_SHELL: return "shell";
        case PRO_FEAT_DATUM: return "datum";
        case PRO_FEAT_DATUM_SURF: return "datum_surface";
        case PRO_FEAT_DATUM_AXIS: return "datum_axis";
        case PRO_FEAT_DATUM_POINT: return "datum_point";
        case PRO_FEAT_CSYS: return "coordinate_system";
        case PRO_FEAT_ZONE: return "zone";
        default: return "other";
    }
}

static const char *resident_feature_status_name(ProFeatStatus status)
{
    switch (status)
    {
        case PRO_FEAT_ACTIVE: return "active";
        case PRO_FEAT_INACTIVE: return "inactive";
        case PRO_FEAT_FAMTAB_SUPPRESSED: return "family_table_suppressed";
        case PRO_FEAT_SIMP_REP_SUPPRESSED: return "simplified_rep_suppressed";
        case PRO_FEAT_PROG_SUPPRESSED: return "program_suppressed";
        case PRO_FEAT_SUPPRESSED: return "suppressed";
        case PRO_FEAT_UNREGENERATED: return "unregenerated";
        default: return "invalid";
    }
}

static void resident_write_id_array(FILE *out, int *ids, int count)
{
    int index;
    fputc('[', out);
    for (index = 0; index < count; index++)
    {
        if (index > 0)
            fputc(',', out);
        fprintf(out, "%d", ids[index]);
    }
    fputc(']', out);
}

static ProError resident_feature_visit(
    ProFeature *feature, ProError filter_status, ProAppData app_data)
{
    ResidentFeatureContext *context = (ResidentFeatureContext *)app_data;
    ProName name = L"";
    ProFeattype type = -1;
    ProFeatStatus status = PRO_FEAT_INVALID;
    ProBoolean visible = PRO_B_FALSE;
    int *parents = NULL;
    int *children = NULL;
    int parent_count = 0;
    int child_count = 0;
    ProError name_status;
    ProError type_status;
    ProError feature_status;
    ProError visible_status;
    ProError parents_status;
    ProError children_status;

    (void)filter_status;
    name_status = ProModelitemNameGet((ProModelitem *)feature, name);
    type_status = ProFeatureTypeGet(feature, &type);
    feature_status = ProFeatureStatusGet(feature, &status);
    visible_status = ProFeatureVisibilityGet(feature, &visible);
    parents_status = ProFeatureParentsGet(feature, &parents, &parent_count);
    children_status = ProFeatureChildrenGet(feature, &children, &child_count);
    if (context->count > 0)
        fputc(',', context->out);
    fprintf(context->out, "{\"id\":%d,\"name\":", feature->id);
    if (name_status == PRO_TK_NO_ERROR)
        json_wide(context->out, name);
    else
        fputs("null", context->out);
    fprintf(context->out,
        ",\"type\":\"%s\",\"type_code\":%d,"
        "\"status\":\"%s\",\"status_code\":%d,\"visible\":%s,"
        "\"parent_ids\":",
        type_status == PRO_TK_NO_ERROR ? resident_feature_type_name(type) : "other",
        type_status == PRO_TK_NO_ERROR ? (int)type : -1,
        feature_status == PRO_TK_NO_ERROR ? resident_feature_status_name(status) : "invalid",
        feature_status == PRO_TK_NO_ERROR ? (int)status : -1,
        visible_status == PRO_TK_NO_ERROR && visible == PRO_B_TRUE ? "true" : "false");
    resident_write_id_array(
        context->out, parents,
        parents_status == PRO_TK_NO_ERROR ? parent_count : 0);
    fputs(",\"child_ids\":", context->out);
    resident_write_id_array(
        context->out, children,
        children_status == PRO_TK_NO_ERROR ? child_count : 0);
    fputc('}', context->out);
    if (parents != NULL)
        ProArrayFree((ProArray *)&parents);
    if (children != NULL)
        ProArrayFree((ProArray *)&children);
    context->count++;
    return PRO_TK_NO_ERROR;
}

static int resident_feature_snapshot(const wchar_t *output_path)
{
    FILE *out = NULL;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProMdlType model_type;
    ProError status;
    ResidentFeatureContext context;

    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;
    status = ProMdlCurrentGet(&model);
    if (status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            "{\"ok\":false,\"readonly\":true,\"stage\":\"current_model\","
            "\"error_code\":%d}\n", status);
        fclose(out);
        return 1;
    }
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
    {
        fprintf(out,
            "{\"ok\":false,\"readonly\":true,\"stage\":\"model_name\","
            "\"error_code\":%d}\n", status);
        fclose(out);
        return 1;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART)
    {
        fprintf(out,
            "{\"ok\":false,\"readonly\":true,\"stage\":\"model_type_guard\","
            "\"error_code\":%d}\n",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        fclose(out);
        return 1;
    }
    fputs("{\"ok\":true,\"readonly\":true,\"persistent\":true,"
          "\"model\":", out);
    json_wide(out, model_name);
    fputs(",\"features\":[", out);
    context.out = out;
    context.count = 0;
    status = ProSolidFeatVisit(
        (ProSolid)model, resident_feature_visit, NULL,
        (ProAppData)&context);
    fprintf(out,
        "],\"feature_count\":%d,\"visit_status\":%d}\n",
        context.count, status);
    fclose(out);
    return status == PRO_TK_NO_ERROR || status == PRO_TK_E_NOT_FOUND ? 0 : 1;
}

static int resident_path_has_extension(
    const wchar_t *path, const wchar_t *extension)
{
    size_t path_length = wcslen(path);
    size_t extension_length = wcslen(extension);
    size_t index;
    if (path_length < extension_length)
        return 0;
    for (index = 0; index + extension_length <= path_length; index++)
    {
        if (_wcsnicmp(path + index, extension, extension_length) == 0)
            return 1;
    }
    return 0;
}

static BOOL CALLBACK resident_foreground_creo_window(
    HWND window, LPARAM app_data)
{
    wchar_t title[512];
    int length;
    (void)app_data;
    if (!IsWindowVisible(window))
        return TRUE;
    length = GetWindowTextW(window, title, 512);
    if (length > 0 && wcsstr(title, L"Creo Parametric") != NULL)
    {
        ShowWindow(window, SW_RESTORE);
        BringWindowToTop(window);
        SetForegroundWindow(window);
        return FALSE;
    }
    return TRUE;
}

static int resident_display_execute(
    const wchar_t *output_path,
    const wchar_t *model_path,
    const wchar_t *expected_name)
{
    FILE *out = NULL;
    ProMdl model = NULL;
    ProMdl current = NULL;
    ProMdlName model_name;
    ProMdlName current_name;
    ProMdlfileType file_type = PRO_MDLFILE_PART;
    ProMdlType window_model_type = PRO_PART;
    ProError status;
    ProError activate_status = PRO_TK_NO_ERROR;
    ProError refit_status = PRO_TK_NO_ERROR;
    ProError repaint_status = PRO_TK_NO_ERROR;
    int window_id = -1;

    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;
    if (GetFileAttributesW(model_path) == INVALID_FILE_ATTRIBUTES)
    {
        fputs("{\"ok\":false,\"display_only\":true,"
              "\"stage\":\"model_file\",\"error_code\":-4}\n", out);
        fclose(out);
        return 1;
    }
    if (resident_path_has_extension(model_path, L".asm"))
    {
        file_type = PRO_MDLFILE_ASSEMBLY;
        window_model_type = PRO_ASSEMBLY;
    }
    else if (!resident_path_has_extension(model_path, L".prt"))
    {
        fputs("{\"ok\":false,\"display_only\":true,"
              "\"stage\":\"model_file_type\",\"error_code\":-5}\n", out);
        fclose(out);
        return 1;
    }
    status = ProMdlFiletypeLoad(
        (wchar_t *)model_path, file_type, PRO_B_FALSE, &model);
    if (status != PRO_TK_NO_ERROR)
        goto load_failure;
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR)
        goto name_failure;
    if (_wcsicmp(model_name, expected_name) != 0)
    {
        status = PRO_TK_BAD_CONTEXT;
        goto guard_failure;
    }
    status = ProMdlWindowGet(model, &window_id);
    if (status != PRO_TK_NO_ERROR)
    {
        status = ProObjectwindowMdlnameCreate(
            model_name, window_model_type, &window_id);
        if (status != PRO_TK_NO_ERROR)
            goto create_window_failure;
    }
    status = ProWindowCurrentSet(window_id);
    if (status != PRO_TK_NO_ERROR)
        goto set_window_failure;
    status = ProWindowActivate(window_id);
    if (status != PRO_TK_NO_ERROR)
        goto activate_window_failure;
    status = ProMdlDisplay(model);
    if (status != PRO_TK_NO_ERROR)
        goto display_failure;
    status = ProMdlCurrentGet(&current);
    if (status != PRO_TK_NO_ERROR)
        goto current_failure;
    status = ProMdlNameGet(current, current_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(current_name, expected_name) != 0)
    {
        if (status == PRO_TK_NO_ERROR)
            status = PRO_TK_BAD_CONTEXT;
        goto current_guard_failure;
    }
    if (ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
    {
        activate_status = ProWindowActivate(window_id);
        refit_status = ProWindowRefit(window_id);
        repaint_status = ProWindowRepaint(window_id);
    }
    fputs("{\"ok\":true,\"display_only\":true,"
          "\"saved_or_modified\":false,\"persistent\":true,"
          "\"model\":", out);
    json_wide(out, model_name);
    fprintf(out,
        ",\"window_id\":%d,\"activate_status\":%d,"
        "\"refit_status\":%d,\"repaint_status\":%d}\n",
        window_id, activate_status, refit_status, repaint_status);
    fclose(out);
    EnumWindows(resident_foreground_creo_window, 0);
    return 0;

load_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"load_model_file\"", out);
    goto display_error;
name_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"model_name\"", out);
    goto display_error;
guard_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"model_name_guard\"", out);
    goto display_error;
create_window_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"create_model_window\"", out);
    goto display_error;
set_window_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"set_current_window\"", out);
    goto display_error;
activate_window_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"activate_model_window\"", out);
    goto display_error;
display_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"display_model\"", out);
    goto display_error;
current_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"current_model_readback\"", out);
    goto display_error;
current_guard_failure:
    fputs("{\"ok\":false,\"display_only\":true,\"stage\":\"current_model_guard\"", out);
display_error:
    fprintf(out, ",\"error_code\":%d}\n", status);
    fclose(out);
    return 1;
}

static int resident_dimension_batch_execute(
    const wchar_t *output_path,
    const wchar_t *model_name,
    const wchar_t *assembly_name,
    int count,
    ResidentDimensionChange *changes,
    int enforce_expected_values)
{
    FILE *out = NULL;
    ProMdl current = NULL;
    ProMdl model = NULL;
    ProMdl assembly = NULL;
    ProMdlName current_name;
    ProMdlName actual_name;
    ProMdlType model_type;
    ProError status = PRO_TK_NO_ERROR;
    ProError model_regen_status = PRO_TK_GENERAL_ERROR;
    ProError assembly_regen_status = PRO_TK_GENERAL_ERROR;
    int model_regen_attempts = 0;
    int assembly_regen_attempts = 0;
    int index;
    int window_id = -1;
    int exit_code = 1;

    if (count < 1 || count > MAX_MODIFICATIONS || changes == NULL)
        return 2;
    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;

    status = ProMdlCurrentGet(&current);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto done;
    }
    status = ProMdlNameGet(current, current_name);
    if (status != PRO_TK_NO_ERROR ||
        (_wcsicmp(current_name, model_name) != 0 &&
         _wcsicmp(current_name, assembly_name) != 0))
    {
        exit_code = write_error(
            out, "current_model_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProMdlnameInit((wchar_t *)model_name, PRO_MDLFILE_PART, &model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "skeleton_init", status);
        goto done;
    }
    status = ProMdlnameInit(
        (wchar_t *)assembly_name, PRO_MDLFILE_ASSEMBLY, &assembly);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "assembly_init", status);
        goto done;
    }
    status = ProMdlNameGet(model, actual_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(actual_name, model_name) != 0)
    {
        exit_code = write_error(
            out, "skeleton_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART)
    {
        exit_code = write_error(
            out, "skeleton_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto done;
    }

    for (index = 0; index < count; ++index)
    {
        ProFeature feature;
        ProFeatStatus feature_status = PRO_FEAT_INVALID;
        ProDimension dimension;
        ProFeature owner;
        ProBoolean relation_driven = PRO_B_FALSE;
        status = ProFeatureInit(
            (ProSolid)model, changes[index].owner_feature_id, &feature);
        if (status == PRO_TK_NO_ERROR)
            status = ProFeatureStatusGet(&feature, &feature_status);
        if (status != PRO_TK_NO_ERROR || feature_status != PRO_FEAT_ACTIVE)
        {
            exit_code = write_error(
                out, "feature_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
            goto rollback;
        }
        status = find_dimension_by_symbol(
            (ProSolid)model, changes[index].symbol, &dimension);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "dimension_symbol_guard", status);
            goto rollback;
        }
        status = ProDimensionOwnerfeatureGet(&dimension, &owner);
        if (status != PRO_TK_NO_ERROR ||
            owner.id != changes[index].owner_feature_id)
        {
            exit_code = write_error(
                out, "dimension_owner_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
            goto rollback;
        }
        status = ProDimensionIsReldriven(&dimension, &relation_driven);
        if (status != PRO_TK_NO_ERROR || relation_driven == PRO_B_TRUE)
        {
            exit_code = write_error(
                out, "relation_driven_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_NO_PERMISSION : status);
            goto rollback;
        }
        status = ProDimensionValueGet(
            &dimension, &changes[index].original_value);
        if (status != PRO_TK_NO_ERROR ||
            (enforce_expected_values &&
             !nearly_equal(
                 changes[index].original_value,
                 changes[index].expected_value)))
        {
            exit_code = write_error(
                out, "expected_value_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
            goto rollback;
        }
        changes[index].dimension_id = dimension.id;
    }

    for (index = 0; index < count; ++index)
    {
        ProDimension dimension;
        status = find_dimension_by_symbol(
            (ProSolid)model, changes[index].symbol, &dimension);
        if (status == PRO_TK_NO_ERROR)
            status = ProDimensionValueSet(
                &dimension, changes[index].new_value);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "dimension_set", status);
            goto rollback;
        }
        changes[index].changed = 1;
    }

    do
    {
        model_regen_status = ProSolidRegenerate(
            (ProSolid)model, PRO_REGEN_NO_FLAGS);
        ++model_regen_attempts;
    } while (model_regen_status == PRO_TK_REGEN_AGAIN &&
             model_regen_attempts < 3);
    if (model_regen_status != PRO_TK_NO_ERROR &&
        model_regen_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "skeleton_regenerate", model_regen_status);
        goto rollback;
    }
    for (index = 0; index < count; ++index)
    {
        ProDimension dimension;
        status = find_dimension_by_symbol(
            (ProSolid)model, changes[index].symbol, &dimension);
        if (status == PRO_TK_NO_ERROR)
            status = ProDimensionValueGet(
                &dimension, &changes[index].verified_value);
        if (status != PRO_TK_NO_ERROR ||
            !nearly_equal(
                changes[index].verified_value, changes[index].new_value))
        {
            exit_code = write_error(
                out, "dimension_readback",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto rollback;
        }
    }

    do
    {
        assembly_regen_status = ProSolidRegenerate(
            (ProSolid)assembly, PRO_REGEN_NO_FLAGS);
        ++assembly_regen_attempts;
    } while (assembly_regen_status == PRO_TK_REGEN_AGAIN &&
             assembly_regen_attempts < 3);
    if (assembly_regen_status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(
            out, "assembly_regenerate", assembly_regen_status);
        goto rollback;
    }
    status = ProMdlSave(model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_skeleton", status);
        goto rollback;
    }
    status = ProMdlSave(assembly);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_assembly", status);
        goto rollback;
    }
    if (ProMdlWindowGet(assembly, &window_id) == PRO_TK_NO_ERROR)
    {
        ProWindowCurrentSet(window_id);
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"persistent\":true,\"atomic\":true,"
          "\"model\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"assembly\":", out);
    write_wide_json_string(out, assembly_name);
    fputs(",\"modifications\":[", out);
    for (index = 0; index < count; ++index)
    {
        if (index > 0)
            fputc(',', out);
        fprintf(out,
            "{\"owner_feature_id\":%d,\"symbol\":",
            changes[index].owner_feature_id);
        write_wide_json_string(out, changes[index].symbol);
        fprintf(out,
            ",\"dimension_id\":%d,\"old_value\":%.17g,"
            "\"new_value\":%.17g,\"verified_value\":%.17g}",
            changes[index].dimension_id,
            changes[index].original_value,
            changes[index].new_value,
            changes[index].verified_value);
    }
    fprintf(out,
        "],\"skeleton_regenerate_status\":%d,"
        "\"skeleton_regenerate_attempts\":%d,"
        "\"assembly_regenerate_status\":%d,"
        "\"assembly_regenerate_attempts\":%d,"
        "\"window_id\":%d,\"saved\":true}\n",
        model_regen_status, model_regen_attempts,
        assembly_regen_status, assembly_regen_attempts, window_id);
    exit_code = 0;
    goto done;

rollback:
    for (index = 0; index < count; ++index)
    {
        if (changes[index].changed)
        {
            ProDimension dimension;
            if (find_dimension_by_symbol(
                    (ProSolid)model,
                    changes[index].symbol,
                    &dimension) == PRO_TK_NO_ERROR)
                ProDimensionValueSet(
                    &dimension, changes[index].original_value);
        }
    }
    if (model != NULL)
        ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
    if (assembly != NULL)
        ProSolidRegenerate((ProSolid)assembly, PRO_REGEN_NO_FLAGS);

done:
    fclose(out);
    return exit_code;
}

static ProError resident_component_find_visit(
    ProFeature *feature, ProError filter_status, ProAppData app_data)
{
    ResidentComponentSwitchContext *context =
        (ResidentComponentSwitchContext *)app_data;
    ProFeattype type = PRO_FEAT_INVALID;
    ProFeatStatus status = PRO_FEAT_INVALID;
    ProMdlfileType model_file_type = PRO_MDLFILE_UNUSED;
    ProFamilyMdlName model_name = L"";

    (void)filter_status;
    if (ProFeatureTypeGet(feature, &type) != PRO_TK_NO_ERROR ||
        type != PRO_FEAT_COMPONENT ||
        ProFeatureStatusGet(feature, &status) != PRO_TK_NO_ERROR ||
        ProAsmcompMdlMdlnameGet(
            (ProAsmcomp *)feature, &model_file_type, model_name) !=
            PRO_TK_NO_ERROR)
        return PRO_TK_NO_ERROR;

    if (_wcsicmp(model_name, context->suppress_model_name) == 0)
    {
        context->suppress_match_count++;
        context->suppress_feature_id = feature->id;
        context->suppress_status = status;
    }
    if (_wcsicmp(model_name, context->resume_model_name) == 0)
    {
        context->resume_match_count++;
        context->resume_feature_id = feature->id;
        context->resume_status = status;
    }
    return PRO_TK_NO_ERROR;
}

static int resident_component_switch_execute(
    const wchar_t *output_path,
    const wchar_t *assembly_name,
    const wchar_t *top_assembly_name,
    const wchar_t *suppress_model_name,
    const wchar_t *resume_model_name)
{
    FILE *out = NULL;
    ProMdl current = NULL;
    ProMdl assembly = NULL;
    ProMdl top_assembly = NULL;
    ProMdlName current_name;
    ProMdlType model_type = PRO_MDL_UNUSED;
    ProError status = PRO_TK_NO_ERROR;
    ProError assembly_regen_status = PRO_TK_GENERAL_ERROR;
    ProError top_regen_status = PRO_TK_GENERAL_ERROR;
    ProFeatureDeleteOptions suppress_option = PRO_FEAT_DELETE_NO_OPTS;
    ProFeatureResumeOptions resume_option = PRO_FEAT_RESUME_NO_OPTS;
    ResidentComponentSwitchContext context;
    int assembly_regen_attempts = 0;
    int top_regen_attempts = 0;
    int suppress_changed = 0;
    int resume_changed = 0;
    int window_id = -1;
    int exit_code = 1;

    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;
    ZeroMemory(&context, sizeof(context));
    context.suppress_model_name = suppress_model_name;
    context.resume_model_name = resume_model_name;
    context.suppress_feature_id = -1;
    context.resume_feature_id = -1;
    context.suppress_status = PRO_FEAT_INVALID;
    context.resume_status = PRO_FEAT_INVALID;

    if (_wcsicmp(suppress_model_name, resume_model_name) == 0)
    {
        exit_code = write_error(out, "component_name_guard", PRO_TK_BAD_INPUTS);
        goto done;
    }
    status = ProMdlCurrentGet(&current);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto done;
    }
    status = ProMdlNameGet(current, current_name);
    if (status != PRO_TK_NO_ERROR ||
        (_wcsicmp(current_name, assembly_name) != 0 &&
         _wcsicmp(current_name, top_assembly_name) != 0))
    {
        exit_code = write_error(
            out, "current_model_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProMdlnameInit(
        (wchar_t *)assembly_name, PRO_MDLFILE_ASSEMBLY, &assembly);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "assembly_init", status);
        goto done;
    }
    status = ProMdlTypeGet(assembly, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_ASSEMBLY)
    {
        exit_code = write_error(
            out, "assembly_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto done;
    }
    status = ProMdlnameInit(
        (wchar_t *)top_assembly_name, PRO_MDLFILE_ASSEMBLY, &top_assembly);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "top_assembly_init", status);
        goto done;
    }
    status = ProSolidFeatVisit(
        (ProSolid)assembly, resident_component_find_visit, NULL,
        (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR ||
        context.suppress_match_count != 1 ||
        context.resume_match_count != 1)
    {
        exit_code = write_error(
            out, "component_match_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_E_AMBIGUOUS : status);
        goto done;
    }
    if (context.suppress_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "suppress_status_guard", PRO_TK_BAD_CONTEXT);
        goto done;
    }
    if (context.resume_status != PRO_FEAT_SUPPRESSED)
    {
        exit_code = write_error(out, "resume_status_guard", PRO_TK_BAD_CONTEXT);
        goto done;
    }

    status = ProFeatureSuppress(
        (ProSolid)assembly, &context.suppress_feature_id, 1,
        &suppress_option, 1);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "component_suppress", status);
        goto rollback;
    }
    suppress_changed = 1;
    status = ProFeatureResume(
        (ProSolid)assembly, &context.resume_feature_id, 1,
        &resume_option, 1);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "component_resume", status);
        goto rollback;
    }
    resume_changed = 1;

    {
        ProFeature feature;
        ProFeatStatus readback = PRO_FEAT_INVALID;
        status = ProFeatureInit(
            (ProSolid)assembly, context.suppress_feature_id, &feature);
        if (status == PRO_TK_NO_ERROR)
            status = ProFeatureStatusGet(&feature, &readback);
        if (status != PRO_TK_NO_ERROR || readback != PRO_FEAT_SUPPRESSED)
        {
            exit_code = write_error(
                out, "suppress_readback",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto rollback;
        }
        status = ProFeatureInit(
            (ProSolid)assembly, context.resume_feature_id, &feature);
        if (status == PRO_TK_NO_ERROR)
            status = ProFeatureStatusGet(&feature, &readback);
        if (status != PRO_TK_NO_ERROR || readback != PRO_FEAT_ACTIVE)
        {
            exit_code = write_error(
                out, "resume_readback",
                status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
            goto rollback;
        }
    }

    do
    {
        assembly_regen_status = ProSolidRegenerate(
            (ProSolid)assembly, PRO_REGEN_NO_FLAGS);
        ++assembly_regen_attempts;
    } while (assembly_regen_status == PRO_TK_REGEN_AGAIN &&
             assembly_regen_attempts < 3);
    if (assembly_regen_status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(
            out, "assembly_regenerate", assembly_regen_status);
        goto rollback;
    }
    do
    {
        top_regen_status = ProSolidRegenerate(
            (ProSolid)top_assembly, PRO_REGEN_NO_FLAGS);
        ++top_regen_attempts;
    } while (top_regen_status == PRO_TK_REGEN_AGAIN &&
             top_regen_attempts < 3);
    if (top_regen_status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "top_regenerate", top_regen_status);
        goto rollback;
    }
    status = ProMdlSave(assembly);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_assembly", status);
        goto rollback;
    }
    status = ProMdlSave(top_assembly);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_top_assembly", status);
        goto rollback;
    }
    if (ProMdlWindowGet(top_assembly, &window_id) == PRO_TK_NO_ERROR)
    {
        ProWindowCurrentSet(window_id);
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"persistent\":true,\"atomic\":true,"
          "\"assembly\":", out);
    write_wide_json_string(out, assembly_name);
    fputs(",\"top_assembly\":", out);
    write_wide_json_string(out, top_assembly_name);
    fputs(",\"suppressed_component\":", out);
    write_wide_json_string(out, suppress_model_name);
    fprintf(out, ",\"suppressed_feature_id\":%d,",
            context.suppress_feature_id);
    fputs("\"resumed_component\":", out);
    write_wide_json_string(out, resume_model_name);
    fprintf(out,
        ",\"resumed_feature_id\":%d,"
        "\"assembly_regenerate_status\":%d,"
        "\"top_regenerate_status\":%d,"
        "\"window_id\":%d,\"saved\":true}\n",
        context.resume_feature_id, assembly_regen_status,
        top_regen_status, window_id);
    exit_code = 0;
    goto done;

rollback:
    if (resume_changed)
        ProFeatureSuppress(
            (ProSolid)assembly, &context.resume_feature_id, 1,
            &suppress_option, 1);
    if (suppress_changed)
        ProFeatureResume(
            (ProSolid)assembly, &context.suppress_feature_id, 1,
            &resume_option, 1);
    if (assembly != NULL)
        ProSolidRegenerate((ProSolid)assembly, PRO_REGEN_NO_FLAGS);
    if (top_assembly != NULL)
        ProSolidRegenerate((ProSolid)top_assembly, PRO_REGEN_NO_FLAGS);

done:
    fclose(out);
    return exit_code;
}

static ProError resident_pattern_integer_element_get(
    ProElement tree, ProElemId ids[], int count,
    ProElement *element, int *value)
{
    ProElempath path = NULL;
    ProElempathItem items[4];
    ProError status;
    int index;

    if (tree == NULL || ids == NULL || count < 1 || count > 4 ||
        element == NULL || value == NULL)
        return PRO_TK_BAD_INPUTS;
    *element = NULL;
    status = ProElempathAlloc(&path);
    if (status != PRO_TK_NO_ERROR)
        return status;
    for (index = 0; index < count; ++index)
    {
        items[index].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
        items[index].path_item.elem_id = ids[index];
    }
    status = ProElempathDataSet(path, items, count);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementGet(tree, path, element);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementIntegerGet(*element, NULL, value);
    ProElempathFree(&path);
    return status;
}

static ProError resident_pattern_dimension_visit(
    ProDimension *dimension,
    ProError filter_status,
    ProAppData app_data)
{
    ResidentPatternDimensionContext *context =
        (ResidentPatternDimensionContext *)app_data;
    ProFeature owner;
    ProBoolean relation_driven = PRO_B_FALSE;
    ProName symbol = L"";
    double value = 0.0;
    (void)filter_status;
    if (ProDimensionOwnerfeatureGet(dimension, &owner) != PRO_TK_NO_ERROR ||
        owner.id != context->owner_feature_id ||
        ProDimensionSymbolGet(dimension, symbol) != PRO_TK_NO_ERROR ||
        ProDimensionValueGet(dimension, &value) != PRO_TK_NO_ERROR ||
        ProDimensionIsReldriven(dimension, &relation_driven) !=
            PRO_TK_NO_ERROR ||
        relation_driven == PRO_B_TRUE || value <= 0.0)
        return PRO_TK_NO_ERROR;
    context->dimension = *dimension;
    wcsncpy_s(
        context->symbol,
        sizeof(context->symbol) / sizeof(context->symbol[0]),
        symbol, _TRUNCATE);
    context->match_count++;
    return PRO_TK_NO_ERROR;
}

static ProError resident_pattern_dimension_find_unique(
    ProSolid solid,
    int owner_feature_id,
    ProDimension *dimension,
    ProName symbol)
{
    ResidentPatternDimensionContext context;
    ProError status;
    ZeroMemory(&context, sizeof(context));
    context.owner_feature_id = owner_feature_id;
    status = ProSolidDimensionVisit(
        solid, PRO_B_FALSE,
        resident_pattern_dimension_visit, NULL,
        (ProAppData)&context);
    if (status != PRO_TK_NO_ERROR && status != PRO_TK_E_NOT_FOUND)
        return status;
    if (context.match_count == 0)
        return PRO_TK_E_NOT_FOUND;
    if (context.match_count != 1)
        return PRO_TK_BAD_CONTEXT;
    *dimension = context.dimension;
    wcsncpy_s(symbol, PRO_NAME_SIZE, context.symbol, _TRUNCATE);
    return PRO_TK_NO_ERROR;
}

static int resident_pattern_set_execute(
    const wchar_t *output_path,
    const wchar_t *model_name,
    const wchar_t *top_assembly_name,
    int pattern_feature_id,
    const wchar_t *pattern_name,
    int leader_feature_id,
    const wchar_t *leader_name,
    const wchar_t *spacing_symbol,
    int expected_old_count,
    int new_count,
    double new_spacing)
{
    FILE *out = NULL;
    ProMdl current = NULL;
    ProMdl model = NULL;
    ProMdl top_assembly = NULL;
    ProMdlName current_name;
    ProMdlType model_type = PRO_MDL_UNUSED;
    ProFeatStatus header_status = PRO_FEAT_INVALID;
    ProFeature header;
    ProFeature leader;
    ProFeature actual_header;
    ProFeature actual_leader;
    ProName actual_pattern_name = L"";
    ProName actual_leader_name = L"";
    ProName resolved_spacing_symbol = L"";
    ProPatternStatus pattern_status = PRO_PATTERN_INVALID;
    ProGrppatternStatus group_pattern_status = PRO_GRP_PATTERN_INVALID;
    ProPatternClass pattern_class = PRO_FEAT_PATTERN;
    ProPattern pattern;
    ProFeature *members = NULL;
    ProElement tree = NULL;
    ProElement count_element = NULL;
    ProElemId type_path[1] = {PRO_E_GENPAT_TYPE};
    ProElemId count_path[2] = {
        PRO_E_GENPAT_DIM, PRO_E_GENPAT_DIM_FIRST_DIR_NUM_INST};
    ProElement type_element = NULL;
    int pattern_type = -1;
    int old_count = -1;
    int member_count_before = 0;
    int member_count_after = 0;
    ProDimension spacing_dimension;
    ProFeature spacing_owner;
    ProBoolean relation_driven = PRO_B_FALSE;
    double old_spacing = 0.0;
    double verified_spacing = 0.0;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    ProError status = PRO_TK_NO_ERROR;
    ProError model_regen_status = PRO_TK_GENERAL_ERROR;
    ProError top_regen_status = PRO_TK_GENERAL_ERROR;
    int model_regen_attempts = 0;
    int top_regen_attempts = 0;
    int pattern_changed = 0;
    int spacing_changed = 0;
    int resolved_header_id = -1;
    int resolved_leader_id = -1;
    int window_id = -1;
    int exit_code = 1;

    if (_wfopen_s(&out, output_path, L"wb") != 0 || out == NULL)
        return 2;
    if (new_count < 2 || new_count > 1000 || new_spacing == 0.0 ||
        new_spacing > 1000000.0)
    {
        exit_code = write_error(out, "input_guard", PRO_TK_BAD_INPUTS);
        goto done;
    }
    status = ProMdlCurrentGet(&current);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "current_model", status);
        goto done;
    }
    status = ProMdlNameGet(current, current_name);
    if (status != PRO_TK_NO_ERROR ||
        (_wcsicmp(current_name, model_name) != 0 &&
         _wcsicmp(current_name, top_assembly_name) != 0))
    {
        exit_code = write_error(
            out, "current_model_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProMdlnameInit((wchar_t *)model_name, PRO_MDLFILE_PART, &model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "model_init", status);
        goto done;
    }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART)
    {
        exit_code = write_error(
            out, "model_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto done;
    }
    status = ProMdlnameInit(
        (wchar_t *)top_assembly_name, PRO_MDLFILE_ASSEMBLY, &top_assembly);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "top_assembly_init", status);
        goto done;
    }
    status = ProFeatureInit((ProSolid)model, pattern_feature_id, &header);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureStatusGet(&header, &header_status);
    if (status != PRO_TK_NO_ERROR || header_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(
            out, "pattern_feature_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProModelitemNameGet((ProModelitem *)&header, actual_pattern_name);
    if (_wcsicmp(pattern_name, L"*") != 0 &&
        (status != PRO_TK_NO_ERROR ||
         _wcsicmp(actual_pattern_name, pattern_name) != 0))
    {
        exit_code = write_error(
            out, "pattern_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProFeatureInit((ProSolid)model, leader_feature_id, &leader);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeatureStatusGet(&leader, &header_status);
    if (status != PRO_TK_NO_ERROR || header_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(
            out, "leader_feature_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProModelitemNameGet((ProModelitem *)&leader, actual_leader_name);
    if (_wcsicmp(leader_name, L"*") != 0 &&
        (status != PRO_TK_NO_ERROR ||
         _wcsicmp(actual_leader_name, leader_name) != 0))
    {
        exit_code = write_error(
            out, "leader_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProFeaturePatternStatusGet(&leader, &pattern_status);
    if (status == PRO_TK_NO_ERROR && pattern_status == PRO_PATTERN_LEADER)
        pattern_class = PRO_FEAT_PATTERN;
    else
    {
        status = ProFeatureGrppatternStatusGet(
            &leader, &group_pattern_status);
        if (status != PRO_TK_NO_ERROR ||
            group_pattern_status != PRO_GRP_PATTERN_LEADER)
        {
            exit_code = write_error(
                out, "pattern_leader_status_guard",
                status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
            goto done;
        }
        pattern_class = PRO_GROUP_PATTERN;
    }
    status = ProFeaturePatternGet(&leader, pattern_class, &pattern);
    if (status == PRO_TK_NO_ERROR)
        status = ProPatternHeaderGet(&pattern, &actual_header);
    if (status != PRO_TK_NO_ERROR ||
        (pattern_class == PRO_FEAT_PATTERN &&
         actual_header.id != pattern_feature_id))
    {
        exit_code = write_error(
            out, "pattern_header_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    header = actual_header;
    resolved_header_id = actual_header.id;
    if (status == PRO_TK_NO_ERROR)
        status = ProPatternLeaderGet(&pattern, &actual_leader);
    if (status != PRO_TK_NO_ERROR ||
        (pattern_class == PRO_FEAT_PATTERN &&
         actual_leader.id != leader_feature_id))
    {
        exit_code = write_error(
            out, "pattern_leader_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    resolved_leader_id = actual_leader.id;
    if (pattern_class == PRO_FEAT_PATTERN)
        leader = actual_leader;
    status = ProPatternMembersGet(&pattern, &members);
    if (status == PRO_TK_NO_ERROR && members != NULL)
        status = ProArraySizeGet((ProArray)members, &member_count_before);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "pattern_members_read", status);
        goto done;
    }
    status = ProPatternElemtreeCreate(&leader, pattern_class, &tree);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "pattern_tree", status);
        goto done;
    }
    status = resident_pattern_integer_element_get(
        tree, type_path, 1, &type_element, &pattern_type);
    if (status != PRO_TK_NO_ERROR || pattern_type != PRO_GENPAT_DIM_DRIVEN)
    {
        exit_code = write_error(
            out, "pattern_type_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_INVALID_TYPE : status);
        goto done;
    }
    status = resident_pattern_integer_element_get(
        tree, count_path, 2, &count_element, &old_count);
    if (status != PRO_TK_NO_ERROR || old_count != member_count_before)
    {
        exit_code = write_error(
            out, "pattern_count_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    if (expected_old_count >= 2 && old_count != expected_old_count)
    {
        exit_code = write_error(out, "expected_pattern_count_guard", PRO_TK_BAD_CONTEXT);
        goto done;
    }
    if (_wcsicmp(spacing_symbol, L"*") == 0)
        status = resident_pattern_dimension_find_unique(
            (ProSolid)model, pattern_feature_id,
            &spacing_dimension, resolved_spacing_symbol);
    else
    {
        wcsncpy_s(
            resolved_spacing_symbol, PRO_NAME_SIZE,
            spacing_symbol, _TRUNCATE);
        status = find_dimension_by_symbol(
            (ProSolid)model, resolved_spacing_symbol, &spacing_dimension);
    }
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionOwnerfeatureGet(&spacing_dimension, &spacing_owner);
    if (status != PRO_TK_NO_ERROR || spacing_owner.id != pattern_feature_id)
    {
        exit_code = write_error(
            out, "spacing_dimension_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto done;
    }
    status = ProDimensionIsReldriven(&spacing_dimension, &relation_driven);
    if (status != PRO_TK_NO_ERROR || relation_driven == PRO_B_TRUE)
    {
        exit_code = write_error(
            out, "spacing_relation_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_NO_PERMISSION : status);
        goto done;
    }
    status = ProDimensionValueGet(&spacing_dimension, &old_spacing);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "spacing_read", status);
        goto done;
    }
    if (new_spacing < 0.0)
        new_spacing = old_spacing * (double)(old_count - 1) /
            (double)(new_count - 1);

    status = ProArrayAlloc(
        1, sizeof(ProFeatureCreateOptions), 1, (ProArray *)&options);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "redefine_options", status);
        goto done;
    }
    options[0] = PRO_FEAT_CR_DEFINE_MISS_ELEMS;
    status = ProElementIntegerSet(count_element, new_count);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "pattern_count_set", status);
        goto rollback;
    }
    status = ProFeatureWithoptionsRedefine(
        NULL, &header, tree, options, PRO_REGEN_NO_FLAGS, &errors);
    if (status != PRO_TK_NO_ERROR || errors.error_number != 0)
    {
        exit_code = write_error(
            out, "pattern_redefine",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto rollback;
    }
    pattern_changed = 1;
    status = find_dimension_by_symbol(
        (ProSolid)model, resolved_spacing_symbol, &spacing_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueSet(&spacing_dimension, new_spacing);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "spacing_set", status);
        goto rollback;
    }
    spacing_changed = 1;

    do
    {
        model_regen_status = ProSolidRegenerate(
            (ProSolid)model, PRO_REGEN_NO_FLAGS);
        ++model_regen_attempts;
    } while (model_regen_status == PRO_TK_REGEN_AGAIN &&
             model_regen_attempts < 3);
    if (model_regen_status != PRO_TK_NO_ERROR &&
        model_regen_status != PRO_TK_UNATTACHED_FEATS)
    {
        exit_code = write_error(out, "model_regenerate", model_regen_status);
        goto rollback;
    }
    if (members != NULL)
    {
        ProArrayFree((ProArray *)&members);
        members = NULL;
    }
    status = ProFeatureInit(
        (ProSolid)model,
        pattern_class == PRO_GROUP_PATTERN
            ? leader_feature_id : pattern_feature_id,
        &header);
    if (status == PRO_TK_NO_ERROR)
        status = ProFeaturePatternGet(&header, pattern_class, &pattern);
    if (status == PRO_TK_NO_ERROR)
        status = ProPatternMembersGet(&pattern, &members);
    if (status == PRO_TK_NO_ERROR && members != NULL)
        status = ProArraySizeGet((ProArray)members, &member_count_after);
    if (status != PRO_TK_NO_ERROR || member_count_after != new_count)
    {
        exit_code = write_error(
            out, "pattern_count_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto rollback;
    }
    status = find_dimension_by_symbol(
        (ProSolid)model, resolved_spacing_symbol, &spacing_dimension);
    if (status == PRO_TK_NO_ERROR)
        status = ProDimensionValueGet(&spacing_dimension, &verified_spacing);
    if (status != PRO_TK_NO_ERROR ||
        !nearly_equal(verified_spacing, new_spacing))
    {
        exit_code = write_error(
            out, "spacing_readback",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto rollback;
    }
    do
    {
        top_regen_status = ProSolidRegenerate(
            (ProSolid)top_assembly, PRO_REGEN_NO_FLAGS);
        ++top_regen_attempts;
    } while (top_regen_status == PRO_TK_REGEN_AGAIN &&
             top_regen_attempts < 3);
    if (top_regen_status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "top_regenerate", top_regen_status);
        goto rollback;
    }
    status = ProMdlSave(model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_model", status);
        goto rollback;
    }
    status = ProMdlSave(top_assembly);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_top_assembly", status);
        goto rollback;
    }
    if (ProMdlWindowGet(top_assembly, &window_id) == PRO_TK_NO_ERROR)
    {
        ProWindowCurrentSet(window_id);
        ProWindowActivate(window_id);
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }
    fputs("{\"ok\":true,\"persistent\":true,\"atomic\":true,"
          "\"model\":", out);
    write_wide_json_string(out, model_name);
    fputs(",\"top_assembly\":", out);
    write_wide_json_string(out, top_assembly_name);
    fprintf(out,
        ",\"pattern_feature_id\":%d,\"leader_feature_id\":%d,"
        "\"resolved_header_id\":%d,\"resolved_leader_id\":%d,"
        "\"old_count\":%d,\"new_count\":%d,"
        "\"old_spacing\":%.17g,\"new_spacing\":%.17g,"
        "\"verified_spacing\":%.17g,"
        "\"model_regenerate_status\":%d,"
        "\"top_regenerate_status\":%d,"
        "\"window_id\":%d,\"saved\":true}\n",
        pattern_feature_id, leader_feature_id,
        resolved_header_id, resolved_leader_id,
        old_count, member_count_after, old_spacing, new_spacing,
        verified_spacing, model_regen_status, top_regen_status, window_id);
    exit_code = 0;
    goto done;

rollback:
    if (spacing_changed &&
        find_dimension_by_symbol(
            (ProSolid)model, resolved_spacing_symbol,
            &spacing_dimension) == PRO_TK_NO_ERROR)
        ProDimensionValueSet(&spacing_dimension, old_spacing);
    if (pattern_changed && count_element != NULL)
    {
        ProFeature rollback_header;
        ProErrorlist rollback_errors = {NULL, 0};
        ProElementIntegerSet(count_element, old_count);
        if (ProFeatureInit(
                (ProSolid)model,
                resolved_header_id,
                &rollback_header) == PRO_TK_NO_ERROR)
            ProFeatureWithoptionsRedefine(
                NULL, &rollback_header, tree, options,
                PRO_REGEN_NO_FLAGS, &rollback_errors);
        if (rollback_errors.error_list != NULL)
            ProArrayFree((ProArray *)&rollback_errors.error_list);
    }
    if (model != NULL)
        ProSolidRegenerate((ProSolid)model, PRO_REGEN_NO_FLAGS);
    if (top_assembly != NULL)
        ProSolidRegenerate((ProSolid)top_assembly, PRO_REGEN_NO_FLAGS);

done:
    if (errors.error_list != NULL)
        ProArrayFree((ProArray *)&errors.error_list);
    if (options != NULL)
        ProArrayFree((ProArray *)&options);
    if (members != NULL)
        ProArrayFree((ProArray *)&members);
    if (tree != NULL)
        ProElementFree(&tree);
    fclose(out);
    return exit_code;
}

static void resident_trim_line(wchar_t *line)
{
    size_t length = wcslen(line);
    while (length > 0 &&
           (line[length - 1] == L'\r' || line[length - 1] == L'\n'))
        line[--length] = L'\0';
}

static int resident_command_file(const wchar_t *command_path)
{
    FILE *command = NULL;
    wchar_t line[MAX_COMMAND_LINE];
    wchar_t *arguments[MAX_COMMAND_ARGS] = {0};
    int argument_count = 1;
    int index;
    int result;

    arguments[0] = _wcsdup(L"creo_sheetmetal_flat_wall_bridge.dll");
    if (arguments[0] == NULL)
        return 2;
    if (_wfopen_s(&command, command_path, L"rt, ccs=UTF-8") != 0 ||
        command == NULL)
    {
        free(arguments[0]);
        return 2;
    }
    while (argument_count < MAX_COMMAND_ARGS &&
           fgetws(line, MAX_COMMAND_LINE, command) != NULL)
    {
        resident_trim_line(line);
        arguments[argument_count] = _wcsdup(line);
        if (arguments[argument_count] == NULL)
            break;
        argument_count++;
    }
    fclose(command);
    result = resident_flat_wall_wmain(argument_count, arguments);
    for (index = 0; index < argument_count; index++)
        free(arguments[index]);
    return result;
}

static void resident_set_response(const char *format, int exit_code)
{
    EnterCriticalSection(&resident_lock);
    sprintf_s(
        resident_response, sizeof(resident_response),
        format, exit_code == 0 ? "true" : "false", exit_code);
    InterlockedExchange(&resident_request_pending, 0);
    LeaveCriticalSection(&resident_lock);
    SetEvent(resident_response_event);
}

static void resident_process_request(const char *request)
{
    wchar_t path[2048];
    int exit_code;
    if (strncmp(request, "BASIC|", 6) == 0)
    {
        if (!resident_utf8_to_wide(request + 6, path, 2048))
        {
            resident_set_response(
                "{\"ok\":%s,\"persistent\":true,"
                "\"basic_snapshot\":true,\"exit_code\":%d}\n", 2);
            return;
        }
        exit_code = resident_basic_snapshot(path);
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,"
            "\"basic_snapshot\":true,\"exit_code\":%d}\n", exit_code);
        return;
    }
    if (strncmp(request, "FEATURES|", 9) == 0)
    {
        if (!resident_utf8_to_wide(request + 9, path, 2048))
        {
            resident_set_response(
                "{\"ok\":%s,\"persistent\":true,"
                "\"feature_snapshot\":true,\"exit_code\":%d}\n", 2);
            return;
        }
        exit_code = resident_feature_snapshot(path);
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,"
            "\"feature_snapshot\":true,\"exit_code\":%d}\n", exit_code);
        return;
    }
    if (strncmp(request, "DISPLAY|", 8) == 0)
    {
        char parsed[MAX_REQUEST];
        char *result_utf8;
        char *model_utf8;
        char *expected_utf8;
        wchar_t result_path[2048];
        wchar_t model_path[2048];
        wchar_t expected_name[PRO_NAME_SIZE];
        strcpy_s(parsed, sizeof(parsed), request);
        result_utf8 = parsed + 8;
        model_utf8 = strchr(result_utf8, '|');
        expected_utf8 = NULL;
        if (model_utf8 != NULL)
        {
            *model_utf8++ = '\0';
            expected_utf8 = strchr(model_utf8, '|');
        }
        if (expected_utf8 != NULL)
            *expected_utf8++ = '\0';
        if (model_utf8 == NULL || expected_utf8 == NULL ||
            !resident_utf8_to_wide(result_utf8, result_path, 2048) ||
            !resident_utf8_to_wide(model_utf8, model_path, 2048) ||
            !resident_utf8_to_wide(
                expected_utf8, expected_name, PRO_NAME_SIZE))
        {
            resident_set_response(
                "{\"ok\":%s,\"persistent\":true,"
                "\"display\":true,\"exit_code\":%d}\n", 2);
            return;
        }
        exit_code = resident_display_execute(
            result_path, model_path, expected_name);
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,"
            "\"display\":true,\"exit_code\":%d}\n", exit_code);
        return;
    }
    if (strncmp(request, "DIMBATCH|", 9) == 0)
    {
        char parsed[MAX_REQUEST];
        char *tokens[5 + 4 * MAX_MODIFICATIONS];
        char *context = NULL;
        char *token;
        int token_count = 0;
        int count;
        int index;
        wchar_t output_path[2048];
        wchar_t model_name[PRO_NAME_SIZE];
        wchar_t assembly_name[PRO_NAME_SIZE];
        ResidentDimensionChange changes[MAX_MODIFICATIONS];

        strcpy_s(parsed, sizeof(parsed), request);
        token = strtok_s(parsed, "|", &context);
        while (token != NULL &&
               token_count < (int)(sizeof(tokens) / sizeof(tokens[0])))
        {
            tokens[token_count++] = token;
            token = strtok_s(NULL, "|", &context);
        }
        count = token_count >= 5 ? atoi(tokens[4]) : 0;
        ZeroMemory(changes, sizeof(changes));
        if (token_count < 5 || count < 1 || count > MAX_MODIFICATIONS ||
            token_count != 5 + 4 * count ||
            !resident_utf8_to_wide(tokens[1], output_path, 2048) ||
            !resident_utf8_to_wide(
                tokens[2], model_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(
                tokens[3], assembly_name, PRO_NAME_SIZE))
        {
            resident_set_response(
                "{\"ok\":%s,\"persistent\":true,"
                "\"dimension_batch\":true,\"exit_code\":%d}\n", 2);
            return;
        }
        for (index = 0; index < count; ++index)
        {
            int offset = 5 + 4 * index;
            char *end_expected = NULL;
            char *end_new = NULL;
            long feature_id = strtol(tokens[offset], NULL, 10);
            double expected_value = strtod(
                tokens[offset + 2], &end_expected);
            double new_value = strtod(tokens[offset + 3], &end_new);
            if (feature_id <= 0 || feature_id > INT_MAX ||
                !resident_utf8_to_wide(
                    tokens[offset + 1], changes[index].symbol,
                    PRO_NAME_SIZE) ||
                end_expected == tokens[offset + 2] || *end_expected != '\0' ||
                end_new == tokens[offset + 3] || *end_new != '\0' ||
                !_finite(expected_value) || !_finite(new_value) ||
                expected_value < 0.0 || new_value < 0.0 ||
                expected_value > 1000000.0 || new_value > 1000000.0)
            {
                resident_set_response(
                    "{\"ok\":%s,\"persistent\":true,"
                    "\"dimension_batch\":true,\"exit_code\":%d}\n", 2);
                return;
            }
            changes[index].owner_feature_id = (int)feature_id;
            changes[index].expected_value = expected_value;
            changes[index].new_value = new_value;
        }
        exit_code = resident_dimension_batch_execute(
            output_path, model_name, assembly_name, count, changes, 1);
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,"
            "\"dimension_batch\":true,\"exit_code\":%d}\n", exit_code);
        return;
    }
    if (strncmp(request, "DIMSET|", 7) == 0)
    {
        char parsed[MAX_REQUEST];
        char *tokens[5 + 3 * MAX_MODIFICATIONS];
        char *context = NULL;
        char *token;
        int token_count = 0;
        int count;
        int index;
        wchar_t output_path[2048];
        wchar_t model_name[PRO_NAME_SIZE];
        wchar_t assembly_name[PRO_NAME_SIZE];
        ResidentDimensionChange changes[MAX_MODIFICATIONS];

        strcpy_s(parsed, sizeof(parsed), request);
        token = strtok_s(parsed, "|", &context);
        while (token != NULL &&
               token_count < (int)(sizeof(tokens) / sizeof(tokens[0])))
        {
            tokens[token_count++] = token;
            token = strtok_s(NULL, "|", &context);
        }
        count = token_count >= 5 ? atoi(tokens[4]) : 0;
        ZeroMemory(changes, sizeof(changes));
        if (token_count < 5 || count < 1 || count > MAX_MODIFICATIONS ||
            token_count != 5 + 3 * count ||
            !resident_utf8_to_wide(tokens[1], output_path, 2048) ||
            !resident_utf8_to_wide(
                tokens[2], model_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(
                tokens[3], assembly_name, PRO_NAME_SIZE))
        {
            resident_set_response(
                "{\"ok\":%s,\"persistent\":true,"
                "\"dimension_set\":true,\"exit_code\":%d}\n", 2);
            return;
        }
        for (index = 0; index < count; ++index)
        {
            int offset = 5 + 3 * index;
            char *end_new = NULL;
            long feature_id = strtol(tokens[offset], NULL, 10);
            double new_value = strtod(tokens[offset + 2], &end_new);
            if (feature_id <= 0 || feature_id > INT_MAX ||
                !resident_utf8_to_wide(
                    tokens[offset + 1], changes[index].symbol,
                    PRO_NAME_SIZE) ||
                end_new == tokens[offset + 2] || *end_new != '\0' ||
                !_finite(new_value) || new_value <= 0.0 ||
                new_value > 1000000.0)
            {
                resident_set_response(
                    "{\"ok\":%s,\"persistent\":true,"
                    "\"dimension_set\":true,\"exit_code\":%d}\n", 2);
                return;
            }
            changes[index].owner_feature_id = (int)feature_id;
            changes[index].new_value = new_value;
        }
        exit_code = resident_dimension_batch_execute(
            output_path, model_name, assembly_name, count, changes, 0);
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,"
            "\"dimension_set\":true,\"exit_code\":%d}\n", exit_code);
        return;
    }
    if (strncmp(request, "COMPSWITCH|", 11) == 0)
    {
        char parsed[MAX_REQUEST];
        char *tokens[6];
        char *context = NULL;
        char *token;
        int token_count = 0;
        wchar_t output_path[2048];
        wchar_t assembly_name[PRO_NAME_SIZE];
        wchar_t top_assembly_name[PRO_NAME_SIZE];
        wchar_t suppress_model_name[PRO_NAME_SIZE];
        wchar_t resume_model_name[PRO_NAME_SIZE];

        strcpy_s(parsed, sizeof(parsed), request);
        token = strtok_s(parsed, "|", &context);
        while (token != NULL && token_count < 6)
        {
            tokens[token_count++] = token;
            token = strtok_s(NULL, "|", &context);
        }
        if (token_count != 6 || token != NULL ||
            !resident_utf8_to_wide(tokens[1], output_path, 2048) ||
            !resident_utf8_to_wide(
                tokens[2], assembly_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(
                tokens[3], top_assembly_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(
                tokens[4], suppress_model_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(
                tokens[5], resume_model_name, PRO_NAME_SIZE))
        {
            resident_set_response(
                "{\"ok\":%s,\"persistent\":true,"
                "\"component_switch\":true,\"exit_code\":%d}\n", 2);
            return;
        }
        exit_code = resident_component_switch_execute(
            output_path, assembly_name, top_assembly_name,
            suppress_model_name, resume_model_name);
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,"
            "\"component_switch\":true,\"exit_code\":%d}\n", exit_code);
        return;
    }
    if (strncmp(request, "PATTERNSETID|", 13) == 0)
    {
        char parsed[MAX_REQUEST];
        char *tokens[8];
        char *context = NULL;
        char *token;
        int token_count = 0;
        char *end_pattern_id = NULL;
        char *end_leader_id = NULL;
        char *end_expected_count = NULL;
        char *end_new_count = NULL;
        long pattern_id;
        long leader_id;
        long expected_count;
        long new_count;
        wchar_t output_path[2048];
        wchar_t model_name[PRO_NAME_SIZE];
        wchar_t top_assembly_name[PRO_NAME_SIZE];

        strcpy_s(parsed, sizeof(parsed), request);
        token = strtok_s(parsed, "|", &context);
        while (token != NULL && token_count < 8)
        {
            tokens[token_count++] = token;
            token = strtok_s(NULL, "|", &context);
        }
        pattern_id = token_count == 8
            ? strtol(tokens[4], &end_pattern_id, 10) : 0;
        leader_id = token_count == 8
            ? strtol(tokens[5], &end_leader_id, 10) : 0;
        expected_count = token_count == 8
            ? strtol(tokens[6], &end_expected_count, 10) : 0;
        new_count = token_count == 8
            ? strtol(tokens[7], &end_new_count, 10) : 0;
        if (token_count != 8 || token != NULL ||
            pattern_id <= 0 || pattern_id > INT_MAX ||
            leader_id <= 0 || leader_id > INT_MAX ||
            expected_count < 2 || expected_count > 1000 ||
            new_count < 2 || new_count > 1000 ||
            end_pattern_id == tokens[4] || *end_pattern_id != '\0' ||
            end_leader_id == tokens[5] || *end_leader_id != '\0' ||
            end_expected_count == tokens[6] ||
            *end_expected_count != '\0' ||
            end_new_count == tokens[7] || *end_new_count != '\0' ||
            !resident_utf8_to_wide(tokens[1], output_path, 2048) ||
            !resident_utf8_to_wide(
                tokens[2], model_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(
                tokens[3], top_assembly_name, PRO_NAME_SIZE))
        {
            resident_set_response(
                "{\"ok\":%s,\"persistent\":true,"
                "\"pattern_set_id\":true,\"exit_code\":%d}\n", 2);
            return;
        }
        exit_code = resident_pattern_set_execute(
            output_path, model_name, top_assembly_name,
            (int)pattern_id, L"*", (int)leader_id, L"*", L"*",
            (int)expected_count, (int)new_count, -1.0);
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,"
            "\"pattern_set_id\":true,\"exit_code\":%d}\n", exit_code);
        return;
    }
    if (strncmp(request, "PATTERNSET|", 11) == 0)
    {
        char parsed[MAX_REQUEST];
        char *tokens[11];
        char *context = NULL;
        char *token;
        int token_count = 0;
        char *end_pattern_id = NULL;
        char *end_leader_id = NULL;
        char *end_count = NULL;
        char *end_spacing = NULL;
        long pattern_id;
        long leader_id;
        long new_count;
        double new_spacing;
        wchar_t output_path[2048];
        wchar_t model_name[PRO_NAME_SIZE];
        wchar_t top_assembly_name[PRO_NAME_SIZE];
        wchar_t pattern_name[PRO_NAME_SIZE];
        wchar_t leader_name[PRO_NAME_SIZE];
        wchar_t spacing_symbol[PRO_NAME_SIZE];

        strcpy_s(parsed, sizeof(parsed), request);
        token = strtok_s(parsed, "|", &context);
        while (token != NULL && token_count < 11)
        {
            tokens[token_count++] = token;
            token = strtok_s(NULL, "|", &context);
        }
        pattern_id = token_count == 11
            ? strtol(tokens[4], &end_pattern_id, 10) : 0;
        leader_id = token_count == 11
            ? strtol(tokens[6], &end_leader_id, 10) : 0;
        new_count = token_count == 11
            ? strtol(tokens[9], &end_count, 10) : 0;
        new_spacing = token_count == 11
            ? strtod(tokens[10], &end_spacing) : 0.0;
        if (token_count != 11 || token != NULL ||
            pattern_id <= 0 || pattern_id > INT_MAX ||
            leader_id <= 0 || leader_id > INT_MAX ||
            new_count < 2 || new_count > 1000 ||
            !_finite(new_spacing) || new_spacing <= 0.0 ||
            new_spacing > 1000000.0 ||
            end_pattern_id == tokens[4] || *end_pattern_id != '\0' ||
            end_leader_id == tokens[6] || *end_leader_id != '\0' ||
            end_count == tokens[9] || *end_count != '\0' ||
            end_spacing == tokens[10] || *end_spacing != '\0' ||
            !resident_utf8_to_wide(tokens[1], output_path, 2048) ||
            !resident_utf8_to_wide(tokens[2], model_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(
                tokens[3], top_assembly_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(tokens[5], pattern_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(tokens[7], leader_name, PRO_NAME_SIZE) ||
            !resident_utf8_to_wide(
                tokens[8], spacing_symbol, PRO_NAME_SIZE))
        {
            resident_set_response(
                "{\"ok\":%s,\"persistent\":true,"
                "\"pattern_set\":true,\"exit_code\":%d}\n", 2);
            return;
        }
        exit_code = resident_pattern_set_execute(
            output_path, model_name, top_assembly_name,
            (int)pattern_id, pattern_name,
            (int)leader_id, leader_name, spacing_symbol,
            -1, (int)new_count, new_spacing);
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,"
            "\"pattern_set\":true,\"exit_code\":%d}\n", exit_code);
        return;
    }
    if (!resident_utf8_to_wide(request, path, 2048))
    {
        resident_set_response(
            "{\"ok\":%s,\"persistent\":true,\"exit_code\":%d}\n", 2);
        return;
    }
    exit_code = resident_command_file(path);
    resident_set_response(
        "{\"ok\":%s,\"persistent\":true,\"exit_code\":%d}\n",
        exit_code);
}

static void resident_timer_action(
    char *dialog, ProUITimerID timer_id, ProAppData app_data)
{
    char request[MAX_REQUEST];
    (void)dialog;
    (void)timer_id;
    (void)app_data;
    request[0] = '\0';
    if (InterlockedCompareExchange(&resident_request_pending, 0, 0) != 0)
    {
        EnterCriticalSection(&resident_lock);
        strcpy_s(request, sizeof(request), resident_request);
        LeaveCriticalSection(&resident_lock);
    }
    if (request[0] != '\0')
        resident_process_request(request);
    if (InterlockedCompareExchange(&resident_running, 0, 0) != 0)
        ProUIDialogTimerStart(
            "CreoSafeResidentTimer", resident_timer, 100, PRO_B_FALSE);
}

static DWORD WINAPI resident_pipe_main(LPVOID data)
{
    (void)data;
    while (InterlockedCompareExchange(&resident_running, 0, 0) != 0)
    {
        HANDLE pipe;
        BOOL connected;
        char request[MAX_REQUEST];
        DWORD bytes_read = 0;
        DWORD written = 0;
        DWORD wait_status;

        pipe = CreateNamedPipeW(
            PIPE_NAME, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 4096, 4096, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE)
            break;
        connected = ConnectNamedPipe(pipe, NULL) ? TRUE :
            (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected)
        {
            CloseHandle(pipe);
            continue;
        }
        memset(request, 0, sizeof(request));
        if (!ReadFile(
                pipe, request, (DWORD)sizeof(request) - 1,
                &bytes_read, NULL) || bytes_read == 0)
        {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        request[bytes_read] = '\0';
        while (bytes_read > 0 &&
               (request[bytes_read - 1] == '\r' ||
                request[bytes_read - 1] == '\n'))
            request[--bytes_read] = '\0';
        if (strcmp(request, "PING") == 0)
        {
            const char *ping =
                "{\"ok\":true,\"persistent\":true,\"connected\":true,"
                "\"session_bound\":true,\"transport\":\"dll\","
                "\"health_code\":0}\n";
            WriteFile(pipe, ping, (DWORD)strlen(ping), &written, NULL);
        }
        else
        {
            ResetEvent(resident_response_event);
            EnterCriticalSection(&resident_lock);
            strcpy_s(resident_request, sizeof(resident_request), request);
            resident_response[0] = '\0';
            InterlockedExchange(&resident_request_pending, 1);
            LeaveCriticalSection(&resident_lock);
            wait_status = WaitForSingleObject(resident_response_event, 120000);
            if (wait_status == WAIT_OBJECT_0)
            {
                EnterCriticalSection(&resident_lock);
                WriteFile(
                    pipe, resident_response,
                    (DWORD)strlen(resident_response), &written, NULL);
                LeaveCriticalSection(&resident_lock);
            }
            else
            {
                const char *timeout =
                    "{\"ok\":false,\"persistent\":true,"
                    "\"stage\":\"main_thread_timeout\"}\n";
                WriteFile(
                    pipe, timeout, (DWORD)strlen(timeout), &written, NULL);
            }
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

__declspec(dllexport) int user_initialize(
    int argc, char *argv[], char *version, char *build, wchar_t err_buffer[])
{
    ProName timer_name;
    ProError status;
    (void)argc;
    (void)argv;
    (void)version;
    (void)build;

    resident_startup_log(L"user_initialize_enter", 0);

    InitializeCriticalSection(&resident_lock);
    resident_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    resident_response_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (resident_stop_event == NULL || resident_response_event == NULL)
    {
        wcscpy_s(err_buffer, 80, L"CreoSafeResident event setup failed");
        resident_startup_log(L"event_setup", -1);
        return -1;
    }
    InterlockedExchange(&resident_running, 1);
    InterlockedExchange(&resident_request_pending, 0);
    wcscpy_s(timer_name, sizeof(timer_name) / sizeof(timer_name[0]),
             L"CreoSafeResidentTimer");
    status = ProUITimerCreate(
        resident_timer_action, NULL, timer_name, &resident_timer);
    resident_startup_log(L"timer_create", status);
    if (status != PRO_TK_NO_ERROR)
    {
        swprintf_s(
            err_buffer, 80,
            L"CreoSafeResident timer create failed: %d", status);
        InterlockedExchange(&resident_running, 0);
        return -1;
    }
    resident_pipe_thread = CreateThread(
        NULL, 0, resident_pipe_main, NULL, 0, NULL);
    if (resident_pipe_thread == NULL)
    {
        wcscpy_s(err_buffer, 80, L"CreoSafeResident pipe thread failed");
        resident_startup_log(L"pipe_thread", -1);
        ProUITimerDestroy(resident_timer);
        resident_timer = NULL;
        InterlockedExchange(&resident_running, 0);
        return -1;
    }
    status = ProUIDialogTimerStart(
        "CreoSafeResidentTimer", resident_timer, 100, PRO_B_FALSE);
    resident_startup_log(L"timer_start", status);
    if (status != PRO_TK_NO_ERROR)
    {
        swprintf_s(
            err_buffer, 80,
            L"CreoSafeResident timer start failed: %d", status);
        InterlockedExchange(&resident_running, 0);
        CancelSynchronousIo(resident_pipe_thread);
        return -1;
    }
    resident_startup_log(L"user_initialize_ok", 0);
    return 0;
}

__declspec(dllexport) void user_terminate(void)
{
    resident_startup_log(L"user_terminate", 0);
    InterlockedExchange(&resident_running, 0);
    SetEvent(resident_stop_event);
    if (resident_timer != NULL)
    {
        ProUIDialogTimerStop(resident_timer);
        ProUITimerDestroy(resident_timer);
        resident_timer = NULL;
    }
    if (resident_pipe_thread != NULL)
    {
        CancelSynchronousIo(resident_pipe_thread);
        WaitForSingleObject(resident_pipe_thread, 2000);
        CloseHandle(resident_pipe_thread);
        resident_pipe_thread = NULL;
    }
    if (resident_response_event != NULL)
    {
        CloseHandle(resident_response_event);
        resident_response_event = NULL;
    }
    if (resident_stop_event != NULL)
    {
        CloseHandle(resident_stop_event);
        resident_stop_event = NULL;
    }
    DeleteCriticalSection(&resident_lock);
}
