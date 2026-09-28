#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CODEX_MAX_COMMAND_ARGS 160
#define CODEX_MAX_COMMAND_LINE 4096
#define DECLARE_COMMAND(name) int name(int argc, wchar_t **argv)

DECLARE_COMMAND(codex_cmd_creo_active_sketch_axis_symmetry_bridge);
DECLARE_COMMAND(codex_cmd_creo_active_sketch_four_inset_dimensions_bridge);
DECLARE_COMMAND(codex_cmd_creo_assembly_add_fixed_bridge);
DECLARE_COMMAND(codex_cmd_creo_assembly_components_bridge);
DECLARE_COMMAND(codex_cmd_creo_assembly_create_bridge);
DECLARE_COMMAND(codex_cmd_creo_assembly_mate_align_bridge);
DECLARE_COMMAND(codex_cmd_creo_assembly_repeat_insert_pair_bridge);
DECLARE_COMMAND(codex_cmd_creo_assembly_skeleton_bridge);
DECLARE_COMMAND(codex_cmd_creo_datum_plane_bridge);
DECLARE_COMMAND(codex_cmd_creo_datum_point_bridge);
DECLARE_COMMAND(codex_cmd_creo_dimension_modify_bridge);
DECLARE_COMMAND(codex_cmd_creo_dimension_write_bridge);
DECLARE_COMMAND(codex_cmd_creo_dimensions_bridge);
DECLARE_COMMAND(codex_cmd_creo_display_model_bridge);
DECLARE_COMMAND(codex_cmd_creo_empty_assembly_create_bridge);
DECLARE_COMMAND(codex_cmd_creo_extrude_bridge);
DECLARE_COMMAND(codex_cmd_creo_feature_resume_bridge);
DECLARE_COMMAND(codex_cmd_creo_feature_suppress_bridge);
DECLARE_COMMAND(codex_cmd_creo_features_bridge);
DECLARE_COMMAND(codex_cmd_creo_general_sketch_bridge);
DECLARE_COMMAND(codex_cmd_creo_gas_spring_variant_bridge);
DECLARE_COMMAND(codex_cmd_creo_geometry_inspect_bridge);
DECLARE_COMMAND(codex_cmd_creo_hole_bridge);
DECLARE_COMMAND(codex_cmd_creo_mass_properties_bridge);
DECLARE_COMMAND(codex_cmd_creo_multi_write_bridge);
DECLARE_COMMAND(codex_cmd_creo_model_rename_bridge);
DECLARE_COMMAND(codex_cmd_creo_project_workdir_bridge);
DECLARE_COMMAND(codex_cmd_creo_sheetmetal_plate_bridge);
DECLARE_COMMAND(codex_cmd_creo_sheetmetal_reverse_wall_direction_bridge);
DECLARE_COMMAND(codex_cmd_creo_sheetmetal_skeleton_link_bridge);
DECLARE_COMMAND(codex_cmd_creo_sheetmetal_three_circle_cut_bridge);
DECLARE_COMMAND(codex_cmd_creo_skeleton_box_bridge);
DECLARE_COMMAND(codex_cmd_creo_skeleton_resize_box_bridge);
DECLARE_COMMAND(codex_cmd_creo_skeleton_reverse_direction_bridge);
DECLARE_COMMAND(codex_cmd_creo_template_copy_bridge);
DECLARE_COMMAND(codex_cmd_creo_top_assembly_finalize_bridge);
DECLARE_COMMAND(codex_cmd_creo_verify_copy);
DECLARE_COMMAND(codex_cmd_creo_write_bridge);
int codex_cmd_creo_bridge_ansi(int argc, char **argv);
int codex_cmd_creo_export_bridge_ansi(int argc, char **argv);

static int codex_invoke_ansi(
    int (*command)(int, char **), int argc, wchar_t **wide_arguments)
{
    char **arguments = NULL;
    int index;
    int result = 2;
    arguments = (char **)calloc((size_t)argc, sizeof(char *));
    if (arguments == NULL)
        return 2;
    for (index = 0; index < argc; ++index)
    {
        int size = WideCharToMultiByte(
            CP_UTF8, 0, wide_arguments[index], -1, NULL, 0, NULL, NULL);
        if (size < 1)
            goto done;
        arguments[index] = (char *)malloc((size_t)size);
        if (arguments[index] == NULL ||
            WideCharToMultiByte(
                CP_UTF8, 0, wide_arguments[index], -1,
                arguments[index], size, NULL, NULL) < 1)
            goto done;
    }
    result = command(argc, arguments);
done:
    for (index = 0; index < argc; ++index)
        free(arguments[index]);
    free(arguments);
    return result;
}

static int codex_cmd_creo_bridge(int argc, wchar_t **argv)
{
    return codex_invoke_ansi(codex_cmd_creo_bridge_ansi, argc, argv);
}

static int codex_cmd_creo_export_bridge(int argc, wchar_t **argv)
{
    return codex_invoke_ansi(codex_cmd_creo_export_bridge_ansi, argc, argv);
}

typedef int (*CodexCommandMain)(int argc, wchar_t **argv);
typedef struct codex_command_entry
{
    const wchar_t *executable;
    CodexCommandMain command;
} CodexCommandEntry;

