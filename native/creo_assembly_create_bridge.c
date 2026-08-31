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
#include <ProAsmcomp.h>
#include <ProArray.h>
#include <ProWindows.h>

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

static int output_assembly_exists(const wchar_t *directory, const wchar_t *name)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    _snwprintf_s(
        pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.asm*",
        directory,
        name);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(handle);
    return 1;
}

static int find_latest_saved_assembly(
    const wchar_t *directory,
    const wchar_t *name,
    wchar_t *saved_path,
    size_t saved_path_count)
{
    wchar_t pattern[PRO_PATH_SIZE * 2];
    WIN32_FIND_DATAW data;
    HANDLE handle;
    int found = 0;
    int best_version = -1;
    _snwprintf_s(
        pattern,
        sizeof(pattern) / sizeof(pattern[0]),
        _TRUNCATE,
        L"%ls\\%ls.asm*",
        directory,
        name);
    handle = FindFirstFileW(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        wchar_t *dot = wcsrchr(data.cFileName, L'.');
        int version = dot == NULL ? 0 : _wtoi(dot + 1);
        if (!found || version > best_version)
        {
            _snwprintf_s(
                saved_path,
                saved_path_count,
                _TRUNCATE,
                L"%ls\\%ls",
                directory,
                data.cFileName);
            best_version = version;
            found = 1;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return found;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProError status;
    ProError disconnect_status;
    ProBoolean random_choice = PRO_B_FALSE;
    ProProcessHandle process_handle;
    ProMdl template_model = NULL;
    ProMdl assembly_model = NULL;
    ProMdl component_model = NULL;
    ProMdl current_model = NULL;
    ProMdlName assembly_name;
    ProMdlName expected_component_name;
    ProMdlName actual_component_name;
    ProMdlName current_name;
    ProPath template_path;
    ProPath output_directory;
    ProPath component_path;
    ProPath saved_path;
    ProAsmcomp component_feature;
    ProMatrix identity_matrix = {
        {1.0, 0.0, 0.0, 0.0},
        {0.0, 1.0, 0.0, 0.0},
        {0.0, 0.0, 1.0, 0.0},
        {0.0, 0.0, 0.0, 1.0}
    };
    ProAsmcompconstraint constraint = NULL;
    ProAsmcompconstraint *constraints = NULL;
    ProAsmcompconstraint *readback_constraints = NULL;
    ProAsmcompConstrType readback_type = PRO_ASM_UNDEF;
    ProFeatStatus component_status = PRO_FEAT_INVALID;
    int constraint_added = 0;
    int constraint_count = 0;
    int connected = 0;
    int window_id = -1;
    int regenerate_attempts = 0;
    ProError regenerate_status = PRO_TK_GENERAL_ERROR;
    int exit_code = 1;

    if (argc != 7)
    {
        fwprintf(stderr,
            L"Usage: creo_assembly_create_bridge <result.json> <template.asm> "
            L"<assembly_name> <output_dir> <component.prt> <expected_component_name>\n");
        return 2;
    }
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL)
        return 2;
    wcsncpy_s(template_path,
        sizeof(template_path) / sizeof(template_path[0]), argv[2], _TRUNCATE);
    wcsncpy_s(assembly_name,
        sizeof(assembly_name) / sizeof(assembly_name[0]), argv[3], _TRUNCATE);
    wcsncpy_s(output_directory,
        sizeof(output_directory) / sizeof(output_directory[0]), argv[4], _TRUNCATE);
    wcsncpy_s(component_path,
        sizeof(component_path) / sizeof(component_path[0]), argv[5], _TRUNCATE);
    wcsncpy_s(expected_component_name,
        sizeof(expected_component_name) / sizeof(expected_component_name[0]),
        argv[6], _TRUNCATE);

    if (GetFileAttributesW(template_path) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "assembly_template", PRO_TK_E_NOT_FOUND);
        goto done;
    }
    if (GetFileAttributesW(output_directory) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "output_directory", PRO_TK_INVALID_DIR);
        goto done;
    }
    if (GetFileAttributesW(component_path) == INVALID_FILE_ATTRIBUTES)
    {
        exit_code = write_error(out, "component_file", PRO_TK_E_NOT_FOUND);
        goto done;
    }
    if (output_assembly_exists(output_directory, assembly_name))
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

    status = ProDirectoryChange(output_directory);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "set_project_directory", status);
        goto cleanup;
    }
    status = ProMdlFiletypeLoad(
        template_path, PRO_MDLFILE_ASSEMBLY, PRO_B_FALSE, &template_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "load_assembly_template", status);
        goto cleanup;
    }
    status = ProMdlFiletypeLoad(
        component_path, PRO_MDLFILE_PART, PRO_B_FALSE, &component_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "load_component", status);
        goto cleanup;
    }
    status = ProMdlNameGet(component_model, actual_component_name);
    if (status != PRO_TK_NO_ERROR ||
        _wcsicmp(actual_component_name, expected_component_name) != 0)
    {
        exit_code = write_error(out, "component_name_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_BAD_CONTEXT : status);
        goto cleanup;
    }

    status = ProMdlnameCopy(template_model, assembly_name, &assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "copy_assembly_template", status);
        goto cleanup;
    }
    status = ProAsmcompAssemble(
        (ProAssembly)assembly_model,
        (ProSolid)component_model,
        identity_matrix,
        &component_feature);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "assemble_component", status);
        goto cleanup;
    }

    status = ProArrayAlloc(
        0,
        sizeof(ProAsmcompconstraint),
        1,
        (ProArray *)&constraints);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "constraint_array_alloc", status);
        goto cleanup;
    }
    status = ProAsmcompconstraintAlloc(&constraint);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "constraint_alloc", status);
        goto cleanup;
    }
    status = ProAsmcompconstraintTypeSet(constraint, PRO_ASM_DEF_PLACEMENT);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "default_constraint_type", status);
        goto cleanup;
    }
    status = ProArrayObjectAdd(
        (ProArray *)&constraints,
        PRO_VALUE_UNUSED,
        1,
        &constraint);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "constraint_add", status);
        goto cleanup;
    }
    constraint_added = 1;
    constraint = NULL;
    status = ProAsmcompConstraintsSet(NULL, &component_feature, constraints);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "set_default_constraint", status);
        goto cleanup;
    }

    do
    {
        regenerate_status = ProSolidRegenerate(
            (ProSolid)assembly_model, PRO_REGEN_NO_FLAGS);
        ++regenerate_attempts;
    } while (regenerate_status == PRO_TK_REGEN_AGAIN && regenerate_attempts < 3);
    if (regenerate_status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "regenerate_assembly", regenerate_status);
        goto cleanup;
    }
    status = ProFeatureStatusGet((ProFeature *)&component_feature, &component_status);
    if (status != PRO_TK_NO_ERROR || component_status != PRO_FEAT_ACTIVE)
    {
        exit_code = write_error(out, "component_status",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProAsmcompConstraintsWithComppathGet(
        &component_feature, NULL, &readback_constraints);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "constraint_readback", status);
        goto cleanup;
    }
    status = ProArraySizeGet((ProArray)readback_constraints, &constraint_count);
    if (status != PRO_TK_NO_ERROR || constraint_count != 1)
    {
        exit_code = write_error(out, "constraint_count_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }
    status = ProAsmcompconstraintTypeGet(readback_constraints[0], &readback_type);
    if (status != PRO_TK_NO_ERROR || readback_type != PRO_ASM_DEF_PLACEMENT)
    {
        exit_code = write_error(out, "default_constraint_guard",
            status == PRO_TK_NO_ERROR ? PRO_TK_GENERAL_ERROR : status);
        goto cleanup;
    }

    status = ProMdlSave(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "save_assembly", status);
        goto cleanup;
    }
    if (!find_latest_saved_assembly(
            output_directory,
            assembly_name,
            saved_path,
            sizeof(saved_path) / sizeof(saved_path[0])))
    {
        exit_code = write_error(out, "verify_saved_assembly", PRO_TK_E_NOT_FOUND);
        goto cleanup;
    }

    status = ProMdlDisplay(assembly_model);
    if (status != PRO_TK_NO_ERROR)
    {
        exit_code = write_error(out, "display_assembly", status);
        goto cleanup;
    }
    status = ProMdlCurrentGet(&current_model);
    if (status == PRO_TK_NO_ERROR)
        status = ProMdlNameGet(current_model, current_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(current_name, assembly_name) != 0)
    {
        status = ProObjectwindowMdlnameCreate(assembly_name, PRO_ASSEMBLY, &window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "create_assembly_window", status);
            goto cleanup;
        }
        status = ProWindowCurrentSet(window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "set_assembly_window", status);
            goto cleanup;
        }
        status = ProWindowActivate(window_id);
        if (status != PRO_TK_NO_ERROR)
        {
            exit_code = write_error(out, "activate_assembly_window", status);
            goto cleanup;
        }
    }
    if (window_id < 0 && ProWindowCurrentGet(&window_id) == PRO_TK_NO_ERROR)
        ProWindowActivate(window_id);
    if (window_id >= 0)
    {
        ProWindowRefit(window_id);
        ProWindowRepaint(window_id);
    }

    fputs("{\"ok\":true,\"api_only\":true,\"assembly\":", out);
    write_wide_json_string(out, assembly_name);
    fputs(",\"component\":", out);
    write_wide_json_string(out, actual_component_name);
    fprintf(out,
        ",\"component_feature_id\":%d,\"component_status\":%d,"
        "\"constraint_count\":%d,\"constraint_type\":%d,"
        "\"default_placement\":true,\"regenerate_status\":%d,"
        "\"regenerate_attempts\":%d,\"saved_file\":",
        component_feature.id,
        component_status,
        constraint_count,
        readback_type,
        regenerate_status,
        regenerate_attempts);
    write_wide_json_string(out, saved_path);
    fprintf(out, ",\"window_id\":%d}\n", window_id);
    exit_code = 0;

cleanup:
    if (readback_constraints != NULL)
        ProAsmcompconstraintArrayFree(readback_constraints);
    if (constraints != NULL)
        ProAsmcompconstraintArrayFree(constraints);
    else if (constraint != NULL && !constraint_added)
        ProAsmcompconstraintFree(constraint);
    if (template_model != NULL && template_model != assembly_model)
        ProMdlErase(template_model);
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
