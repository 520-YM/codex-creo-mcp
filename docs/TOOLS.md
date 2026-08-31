# MCP 工具清单

正式工具数量：**60**。

所有工具均不得接收 `project_name` 或固定项目目录。涉及模型文件时，目录来源必须是当前 Creo 会话。

## 1. `creo_get_current_model`

读取当前运行中的 Creo 及当前模型信息；不会修改、保存或显示模型。

## 2. `creo_start_resident_and_get_basic_model`

Creo 每次新会话的首次命令应先调用：启动或复用与当前 Creo 会话绑定的常驻桥接，并快速只读获取工作目录、当前模型名称、类型和外形尺寸；不遍历特征、尺寸、参数或装配组件。关闭该次 Creo 后桥接会自动退出。

## 3. `creo_verify_saved_copy`

只读载入安全输出目录中的 Creo 零件副本，核对字符串参数，然后从内存卸载。

## 4. `creo_create_safe_parameter_copy`

把当前 Creo 零件复制为新文件，只在副本中写入允许的参数并重生成；绝不覆盖已有文件，也不保存源模型。

## 5. `creo_export_step`

从安全输出目录中的 Creo 零件副本导出新的 STEP 文件；拒绝覆盖同名输出。

## 6. `creo_create_safe_multi_parameter_copy`

把当前 Creo 零件复制为新文件，在同一个副本中批量写入 1-10 个业务白名单参数，然后统一重生成；拒绝覆盖且绝不保存源模型。

## 7. `creo_get_dimensions`

只读列出当前 Creo 零件或安全输出目录中某个零件副本的标准尺寸，包括尺寸 ID、符号、数值、类型、关系驱动状态和所属特征。

## 8. `creo_get_project_dimensions`

Read-only list of standard dimensions for one Creo part stored under the live working directory already selected in the current Creo session, including dimension IDs, symbols, values, relation-driven state, and owning features.

## 9. `creo_create_safe_dimension_copy`

按尺寸符号和预期旧值双重校验当前 Creo 零件，只在新副本中修改一个非关系驱动的正尺寸并重生成；拒绝覆盖，不保存源模型。新值限制在旧值的 0.5-2 倍。

## 10. `creo_get_features`

只读列出当前 Creo 零件或安全输出目录中某个零件副本的特征，包括 ID、名称、类型、状态、可见性及父子依赖。

## 11. `creo_create_safe_feature_suppression_copy`

按模型名、特征 ID 和类型多重校验，只在新副本中抑制一个活动、可见、无子特征的圆角、倒角、孔或切除特征；拒绝覆盖且不保存源模型。

## 12. `creo_create_safe_feature_resume_copy`

从安全输出目录中的已抑制零件副本创建一个新副本，并仅在新副本中恢复一个无子特征、父特征均活动的圆角、倒角、孔或切除特征；拒绝覆盖且不修改输入副本或当前模型。

## 13. `creo_create_safe_datum_point_copy`

复制当前 Creo 零件，并仅在新副本中相对于指定坐标系创建一个笛卡尔偏移基准点；坐标限制在模型单位的 -1000 到 1000，拒绝覆盖且不保存源模型。

## 14. `creo_get_mass_properties`

Read computed mass properties for the current Creo part or a part copy in the safe output directory. Uses unit density for calculation only and does not modify the model.

## 15. `creo_create_safe_thru_hole_copy`

Copy the current Creo part and create one regular straight thru-all hole only in the new copy. Placement uses one named datum plane and two named offset datum planes. The operation refuses overwrite and succeeds only when the copy volume is reduced.

## 16. `creo_create_safe_advanced_hole_copy`

Copy the current Creo part and create one advanced hole only in the new copy. Enabled styles are regular straight blind, custom counterbore, custom countersink, and standard threaded holes. Threaded-hole drill diameter is read from the installed Creo .hol table and the saved thread metadata is verified. The operation refuses overwrite, requires an active hole feature, requires volume to decrease, and applies a style-specific maximum-removal guard.

## 17. `creo_create_safe_offset_datum_plane_copy`

Copy the current Creo part and create one datum plane at a nonzero offset from a named planar datum reference only in the new copy. The operation refuses overwrite and succeeds only when solid volume remains unchanged.

## 18. `creo_display_safe_model`

Load a Creo part from the safe output directory, display it in the active Creo window, refit the view, and bring Creo to the foreground. This changes only the current UI display and does not save or modify model data.

## 19. `creo_create_safe_rectangle_extrusion_copy`

Copy the current Creo part and create one solid blind protrusion from a centered rectangular sketch only in the new copy. The operation refuses overwrite and succeeds only when the active feature is saved and solid volume increases.

## 20. `creo_create_safe_rectangle_cut_copy`