static const CodexCommandEntry codex_commands[] = {
    {L"creo_active_sketch_axis_symmetry_bridge.exe", codex_cmd_creo_active_sketch_axis_symmetry_bridge},
    {L"creo_active_sketch_four_inset_dimensions_bridge.exe", codex_cmd_creo_active_sketch_four_inset_dimensions_bridge},
    {L"creo_assembly_add_fixed_bridge.exe", codex_cmd_creo_assembly_add_fixed_bridge},
    {L"creo_assembly_components_bridge.exe", codex_cmd_creo_assembly_components_bridge},
    {L"creo_assembly_create_bridge.exe", codex_cmd_creo_assembly_create_bridge},
    {L"creo_assembly_mate_align_bridge.exe", codex_cmd_creo_assembly_mate_align_bridge},
    {L"creo_assembly_repeat_insert_pair_bridge.exe", codex_cmd_creo_assembly_repeat_insert_pair_bridge},
    {L"creo_assembly_skeleton_bridge.exe", codex_cmd_creo_assembly_skeleton_bridge},
    {L"creo_bridge.exe", codex_cmd_creo_bridge},
    {L"creo_datum_plane_bridge.exe", codex_cmd_creo_datum_plane_bridge},
    {L"creo_datum_point_bridge.exe", codex_cmd_creo_datum_point_bridge},
    {L"creo_dimension_modify_bridge.exe", codex_cmd_creo_dimension_modify_bridge},
    {L"creo_dimension_write_bridge.exe", codex_cmd_creo_dimension_write_bridge},
    {L"creo_dimensions_bridge.exe", codex_cmd_creo_dimensions_bridge},
    {L"creo_display_model_bridge.exe", codex_cmd_creo_display_model_bridge},
    {L"creo_empty_assembly_create_bridge.exe", codex_cmd_creo_empty_assembly_create_bridge},
    {L"creo_export_bridge.exe", codex_cmd_creo_export_bridge},
    {L"creo_extrude_bridge.exe", codex_cmd_creo_extrude_bridge},
    {L"creo_feature_resume_bridge.exe", codex_cmd_creo_feature_resume_bridge},
    {L"creo_feature_suppress_bridge.exe", codex_cmd_creo_feature_suppress_bridge},
    {L"creo_features_bridge.exe", codex_cmd_creo_features_bridge},
    {L"creo_general_sketch_bridge.exe", codex_cmd_creo_general_sketch_bridge},
    {L"creo_gas_spring_variant_bridge.exe", codex_cmd_creo_gas_spring_variant_bridge},
    {L"creo_geometry_inspect_bridge.exe", codex_cmd_creo_geometry_inspect_bridge},
    {L"creo_hole_bridge.exe", codex_cmd_creo_hole_bridge},
    {L"creo_mass_properties_bridge.exe", codex_cmd_creo_mass_properties_bridge},
    {L"creo_multi_write_bridge.exe", codex_cmd_creo_multi_write_bridge},
    {L"creo_model_rename_bridge.exe", codex_cmd_creo_model_rename_bridge},
    {L"creo_project_workdir_bridge.exe", codex_cmd_creo_project_workdir_bridge},
    {L"creo_sheetmetal_plate_bridge.exe", codex_cmd_creo_sheetmetal_plate_bridge},
    {L"creo_sheetmetal_reverse_wall_direction_bridge.exe", codex_cmd_creo_sheetmetal_reverse_wall_direction_bridge},
    {L"creo_sheetmetal_skeleton_link_bridge.exe", codex_cmd_creo_sheetmetal_skeleton_link_bridge},
    {L"creo_sheetmetal_three_circle_cut_bridge.exe", codex_cmd_creo_sheetmetal_three_circle_cut_bridge},
    {L"creo_skeleton_box_bridge.exe", codex_cmd_creo_skeleton_box_bridge},
    {L"creo_skeleton_resize_box_bridge.exe", codex_cmd_creo_skeleton_resize_box_bridge},
    {L"creo_skeleton_reverse_direction_bridge.exe", codex_cmd_creo_skeleton_reverse_direction_bridge},
    {L"creo_template_copy_bridge.exe", codex_cmd_creo_template_copy_bridge},
    {L"creo_top_assembly_finalize_bridge.exe", codex_cmd_creo_top_assembly_finalize_bridge},
    {L"creo_verify_copy.exe", codex_cmd_creo_verify_copy},
    {L"creo_write_bridge.exe", codex_cmd_creo_write_bridge}
};

static void codex_trim_line(wchar_t *line)
{
    size_t length = line == NULL ? 0 : wcslen(line);
    while (length > 0 && (line[length - 1] == L'\r' || line[length - 1] == L'\n'))
        line[--length] = L'\0';
}

int codex_internal_command_file_execute(const wchar_t *command_path)
{
    FILE *command_file = NULL;
    wchar_t line[CODEX_MAX_COMMAND_LINE];
    wchar_t *arguments[CODEX_MAX_COMMAND_ARGS];
    int argument_count = 0;
    int result = 2;
    int index;
    size_t command_index;

    memset(arguments, 0, sizeof(arguments));
    if (_wfopen_s(&command_file, command_path, L"rt, ccs=UTF-8") != 0 ||
        command_file == NULL)
        return 2;
    while (argument_count < CODEX_MAX_COMMAND_ARGS &&
           fgetws(line, CODEX_MAX_COMMAND_LINE, command_file) != NULL)
    {
        codex_trim_line(line);
        arguments[argument_count] = _wcsdup(line);
        if (arguments[argument_count] == NULL)
            break;
        ++argument_count;
    }
    fclose(command_file);
    if (argument_count < 2)
        goto done;
    for (command_index = 0;
         command_index < sizeof(codex_commands) / sizeof(codex_commands[0]);
         ++command_index)
    {
        if (_wcsicmp(arguments[0], codex_commands[command_index].executable) == 0)
        {
            result = codex_commands[command_index].command(argument_count, arguments);
            goto done;
        }
    }
    result = 3;
done:
    for (index = 0; index < argument_count; ++index)
        free(arguments[index]);
    return result;
}