Copy the current Creo part and create one solid blind cut from a centered rectangular sketch only in the new copy. The operation refuses overwrite, requires an active cut feature, requires solid volume to decrease, and rejects removal greater than the requested rectangular prism.

## 21. `creo_create_safe_revolve_copy`

Copy the current Creo part and create one full 360-degree solid revolve from a closed rectangular radial section in the new copy. Supports additive revolve and revolve cut, refuses overwrite, requires an active feature, and verifies the corresponding solid-volume change.

## 22. `creo_create_safe_round_copy`

Copy the current Creo part and create one constant-radius edge round in the new copy. An explicit edge ID may be supplied; otherwise the longest supported straight or circular edge is selected. The tool refuses overwrite and verifies an active round feature, a feature-count increase, and a nonzero solid-volume change.

## 23. `creo_create_safe_chamfer_copy`

Copy the current Creo part and create one equal-distance edge chamfer in the new copy. An explicit edge ID may be supplied; otherwise the longest supported straight or circular edge is selected. The tool refuses overwrite and verifies an active chamfer feature, a feature-count increase, and a nonzero solid-volume change.

## 24. `creo_create_safe_shell_copy`

Copy the current Creo part and create one inward shell in the new copy. A planar surface ID may be supplied as the opening; otherwise the largest planar surface is selected. The tool refuses overwrite and verifies an active shell feature, a feature-count increase, and a decrease in solid volume.

## 25. `creo_create_safe_draft_copy`

Copy the current Creo part and create one constant-angle draft feature in the new copy using a datum plane as the hinge and direction reference. A surface ID may be supplied; otherwise the largest cylindrical surface is selected. The tool refuses overwrite and verifies an active draft feature, a feature-count increase, and a nonzero solid-volume change.

## 26. `creo_create_safe_part_mirror_copy`

Copy the current Creo part and create a whole-part mirror feature in the new copy about a named datum plane. The source model is never saved, overwrite is refused, and the result is verified for an active mirror feature, a feature-count increase, and a nonzero solid-geometry change.

## 27. `creo_set_project_working_directory`

Compatibility tool that reads and verifies the live working directory already selected in the current Creo session. It never creates, selects, or changes a folder and accepts no directory argument.

## 28. `creo_create_project_part`

Create a new millimeter solid Creo part from the installed mmns template inside the live working directory already selected in the current Creo session, refuse overwrite, keep that folder as the live Creo working directory, save the model, and display it.

## 29. `creo_create_project_sheetmetal_part`

Create a new empty native millimeter Creo sheet-metal part from the installed sheet-metal template inside the live working directory already selected in the current Creo session, verify the returned sheet-metal model subtype, keep that live working directory active, save and display the part, and recycle older versions of only that part family.

## 30. `creo_create_project_sheetmetal_planar_wall`

Create a new native millimeter Creo sheet-metal part inside the live working directory already selected in the current Creo session and create its first unattached planar wall from a centered closed rectangle on FRONT. The tool refuses overwrite; verifies sheet-metal subtype, active flat-surface feature type, thickness, volume, and 3D envelope; saves and displays the part; then moves older versions of only that part family to the Windows Recycle Bin.

## 31. `creo_create_project_sheetmetal_planar_wall_current`

Create the first unattached planar wall in the current expected empty native Creo sheet-metal project part on FRONT. The tool verifies subtype, feature status, thickness, volume and envelope, saves and displays the part, and recycles older versions of only that part family. This variant operates on an existing project sheet-metal part.

## 32. `creo_link_project_sheetmetal_wall_to_skeleton`

In a verified top-level Creo assembly context with its sheet-metal component active, create two assembly relations that drive the planar-wall sketch length and width from the standard skeleton dimensions. The tool preserves existing assembly relations, rejects conflicting targets, temporarily changes and restores the skeleton dimensions to prove that both the sheet dimensions and solid envelope update associatively, saves all three models only after verification, leaves the sheet-metal part active, and recycles older versions of only those model families.

## 33. `creo_reverse_project_sheetmetal_planar_wall_to_positive_z`

Reverse the verified first planar-wall thickness direction of the active native Creo sheet-metal project part on FRONT from -Z to +Z. The tool guards the exact wall feature and its thickness/length/width dimensions, preserves assembly-driven length and width, verifies the solid envelope changes from Z=-thickness..0 to Z=0..+thickness with unchanged volume, regenerates the named top assembly, saves both models only after verification, leaves the sheet-metal part active, and recycles older versions of only those two model families.

## 34. `creo_create_project_sheetmetal_flat_wall`

Create one attached flat wall on one verified straight boundary edge of the current expected native Creo sheet-metal project part. The tool calculates the inside bend radius as sheet thickness x 0.5, creates a 90-degree wall using the requested height, verifies the attachment edge ID and length, wall type, active feature, radius rule, regeneration, solid-volume increase and saved file, then moves older versions of only that model family to the Windows Recycle Bin. Any failure before save deletes the partial feature.

## 35. `creo_create_project_sheetmetal_flat_walls_batch`

Create one native Creo sheet-metal flat-wall feature on exactly two verified straight boundary edges in one Pro/TOOLKIT connection and one transaction. Both walls share the requested angle and height; the inside bend radius is sheet thickness x 0.5. The tool changes to the guarded project directory inside the same bridge process, verifies both attachment edge IDs and lengths, feature type, radius rule, regeneration and volume increase, saves once, and then recycles older versions of only the target part family. Any failure before save deletes the complete batch feature.

## 36. `creo_create_project_sheetmetal_three_circle_cut`

Create one through-all extruded cut containing exactly three equal circles on one guarded planar surface of the current expected native Creo sheet-metal project part. The circles are centered on one sketch axis at -spacing, 0, and +spacing from the requested center. The tool verifies surface ID, area and owning feature, layout margins, active cut feature, total cylindrical cut area representing all three holes, exact sheet-thickness removal volume, regeneration and saved file, then moves older versions of only that model family to the Windows Recycle Bin. Any failure before save deletes the partial feature.

## 37. `creo_create_project_extrusion_copy`

Copy the current Creo part into the live working directory already selected in the current Creo session and create one centered rectangle or circle extrusion. Supports additive protrusion and blind cut, refuses overwrite, and verifies the active feature and solid-volume change.

## 38. `creo_get_project_geometry`

Read edge IDs, edge types, lengths, representative coordinates, surface IDs, surface types, areas, and axis-aligned extents from one Creo part inside the current Creo working directory. The model is not saved or modified.

## 39. `creo_display_project_model`

Read the live working directory already selected in the current Creo session, load one direct-child Creo part or assembly from that directory, verify its expected model name and file type, activate its existing window or create a dedicated new window, refit the view, and bring Creo to the foreground without closing, replacing, saving, or modifying any other open model window. No configured or caller-supplied project folder is used.

## 40. `creo_create_project_empty_assembly`

Create a new empty millimeter Creo assembly inside the live working directory already selected in the current Creo session from the installed assembly template, verify the assembly name and type, regenerate, save and display it, and recycle older versions of only that assembly family. No component is added.

## 41. `creo_create_project_assembly`

Create a new millimeter Creo assembly inside the live working directory already selected in the current Creo session from the installed assembly template, add one existing project part with default placement, verify the active component and default-placement constraint, save and display the assembly, then recycle older versions of that assembly family.

## 42. `creo_add_project_component_fixed`

Add an existing project part to the current expected Creo assembly at a verified XYZ translation, apply a fixed placement constraint, regenerate and save the assembly, and recycle older versions of only that assembly family.

## 43. `creo_add_project_component_mate_align`

Add an existing project part to the current expected Creo assembly with one to three named planar constraints. Each constraint is either mate (opposed plane normals) or align (same-direction plane normals), with explicit red/yellow datum sides. The tool rejects duplicate plane use, verifies every reference and constraint by API readback, reports packaged and underconstrained state, optionally requires full constraint, regenerates and saves the assembly, and recycles older versions of only that assembly family. A failure before save deletes the partially added component.

## 44. `creo_get_project_assembly_components`

Read the active component instances of the current expected Creo assembly, including feature IDs, model names, placement matrices, and constraint types and references. The tool is read-only and uses the Pro/TOOLKIT API.

## 45. `creo_repeat_project_component_insert_pair`

Read one existing component instance in the current expected assembly, require its exact ALIGN plus INSERT surface-constraint method, then add two more instances using the same component references, datum sides, orientation, and assembly align surface while changing only the two cylindrical assembly surfaces. The tool verifies each new constraint and X position, regenerates and saves the assembly, and recycles older versions of only that assembly family.

## 46. `creo_add_project_component_align_insert_three`

Load one project part and add exactly three instances to cylindrical surfaces of a host component in the current expected assembly. Every instance uses a verified planar ALIGN constraint plus cylindrical INSERT constraint with yellow datum sides, is checked by API readback, and the assembly is regenerated, saved, and cleaned to its newest version. The tool refuses to run when an instance of the expected component already exists.

## 47. `creo_create_project_standard_skeleton`

Create a standard skeleton model in the current expected project assembly from the millimeter part template, verify it with ProMdlIsSkeleton and assembly readback, save both models, and recycle older versions of only those two model families.

## 48. `creo_create_project_skeleton_box`

Create one solid rectangular extrusion inside the expected standard skeleton on a named datum plane, verify feature status, volume and the requested three envelope dimensions, save and display the skeleton, save its assembly, and recycle older versions of those model families.

## 49. `creo_create_project_skeleton_surface_inset_extrusion`

In the expected standard skeleton, validate one planar top-face surface by ID and full-face area, create a centered additive rectangular extrusion whose four sides are inset equally from the existing XY outline, verify the exact volume increase and +Z envelope increase, save the skeleton and top assembly, display the skeleton, and recycle older versions of only those two model families.

## 50. `creo_create_project_skeleton_surface_outset_extrusion`

In the expected standard skeleton, validate one centered planar top-face surface by ID and its exact X/Y extents, create a centered additive rectangular extrusion whose four sides extend outward equally from that surface, verify the exact volume increase and +Z envelope, save the skeleton and top assembly, display the skeleton, and recycle older versions of only those two model families.

## 51. `creo_reverse_project_skeleton_box_direction`

Reverse one verified rectangular skeleton extrusion to the requested datum-plane side by redefining its Pro/TOOLKIT feature element tree. The tool checks the existing feature, direction readback, exact volume and signed XYZ outline, regenerates and saves the skeleton and top assembly, displays the skeleton, and recycles older versions of only those model families.

## 52. `creo_set_current_hinge_pattern`

通过当前 Creo 会话的常驻 API 修改五个零总装配骨架中的“铰链阵列”。工具唯一核对阵列特征 ID 8242、名称、组阵列入口 ID 8241、间距尺寸 d229、关系驱动状态及当前成员数量，再原子修改数量和间距，读回验证、重新生成并保存骨架和总装配、返回总装配，并把这两个模型族的旧版本移入回收站。

## 53. `creo_switch_current_project_component_visibility`

在当前五个零总装配范围内，通过常驻 Creo API 将指定子装配中的一个活动组件隐含，同时将另一个普通隐含组件恢复。工具按组件模型名唯一定位并核对原状态，原子执行、读回验证、重新生成和保存子装配及总装配、返回总装配，并把这两个装配模型族的旧版本移入回收站。

## 54. `creo_set_current_top_skeleton_backframe_size`

通过当前 Creo 会话的常驻桥接，把当前五个零总装配对应骨架中“后框”（特征 ID 40）的 d3、d2 直接设置为目标长宽。工具在写入前读取真实旧值并核对特征归属及关系驱动状态，随后原子修改、重生成骨架和总装配、保存、读回验证、返回总装配，并把这两个模型族的旧版本移入回收站。

## 55. `creo_resize_project_skeleton_box`

Change the length and width dimensions of one verified skeleton box feature while preserving its checked height. Dimension symbols and expected old values guard against editing the wrong geometry. The result is regenerated, checked by volume and envelope, saved and displayed, and older assembly/skeleton versions are recycled.

## 56. `creo_modify_project_feature_dimensions`

Modify one to eight driving dimensions of one named feature in the current expected Creo project part. Every change is guarded by dimension symbol, expected old value, feature ownership and relation status. The part is regenerated, all new values are read back, the active feature is verified, the model is saved, and older versions of only that model family are moved to the Windows Recycle Bin. Any failure before save restores every changed dimension.

## 57. `creo_constrain_active_rectangle_symmetric_to_axes`

While the expected Creo part or skeleton is actively editing a rectangular sketch, identify exactly two vertical and two horizontal rectangle lines by guarded dimensions, create X=0 and Y=0 construction centerlines when missing, add left/right symmetry about Y and bottom/top symmetry about X, verify both constraints by API readback, and update the active sketch without accepting, exiting, regenerating, or saving it.

## 58. `creo_dimension_active_rectangle_four_side_insets`

While the expected Creo model is actively editing a rectangular sketch, remove the two guarded overall rectangle dimensions and any automatic symmetry assumptions, project four specified model edges into the sketch, create four strong line-to-line inset dimensions with the requested value and outside placement, regenerate and verify the section, then update the active sketch without accepting, exiting, regenerating the solid, or saving.

## 59. `creo_create_project_general_sketch`

Create one independent sketched datum-curve feature in the current expected Creo project part. The sketch may contain 1-32 lines, circles, and arcs on a named datum plane with an explicit orientation plane. The tool rejects duplicate feature names and invalid or degenerate geometry, verifies every created section entity, regenerates the part, requires the solid volume to remain unchanged, saves the model, and moves older versions of only that model family to the Windows Recycle Bin. A failure before save deletes the partially created sketch.

## 60. `creo_cleanup_project_versions`

After a newly generated part has been verified, move every other .prt or .prt.<version> file in the current Creo working directory to the Windows Recycle Bin. The explicitly named keep file is never removed and unrelated files are untouched.
