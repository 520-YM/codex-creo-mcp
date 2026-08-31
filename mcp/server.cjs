'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const readline = require('readline');
const { spawn, spawnSync } = require('child_process');

const bridgeRoot = path.resolve(
  process.env.CREO_BRIDGE_ROOT ||
  path.join(__dirname, 'bin')
);
const safeOutputRoot = path.resolve(
  process.env.CREO_SAFE_OUTPUT_ROOT ||
  path.join(__dirname, 'output')
);
const creoPartTemplate = path.resolve(
  process.env.CREO_PART_TEMPLATE ||
  path.join(__dirname, 'templates', 'mmns_part_solid_abs.prt')
);
const creoSheetmetalTemplate = path.resolve(
  process.env.CREO_SHEETMETAL_TEMPLATE ||
  path.join(__dirname, 'templates', 'mmns_part_sheetmetal_abs.prt')
);
const creoAssemblyTemplate = path.resolve(
  process.env.CREO_ASSEMBLY_TEMPLATE ||
  path.join(__dirname, 'templates', 'mmns_asm_design_abs.asm')
);
const cleanupProjectVersionsScript = path.join(__dirname, 'cleanup_project_versions.ps1');
const cleanupProjectModelVersionsScript = path.join(
  __dirname, 'cleanup_project_model_versions.ps1');
const residentBackframeResizeScript = path.join(
  bridgeRoot, 'invoke_backframe_resize_resident_v4.ps1');
const residentComponentSwitchScript = path.join(
  bridgeRoot, 'invoke_component_visibility_switch_resident_v5.ps1');
const residentHingePatternScript = path.join(
  bridgeRoot, 'invoke_hinge_pattern_set_resident_v6.ps1');
const creoRuntimeObj =
  process.env.CREO_RUNTIME_OBJ ||
  path.join(__dirname, 'creo-runtime', 'obj');
const creoRuntimeLib =
  process.env.CREO_RUNTIME_LIB ||
  path.join(__dirname, 'creo-runtime', 'lib');
const proCommMsgExe =
  process.env.PRO_COMM_MSG_EXE ||
  path.join(creoRuntimeObj, 'pro_comm_msg.exe');
const nmsQueryExe = path.resolve(
  path.join(creoRuntimeObj, '..', 'nms', 'nmsq.exe'));
const persistentFlatWallBridge = path.join(
  bridgeRoot, 'creo_sheetmetal_flat_wall_persistent_bridge.exe');
const persistentFlatWallPipe = '\\\\.\\pipe\\creo_safe_flat_wall_v1';
const creoHoleTableRoot = path.resolve(
  process.env.CREO_HOLE_TABLE_ROOT ||
  path.join(creoRuntimeObj, '..', '..', 'text', 'hole')
);
const creoSingleFlatWallSection = path.resolve(
  process.env.CREO_SINGLE_FLAT_WALL_SECTION ||
  path.join(creoRuntimeObj, '..', '..', 'text', 'smt', 'flat_shapes',
    'rect_flat_wall.sec')
);

function ensureHoleTableToken(value, label) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9_.-]{1,31}$/.test(value)) {
    throw new Error(`${label} must contain only letters, digits, underscore, dash, or dot.`);
  }
  return value;
}

function tapDrillDiameterFromHoleTable(threadSeries, threadSize) {
  const series = ensureHoleTableToken(threadSeries, 'thread_series');
  const size = ensureHoleTableToken(threadSize, 'thread_size');
  const tableName = fs.readdirSync(creoHoleTableRoot)
    .find((name) => name.toLowerCase() === `${series.toLowerCase()}.hol`);
  if (!tableName) {
    throw new Error(`Creo hole table not found for thread_series ${series}.`);
  }
  const tablePath = path.resolve(creoHoleTableRoot, tableName);
  if (path.dirname(tablePath).toLowerCase() !== creoHoleTableRoot.toLowerCase()) {
    throw new Error('Resolved Creo hole table escaped the configured hole-table directory.');
  }
  const lines = fs.readFileSync(tablePath, 'utf8').split(/\r?\n/);
  const headerIndex = lines.findIndex((line) => /^\s*FASTENER_ID\s+/i.test(line));
  if (headerIndex < 0) {
    throw new Error(`Creo hole table ${tableName} has no FASTENER_ID header.`);
  }
  const columns = lines[headerIndex].trim().split(/\s+/);
  const tapIndex = columns.findIndex((column) => column.toUpperCase() === 'TAP_DR');
  if (tapIndex < 0) {
    throw new Error(`Creo hole table ${tableName} has no TAP_DR column.`);
  }
  for (let i = headerIndex + 1; i < lines.length; i += 1) {
    const fields = lines[i].trim().split(/\s+/);
    if (fields[0] && fields[0].toLowerCase() === size.toLowerCase()) {
      const diameter = Number(fields[tapIndex]);
      if (!Number.isFinite(diameter) || diameter <= 0 || diameter > 100) {
        throw new Error(`Invalid TAP_DR for ${series} ${size}.`);
      }
      return diameter;
    }
  }
  throw new Error(`thread_size ${size} was not found in Creo hole table ${tableName}.`);
}
const allowedParameters = [
  'CNAME',
  '材质规格',
  '用量',
  '表面处理',
  '类属',
  '项目',
  '备注',
  '料号',
  '版本号',
  '设计',
];

const toolDefinitions = [
  {
    name: 'creo_get_current_model',
    description: '读取当前运行中的 Creo 及当前模型信息；不会修改、保存或显示模型。',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    annotations: {
      title: '读取 Creo 当前模型',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_start_resident_and_get_basic_model',
    description: 'Creo 每次新会话的首次命令应先调用：启动或复用与当前 Creo 会话绑定的常驻桥接，并快速只读获取工作目录、当前模型名称、类型和外形尺寸；不遍历特征、尺寸、参数或装配组件。关闭该次 Creo 后桥接会自动退出。',
    inputSchema: { type: 'object', properties: {}, additionalProperties: false },
    annotations: {
      title: 'Creo 首次快速握手',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_verify_saved_copy',
    description: '只读载入安全输出目录中的 Creo 零件副本，核对字符串参数，然后从内存卸载。',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: {
          type: 'string',
          description: '安全输出目录中的零件文件名，例如 toolkit_codex_safe.prt.2。',
        },
        parameter: {
          type: 'string',
          enum: allowedParameters,
          default: 'CNAME',
        },
        expected_value: { type: 'string', maxLength: 80 },
      },
      required: ['model_file', 'expected_value'],
      additionalProperties: false,
    },
    annotations: {
      title: '核对 Creo 安全副本',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_parameter_copy',
    description: '把当前 Creo 零件复制为新文件，只在副本中写入允许的参数并重生成；绝不覆盖已有文件，也不保存源模型。',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string',
          description: '当前模型名（不含 .prt），用于防止操作错模型。',
          minLength: 1,
          maxLength: 80,
        },
        copy_name: {
          type: 'string',
          description: '新副本名（不含 .prt），仅允许字母、数字、下划线和短横线。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 80,
        },
        parameter: { type: 'string', enum: allowedParameters },
        new_value: { type: 'string', maxLength: 80 },
      },
      required: ['expected_model', 'copy_name', 'parameter', 'new_value'],
      additionalProperties: false,
    },
    annotations: {
      title: '创建 Creo 参数化安全副本',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_export_step',
    description: '从安全输出目录中的 Creo 零件副本导出新的 STEP 文件；拒绝覆盖同名输出。',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: {
          type: 'string',
          description: '安全输出目录中的零件文件名，例如 toolkit_codex_safe.prt.2。',
        },
        output_name: {
          type: 'string',
          description: 'STEP 文件基础名，不含扩展名。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 80,
        },
      },
      required: ['model_file', 'output_name'],
      additionalProperties: false,
    },
    annotations: {
      title: '导出 Creo STEP',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_multi_parameter_copy',
    description: '把当前 Creo 零件复制为新文件，在同一个副本中批量写入 1-10 个业务白名单参数，然后统一重生成；拒绝覆盖且绝不保存源模型。',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string',
          description: '当前模型名（不含 .prt），用于防止操作错模型。',
          minLength: 1,
          maxLength: 80,
        },
        copy_name: {
          type: 'string',
          description: '新副本名（不含 .prt），仅允许字母、数字、下划线和短横线。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 80,
        },
        updates: {
          type: 'array',
          minItems: 1,
          maxItems: 10,
          items: {
            type: 'object',
            properties: {
              parameter: { type: 'string', enum: allowedParameters },
              new_value: { type: 'string', maxLength: 80 },
            },
            required: ['parameter', 'new_value'],
            additionalProperties: false,
          },
        },
      },
      required: ['expected_model', 'copy_name', 'updates'],
      additionalProperties: false,
    },
    annotations: {
      title: '创建 Creo 多参数安全副本',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_get_dimensions',
    description: '只读列出当前 Creo 零件或安全输出目录中某个零件副本的标准尺寸，包括尺寸 ID、符号、数值、类型、关系驱动状态和所属特征。',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: {
          type: 'string',
          description: '可选；安全输出目录中的零件文件名。省略时读取当前模型。',
        },
      },
      additionalProperties: false,
    },
    annotations: {
      title: '读取 Creo 尺寸清单',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_get_project_dimensions',
    description: 'Read-only list of standard dimensions for one Creo part stored under the live working directory already selected in the current Creo session, including dimension IDs, symbols, values, relation-driven state, and owning features.',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: { type: 'string', minLength: 1, maxLength: 120 },
      },
      required: ['model_file'],
      additionalProperties: false,
    },
    annotations: {
      title: '读取项目 Creo 尺寸清单',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_dimension_copy',
    description: '按尺寸符号和预期旧值双重校验当前 Creo 零件，只在新副本中修改一个非关系驱动的正尺寸并重生成；拒绝覆盖，不保存源模型。新值限制在旧值的 0.5-2 倍。',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string',
          description: '当前模型名（不含 .prt），用于防止操作错模型。',
          minLength: 1,
          maxLength: 80,
        },
        copy_name: {
          type: 'string',
          description: '新副本名（不含 .prt），仅允许字母、数字、下划线和短横线。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 80,
        },
        dimension_symbol: {
          type: 'string',
          description: '唯一尺寸符号，例如 d11。',
          pattern: '^[A-Za-z][A-Za-z0-9_]*$',
          minLength: 1,
          maxLength: 80,
        },
        expected_value: {
          type: 'number',
          exclusiveMinimum: 0,
          description: '读取到的旧尺寸值；不一致时安全停止。',
        },
        new_value: {
          type: 'number',
          exclusiveMinimum: 0,
          description: '新尺寸值，必须在旧值的 0.5-2 倍范围内。',
        },
      },
      required: ['expected_model', 'copy_name', 'dimension_symbol', 'expected_value', 'new_value'],
      additionalProperties: false,
    },
    annotations: {
      title: '创建 Creo 尺寸修改安全副本',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_get_features',
    description: '只读列出当前 Creo 零件或安全输出目录中某个零件副本的特征，包括 ID、名称、类型、状态、可见性及父子依赖。',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: {
          type: 'string',
          description: '可选；安全输出目录中的零件文件名。省略时读取当前模型。',
        },
      },
      additionalProperties: false,
    },
    annotations: {
      title: '读取 Creo 特征清单',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_feature_suppression_copy',
    description: '按模型名、特征 ID 和类型多重校验，只在新副本中抑制一个活动、可见、无子特征的圆角、倒角、孔或切除特征；拒绝覆盖且不保存源模型。',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string',
          description: '当前模型名（不含 .prt），用于防止操作错模型。',
          minLength: 1,
          maxLength: 80,
        },
        copy_name: {
          type: 'string',
          description: '新副本名（不含 .prt），仅允许字母、数字、下划线和短横线。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 80,
        },
        feature_id: {
          type: 'integer',
          minimum: 1,
          description: '读取到的特征 ID。',
        },
        expected_type_code: {
          type: 'integer',
          enum: [913, 914, 911, 916],
          description: '预期特征类型：913 圆角、914 倒角、911 孔、916 切除。',
        },
      },
      required: ['expected_model', 'copy_name', 'feature_id', 'expected_type_code'],
      additionalProperties: false,
    },
    annotations: {
      title: '创建 Creo 特征抑制安全副本',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_feature_resume_copy',
    description: '从安全输出目录中的已抑制零件副本创建一个新副本，并仅在新副本中恢复一个无子特征、父特征均活动的圆角、倒角、孔或切除特征；拒绝覆盖且不修改输入副本或当前模型。',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: {
          type: 'string',
          description: '安全输出目录中的已抑制零件文件名，例如 prt0001_sup_r130_mcp.prt.2。',
        },
        expected_model: {
          type: 'string',
          description: '输入副本的预期模型名（不含 .prt），用于防止读取错文件。',
          minLength: 1,
          maxLength: 31,
        },
        copy_name: {
          type: 'string',
          description: '新恢复副本名（不含 .prt），仅允许字母、数字、下划线和短横线。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 31,
        },
        feature_id: {
          type: 'integer',
          minimum: 1,
          description: '读取到的已抑制特征 ID。',
        },
        expected_type_code: {
          type: 'integer',
          enum: [913, 914, 911, 916],
          description: '预期特征类型：913 圆角、914 倒角、911 孔、916 切除。',
        },
      },
      required: ['model_file', 'expected_model', 'copy_name', 'feature_id', 'expected_type_code'],
      additionalProperties: false,
    },
    annotations: {
      title: '创建 Creo 特征恢复安全副本',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_datum_point_copy',
    description: '复制当前 Creo 零件，并仅在新副本中相对于指定坐标系创建一个笛卡尔偏移基准点；坐标限制在模型单位的 -1000 到 1000，拒绝覆盖且不保存源模型。',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string',
          description: '当前模型名（不含 .prt），用于防止操作错模型。',
          minLength: 1,
          maxLength: 31,
        },
        copy_name: {
          type: 'string',
          description: '新副本名（不含 .prt）。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 31,
        },
        reference_csys: {
          type: 'string',
          description: '参考坐标系名称，通常为 PRT_CSYS_DEF。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 31,
          default: 'PRT_CSYS_DEF',
        },
        point_name: {
          type: 'string',
          description: '新基准点名称。',
          pattern: '^[A-Za-z0-9_-]+$',
          minLength: 1,
          maxLength: 31,
        },
        x: { type: 'number', minimum: -1000, maximum: 1000 },
        y: { type: 'number', minimum: -1000, maximum: 1000 },
        z: { type: 'number', minimum: -1000, maximum: 1000 },
      },
      required: ['expected_model', 'copy_name', 'point_name', 'x', 'y', 'z'],
      additionalProperties: false,
    },
    annotations: {
      title: '创建 Creo 基准点安全副本',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_get_mass_properties',
    description: 'Read computed mass properties for the current Creo part or a part copy in the safe output directory. Uses unit density for calculation only and does not modify the model.',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: {
          type: 'string',
          description: 'Optional part file name in the safe output directory. Omit to read the current model.',
        },
      },
      additionalProperties: false,
    },
    annotations: {
      title: 'Read Creo mass properties',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_thru_hole_copy',
    description: 'Copy the current Creo part and create one regular straight thru-all hole only in the new copy. Placement uses one named datum plane and two named offset datum planes. The operation refuses overwrite and succeeds only when the copy volume is reduced.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        hole_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        primary_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'FRONT',
        },
        reference_plane1: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'RIGHT',
        },
        reference_plane2: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
        },
        diameter: { type: 'number', minimum: 0.1, maximum: 100 },
        offset1: { type: 'number', minimum: -1000, maximum: 1000 },
        offset2: { type: 'number', minimum: -1000, maximum: 1000 },
        thru_side: {
          type: 'integer', enum: [1, 2], default: 2,
          description: 'Side of the primary datum plane to cut through.',
        },
      },
      required: ['expected_model', 'copy_name', 'hole_name', 'diameter', 'offset1', 'offset2'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo thru-hole copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_advanced_hole_copy',
    description: 'Copy the current Creo part and create one advanced hole only in the new copy. Enabled styles are regular straight blind, custom counterbore, custom countersink, and standard threaded holes. Threaded-hole drill diameter is read from the installed Creo .hol table and the saved thread metadata is verified. The operation refuses overwrite, requires an active hole feature, requires volume to decrease, and applies a style-specific maximum-removal guard.',
    inputSchema: {
      type: 'object',
      properties: {
        style: {
          type: 'string',
          enum: ['blind', 'counterbore', 'countersink', 'threaded'],
          default: 'blind',
        },
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        hole_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        primary_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'FRONT',
        },
        reference_plane1: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'RIGHT',
        },
        reference_plane2: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
        },
        diameter: { type: 'number', minimum: 0.1, maximum: 100 },
        depth: { type: 'number', minimum: 0.1, maximum: 500 },
        counterbore_diameter: {
          type: 'number', minimum: 0.2, maximum: 200,
          description: 'Required for counterbore style and must be greater than diameter.',
        },
        counterbore_depth: {
          type: 'number', minimum: 0.1, maximum: 500,
          description: 'Required for counterbore style and must be less than depth.',
        },
        countersink_diameter: {
          type: 'number', minimum: 0.2, maximum: 200,
          description: 'Required for countersink style and must be greater than diameter.',
        },
        countersink_angle: {
          type: 'number', minimum: 30, maximum: 150,
          description: 'Included countersink angle in degrees; required for countersink style.',
        },
        thread_series: {
          type: 'string', pattern: '^[A-Za-z0-9_.-]+$', minLength: 1, maxLength: 31,
          default: 'ISO',
          description: 'Installed Creo .hol table name without extension.',
        },
        thread_size: {
          type: 'string', pattern: '^[A-Za-z0-9_.-]+$', minLength: 1, maxLength: 31,
          description: 'FASTENER_ID from the selected Creo hole table, for example M4x.7.',
        },
        thread_depth: {
          type: 'number', minimum: 0.1, maximum: 500,
          description: 'Required for threaded style and must be less than depth.',
        },
        offset1: { type: 'number', minimum: -1000, maximum: 1000 },
        offset2: { type: 'number', minimum: -1000, maximum: 1000 },
        direction_side: {
          type: 'integer', enum: [1, 2], default: 2,
          description: 'Side of the primary datum plane into which the blind hole is cut.',
        },
      },
      required: [
        'expected_model', 'copy_name', 'hole_name', 'depth', 'offset1', 'offset2',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo advanced-hole copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_offset_datum_plane_copy',
    description: 'Copy the current Creo part and create one datum plane at a nonzero offset from a named planar datum reference only in the new copy. The operation refuses overwrite and succeeds only when solid volume remains unchanged.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        reference_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'FRONT',
        },
        plane_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        offset: {
          type: 'number', minimum: -1000, maximum: 1000,
          description: 'Nonzero offset in current model units; absolute value must be at least 0.001.',
        },
      },
      required: ['expected_model', 'copy_name', 'plane_name', 'offset'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo offset datum plane copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_display_safe_model',
    description: 'Load a Creo part from the safe output directory, display it in the active Creo window, refit the view, and bring Creo to the foreground. This changes only the current UI display and does not save or modify model data.',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: {
          type: 'string',
          description: 'Part file name in the safe output directory.',
        },
        expected_model: {
          type: 'string', minLength: 1, maxLength: 31,
          description: 'Expected model name without .prt.',
        },
      },
      required: ['model_file', 'expected_model'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Display safe Creo model',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_rectangle_extrusion_copy',
    description: 'Copy the current Creo part and create one solid blind protrusion from a centered rectangular sketch only in the new copy. The operation refuses overwrite and succeeds only when the active feature is saved and solid volume increases.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        sketch_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'FRONT',
        },
        orientation_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        width: { type: 'number', minimum: 0.1, maximum: 500 },
        height: { type: 'number', minimum: 0.1, maximum: 500 },
        depth: { type: 'number', minimum: 0.1, maximum: 500 },
        direction_side: {
          type: 'integer', enum: [1, 2], default: 2,
          description: 'Extrusion side. With the default FRONT sketch plane and TOP orientation plane, side 2 is +Z.',
        },
      },
      required: ['expected_model', 'copy_name', 'feature_name', 'width', 'height', 'depth'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo rectangle extrusion copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_rectangle_cut_copy',
    description: 'Copy the current Creo part and create one solid blind cut from a centered rectangular sketch only in the new copy. The operation refuses overwrite, requires an active cut feature, requires solid volume to decrease, and rejects removal greater than the requested rectangular prism.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        sketch_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'FRONT',
        },
        orientation_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        width: { type: 'number', minimum: 0.1, maximum: 500 },
        height: { type: 'number', minimum: 0.1, maximum: 500 },
        depth: { type: 'number', minimum: 0.1, maximum: 500 },
        direction_side: { type: 'integer', enum: [1, 2], default: 1 },
      },
      required: ['expected_model', 'copy_name', 'feature_name', 'width', 'height', 'depth'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo rectangle cut copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_revolve_copy',
    description: 'Copy the current Creo part and create one full 360-degree solid revolve from a closed rectangular radial section in the new copy. Supports additive revolve and revolve cut, refuses overwrite, requires an active feature, and verifies the corresponding solid-volume change.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        sketch_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'RIGHT',
          description: 'Datum plane containing the intended revolve axis.',
        },
        orientation_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
          description: 'Datum plane whose trace orients the internal sketch axis.',
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        operation_mode: { type: 'string', enum: ['add', 'cut'] },
        axial_width: {
          type: 'number', minimum: 0.1, maximum: 500,
          description: 'Width of the rectangular section along the revolve axis.',
        },
        inner_radius: {
          type: 'number', minimum: 0.1, maximum: 500,
          description: 'Distance from the sketch centerline axis to the near radial edge.',
        },
        radial_thickness: {
          type: 'number', minimum: 0.1, maximum: 500,
          description: 'Radial thickness from inner_radius to the far radial edge.',
        },
        direction_side: { type: 'integer', enum: [1, 2], default: 1 },
      },
      required: [
        'expected_model', 'copy_name', 'feature_name', 'operation_mode',
        'axial_width', 'inner_radius', 'radial_thickness',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo revolve copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_round_copy',
    description: 'Copy the current Creo part and create one constant-radius edge round in the new copy. An explicit edge ID may be supplied; otherwise the longest supported straight or circular edge is selected. The tool refuses overwrite and verifies an active round feature, a feature-count increase, and a nonzero solid-volume change.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        edge_id: {
          type: 'integer', minimum: 1,
          description: 'Optional Creo edge ID. If omitted, the bridge selects the longest supported edge.',
        },
        radius: {
          type: 'number', minimum: 0.1, maximum: 100,
          description: 'Constant round radius in model units.',
        },
      },
      required: ['expected_model', 'copy_name', 'feature_name', 'radius'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo round copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_chamfer_copy',
    description: 'Copy the current Creo part and create one equal-distance edge chamfer in the new copy. An explicit edge ID may be supplied; otherwise the longest supported straight or circular edge is selected. The tool refuses overwrite and verifies an active chamfer feature, a feature-count increase, and a nonzero solid-volume change.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        edge_id: {
          type: 'integer', minimum: 1,
          description: 'Optional Creo edge ID. If omitted, the bridge selects the longest supported edge.',
        },
        distance: {
          type: 'number', minimum: 0.1, maximum: 100,
          description: 'Equal chamfer distance in model units.',
        },
      },
      required: ['expected_model', 'copy_name', 'feature_name', 'distance'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo chamfer copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_shell_copy',
    description: 'Copy the current Creo part and create one inward shell in the new copy. A planar surface ID may be supplied as the opening; otherwise the largest planar surface is selected. The tool refuses overwrite and verifies an active shell feature, a feature-count increase, and a decrease in solid volume.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        removed_surface_id: {
          type: 'integer', minimum: 1,
          description: 'Optional Creo surface ID to remove. If omitted, the largest planar surface is selected.',
        },
        thickness: {
          type: 'number', minimum: 0.1, maximum: 100,
          description: 'Inward shell wall thickness in model units.',
        },
      },
      required: ['expected_model', 'copy_name', 'feature_name', 'thickness'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo shell copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_draft_copy',
    description: 'Copy the current Creo part and create one constant-angle draft feature in the new copy using a datum plane as the hinge and direction reference. A surface ID may be supplied; otherwise the largest cylindrical surface is selected. The tool refuses overwrite and verifies an active draft feature, a feature-count increase, and a nonzero solid-volume change.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        drafted_surface_id: {
          type: 'integer', minimum: 1,
          description: 'Optional Creo surface ID. If omitted, the largest cylindrical surface is selected.',
        },
        hinge_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'FRONT',
          description: 'Datum plane used as both draft hinge and direction reference.',
        },
        angle_degrees: {
          type: 'number', minimum: 0.1, maximum: 30,
          description: 'Constant draft angle in degrees.',
        },
        direction_side: { type: 'integer', enum: [1, 2], default: 1 },
      },
      required: ['expected_model', 'copy_name', 'feature_name', 'angle_degrees'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo draft copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_safe_part_mirror_copy',
    description: 'Copy the current Creo part and create a whole-part mirror feature in the new copy about a named datum plane. The source model is never saved, overwrite is refused, and the result is verified for an active mirror feature, a feature-count increase, and a nonzero solid-geometry change.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', minLength: 1, maxLength: 31 },
        copy_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        mirror_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'RIGHT',
          description: 'Datum plane about which the complete part geometry is mirrored.',
        },
      },
      required: ['expected_model', 'copy_name', 'feature_name'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create safe Creo whole-part mirror copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_set_project_working_directory',
    description: 'Compatibility tool that reads and verifies the live working directory already selected in the current Creo session. It never creates, selects, or changes a folder and accepts no directory argument.',
    inputSchema: {
      type: 'object',
      properties: {
      },
      required: [],
      additionalProperties: false,
    },
    annotations: {
      title: 'Read current Creo working directory',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_part',
    description: 'Create a new millimeter solid Creo part from the installed mmns template inside the live working directory already selected in the current Creo session, refuse overwrite, keep that folder as the live Creo working directory, save the model, and display it.',
    inputSchema: {
      type: 'object',
      properties: {
        model_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'Creo part name without .prt.',
        },
      },
      required: ['model_name'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create Creo project part',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_sheetmetal_part',
    description: 'Create a new empty native millimeter Creo sheet-metal part from the installed sheet-metal template inside the live working directory already selected in the current Creo session, verify the returned sheet-metal model subtype, keep that live working directory active, save and display the part, and recycle older versions of only that part family.',
    inputSchema: {
      type: 'object',
      properties: {
        model_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'New empty native sheet-metal part name without .prt.',
        },
      },
      required: ['model_name'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create empty Creo sheet-metal project part',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_sheetmetal_planar_wall',
    description: 'Create a new native millimeter Creo sheet-metal part inside the live working directory already selected in the current Creo session and create its first unattached planar wall from a centered closed rectangle on FRONT. The tool refuses overwrite; verifies sheet-metal subtype, active flat-surface feature type, thickness, volume, and 3D envelope; saves and displays the part; then moves older versions of only that part family to the Windows Recycle Bin.',
    inputSchema: {
      type: 'object',
      properties: {
        model_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'New native sheet-metal part name without .prt.',
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'Unique planar-wall feature name.',
        },
        length: { type: 'number', minimum: 0.1, maximum: 100000 },
        width: { type: 'number', minimum: 0.1, maximum: 100000 },
        thickness: { type: 'number', minimum: 0.01, maximum: 1000 },
      },
      required: [
        'model_name', 'feature_name',
        'length', 'width', 'thickness',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create Creo sheet-metal planar wall',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_sheetmetal_planar_wall_current',
    description: 'Create the first unattached planar wall in the current expected empty native Creo sheet-metal project part on FRONT. The tool verifies subtype, feature status, thickness, volume and envelope, saves and displays the part, and recycles older versions of only that part family. This variant operates on an existing project sheet-metal part.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        length: { type: 'number', minimum: 0.1, maximum: 100000 },
        width: { type: 'number', minimum: 0.1, maximum: 100000 },
        thickness: { type: 'number', minimum: 0.01, maximum: 1000 },
      },
      required: [
        'expected_model', 'feature_name',
        'length', 'width', 'thickness',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create planar wall in current Creo sheet-metal part',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_link_project_sheetmetal_wall_to_skeleton',
    description: 'In a verified top-level Creo assembly context with its sheet-metal component active, create two assembly relations that drive the planar-wall sketch length and width from the standard skeleton dimensions. The tool preserves existing assembly relations, rejects conflicting targets, temporarily changes and restores the skeleton dimensions to prove that both the sheet dimensions and solid envelope update associatively, saves all three models only after verification, leaves the sheet-metal part active, and recycles older versions of only those model families.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_skeleton: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_sheetmetal: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        sheetmetal_component_feature_id: { type: 'integer', minimum: 1, maximum: 1000000 },
        skeleton_feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        sheetmetal_profile_feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        sheetmetal_profile_feature_id: { type: 'integer', minimum: 1, maximum: 1000000 },
        skeleton_length_symbol: { type: 'string', pattern: '^d[0-9]+$' },
        skeleton_width_symbol: { type: 'string', pattern: '^d[0-9]+$' },
        sheetmetal_length_symbol: { type: 'string', pattern: '^d[0-9]+$' },
        sheetmetal_width_symbol: { type: 'string', pattern: '^d[0-9]+$' },
        expected_length: { type: 'number', minimum: 0.1, maximum: 100000 },
        expected_width: { type: 'number', minimum: 0.1, maximum: 100000 },
        thickness: { type: 'number', minimum: 0.01, maximum: 1000 },
      },
      required: [
        'expected_assembly', 'expected_skeleton',
        'expected_sheetmetal', 'sheetmetal_component_feature_id',
        'skeleton_feature_name', 'sheetmetal_profile_feature_name',
        'sheetmetal_profile_feature_id', 'skeleton_length_symbol',
        'skeleton_width_symbol', 'sheetmetal_length_symbol',
        'sheetmetal_width_symbol', 'expected_length', 'expected_width',
        'thickness',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Link Creo sheet-metal wall to skeleton',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_reverse_project_sheetmetal_planar_wall_to_positive_z',
    description: 'Reverse the verified first planar-wall thickness direction of the active native Creo sheet-metal project part on FRONT from -Z to +Z. The tool guards the exact wall feature and its thickness/length/width dimensions, preserves assembly-driven length and width, verifies the solid envelope changes from Z=-thickness..0 to Z=0..+thickness with unchanged volume, regenerates the named top assembly, saves both models only after verification, leaves the sheet-metal part active, and recycles older versions of only those two model families.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_sheetmetal: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_id: { type: 'integer', minimum: 1, maximum: 1000000 },
        thickness_symbol: { type: 'string', pattern: '^d[0-9]+$' },
        length_symbol: { type: 'string', pattern: '^d[0-9]+$' },
        width_symbol: { type: 'string', pattern: '^d[0-9]+$' },
        expected_length: { type: 'number', minimum: 0.1, maximum: 100000 },
        expected_width: { type: 'number', minimum: 0.1, maximum: 100000 },
        thickness: { type: 'number', minimum: 0.01, maximum: 1000 },
      },
      required: [
        'expected_assembly', 'expected_sheetmetal',
        'feature_name', 'feature_id', 'thickness_symbol', 'length_symbol',
        'width_symbol', 'expected_length', 'expected_width', 'thickness',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Reverse Creo planar wall to +Z',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_sheetmetal_flat_wall',
    description: 'Create one attached flat wall on one verified straight boundary edge of the current expected native Creo sheet-metal project part. The tool calculates the inside bend radius as sheet thickness x 0.5, creates a 90-degree wall using the requested height, verifies the attachment edge ID and length, wall type, active feature, radius rule, regeneration, solid-volume increase and saved file, then moves older versions of only that model family to the Windows Recycle Bin. Any failure before save deletes the partial feature.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        edge_id: { type: 'integer', minimum: 1, maximum: 100000000 },
        expected_edge_length: { type: 'number', minimum: 0.01, maximum: 100000 },
        angle: { type: 'number', minimum: 1, maximum: 179, default: 90 },
        wall_height: { type: 'number', minimum: 0.01, maximum: 100000 },
      },
      required: [
        'expected_model', 'feature_name', 'edge_id',
        'expected_edge_length', 'wall_height',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create one Creo sheet-metal flat wall',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_sheetmetal_flat_walls_batch',
    description: 'Create one native Creo sheet-metal flat-wall feature on exactly two verified straight boundary edges in one Pro/TOOLKIT connection and one transaction. Both walls share the requested angle and height; the inside bend radius is sheet thickness x 0.5. The tool changes to the guarded project directory inside the same bridge process, verifies both attachment edge IDs and lengths, feature type, radius rule, regeneration and volume increase, saves once, and then recycles older versions of only the target part family. Any failure before save deletes the complete batch feature.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        edges: {
          type: 'array', minItems: 2, maxItems: 2,
          items: {
            type: 'object',
            properties: {
              edge_id: { type: 'integer', minimum: 1, maximum: 100000000 },
              expected_edge_length: { type: 'number', minimum: 0.01, maximum: 100000 },
            },
            required: ['edge_id', 'expected_edge_length'],
            additionalProperties: false,
          },
        },
        angle: { type: 'number', minimum: 1, maximum: 179, default: 90 },
        wall_height: { type: 'number', minimum: 0.01, maximum: 100000 },
      },
      required: [
        'expected_model', 'feature_name', 'edges', 'wall_height',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create two Creo sheet-metal flat walls in one batch',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_sheetmetal_three_circle_cut',
    description: 'Create one through-all extruded cut containing exactly three equal circles on one guarded planar surface of the current expected native Creo sheet-metal project part. The circles are centered on one sketch axis at -spacing, 0, and +spacing from the requested center. The tool verifies surface ID, area and owning feature, layout margins, active cut feature, total cylindrical cut area representing all three holes, exact sheet-thickness removal volume, regeneration and saved file, then moves older versions of only that model family to the Windows Recycle Bin. Any failure before save deletes the partial feature.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        surface_id: { type: 'integer', minimum: 1, maximum: 100000000 },
        expected_surface_area: { type: 'number', minimum: 0.01, maximum: 100000000 },
        owner_feature_id: { type: 'integer', minimum: 1, maximum: 100000000 },
        owner_feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        orientation_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
        },
        diameter: { type: 'number', minimum: 0.01, maximum: 1000 },
        spacing: { type: 'number', minimum: 0.01, maximum: 100000 },
        center_u: { type: 'number', minimum: -100000, maximum: 100000, default: 0 },
        center_v: { type: 'number', minimum: -100000, maximum: 100000, default: 0 },
      },
      required: [
        'expected_model', 'feature_name', 'surface_id',
        'expected_surface_area', 'owner_feature_id', 'owner_feature_name',
        'diameter', 'spacing',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create three-hole sheet-metal extruded cut',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_extrusion_copy',
    description: 'Copy the current Creo part into the live working directory already selected in the current Creo session and create one centered rectangle or circle extrusion. Supports additive protrusion and blind cut, refuses overwrite, and verifies the active feature and solid-volume change.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: { type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31 },
        copy_name: { type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31 },
        feature_name: { type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31 },
        profile: { type: 'string', enum: ['rectangle', 'circle'] },
        operation_mode: { type: 'string', enum: ['add', 'cut'] },
        width: { type: 'number', minimum: 0.1, maximum: 500, description: 'Rectangle width; required for rectangle profile.' },
        height: { type: 'number', minimum: 0.1, maximum: 500, description: 'Rectangle height; required for rectangle profile.' },
        diameter: { type: 'number', minimum: 0.1, maximum: 500, description: 'Circle diameter; required for circle profile.' },
        depth: { type: 'number', minimum: 0.1, maximum: 500 },
        sketch_plane: { type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31, default: 'FRONT' },
        orientation_plane: { type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31, default: 'TOP' },
        direction_side: { type: 'integer', enum: [1, 2], default: 1 },
      },
      required: ['expected_model', 'copy_name', 'feature_name', 'profile', 'operation_mode', 'depth'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create Creo project extrusion copy',
      readOnlyHint: false,
      destructiveHint: false,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_get_project_geometry',
    description: 'Read edge IDs, edge types, lengths, representative coordinates, surface IDs, surface types, areas, and axis-aligned extents from one Creo part inside the current Creo working directory. The model is not saved or modified.',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: { type: 'string', minLength: 1, maxLength: 120, description: 'Part file name directly inside the current Creo working directory, for example cube_100.prt.2.' },
        expected_model: { type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31 },
      },
      required: ['model_file', 'expected_model'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Read Creo project geometry',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_display_project_model',
    description: 'Read the live working directory already selected in the current Creo session, load one direct-child Creo part or assembly from that directory, verify its expected model name and file type, activate its existing window or create a dedicated new window, refit the view, and bring Creo to the foreground without closing, replacing, saving, or modifying any other open model window. No configured or caller-supplied project folder is used.',
    inputSchema: {
      type: 'object',
      properties: {
        model_file: { type: 'string', minLength: 1, maxLength: 120 },
        expected_model: { type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31 },
      },
      required: ['model_file', 'expected_model'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Display Creo project model',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_empty_assembly',
    description: 'Create a new empty millimeter Creo assembly inside the live working directory already selected in the current Creo session from the installed assembly template, verify the assembly name and type, regenerate, save and display it, and recycle older versions of only that assembly family. No component is added.',
    inputSchema: {
      type: 'object',
      properties: {
        assembly_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
      },
      required: ['assembly_name'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create empty Creo project assembly',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_assembly',
    description: 'Create a new millimeter Creo assembly inside the live working directory already selected in the current Creo session from the installed assembly template, add one existing project part with default placement, verify the active component and default-placement constraint, save and display the assembly, then recycle older versions of that assembly family.',
    inputSchema: {
      type: 'object',
      properties: {
        assembly_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'New Creo assembly name without .asm.',
        },
        component_model_file: {
          type: 'string', minLength: 1, maxLength: 120,
          description: 'Existing .prt or .prt.<version> file directly inside the current Creo working directory.',
        },
        expected_component: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'Expected internal Creo component model name.',
        },
      },
      required: ['assembly_name', 'component_model_file', 'expected_component'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create Creo project assembly',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_add_project_component_fixed',
    description: 'Add an existing project part to the current expected Creo assembly at a verified XYZ translation, apply a fixed placement constraint, regenerate and save the assembly, and recycle older versions of only that assembly family.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        component_model_file: { type: 'string', minLength: 1, maxLength: 120 },
        expected_component: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        translation_x: { type: 'number', minimum: -100000, maximum: 100000, default: 0 },
        translation_y: { type: 'number', minimum: -100000, maximum: 100000, default: 0 },
        translation_z: { type: 'number', minimum: -100000, maximum: 100000, default: 0 },
      },
      required: ['expected_assembly', 'component_model_file', 'expected_component'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Add fixed Creo assembly component',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_add_project_component_mate_align',
    description: 'Add an existing project part to the current expected Creo assembly with one to three named planar constraints. Each constraint is either mate (opposed plane normals) or align (same-direction plane normals), with explicit red/yellow datum sides. The tool rejects duplicate plane use, verifies every reference and constraint by API readback, reports packaged and underconstrained state, optionally requires full constraint, regenerates and saves the assembly, and recycles older versions of only that assembly family. A failure before save deletes the partially added component.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        component_model_file: {
          type: 'string', minLength: 1, maxLength: 120,
          description: 'Existing .prt or .prt.<version> file directly inside the current Creo working directory.',
        },
        expected_component: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        require_fully_constrained: {
          type: 'boolean', default: false,
          description: 'Fail and remove the new component unless the supplied constraints fully locate it.',
        },
        constraints: {
          type: 'array', minItems: 1, maxItems: 3,
          items: {
            type: 'object',
            properties: {
              type: { type: 'string', enum: ['mate', 'align'] },
              assembly_plane: {
                type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
              },
              component_plane: {
                type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
              },
              assembly_side: {
                type: 'string', enum: ['yellow', 'red'], default: 'yellow',
              },
              component_side: {
                type: 'string', enum: ['yellow', 'red'], default: 'yellow',
              },
            },
            required: ['type', 'assembly_plane', 'component_plane'],
            additionalProperties: false,
          },
        },
      },
      required: [
        'expected_assembly', 'component_model_file',
        'expected_component', 'constraints',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Add mate/align constrained Creo component',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_get_project_assembly_components',
    description: 'Read the active component instances of the current expected Creo assembly, including feature IDs, model names, placement matrices, and constraint types and references. The tool is read-only and uses the Pro/TOOLKIT API.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
      },
      required: ['expected_assembly'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Read Creo assembly components',
      readOnlyHint: true,
      destructiveHint: false,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_repeat_project_component_insert_pair',
    description: 'Read one existing component instance in the current expected assembly, require its exact ALIGN plus INSERT surface-constraint method, then add two more instances using the same component references, datum sides, orientation, and assembly align surface while changing only the two cylindrical assembly surfaces. The tool verifies each new constraint and X position, regenerates and saves the assembly, and recycles older versions of only that assembly family.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        source_component_feature_id: { type: 'integer', minimum: 1 },
        component_model_file: {
          type: 'string', minLength: 1, maxLength: 120,
          description: 'Existing component .prt or .prt.<version> file directly inside the current Creo working directory.',
        },
        expected_component: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        target_insert_surface_ids: {
          type: 'array', minItems: 2, maxItems: 2,
          items: { type: 'integer', minimum: 1 },
        },
        target_x_positions: {
          type: 'array', minItems: 2, maxItems: 2,
          items: { type: 'number', minimum: -100000, maximum: 100000 },
        },
      },
      required: [
        'expected_assembly', 'source_component_feature_id',
        'component_model_file', 'expected_component',
        'target_insert_surface_ids', 'target_x_positions',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Repeat insert-constrained Creo component twice',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_add_project_component_align_insert_three',
    description: 'Load one project part and add exactly three instances to cylindrical surfaces of a host component in the current expected assembly. Every instance uses a verified planar ALIGN constraint plus cylindrical INSERT constraint with yellow datum sides, is checked by API readback, and the assembly is regenerated, saved, and cleaned to its newest version. The tool refuses to run when an instance of the expected component already exists.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        host_component_feature_id: { type: 'integer', minimum: 1 },
        expected_host_component: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        component_model_file: { type: 'string', minLength: 1, maxLength: 120 },
        expected_component: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        assembly_align_surface_id: { type: 'integer', minimum: 1 },
        component_align_surface_id: { type: 'integer', minimum: 1 },
        component_insert_surface_id: { type: 'integer', minimum: 1 },
        target_insert_surface_ids: {
          type: 'array', minItems: 3, maxItems: 3,
          items: { type: 'integer', minimum: 1 },
        },
        target_x_positions: {
          type: 'array', minItems: 3, maxItems: 3,
          items: { type: 'number', minimum: -100000, maximum: 100000 },
        },
      },
      required: [
        'expected_assembly', 'host_component_feature_id',
        'expected_host_component', 'component_model_file', 'expected_component',
        'assembly_align_surface_id', 'component_align_surface_id',
        'component_insert_surface_id', 'target_insert_surface_ids',
        'target_x_positions',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Add three align/insert constrained Creo components',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_standard_skeleton',
    description: 'Create a standard skeleton model in the current expected project assembly from the millimeter part template, verify it with ProMdlIsSkeleton and assembly readback, save both models, and recycle older versions of only those two model families.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        skeleton_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
      },
      required: ['expected_assembly', 'skeleton_name'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create standard Creo skeleton',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_skeleton_box',
    description: 'Create one solid rectangular extrusion inside the expected standard skeleton on a named datum plane, verify feature status, volume and the requested three envelope dimensions, save and display the skeleton, save its assembly, and recycle older versions of those model families.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_skeleton: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', minLength: 1, maxLength: 31,
          description: 'Creo feature name; Unicode letters such as Chinese are allowed.',
        },
        length: { type: 'number', minimum: 0.1, maximum: 100000 },
        width: { type: 'number', minimum: 0.1, maximum: 100000 },
        height: { type: 'number', minimum: 0.1, maximum: 100000 },
        sketch_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'FRONT',
        },
        orientation_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
        },
        direction_side: { type: 'integer', enum: [1, 2], default: 1 },
      },
      required: [
        'expected_assembly', 'expected_skeleton',
        'feature_name', 'length', 'width', 'height',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create box in Creo skeleton',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_skeleton_surface_inset_extrusion',
    description: 'In the expected standard skeleton, validate one planar top-face surface by ID and full-face area, create a centered additive rectangular extrusion whose four sides are inset equally from the existing XY outline, verify the exact volume increase and +Z envelope increase, save the skeleton and top assembly, display the skeleton, and recycle older versions of only those two model families.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_skeleton: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', minLength: 1, maxLength: 31,
          description: 'Creo feature name; Unicode letters such as Chinese are allowed.',
        },
        surface_id: { type: 'integer', minimum: 1 },
        inset: { type: 'number', minimum: 0.1, maximum: 100000 },
        depth: { type: 'number', minimum: 0.1, maximum: 100000 },
        orientation_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
        },
        direction_side: { type: 'integer', enum: [1, 2], default: 1 },
      },
      required: [
        'expected_assembly', 'expected_skeleton',
        'feature_name', 'surface_id', 'inset', 'depth',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create inset rectangle extrusion on skeleton surface',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_skeleton_surface_outset_extrusion',
    description: 'In the expected standard skeleton, validate one centered planar top-face surface by ID and its exact X/Y extents, create a centered additive rectangular extrusion whose four sides extend outward equally from that surface, verify the exact volume increase and +Z envelope, save the skeleton and top assembly, display the skeleton, and recycle older versions of only those two model families.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_skeleton: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', minLength: 1, maxLength: 31,
          description: 'Creo feature name; Unicode letters such as Chinese are allowed.',
        },
        surface_id: { type: 'integer', minimum: 1 },
        expected_surface_length: { type: 'number', minimum: 0.1, maximum: 100000 },
        expected_surface_width: { type: 'number', minimum: 0.1, maximum: 100000 },
        outset: { type: 'number', minimum: 0.1, maximum: 100000 },
        depth: { type: 'number', minimum: 0.1, maximum: 100000 },
        orientation_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          default: 'TOP',
        },
        direction_side: { type: 'integer', enum: [2], default: 2 },
      },
      required: [
        'expected_assembly', 'expected_skeleton',
        'feature_name', 'surface_id', 'expected_surface_length',
        'expected_surface_width', 'outset', 'depth',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create outset rectangle extrusion on skeleton surface',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_reverse_project_skeleton_box_direction',
    description: 'Reverse one verified rectangular skeleton extrusion to the requested datum-plane side by redefining its Pro/TOOLKIT feature element tree. The tool checks the existing feature, direction readback, exact volume and signed XYZ outline, regenerates and saves the skeleton and top assembly, displays the skeleton, and recycles older versions of only those model families.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_skeleton: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_length: { type: 'number', minimum: 0.1, maximum: 100000 },
        expected_width: { type: 'number', minimum: 0.1, maximum: 100000 },
        expected_height: { type: 'number', minimum: 0.1, maximum: 100000 },
        direction_side: {
          type: 'integer', enum: [1, 2],
          description: 'Requested extrusion side. For FRONT with TOP orientation in this workflow, side 2 is +Z.',
        },
      },
      required: [
        'expected_assembly', 'expected_skeleton', 'feature_name',
        'expected_length', 'expected_width', 'expected_height', 'direction_side',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Reverse Creo skeleton box direction',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_set_current_hinge_pattern',
    description: '通过当前 Creo 会话的常驻 API 修改五个零总装配骨架中的“铰链阵列”。工具唯一核对阵列特征 ID 8242、名称、组阵列入口 ID 8241、间距尺寸 d229、关系驱动状态及当前成员数量，再原子修改数量和间距，读回验证、重新生成并保存骨架和总装配、返回总装配，并把这两个模型族的旧版本移入回收站。',
    inputSchema: {
      type: 'object',
      properties: {
        new_count: { type: 'integer', minimum: 2, maximum: 1000 },
        new_spacing: { type: 'number', exclusiveMinimum: 0, maximum: 1000000 },
      },
      required: ['new_count', 'new_spacing'],
      additionalProperties: false,
    },
    annotations: {
      title: '修改当前骨架铰链阵列',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_switch_current_project_component_visibility',
    description: '在当前五个零总装配范围内，通过常驻 Creo API 将指定子装配中的一个活动组件隐含，同时将另一个普通隐含组件恢复。工具按组件模型名唯一定位并核对原状态，原子执行、读回验证、重新生成和保存子装配及总装配、返回总装配，并把这两个装配模型族的旧版本移入回收站。',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        suppress_component: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        resume_component: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
      },
      required: ['expected_assembly', 'suppress_component', 'resume_component'],
      additionalProperties: false,
    },
    annotations: {
      title: '切换当前项目装配组件隐含状态',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_set_current_top_skeleton_backframe_size',
    description: '通过当前 Creo 会话的常驻桥接，把当前五个零总装配对应骨架中“后框”（特征 ID 40）的 d3、d2 直接设置为目标长宽。工具在写入前读取真实旧值并核对特征归属及关系驱动状态，随后原子修改、重生成骨架和总装配、保存、读回验证、返回总装配，并把这两个模型族的旧版本移入回收站。',
    inputSchema: {
      type: 'object',
      properties: {
        new_length: { type: 'number', exclusiveMinimum: 0, maximum: 1000000 },
        new_width: { type: 'number', exclusiveMinimum: 0, maximum: 1000000 },
      },
      required: ['new_length', 'new_width'],
      additionalProperties: false,
    },
    annotations: {
      title: '快速修改当前骨架后框长宽',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_resize_project_skeleton_box',
    description: 'Change the length and width dimensions of one verified skeleton box feature while preserving its checked height. Dimension symbols and expected old values guard against editing the wrong geometry. The result is regenerated, checked by volume and envelope, saved and displayed, and older assembly/skeleton versions are recycled.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_assembly: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_skeleton: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        new_feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'Optional renamed feature. Omit to keep feature_name unchanged.',
        },
        length_symbol: {
          type: 'string', pattern: '^[A-Za-z][A-Za-z0-9_]*$', minLength: 1, maxLength: 31,
          default: 'd3',
        },
        width_symbol: {
          type: 'string', pattern: '^[A-Za-z][A-Za-z0-9_]*$', minLength: 1, maxLength: 31,
          default: 'd2',
        },
        height_symbol: {
          type: 'string', pattern: '^[A-Za-z][A-Za-z0-9_]*$', minLength: 1, maxLength: 31,
          default: 'd1',
        },
        expected_length: { type: 'number', minimum: 0.1, maximum: 100000 },
        expected_width: { type: 'number', minimum: 0.1, maximum: 100000 },
        expected_height: { type: 'number', minimum: 0.1, maximum: 100000 },
        new_length: { type: 'number', minimum: 0.1, maximum: 100000 },
        new_width: { type: 'number', minimum: 0.1, maximum: 100000 },
      },
      required: [
        'expected_assembly', 'expected_skeleton', 'feature_name',
        'expected_length', 'expected_width', 'expected_height',
        'new_length', 'new_width',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Resize Creo skeleton box',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_modify_project_feature_dimensions',
    description: 'Modify one to eight driving dimensions of one named feature in the current expected Creo project part. Every change is guarded by dimension symbol, expected old value, feature ownership and relation status. The part is regenerated, all new values are read back, the active feature is verified, the model is saved, and older versions of only that model family are moved to the Windows Recycle Bin. Any failure before save restores every changed dimension.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'Current Creo part name without .prt.',
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'Exact owning feature name.',
        },
        modifications: {
          type: 'array', minItems: 1, maxItems: 8,
          items: {
            type: 'object',
            properties: {
              dimension_symbol: {
                type: 'string', pattern: '^[A-Za-z][A-Za-z0-9_]*$',
                minLength: 1, maxLength: 31,
              },
              expected_value: {
                type: 'number', exclusiveMinimum: 0, maximum: 1000000,
                description: 'Required current value in Creo model units.',
              },
              new_value: {
                type: 'number', exclusiveMinimum: 0, maximum: 1000000,
                description: 'New value in Creo model units.',
              },
            },
            required: ['dimension_symbol', 'expected_value', 'new_value'],
            additionalProperties: false,
          },
        },
      },
      required: ['expected_model', 'feature_name', 'modifications'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Modify Creo feature dimensions',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_constrain_active_rectangle_symmetric_to_axes',
    description: 'While the expected Creo part or skeleton is actively editing a rectangular sketch, identify exactly two vertical and two horizontal rectangle lines by guarded dimensions, create X=0 and Y=0 construction centerlines when missing, add left/right symmetry about Y and bottom/top symmetry about X, verify both constraints by API readback, and update the active sketch without accepting, exiting, regenerating, or saving it.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        expected_length: { type: 'number', minimum: 0.1, maximum: 100000 },
        expected_width: { type: 'number', minimum: 0.1, maximum: 100000 },
      },
      required: ['expected_model', 'expected_length', 'expected_width'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Constrain active rectangle symmetrically to sketch axes',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_dimension_active_rectangle_four_side_insets',
    description: 'While the expected Creo model is actively editing a rectangular sketch, remove the two guarded overall rectangle dimensions and any automatic symmetry assumptions, project four specified model edges into the sketch, create four strong line-to-line inset dimensions with the requested value and outside placement, regenerate and verify the section, then update the active sketch without accepting, exiting, regenerating the solid, or saving.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
        },
        inner_length: { type: 'number', minimum: 0.1, maximum: 100000 },
        inner_width: { type: 'number', minimum: 0.1, maximum: 100000 },
        inset: { type: 'number', minimum: 0.1, maximum: 100000 },
        model_edge_ids: {
          type: 'array', minItems: 4, maxItems: 4,
          items: { type: 'integer', minimum: 1 },
        },
        feature_name: { type: 'string', minLength: 1, maxLength: 31 },
        feature_id: { type: 'integer', minimum: 1 },
      },
      required: [
        'expected_model', 'inner_length', 'inner_width', 'inset', 'model_edge_ids',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Dimension active rectangle with four equal insets',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_create_project_general_sketch',
    description: 'Create one independent sketched datum-curve feature in the current expected Creo project part. The sketch may contain 1-32 lines, circles, and arcs on a named datum plane with an explicit orientation plane. The tool rejects duplicate feature names and invalid or degenerate geometry, verifies every created section entity, regenerates the part, requires the solid volume to remain unchanged, saves the model, and moves older versions of only that model family to the Windows Recycle Bin. A failure before save deletes the partially created sketch.',
    inputSchema: {
      type: 'object',
      properties: {
        expected_model: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'Current Creo part name without .prt.',
        },
        feature_name: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'New unique sketch feature name.',
        },
        sketch_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'Named datum plane that carries the sketch, for example FRONT.',
        },
        orientation_plane: {
          type: 'string', pattern: '^[A-Za-z0-9_-]+$', minLength: 1, maxLength: 31,
          description: 'A different named datum plane used to orient the sketch, for example TOP.',
        },
        view_side: {
          type: 'integer', enum: [1, 2], default: 1,
          description: 'Sketch-plane viewing side.',
        },
        orientation_direction: {
          type: 'string', enum: ['up', 'down', 'left', 'right'], default: 'up',
          description: 'Direction of the orientation-plane reference in sketch coordinates.',
        },
        entities: {
          type: 'array', minItems: 1, maxItems: 32,
          items: {
            type: 'object',
            properties: {
              kind: { type: 'string', enum: ['line', 'circle', 'arc'] },
              x1: { type: 'number', minimum: -1000000, maximum: 1000000 },
              y1: { type: 'number', minimum: -1000000, maximum: 1000000 },
              x2: { type: 'number', minimum: -1000000, maximum: 1000000 },
              y2: { type: 'number', minimum: -1000000, maximum: 1000000 },
              cx: { type: 'number', minimum: -1000000, maximum: 1000000 },
              cy: { type: 'number', minimum: -1000000, maximum: 1000000 },
              radius: { type: 'number', exclusiveMinimum: 0, maximum: 1000000 },
              start_angle_deg: { type: 'number', minimum: -360, maximum: 360 },
              end_angle_deg: { type: 'number', minimum: -360, maximum: 360 },
            },
            required: ['kind'],
            additionalProperties: false,
          },
        },
      },
      required: [
        'expected_model', 'feature_name',
        'sketch_plane', 'orientation_plane', 'entities',
      ],
      additionalProperties: false,
    },
    annotations: {
      title: 'Create general Creo sketch',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: false,
      openWorldHint: false,
    },
  },
  {
    name: 'creo_cleanup_project_versions',
    description: 'After a newly generated part has been verified, move every other .prt or .prt.<version> file in the current Creo working directory to the Windows Recycle Bin. The explicitly named keep file is never removed and unrelated files are untouched.',
    inputSchema: {
      type: 'object',
      properties: {
        keep_model_file: { type: 'string', minLength: 1, maxLength: 120, description: 'Exact latest Creo part file to keep in the current Creo working directory.' },
      },
      required: ['keep_model_file'],
      additionalProperties: false,
    },
    annotations: {
      title: 'Clean old Creo project versions',
      readOnlyHint: false,
      destructiveHint: true,
      idempotentHint: true,
      openWorldHint: false,
    },
  },
];

function send(message) {
  process.stdout.write(`${JSON.stringify(message)}\n`);
}

function toolResult(data, isError = false) {
  return {
    content: [{ type: 'text', text: JSON.stringify(data) }],
    structuredContent: data,
    isError,
  };
}

function ensureSimpleName(value, label) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9_-]{1,80}$/.test(value)) {
    throw new Error(`${label} 只能包含字母、数字、下划线和短横线，长度为 1-80。`);
  }
  return value;
}

function ensureCreoModelName(value, label) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9_-]{1,31}$/.test(value)) {
    throw new Error(`${label} 必须是 1-31 位，只能包含字母、数字、下划线和短横线。`);
  }
  return value;
}

function ensureCreoFeatureName(value, label) {
  if (typeof value !== 'string' ||
      Array.from(value).length < 1 || Array.from(value).length > 31 ||
      /[<>:"/\\|?*\x00-\x1F]/u.test(value) || /^[\s.]+$/u.test(value) ||
      /[\s.]$/u.test(value)) {
    throw new Error(
      `${label} must be 1-31 characters and cannot contain path or Windows-reserved characters.`);
  }
  return value;
}

function ensureString(value, label, maxLength = 80) {
  if (typeof value !== 'string' || value.length > maxLength) {
    throw new Error(`${label} 必须是长度不超过 ${maxLength} 的文本。`);
  }
  return value;
}

function resolveSafeModelFile(value) {
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error('model_file 不能为空。');
  }
  const candidate = path.resolve(safeOutputRoot, value);
  const relative = path.relative(safeOutputRoot, candidate);
  if (relative.startsWith('..') || path.isAbsolute(relative)) {
    throw new Error('model_file 必须位于 Creo 安全输出目录内。');
  }
  if (!/\.prt(?:\.\d+)?$/i.test(candidate)) {
    throw new Error('model_file 必须是 Creo .prt 文件。');
  }
  if (!fs.existsSync(candidate) || !fs.statSync(candidate).isFile()) {
    throw new Error(`找不到模型文件：${candidate}`);
  }
  return candidate;
}

let currentToolWorkingDirectory = null;

function resolveCurrentCreoWorkingDirectory() {
  if (currentToolWorkingDirectory) {
    return currentToolWorkingDirectory;
  }
  const session = runPersistentBasicModelRead();
  if (typeof session.working_directory !== 'string' ||
      session.working_directory.length === 0) {
    throw new Error('当前 Creo 会话没有可用的工作目录。');
  }
  const directory = path.resolve(session.working_directory);
  if (!fs.existsSync(directory) || !fs.statSync(directory).isDirectory()) {
    throw new Error(`当前 Creo 工作目录不存在：${directory}`);
  }
  currentToolWorkingDirectory = directory;
  return directory;
}

function resolveCurrentWorkingDirectoryModelFile(value) {
  const projectDirectory = resolveCurrentCreoWorkingDirectory();
  if (typeof value !== 'string' || value.length === 0 || value.length > 120 ||
      path.basename(value) !== value || !/^[^<>:"/\\|?*\x00-\x1F]+\.prt(?:\.\d+)?$/i.test(value)) {
    throw new Error('model_file 必须是当前 Creo 工作目录中的 .prt 或 .prt.<版本> 文件名。');
  }
  const candidate = path.resolve(projectDirectory, value);
  const relative = path.relative(projectDirectory, candidate);
  if (relative.startsWith('..') || path.isAbsolute(relative)) {
    throw new Error('model_file 解析后越出了当前 Creo 工作目录。');
  }
  if (!fs.existsSync(candidate) || !fs.statSync(candidate).isFile()) {
    throw new Error(`当前 Creo 工作目录中找不到模型文件：${candidate}`);
  }
  return candidate;
}

function resolveWorkingDirectoryPartOrAssemblyFile(workingDirectory, value) {
  if (typeof workingDirectory !== 'string' || workingDirectory.length === 0) {
    throw new Error('当前 Creo 会话没有可用的工作目录。');
  }
  const directory = path.resolve(workingDirectory);
  if (!fs.existsSync(directory) || !fs.statSync(directory).isDirectory()) {
    throw new Error(`当前 Creo 工作目录不存在：${directory}`);
  }
  if (typeof value !== 'string' || value.length === 0 || value.length > 120 ||
      path.basename(value) !== value ||
      !/^[^<>:"/\\|?*\x00-\x1F]+\.(?:prt|asm)(?:\.\d+)?$/i.test(value)) {
    throw new Error(
      'model_file 必须是当前 Creo 工作目录中的 .prt/.asm 或带版本号的直接子文件名。');
  }
  const candidate = path.resolve(directory, value);
  const relative = path.relative(directory, candidate);
  if (!relative || relative.startsWith('..') || path.isAbsolute(relative) ||
      relative.includes(path.sep)) {
    throw new Error('model_file 解析后越出了当前 Creo 工作目录。');
  }
  if (!fs.existsSync(candidate) || !fs.statSync(candidate).isFile()) {
    throw new Error(`当前 Creo 工作目录中找不到模型文件：${candidate}`);
  }
  return candidate;
}

function ensureProjectModelDoesNotExist(projectDirectory, modelName) {
  const prefix = `${modelName.toLowerCase()}.`;
  const existing = fs.readdirSync(projectDirectory)
    .some((fileName) => fileName.toLowerCase().startsWith(prefix));
  if (existing) {
    throw new Error(`安全停止：当前 Creo 工作目录中已存在 ${modelName} 的文件，拒绝覆盖。`);
  }
}

function ensureProjectModelTypeDoesNotExist(projectDirectory, modelName, extension) {
  const normalizedExtension = extension.toLowerCase().startsWith('.')
    ? extension.toLowerCase()
    : `.${extension.toLowerCase()}`;
  const prefix = `${modelName.toLowerCase()}${normalizedExtension}`;
  const existing = fs.readdirSync(projectDirectory).some((fileName) => {
    const normalized = fileName.toLowerCase();
    return normalized === prefix || normalized.startsWith(`${prefix}.`);
  });
  if (existing) {
    throw new Error(
      `安全停止：当前 Creo 工作目录中已存在 ${modelName}${normalizedExtension}，拒绝覆盖。`);
  }
}

function runProjectCleanup(projectDirectory, keepFile) {
  if (!fs.existsSync(cleanupProjectVersionsScript)) {
    throw new Error(`缺少项目清理脚本：${cleanupProjectVersionsScript}`);
  }
  const completed = spawnSync(
    'powershell.exe',
    [
      '-NoLogo',
      '-NoProfile',
      '-NonInteractive',
      '-ExecutionPolicy', 'Bypass',
      '-File', cleanupProjectVersionsScript,
      '-ProjectRoot', path.dirname(projectDirectory),
      '-ProjectDirectory', projectDirectory,
      '-KeepFile', keepFile,
    ],
    {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 60000,
      maxBuffer: 4 * 1024 * 1024,
    }
  );
  if (completed.error) {
    throw completed.error;
  }
  if (completed.status !== 0) {
    const detail = {
      ok: false,
      stage: 'cleanup_project_versions',
      exit_code: completed.status,
      stderr: (completed.stderr || '').trim(),
    };
    const error = new Error('Creo 项目旧版本清理未完成。');
    error.detail = detail;
    throw error;
  }
  const output = (completed.stdout || '').trim().replace(/^\uFEFF/, '');
  if (!output) {
    throw new Error('Creo 项目清理脚本没有返回验证结果。');
  }
  return JSON.parse(output);
}

function ensureFiniteNumber(value, label, minimum = -100000, maximum = 100000) {
  const number = Number(value);
  if (!Number.isFinite(number) || number < minimum || number > maximum) {
    throw new Error(`${label} 必须是 ${minimum} 到 ${maximum} 之间的有限数值。`);
  }
  return number;
}

function ensureDimensionSymbol(value, label, fallback) {
  const symbol = value === undefined ? fallback : value;
  if (typeof symbol !== 'string' || !/^[A-Za-z][A-Za-z0-9_]{0,30}$/.test(symbol)) {
    throw new Error(`${label} 必须是 1-31 位 Creo 尺寸符号。`);
  }
  return symbol;
}

function resolveReturnedProjectModel(projectDirectory, value, allowedExtensions) {
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error('Creo 桥接程序没有返回已保存的模型路径。');
  }
  const candidate = path.resolve(value);
  const relative = path.relative(projectDirectory, candidate);
  if (!relative || relative.startsWith('..') || path.isAbsolute(relative) ||
      path.dirname(candidate).toLowerCase() !== projectDirectory.toLowerCase()) {
    throw new Error(`桥接程序返回的模型越出了当前 Creo 工作目录：${candidate}`);
  }
  const extensionPattern = allowedExtensions.map((item) => item.replace('.', '')).join('|');
  const filePattern = new RegExp(
    `^[A-Za-z0-9_-]{1,80}\\.(?:${extensionPattern})(?:\\.\\d+)?$`, 'i');
  if (!filePattern.test(path.basename(candidate))) {
    throw new Error(`桥接程序返回了不支持的 Creo 文件：${candidate}`);
  }
  if (!fs.existsSync(candidate) || !fs.statSync(candidate).isFile()) {
    throw new Error(`桥接程序报告的已保存文件不存在：${candidate}`);
  }
  return candidate;
}

function runProjectModelVersionCleanup(projectDirectory, keepFiles) {
  if (!fs.existsSync(cleanupProjectModelVersionsScript)) {
    throw new Error(`缺少项目模型版本清理脚本：${cleanupProjectModelVersionsScript}`);
  }
  const keepNames = keepFiles.map((keepFile) => {
    const resolved = resolveReturnedProjectModel(
      projectDirectory, keepFile, ['.prt', '.asm']);
    return path.basename(resolved);
  });
  const keepFilesBase64 = Buffer.from(
    JSON.stringify(keepNames), 'utf8').toString('base64');
  const completed = spawnSync(
    'powershell.exe',
    [
      '-NoLogo',
      '-NoProfile',
      '-NonInteractive',
      '-ExecutionPolicy', 'Bypass',
      '-File', cleanupProjectModelVersionsScript,
      '-ProjectRoot', path.dirname(projectDirectory),
      '-ProjectDirectory', projectDirectory,
      '-KeepFilesBase64', keepFilesBase64,
    ],
    {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 60000,
      maxBuffer: 4 * 1024 * 1024,
    }
  );
  if (completed.error) {
    throw completed.error;
  }
  if (completed.status !== 0) {
    const error = new Error('Creo 项目模型旧版本清理未完成。');
    error.detail = {
      ok: false,
      stage: 'cleanup_project_model_versions',
      exit_code: completed.status,
      stderr: (completed.stderr || '').trim(),
    };
    throw error;
  }
  const output = (completed.stdout || '').trim().replace(/^\uFEFF/, '');
  if (!output) {
    throw new Error('Creo 项目模型清理脚本没有返回验证结果。');
  }
  return JSON.parse(output);
}

function withProjectModelCleanup(projectDirectory, result, keepFiles) {
  const cleanup = runProjectModelVersionCleanup(projectDirectory, keepFiles);
  return { ...result, version_cleanup: cleanup };
}

function runBridge(executableName, args) {
  const executable = path.join(bridgeRoot, executableName);
  if (!fs.existsSync(executable)) {
    throw new Error(`缺少 Creo 桥接程序：${executable}`);
  }

  const runtimeDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'creo-mcp-'));
  const resultPath = path.join(runtimeDirectory, 'result.json');
  const env = {
    ...process.env,
    PRO_COMM_MSG_EXE: proCommMsgExe,
    PATH: `${creoRuntimeObj};${creoRuntimeLib};${process.env.PATH || ''}`,
  };

  try {
    const completed = spawnSync(
      executable,
      [resultPath, ...args],
      {
        encoding: 'utf8',
        env,
        windowsHide: true,
        timeout: 60000,
        maxBuffer: 4 * 1024 * 1024,
      }
    );

    let data = null;
    if (fs.existsSync(resultPath)) {
      data = JSON.parse(fs.readFileSync(resultPath, 'utf8').replace(/^\uFEFF/, ''));
    }
    if (completed.error) {
      throw completed.error;
    }
    if (completed.status !== 0) {
      const detail = data || {
        ok: false,
        exit_code: completed.status,
        stderr: (completed.stderr || '').trim(),
      };
      const error = new Error(`Creo 桥接操作未完成（退出码 ${completed.status}）。`);
      error.detail = detail;
      throw error;
    }
    return data || { ok: true };
  } finally {
    fs.rmSync(runtimeDirectory, { recursive: true, force: true });
  }
}

function runResidentBackframeResize(newLength, newWidth) {
  if (!fs.existsSync(residentBackframeResizeScript)) {
    throw new Error(`缺少 Creo 常驻尺寸脚本：${residentBackframeResizeScript}`);
  }
  const completed = spawnSync(
    'powershell.exe',
    [
      '-NoLogo', '-NoProfile', '-NonInteractive',
      '-ExecutionPolicy', 'Bypass',
      '-File', residentBackframeResizeScript,
      '-NewLength', String(newLength),
      '-NewWidth', String(newWidth),
    ],
    {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 60000,
      maxBuffer: 4 * 1024 * 1024,
    }
  );
  const output = (completed.stdout || '').trim().replace(/^\uFEFF/, '');
  let data = null;
  if (output) {
    try { data = JSON.parse(output); } catch (_) { data = null; }
  }
  if (completed.error) throw completed.error;
  if (completed.status !== 0 || !data || !data.Ok) {
    const error = new Error('Creo 常驻骨架尺寸修改未完成。');
    error.detail = data || {
      ok: false,
      exit_code: completed.status,
      stdout: output,
      stderr: (completed.stderr || '').trim(),
    };
    throw error;
  }
  return data;
}

function runResidentComponentSwitch(assembly, suppressComponent, resumeComponent) {
  if (!fs.existsSync(residentComponentSwitchScript)) {
    throw new Error(`缺少 Creo 常驻组件切换脚本：${residentComponentSwitchScript}`);
  }
  const completed = spawnSync(
    'powershell.exe',
    [
      '-NoLogo', '-NoProfile', '-NonInteractive',
      '-ExecutionPolicy', 'Bypass',
      '-File', residentComponentSwitchScript,
      '-Assembly', assembly,
      '-SuppressComponent', suppressComponent,
      '-ResumeComponent', resumeComponent,
    ],
    {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 60000,
      maxBuffer: 4 * 1024 * 1024,
    }
  );
  const output = (completed.stdout || '').trim().replace(/^\uFEFF/, '');
  let data = null;
  if (output) {
    try { data = JSON.parse(output); } catch (_) { data = null; }
  }
  if (completed.error) throw completed.error;
  if (completed.status !== 0 || !data || !data.Ok) {
    const error = new Error('Creo 常驻组件隐含状态切换未完成。');
    error.detail = data || {
      ok: false,
      exit_code: completed.status,
      stdout: output,
      stderr: (completed.stderr || '').trim(),
    };
    throw error;
  }
  return data;
}

function runResidentHingePattern(newCount, newSpacing) {
  if (!fs.existsSync(residentHingePatternScript)) {
    throw new Error(`缺少 Creo 常驻铰链阵列脚本：${residentHingePatternScript}`);
  }
  const completed = spawnSync(
    'powershell.exe',
    [
      '-NoLogo', '-NoProfile', '-NonInteractive',
      '-ExecutionPolicy', 'Bypass',
      '-File', residentHingePatternScript,
      '-NewCount', String(newCount),
      '-NewSpacing', String(newSpacing),
    ],
    {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 60000,
      maxBuffer: 4 * 1024 * 1024,
    }
  );
  const output = (completed.stdout || '').trim().replace(/^\uFEFF/, '');
  let data = null;
  if (output) {
    try { data = JSON.parse(output); } catch (_) { data = null; }
  }
  if (completed.error) throw completed.error;
  if (completed.status !== 0 || !data || !data.Ok) {
    const error = new Error('Creo 常驻铰链阵列修改未完成。');
    error.detail = data || {
      ok: false,
      exit_code: completed.status,
      stdout: output,
      stderr: (completed.stderr || '').trim(),
    };
    throw error;
  }
  return data;
}

function sleepSync(milliseconds) {
  Atomics.wait(
    new Int32Array(new SharedArrayBuffer(4)), 0, 0, milliseconds);
}

function requestPersistentFlatWall(message) {
  let descriptor;
  try {
    descriptor = fs.openSync(persistentFlatWallPipe, 'r+');
    fs.writeSync(descriptor, Buffer.from(`${message}\n`, 'utf8'));
    const response = Buffer.alloc(8192);
    const bytesRead = fs.readSync(descriptor, response, 0, response.length, null);
    if (bytesRead < 1) {
      throw new Error('Persistent flat-wall bridge returned an empty response.');
    }
    return JSON.parse(response.subarray(0, bytesRead).toString('utf8').trim());
  } finally {
    if (descriptor !== undefined) {
      fs.closeSync(descriptor);
    }
  }
}

function processIsAlive(processId) {
  if (!Number.isInteger(processId) || processId < 1) {
    return false;
  }
  try {
    process.kill(processId, 0);
    return true;
  } catch (_) {
    return false;
  }
}

function terminateProcessTree(processId) {
  if (!Number.isInteger(processId) || processId < 1) {
    return false;
  }
  const taskkill = path.join(
    process.env.SystemRoot || 'C:\\Windows', 'System32', 'taskkill.exe');
  const terminated = spawnSync(
    taskkill,
    ['/PID', String(processId), '/T', '/F'],
    {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 3000,
      maxBuffer: 1024 * 1024,
    }
  );
  return !terminated.error && terminated.status === 0;
}

function discoverCreoSessions() {
  if (!fs.existsSync(nmsQueryExe)) {
    return [];
  }
  const queried = spawnSync(
    nmsQueryExe,
    ['-host', os.hostname(), '-query', '_ProcessName=Pro/Engineer'],
    {
      encoding: 'utf8',
      windowsHide: true,
      timeout: 3000,
      maxBuffer: 1024 * 1024,
    }
  );
  if (queried.error || queried.status !== 0) {
    return [];
  }
  const records = [];
  const blocks = String(queried.stdout || '').match(/\{[\s\S]*?\}/g) || [];
  for (const block of blocks) {
    const fields = {};
    for (const line of block.split(/\r?\n/)) {
      const matched = line.match(/^\s*_([^:]+):\s*(.*?)\s*$/);
      if (matched) {
        fields[matched[1]] = matched[2];
      }
    }
    const processId = Number(fields.ProcessID);
    const required = [
      fields.Display,
      fields.RPC_Peer_address_version,
      fields.RPC_Peer_channel_address_type,
      fields.RPC_number,
      fields.RPC_version,
      fields.RPC_inetaddr,
    ];
    // nmsq only emits an "Associated process" record for a registered Creo
    // session. The MCP job can be denied PROCESS_QUERY access to an external
    // xtop.exe even when both programs run as the same desktop user, so do not
    // reject a valid PTC record based on process.kill(pid, 0). The bridge's
    // exact-session ProEngineerConnect call remains the authoritative health
    // check.
    if (!Number.isInteger(processId) || processId < 1 ||
        required.some((value) => !value)) {
      continue;
    }
    const connectId = [
      `host:${fields.Display}`,
      `addr_ver:${fields.RPC_Peer_address_version}`,
      `addr_type:${fields.RPC_Peer_channel_address_type}`,
      `rpcnum:${fields.RPC_number}`,
      `rpcversion:${fields.RPC_version}`,
      `netaddr:${fields.RPC_inetaddr}`,
    ].join(':');
    records.push({
      process_id: processId,
      display: fields.Display,
      user: fields.UserID || null,
      connect_id: connectId,
    });
  }
  return records;
}

function ensurePersistentFlatWallBridge() {
  try {
    const ping = requestPersistentFlatWall('PING');
    if (ping.ok && ping.persistent && ping.connected) {
      return { ...ping, resident_started: false };
    }
  } catch (_) {
    // The worker is not running yet; start it below.
  }
  if (!fs.existsSync(persistentFlatWallBridge)) {
    throw new Error(
      `Persistent Creo flat-wall bridge is missing: ${persistentFlatWallBridge}`);
  }
  const env = {
    ...process.env,
    PRO_COMM_MSG_EXE: proCommMsgExe,
    CREO_CONNECT_TIMEOUT_SEC: '6',
    PATH: `${creoRuntimeObj};${creoRuntimeLib};${process.env.PATH || ''}`,
  };
  let lastError;
  const launchDiagnostics = [];
  const maximumLaunchAttempts = 2;
  for (let launchAttempt = 1;
    launchAttempt <= maximumLaunchAttempts;
    launchAttempt += 1) {
    const sessions = discoverCreoSessions();
    if (sessions.length === 0) {
      throw new Error(
        'No live Creo Parametric session is registered with the PTC name server.');
    }
    // A normal desktop workflow has one live Creo process. If more than one is
    // present, prefer the most recently assigned process id and bind the worker
    // to its exact RPC connection id instead of allowing a random attachment.
    sessions.sort((left, right) => right.process_id - left.process_id);
    const targetSession = sessions[0];
    const startupStatusPath = path.join(
      os.tmpdir(),
      `creo-resident-start-${process.pid}-${Date.now()}-${launchAttempt}.json`);
    const launchEnv = {
      ...env,
      CREO_CONNECT_ID: targetSession.connect_id,
      CREO_CONNECT_DISPLAY: targetSession.display,
      CREO_CONNECT_USER: targetSession.user || '',
      CREO_BRIDGE_STARTUP_STATUS: startupStatusPath,
    };
    const launchedAt = Date.now();
    const worker = spawn(persistentFlatWallBridge, [], {
      cwd: path.dirname(persistentFlatWallBridge),
      // A resident Pro/TOOLKIT worker must not share the MCP host's process
      // group.  In the Codex desktop host, a non-detached child can be torn
      // down before it creates its named pipe even though the same EXE works
      // when started interactively.
      detached: true,
      env: launchEnv,
      stdio: 'ignore',
      windowsHide: true,
    });
    worker.unref();
    const diagnostic = {
      launch_attempt: launchAttempt,
      process_id: worker.pid || null,
      target_creo_process_id: targetSession.process_id,
      detached: true,
    };
    launchDiagnostics.push(diagnostic);

    // A healthy local cold start normally takes about five seconds. Creo 10 can
    // take roughly fifteen seconds to return PRO_TK_E_BUSY even when the API
    // timeout is lower, so wait long enough to capture the real status file.
    // Retry only a fast PRO_TK_E_NOT_FOUND; repeating a BUSY initialization
    // merely occupies the single-instance mutex and adds latency.
    for (let pollAttempt = 0; pollAttempt < 170; pollAttempt += 1) {
      sleepSync(100);
      try {
        const ping = requestPersistentFlatWall('PING');
        if (ping.ok && ping.persistent && ping.connected) {
          diagnostic.ready_after_ms = Date.now() - launchedAt;
          try {
            fs.rmSync(startupStatusPath, { force: true });
          } catch (_) {
            // Diagnostic cleanup is best effort only.
          }
          return {
            ...ping,
            resident_started: true,
            launch_attempt: launchAttempt,
            launch_diagnostics: launchDiagnostics,
          };
        }
        lastError = new Error(
          `Unexpected persistent bridge ping: ${JSON.stringify(ping)}`);
      } catch (error) {
        lastError = error;
      }
      if (worker.pid && !processIsAlive(worker.pid)) {
        diagnostic.exited_early = true;
        diagnostic.elapsed_ms = Date.now() - launchedAt;
        if (fs.existsSync(startupStatusPath)) {
          try {
            diagnostic.startup_status = JSON.parse(
              fs.readFileSync(startupStatusPath, 'utf8').replace(/^\uFEFF/, ''));
          } catch (_) {
            diagnostic.startup_status = { stage: 'invalid_status_file' };
          }
        }
        lastError = new Error(
          `Persistent bridge worker ${worker.pid} exited before pipe readiness` +
          `${diagnostic.startup_status ? `: ${JSON.stringify(diagnostic.startup_status)}` : '.'}`);
        break;
      }
    }
    diagnostic.elapsed_ms = Date.now() - launchedAt;
    diagnostic.still_alive = processIsAlive(worker.pid);
    if (diagnostic.still_alive) {
      diagnostic.terminated_after_timeout = terminateProcessTree(worker.pid);
      sleepSync(150);
      diagnostic.still_alive = processIsAlive(worker.pid);
    }
    if (!diagnostic.startup_status && fs.existsSync(startupStatusPath)) {
      try {
        diagnostic.startup_status = JSON.parse(
          fs.readFileSync(startupStatusPath, 'utf8').replace(/^\uFEFF/, ''));
      } catch (_) {
        diagnostic.startup_status = { stage: 'invalid_status_file' };
      }
    }
    try {
      fs.rmSync(startupStatusPath, { force: true });
    } catch (_) {
      // Diagnostic cleanup is best effort only.
    }
    const startupErrorCode = diagnostic.startup_status?.error_code;
    if (startupErrorCode === -38 ||
        (startupErrorCode !== undefined && startupErrorCode !== -4)) {
      break;
    }
    if (launchAttempt < maximumLaunchAttempts) {
      sleepSync(250);
    }
  }
  throw new Error(
    `Persistent Creo flat-wall bridge did not become ready after ` +
    `${maximumLaunchAttempts} launch attempts: ` +
    `${lastError?.message || 'timeout'}; diagnostics=` +
    `${JSON.stringify(launchDiagnostics)}`);
}

function runPersistentDisplayModel(modelFile, expectedModel) {
  const runtimeDirectory = fs.mkdtempSync(
    path.join(os.tmpdir(), 'creo-mcp-display-'));
  const resultPath = path.join(runtimeDirectory, 'result.json');
  const totalStarted = process.hrtime.bigint();
  try {
    const connectionStarted = process.hrtime.bigint();
    const bridge = ensurePersistentFlatWallBridge();
    const connectionFinished = process.hrtime.bigint();
    const displayStarted = process.hrtime.bigint();
    const response = requestPersistentFlatWall(
      `DISPLAY|${resultPath}|${modelFile}|${expectedModel}`);
    const displayFinished = process.hrtime.bigint();
    let data = null;
    if (fs.existsSync(resultPath)) {
      data = JSON.parse(
        fs.readFileSync(resultPath, 'utf8').replace(/^\uFEFF/, ''));
    }
    if (!response.ok || response.exit_code !== 0 || !data?.ok) {
      const error = new Error(
        `Creo 常驻显示命令未完成（退出码 ${response.exit_code}）。`);
      error.detail = data || response;
      throw error;
    }
    const milliseconds = (end, start) =>
      Number(end - start) / 1_000_000;
    return {
      ...data,
      persistent_bridge: true,
      resident_started: Boolean(bridge.resident_started),
      connection_reused: !bridge.resident_started,
      automatic_disconnect: 'when_current_creo_session_exits',
      timings_ms: {
        bridge_start_or_reuse: Number(milliseconds(
          connectionFinished, connectionStarted).toFixed(1)),
        display_model: Number(milliseconds(
          displayFinished, displayStarted).toFixed(1)),
        total: Number(milliseconds(
          displayFinished, totalStarted).toFixed(1)),
      },
    };
  } finally {
    fs.rmSync(runtimeDirectory, { recursive: true, force: true });
  }
}

function runPersistentBasicModelRead() {
  const runtimeDirectory = fs.mkdtempSync(
    path.join(os.tmpdir(), 'creo-mcp-basic-'));
  const resultPath = path.join(runtimeDirectory, 'result.json');
  const totalStarted = process.hrtime.bigint();
  try {
    const connectionStarted = process.hrtime.bigint();
    const bridge = ensurePersistentFlatWallBridge();
    const connectionFinished = process.hrtime.bigint();
    const readStarted = process.hrtime.bigint();
    const response = requestPersistentFlatWall(`BASIC|${resultPath}`);
    const readFinished = process.hrtime.bigint();
    let data = null;
    if (fs.existsSync(resultPath)) {
      data = JSON.parse(
        fs.readFileSync(resultPath, 'utf8').replace(/^\uFEFF/, ''));
    }
    if (!response.ok || response.exit_code !== 0 || !data?.ok) {
      const error = new Error(
        `Creo 首次快速握手未完成（退出码 ${response.exit_code}）。`);
      error.detail = data || response;
      throw error;
    }
    const milliseconds = (end, start) =>
      Number(end - start) / 1_000_000;
    return {
      ...data,
      persistent_bridge: true,
      resident_started: Boolean(bridge.resident_started),
      connection_reused: !bridge.resident_started,
      automatic_disconnect: 'when_current_creo_session_exits',
      timings_ms: {
        bridge_start_or_reuse: Number(milliseconds(
          connectionFinished, connectionStarted).toFixed(1)),
        basic_model_read: Number(milliseconds(
          readFinished, readStarted).toFixed(1)),
        total: Number(milliseconds(
          readFinished, totalStarted).toFixed(1)),
      },
    };
  } finally {
    fs.rmSync(runtimeDirectory, { recursive: true, force: true });
  }
}

function runPersistentFlatWall(args) {
  const runtimeDirectory = fs.mkdtempSync(
    path.join(os.tmpdir(), 'creo-mcp-flat-wall-'));
  const resultPath = path.join(runtimeDirectory, 'result.json');
  const commandPath = path.join(runtimeDirectory, 'command.txt');
  try {
    fs.writeFileSync(
      commandPath,
      `${[resultPath, ...args].join('\n')}\n`,
      'utf8');
    ensurePersistentFlatWallBridge();
    const response = requestPersistentFlatWall(commandPath);
    let data = null;
    if (fs.existsSync(resultPath)) {
      data = JSON.parse(
        fs.readFileSync(resultPath, 'utf8').replace(/^\uFEFF/, ''));
    }
    if (!response.ok || response.exit_code !== 0) {
      const error = new Error(
        `Persistent Creo flat-wall operation did not complete (exit code ${response.exit_code}).`);
      error.detail = data || response;
      throw error;
    }
    return {
      ...(data || { ok: true }),
      persistent_bridge: true,
      connection_reused: true,
    };
  } finally {
    fs.rmSync(runtimeDirectory, { recursive: true, force: true });
  }
}

function handleToolCall(name, args) {
  // Every formal tool resolves files from the live directory selected in Creo.
  // Reset once per call so changing Creo's work directory takes effect immediately.
  currentToolWorkingDirectory = null;
  if (name === 'creo_set_current_hinge_pattern') {
    const newCount = Number(args.new_count);
    if (!Number.isInteger(newCount) || newCount < 2 || newCount > 1000) {
      throw new Error('new_count 必须是 2 到 1000 之间的整数。');
    }
    const newSpacing = ensureFiniteNumber(
      args.new_spacing, 'new_spacing', 0.000001, 1000000);
    return toolResult(runResidentHingePattern(newCount, newSpacing));
  }

  if (name === 'creo_switch_current_project_component_visibility') {
    const assembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const suppressComponent = ensureCreoModelName(
      args.suppress_component, 'suppress_component');
    const resumeComponent = ensureCreoModelName(
      args.resume_component, 'resume_component');
    if (suppressComponent.toLowerCase() === resumeComponent.toLowerCase()) {
      throw new Error('suppress_component 与 resume_component 不能相同。');
    }
    return toolResult(runResidentComponentSwitch(
      assembly, suppressComponent, resumeComponent));
  }

  if (name === 'creo_set_current_top_skeleton_backframe_size') {
    const newLength = ensureFiniteNumber(
      args.new_length, 'new_length', 0.000001, 1000000);
    const newWidth = ensureFiniteNumber(
      args.new_width, 'new_width', 0.000001, 1000000);
    return toolResult(runResidentBackframeResize(newLength, newWidth));
  }

  if (name === 'creo_constrain_active_rectangle_symmetric_to_axes') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const expectedLength = ensureFiniteNumber(
      args.expected_length, 'expected_length', 0.1, 100000);
    const expectedWidth = ensureFiniteNumber(
      args.expected_width, 'expected_width', 0.1, 100000);
    return toolResult(runBridge('creo_active_sketch_axis_symmetry_bridge.exe', [
      expectedModel,
      String(expectedLength),
      String(expectedWidth),
    ]));
  }

  if (name === 'creo_dimension_active_rectangle_four_side_insets') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const innerLength = ensureFiniteNumber(
      args.inner_length, 'inner_length', 0.1, 100000);
    const innerWidth = ensureFiniteNumber(
      args.inner_width, 'inner_width', 0.1, 100000);
    const inset = ensureFiniteNumber(args.inset, 'inset', 0.1, 100000);
    if (!Array.isArray(args.model_edge_ids) || args.model_edge_ids.length !== 4 ||
        args.model_edge_ids.some(id => !Number.isInteger(id) || id < 1)) {
      throw new Error('model_edge_ids must contain exactly four positive integers.');
    }
    const bridgeArgs = [
      expectedModel,
      String(innerLength),
      String(innerWidth),
      String(inset),
      ...args.model_edge_ids.map(String),
    ];
    const closedFeatureMode = args.feature_name !== undefined || args.feature_id !== undefined;
    let projectDirectory;
    if (closedFeatureMode) {
      if (args.feature_name === undefined || args.feature_id === undefined) {
        throw new Error('Closed-feature mode requires feature_name and feature_id.');
      }
      const featureName = ensureCreoFeatureName(args.feature_name, 'feature_name');
      const featureId = Number(args.feature_id);
      if (!Number.isInteger(featureId) || featureId < 1) {
        throw new Error('feature_id must be a positive integer.');
      }
      projectDirectory = resolveCurrentCreoWorkingDirectory();
      runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
      bridgeArgs.push(featureName, String(featureId));
    }
    const result = runBridge(
      'creo_active_sketch_four_inset_dimensions_bridge.exe', bridgeArgs);
    if (!closedFeatureMode) return toolResult(result);
    const escapedModel = expectedModel.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const matcher = new RegExp(`^${escapedModel}\\.prt(?:\\.(\\d+))?$`, 'i');
    const candidates = fs.readdirSync(projectDirectory)
      .map(fileName => ({ fileName, match: fileName.match(matcher) }))
      .filter(item => item.match)
      .sort((a, b) => Number(b.match[1] || 0) - Number(a.match[1] || 0));
    if (candidates.length === 0) {
      throw new Error('Saved skeleton file was not found after redefine.');
    }
    result.saved_file = path.join(projectDirectory, candidates[0].fileName);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_get_current_model') {
    try {
      return toolResult(runBridge('creo_bridge.exe', []));
    } catch (error) {
      if (error.detail && error.detail.stage === 'current_model' && error.detail.error_code === -8) {
        return toolResult({
          ok: true,
          creo_running: true,
          model_open: false,
          readonly: true,
        });
      }
      throw error;
    }
  }

  if (name === 'creo_start_resident_and_get_basic_model') {
    return toolResult(runPersistentBasicModelRead());
  }

  if (name === 'creo_set_project_working_directory') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    return toolResult({
      ok: true,
      readonly: true,
      changed: false,
      working_directory: projectDirectory,
      directory_source: 'current_creo_session',
    });
  }

  if (name === 'creo_create_project_part') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const modelName = ensureCreoModelName(args.model_name, 'model_name');
    if (!fs.existsSync(creoPartTemplate) || !fs.statSync(creoPartTemplate).isFile()) {
      throw new Error(`找不到 Creo 毫米制零件模板：${creoPartTemplate}`);
    }
    ensureProjectModelDoesNotExist(projectDirectory, modelName);
    return toolResult(runBridge('creo_template_copy_bridge.exe', [
      creoPartTemplate,
      modelName,
      projectDirectory,
      'KEEP_WORKDIR',
    ]));
  }

  if (name === 'creo_create_project_sheetmetal_part') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const modelName = ensureCreoModelName(args.model_name, 'model_name');
    if (!fs.existsSync(creoSheetmetalTemplate) ||
        !fs.statSync(creoSheetmetalTemplate).isFile()) {
      throw new Error(`Creo sheet-metal template not found: ${creoSheetmetalTemplate}`);
    }
    ensureProjectModelTypeDoesNotExist(projectDirectory, modelName, '.prt');
    const result = runBridge('creo_template_copy_bridge.exe', [
      creoSheetmetalTemplate,
      modelName,
      projectDirectory,
      'KEEP_WORKDIR',
    ]);
    if (result.model_subtype_code !== 4) {
      const error = new Error('Created Creo model is not a native sheet-metal part.');
      error.detail = { ...result, stage: 'sheetmetal_subtype_guard' };
      throw error;
    }
    result.model_subtype = 'sheetmetal';
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_create_project_sheetmetal_planar_wall') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const modelName = ensureCreoModelName(args.model_name, 'model_name');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const length = Number(args.length);
    const width = Number(args.width);
    const thickness = Number(args.thickness);
    if (![length, width].every((value) =>
      Number.isFinite(value) && value >= 0.1 && value <= 100000)) {
      throw new Error('length and width must be finite numbers from 0.1 to 100000 mm.');
    }
    if (!Number.isFinite(thickness) || thickness < 0.01 || thickness > 1000) {
      throw new Error('thickness must be a finite number from 0.01 to 1000 mm.');
    }
    if (!fs.existsSync(creoSheetmetalTemplate) ||
        !fs.statSync(creoSheetmetalTemplate).isFile()) {
      throw new Error(`Creo sheet-metal template not found: ${creoSheetmetalTemplate}`);
    }
    ensureProjectModelDoesNotExist(projectDirectory, modelName);
    const result = runBridge('creo_sheetmetal_plate_bridge.exe', [
      modelName,
      featureName,
      String(length),
      String(width),
      String(thickness),
      projectDirectory,
      creoSheetmetalTemplate,
    ]);
    const savedPath = resolveReturnedProjectModel(
      projectDirectory, result.saved_file, ['.prt']);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [savedPath]));
  }

  if (name === 'creo_create_project_sheetmetal_planar_wall_current') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const length = ensureFiniteNumber(args.length, 'length', 0.1, 100000);
    const width = ensureFiniteNumber(args.width, 'width', 0.1, 100000);
    const thickness = ensureFiniteNumber(
      args.thickness, 'thickness', 0.01, 1000);
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_sheetmetal_plate_bridge.exe', [
      expectedModel,
      featureName,
      String(length),
      String(width),
      String(thickness),
    ]);
    const savedPath = resolveReturnedProjectModel(
      projectDirectory, result.saved_file, ['.prt']);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [savedPath]));
  }

  if (name === 'creo_link_project_sheetmetal_wall_to_skeleton') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const expectedSkeleton = ensureCreoModelName(
      args.expected_skeleton, 'expected_skeleton');
    const expectedSheetmetal = ensureCreoModelName(
      args.expected_sheetmetal, 'expected_sheetmetal');
    const skeletonFeature = ensureCreoModelName(
      args.skeleton_feature_name, 'skeleton_feature_name');
    const sheetProfileFeature = ensureCreoModelName(
      args.sheetmetal_profile_feature_name, 'sheetmetal_profile_feature_name');
    const componentFeatureId = Number(args.sheetmetal_component_feature_id);
    const profileFeatureId = Number(args.sheetmetal_profile_feature_id);
    if (![componentFeatureId, profileFeatureId].every((value) =>
      Number.isInteger(value) && value >= 1 && value <= 1000000)) {
      throw new Error('Component and profile feature IDs must be integers from 1 to 1000000.');
    }
    const dimensionSymbols = [
      args.skeleton_length_symbol,
      args.skeleton_width_symbol,
      args.sheetmetal_length_symbol,
      args.sheetmetal_width_symbol,
    ];
    if (!dimensionSymbols.every((value) =>
      typeof value === 'string' && /^d[0-9]+$/.test(value))) {
      throw new Error('Dimension symbols must use Creo d<number> notation.');
    }
    const expectedLength = ensureFiniteNumber(
      args.expected_length, 'expected_length', 0.1, 100000);
    const expectedWidth = ensureFiniteNumber(
      args.expected_width, 'expected_width', 0.1, 100000);
    const thickness = ensureFiniteNumber(
      args.thickness, 'thickness', 0.01, 1000);
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_sheetmetal_skeleton_link_bridge.exe', [
      expectedAssembly,
      expectedSkeleton,
      expectedSheetmetal,
      String(componentFeatureId),
      skeletonFeature,
      sheetProfileFeature,
      String(profileFeatureId),
      ...dimensionSymbols,
      String(expectedLength),
      String(expectedWidth),
      String(thickness),
    ]);
    const assemblySavedPath = resolveReturnedProjectModel(
      projectDirectory, result.assembly_saved_file, ['.asm']);
    const skeletonSavedPath = resolveReturnedProjectModel(
      projectDirectory, result.skeleton_saved_file, ['.prt']);
    const sheetmetalSavedPath = resolveReturnedProjectModel(
      projectDirectory, result.sheetmetal_saved_file, ['.prt']);
    return toolResult(withProjectModelCleanup(projectDirectory, result, [
      assemblySavedPath,
      skeletonSavedPath,
      sheetmetalSavedPath,
    ]));
  }

  if (name === 'creo_reverse_project_sheetmetal_planar_wall_to_positive_z') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const expectedSheetmetal = ensureCreoModelName(
      args.expected_sheetmetal, 'expected_sheetmetal');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const featureId = Number(args.feature_id);
    if (!Number.isInteger(featureId) || featureId < 1 || featureId > 1000000) {
      throw new Error('feature_id must be an integer from 1 to 1000000.');
    }
    const dimensionSymbols = [
      args.thickness_symbol,
      args.length_symbol,
      args.width_symbol,
    ];
    if (!dimensionSymbols.every((value) =>
      typeof value === 'string' && /^d[0-9]+$/.test(value))) {
      throw new Error('Dimension symbols must use Creo d<number> notation.');
    }
    const expectedLength = ensureFiniteNumber(
      args.expected_length, 'expected_length', 0.1, 100000);
    const expectedWidth = ensureFiniteNumber(
      args.expected_width, 'expected_width', 0.1, 100000);
    const thickness = ensureFiniteNumber(
      args.thickness, 'thickness', 0.01, 1000);
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge(
      'creo_sheetmetal_reverse_wall_direction_bridge.exe', [
        expectedAssembly,
        expectedSheetmetal,
        featureName,
        String(featureId),
        ...dimensionSymbols,
        String(expectedLength),
        String(expectedWidth),
        String(thickness),
      ]);
    const assemblySavedPath = resolveReturnedProjectModel(
      projectDirectory, result.assembly_saved_file, ['.asm']);
    const sheetmetalSavedPath = resolveReturnedProjectModel(
      projectDirectory, result.sheetmetal_saved_file, ['.prt']);
    return toolResult(withProjectModelCleanup(projectDirectory, result, [
      assemblySavedPath,
      sheetmetalSavedPath,
    ]));
  }

  if (name === 'creo_create_project_sheetmetal_flat_wall') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const edgeId = Number(args.edge_id);
    const expectedLength = Number(args.expected_edge_length);
    const angle = args.angle === undefined ? 90 : Number(args.angle);
    const wallHeight = Number(args.wall_height);
    if (!Number.isInteger(edgeId) || edgeId < 1 || edgeId > 100000000) {
      throw new Error('edge_id is invalid.');
    }
    if (!Number.isFinite(expectedLength) ||
        expectedLength < 0.01 || expectedLength > 100000) {
      throw new Error('expected_edge_length must be from 0.01 to 100000 mm.');
    }
    if (!Number.isFinite(angle) || angle < 1 || angle > 179) {
      throw new Error('angle must be from 1 to 179 degrees.');
    }
    if (!Number.isFinite(wallHeight) || wallHeight < 0.01 || wallHeight > 100000) {
      throw new Error('wall_height must be from 0.01 to 100000 mm.');
    }
    if (!fs.existsSync(creoSingleFlatWallSection) ||
        !fs.statSync(creoSingleFlatWallSection).isFile()) {
      throw new Error(`Creo flat-wall section not found: ${creoSingleFlatWallSection}`);
    }
    const result = runPersistentFlatWall([
      expectedModel,
      featureName,
      projectDirectory,
      String(edgeId),
      String(expectedLength),
      String(angle),
      String(wallHeight),
      'none',
      creoSingleFlatWallSection,
    ]);
    const savedPath = resolveReturnedProjectModel(
      projectDirectory, result.saved_file, ['.prt']);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [savedPath]));
  }

  if (name === 'creo_create_project_sheetmetal_flat_walls_batch') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const angle = args.angle === undefined ? 90 : Number(args.angle);
    const wallHeight = Number(args.wall_height);
    if (!Array.isArray(args.edges) || args.edges.length !== 2) {
      throw new Error('edges must contain exactly two boundary edges.');
    }
    const edges = args.edges.map((edge, index) => {
      const edgeId = Number(edge && edge.edge_id);
      const expectedLength = Number(edge && edge.expected_edge_length);
      if (!Number.isInteger(edgeId) || edgeId < 1 || edgeId > 100000000) {
        throw new Error(`edges[${index}].edge_id is invalid.`);
      }
      if (!Number.isFinite(expectedLength) ||
          expectedLength < 0.01 || expectedLength > 100000) {
        throw new Error(
          `edges[${index}].expected_edge_length must be from 0.01 to 100000 mm.`);
      }
      return { edgeId, expectedLength };
    });
    if (edges[0].edgeId === edges[1].edgeId) {
      throw new Error('The two edge IDs must be different.');
    }
    if (!Number.isFinite(angle) || angle < 1 || angle > 179) {
      throw new Error('angle must be from 1 to 179 degrees.');
    }
    if (!Number.isFinite(wallHeight) || wallHeight < 0.01 || wallHeight > 100000) {
      throw new Error('wall_height must be from 0.01 to 100000 mm.');
    }
    if (!fs.existsSync(creoSingleFlatWallSection) ||
        !fs.statSync(creoSingleFlatWallSection).isFile()) {
      throw new Error(`Creo flat-wall section not found: ${creoSingleFlatWallSection}`);
    }
    const result = runPersistentFlatWall([
      expectedModel,
      featureName,
      projectDirectory,
      String(edges[0].edgeId),
      String(edges[0].expectedLength),
      String(edges[1].edgeId),
      String(edges[1].expectedLength),
      String(angle),
      String(wallHeight),
      'none',
      creoSingleFlatWallSection,
    ]);
    const savedPath = resolveReturnedProjectModel(
      projectDirectory, result.saved_file, ['.prt']);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [savedPath]));
  }

  if (name === 'creo_create_project_sheetmetal_three_circle_cut') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const ownerFeatureName = ensureCreoModelName(
      args.owner_feature_name, 'owner_feature_name');
    const orientationPlane = ensureCreoModelName(
      args.orientation_plane || 'TOP', 'orientation_plane');
    const surfaceId = Number(args.surface_id);
    const expectedArea = Number(args.expected_surface_area);
    const ownerFeatureId = Number(args.owner_feature_id);
    const diameter = Number(args.diameter);
    const spacing = Number(args.spacing);
    const centerU = args.center_u === undefined ? 0 : Number(args.center_u);
    const centerV = args.center_v === undefined ? 0 : Number(args.center_v);
    if (!Number.isInteger(surfaceId) || surfaceId < 1 || surfaceId > 100000000) {
      throw new Error('surface_id is invalid.');
    }
    if (!Number.isInteger(ownerFeatureId) ||
        ownerFeatureId < 1 || ownerFeatureId > 100000000) {
      throw new Error('owner_feature_id is invalid.');
    }
    if (!Number.isFinite(expectedArea) || expectedArea < 0.01 || expectedArea > 100000000) {
      throw new Error('expected_surface_area is invalid.');
    }
    if (!Number.isFinite(diameter) || diameter < 0.01 || diameter > 1000) {
      throw new Error('diameter is invalid.');
    }
    if (!Number.isFinite(spacing) || spacing < 0.01 || spacing > 100000) {
      throw new Error('spacing is invalid.');
    }
    if (![centerU, centerV].every((value) =>
      Number.isFinite(value) && value >= -100000 && value <= 100000)) {
      throw new Error('center_u and center_v are invalid.');
    }
    const result = runBridge('creo_sheetmetal_three_circle_cut_bridge.exe', [
      expectedModel,
      featureName,
      projectDirectory,
      String(surfaceId),
      String(expectedArea),
      String(ownerFeatureId),
      ownerFeatureName,
      orientationPlane,
      String(diameter),
      String(spacing),
      String(centerU),
      String(centerV),
    ]);
    const savedPath = resolveReturnedProjectModel(
      projectDirectory, result.saved_file, ['.prt']);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [savedPath]));
  }

  if (name === 'creo_create_project_extrusion_copy') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const sketchPlane = ensureCreoModelName(args.sketch_plane || 'FRONT', 'sketch_plane');
    const orientationPlane = ensureCreoModelName(
      args.orientation_plane || 'TOP', 'orientation_plane');
    if (sketchPlane.toLowerCase() === orientationPlane.toLowerCase()) {
      throw new Error('sketch_plane and orientation_plane must be different.');
    }
    const profile = args.profile;
    const operationMode = args.operation_mode;
    if (!['rectangle', 'circle'].includes(profile)) {
      throw new Error('profile must be rectangle or circle.');
    }
    if (!['add', 'cut'].includes(operationMode)) {
      throw new Error('operation_mode must be add or cut.');
    }
    const depth = Number(args.depth);
    const directionSide = args.direction_side === undefined
      ? 2
      : Number(args.direction_side);
    if (!Number.isFinite(depth) || depth < 0.1 || depth > 500) {
      throw new Error('depth must be a finite number from 0.1 to 500 model units.');
    }
    if (!Number.isInteger(directionSide) || ![1, 2].includes(directionSide)) {
      throw new Error('direction_side must be 1 or 2.');
    }
    let width;
    let height;
    let profileMode;
    if (profile === 'circle') {
      const diameter = Number(args.diameter);
      if (!Number.isFinite(diameter) || diameter < 0.1 || diameter > 500) {
        throw new Error('diameter is required for circle profile and must be from 0.1 to 500.');
      }
      width = diameter;
      height = 0.1;
      profileMode = operationMode === 'cut' ? 'CIRCLE_CUT' : 'CIRCLE';
    } else {
      width = Number(args.width);
      height = Number(args.height);
      if (![width, height].every((value) =>
        Number.isFinite(value) && value >= 0.1 && value <= 500)) {
        throw new Error('width and height are required for rectangle profile and must be from 0.1 to 500.');
      }
      profileMode = operationMode === 'cut' ? 'RECTANGLE_CUT' : 'RECTANGLE';
    }
    ensureProjectModelDoesNotExist(projectDirectory, copyName);
    return toolResult(runBridge('creo_extrude_bridge.exe', [
      expectedModel,
      copyName,
      projectDirectory,
      sketchPlane,
      orientationPlane,
      featureName,
      String(width),
      String(height),
      String(depth),
      String(directionSide),
      profileMode,
    ]));
  }

  if (name === 'creo_create_project_empty_assembly') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const assemblyName = ensureCreoModelName(args.assembly_name, 'assembly_name');
    if (!fs.existsSync(creoAssemblyTemplate) ||
        !fs.statSync(creoAssemblyTemplate).isFile()) {
      throw new Error(`Creo assembly template was not found: ${creoAssemblyTemplate}`);
    }
    ensureProjectModelTypeDoesNotExist(projectDirectory, assemblyName, '.asm');
    const result = runBridge('creo_empty_assembly_create_bridge.exe', [
      creoAssemblyTemplate,
      assemblyName,
      projectDirectory,
    ]);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_create_project_assembly') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const assemblyName = ensureCreoModelName(args.assembly_name, 'assembly_name');
    const componentFile = resolveCurrentWorkingDirectoryModelFile(args.component_model_file);
    const expectedComponent = ensureCreoModelName(
      args.expected_component, 'expected_component');
    if (!fs.existsSync(creoAssemblyTemplate) ||
        !fs.statSync(creoAssemblyTemplate).isFile()) {
      throw new Error(`找不到 Creo 毫米制装配模板：${creoAssemblyTemplate}`);
    }
    ensureProjectModelTypeDoesNotExist(projectDirectory, assemblyName, '.asm');
    const result = runBridge('creo_assembly_create_bridge.exe', [
      creoAssemblyTemplate,
      assemblyName,
      projectDirectory,
      componentFile,
      expectedComponent,
    ]);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_add_project_component_fixed') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const componentFile = resolveCurrentWorkingDirectoryModelFile(args.component_model_file);
    const expectedComponent = ensureCreoModelName(
      args.expected_component, 'expected_component');
    const tx = ensureFiniteNumber(args.translation_x ?? 0, 'translation_x');
    const ty = ensureFiniteNumber(args.translation_y ?? 0, 'translation_y');
    const tz = ensureFiniteNumber(args.translation_z ?? 0, 'translation_z');
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_assembly_add_fixed_bridge.exe', [
      expectedAssembly,
      componentFile,
      expectedComponent,
      String(tx),
      String(ty),
      String(tz),
    ]);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_add_project_component_mate_align') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const componentFile = resolveCurrentWorkingDirectoryModelFile(args.component_model_file);
    const expectedComponent = ensureCreoModelName(
      args.expected_component, 'expected_component');
    const requireFullyConstrained = args.require_fully_constrained === true;
    if (!Array.isArray(args.constraints) ||
        args.constraints.length < 1 || args.constraints.length > 3) {
      throw new Error('constraints must contain 1-3 plane constraints.');
    }
    const assemblyPlanes = new Set();
    const componentPlanes = new Set();
    const flattened = [];
    for (const [index, constraint] of args.constraints.entries()) {
      if (!constraint || typeof constraint !== 'object' || Array.isArray(constraint)) {
        throw new Error(`constraints[${index}] must be a plane constraint object.`);
      }
      if (constraint.type !== 'mate' && constraint.type !== 'align') {
        throw new Error(`constraints[${index}].type must be mate or align.`);
      }
      const assemblyPlane = ensureCreoModelName(
        constraint.assembly_plane, `constraints[${index}].assembly_plane`);
      const componentPlane = ensureCreoModelName(
        constraint.component_plane, `constraints[${index}].component_plane`);
      const assemblyKey = assemblyPlane.toLowerCase();
      const componentKey = componentPlane.toLowerCase();
      if (assemblyPlanes.has(assemblyKey)) {
        throw new Error(`Assembly plane is used more than once: ${assemblyPlane}.`);
      }
      if (componentPlanes.has(componentKey)) {
        throw new Error(`Component plane is used more than once: ${componentPlane}.`);
      }
      assemblyPlanes.add(assemblyKey);
      componentPlanes.add(componentKey);
      const assemblySide = constraint.assembly_side === undefined
        ? 'yellow' : constraint.assembly_side;
      const componentSide = constraint.component_side === undefined
        ? 'yellow' : constraint.component_side;
      if (!['yellow', 'red'].includes(assemblySide) ||
          !['yellow', 'red'].includes(componentSide)) {
        throw new Error(`constraints[${index}] datum sides must be yellow or red.`);
      }
      flattened.push(
        constraint.type,
        assemblyPlane,
        componentPlane,
        assemblySide,
        componentSide);
    }
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_assembly_mate_align_bridge.exe', [
      expectedAssembly,
      componentFile,
      expectedComponent,
      requireFullyConstrained ? '1' : '0',
      String(args.constraints.length),
      ...flattened,
    ]);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_get_project_assembly_components') {
    resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    return toolResult(runBridge('creo_assembly_components_bridge.exe', [
      expectedAssembly,
    ]));
  }

  if (name === 'creo_repeat_project_component_insert_pair') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const componentFile = resolveCurrentWorkingDirectoryModelFile(args.component_model_file);
    const expectedComponent = ensureCreoModelName(
      args.expected_component, 'expected_component');
    const sourceFeatureId = Number(args.source_component_feature_id);
    if (!Number.isInteger(sourceFeatureId) || sourceFeatureId < 1) {
      throw new Error('source_component_feature_id must be a positive integer.');
    }
    if (!Array.isArray(args.target_insert_surface_ids) ||
        args.target_insert_surface_ids.length !== 2) {
      throw new Error('target_insert_surface_ids must contain exactly two IDs.');
    }
    const targetSurfaceIds = args.target_insert_surface_ids.map((value, index) => {
      const id = Number(value);
      if (!Number.isInteger(id) || id < 1) {
        throw new Error(`target_insert_surface_ids[${index}] must be a positive integer.`);
      }
      return id;
    });
    if (targetSurfaceIds[0] === targetSurfaceIds[1]) {
      throw new Error('target_insert_surface_ids must be different.');
    }
    if (!Array.isArray(args.target_x_positions) ||
        args.target_x_positions.length !== 2) {
      throw new Error('target_x_positions must contain exactly two values.');
    }
    const targetXPositions = args.target_x_positions.map((value, index) =>
      ensureFiniteNumber(value, `target_x_positions[${index}]`));
    if (Math.abs(targetXPositions[0] - targetXPositions[1]) < 1e-6) {
      throw new Error('target_x_positions must be different.');
    }
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_assembly_repeat_insert_pair_bridge.exe', [
      expectedAssembly,
      String(sourceFeatureId),
      componentFile,
      expectedComponent,
      String(targetSurfaceIds[0]),
      String(targetXPositions[0]),
      String(targetSurfaceIds[1]),
      String(targetXPositions[1]),
    ]);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_add_project_component_align_insert_three') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const expectedHostComponent = ensureCreoModelName(
      args.expected_host_component, 'expected_host_component');
    const componentFile = resolveCurrentWorkingDirectoryModelFile(args.component_model_file);
    const expectedComponent = ensureCreoModelName(
      args.expected_component, 'expected_component');
    const positiveInteger = (value, label) => {
      const parsed = Number(value);
      if (!Number.isInteger(parsed) || parsed < 1) {
        throw new Error(`${label} must be a positive integer.`);
      }
      return parsed;
    };
    const hostFeatureId = positiveInteger(
      args.host_component_feature_id, 'host_component_feature_id');
    const assemblyAlignSurfaceId = positiveInteger(
      args.assembly_align_surface_id, 'assembly_align_surface_id');
    const componentAlignSurfaceId = positiveInteger(
      args.component_align_surface_id, 'component_align_surface_id');
    const componentInsertSurfaceId = positiveInteger(
      args.component_insert_surface_id, 'component_insert_surface_id');
    if (!Array.isArray(args.target_insert_surface_ids) ||
        args.target_insert_surface_ids.length !== 3) {
      throw new Error('target_insert_surface_ids must contain exactly three IDs.');
    }
    const targetSurfaceIds = args.target_insert_surface_ids.map((value, index) =>
      positiveInteger(value, `target_insert_surface_ids[${index}]`));
    if (new Set(targetSurfaceIds).size !== 3) {
      throw new Error('target_insert_surface_ids must be different.');
    }
    if (!Array.isArray(args.target_x_positions) ||
        args.target_x_positions.length !== 3) {
      throw new Error('target_x_positions must contain exactly three values.');
    }
    const targetXPositions = args.target_x_positions.map((value, index) =>
      ensureFiniteNumber(value, `target_x_positions[${index}]`));
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_assembly_repeat_insert_pair_bridge.exe', [
      expectedAssembly,
      String(hostFeatureId),
      expectedHostComponent,
      componentFile,
      expectedComponent,
      String(assemblyAlignSurfaceId),
      String(componentAlignSurfaceId),
      String(componentInsertSurfaceId),
      String(targetSurfaceIds[0]), String(targetXPositions[0]),
      String(targetSurfaceIds[1]), String(targetXPositions[1]),
      String(targetSurfaceIds[2]), String(targetXPositions[2]),
    ]);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_create_project_standard_skeleton') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const skeletonName = ensureCreoModelName(args.skeleton_name, 'skeleton_name');
    if (!fs.existsSync(creoPartTemplate) || !fs.statSync(creoPartTemplate).isFile()) {
      throw new Error(`找不到 Creo 毫米制零件模板：${creoPartTemplate}`);
    }
    ensureProjectModelDoesNotExist(projectDirectory, skeletonName);
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_assembly_skeleton_bridge.exe', [
      expectedAssembly,
      skeletonName,
      creoPartTemplate,
    ]);
    return toolResult(withProjectModelCleanup(projectDirectory, result, [
      result.assembly_saved_file,
      result.skeleton_saved_file,
    ]));
  }

  if (name === 'creo_create_project_skeleton_box') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const expectedSkeleton = ensureCreoModelName(
      args.expected_skeleton, 'expected_skeleton');
    const featureName = ensureCreoFeatureName(args.feature_name, 'feature_name');
    const length = ensureFiniteNumber(args.length, 'length', 0.1, 100000);
    const width = ensureFiniteNumber(args.width, 'width', 0.1, 100000);
    const height = ensureFiniteNumber(args.height, 'height', 0.1, 100000);
    const sketchPlane = ensureCreoModelName(args.sketch_plane || 'FRONT', 'sketch_plane');
    const orientationPlane = ensureCreoModelName(
      args.orientation_plane || 'TOP', 'orientation_plane');
    if (sketchPlane.toLowerCase() === orientationPlane.toLowerCase()) {
      throw new Error('sketch_plane 与 orientation_plane 不能相同。');
    }
    const directionSide = args.direction_side === undefined
      ? 1
      : Number(args.direction_side);
    if (!Number.isInteger(directionSide) || ![1, 2].includes(directionSide)) {
      throw new Error('direction_side 必须为 1 或 2。');
    }
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_skeleton_box_bridge.exe', [
      expectedAssembly,
      expectedSkeleton,
      featureName,
      String(length),
      String(width),
      String(height),
      sketchPlane,
      orientationPlane,
      String(directionSide),
    ]);
    return toolResult(withProjectModelCleanup(projectDirectory, result, [
      result.assembly_saved_file,
      result.skeleton_saved_file,
    ]));
  }

  if (name === 'creo_create_project_skeleton_surface_inset_extrusion') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const expectedSkeleton = ensureCreoModelName(
      args.expected_skeleton, 'expected_skeleton');
    const featureName = ensureCreoFeatureName(args.feature_name, 'feature_name');
    const surfaceId = Number(args.surface_id);
    if (!Number.isInteger(surfaceId) || surfaceId < 1) {
      throw new Error('surface_id must be a positive integer.');
    }
    const inset = ensureFiniteNumber(args.inset, 'inset', 0.1, 100000);
    const depth = ensureFiniteNumber(args.depth, 'depth', 0.1, 100000);
    const orientationPlane = ensureCreoModelName(
      args.orientation_plane || 'TOP', 'orientation_plane');
    const directionSide = args.direction_side === undefined
      ? 1
      : Number(args.direction_side);
    if (!Number.isInteger(directionSide) || ![1, 2].includes(directionSide)) {
      throw new Error('direction_side must be 1 or 2.');
    }
    const current = runBridge('creo_bridge.exe', []);
    if (!current.model || current.model.name.toLowerCase() !== expectedSkeleton.toLowerCase()) {
      throw new Error(`Current Creo model must be ${expectedSkeleton}.`);
    }
    const size = current.model.outline?.computed?.size;
    if (!Array.isArray(size) || size.length !== 3) {
      throw new Error('Unable to read current skeleton outline.');
    }
    const length = Number(size[0]) - 2 * inset;
    const width = Number(size[1]) - 2 * inset;
    if (!(length > 0.1) || !(width > 0.1)) {
      throw new Error('inset is too large for the current XY outline.');
    }
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_skeleton_box_bridge.exe', [
      expectedAssembly,
      expectedSkeleton,
      featureName,
      String(length),
      String(width),
      String(depth),
      `ID${surfaceId}`,
      orientationPlane,
      String(directionSide),
      String(surfaceId),
      String(inset),
    ]);
    return toolResult(withProjectModelCleanup(projectDirectory, result, [
      result.assembly_saved_file,
      result.skeleton_saved_file,
    ]));
  }

  if (name === 'creo_create_project_skeleton_surface_outset_extrusion') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const expectedSkeleton = ensureCreoModelName(
      args.expected_skeleton, 'expected_skeleton');
    const featureName = ensureCreoFeatureName(args.feature_name, 'feature_name');
    const surfaceId = Number(args.surface_id);
    if (!Number.isInteger(surfaceId) || surfaceId < 1) {
      throw new Error('surface_id must be a positive integer.');
    }
    const surfaceLength = ensureFiniteNumber(
      args.expected_surface_length, 'expected_surface_length', 0.1, 100000);
    const surfaceWidth = ensureFiniteNumber(
      args.expected_surface_width, 'expected_surface_width', 0.1, 100000);
    const outset = ensureFiniteNumber(args.outset, 'outset', 0.1, 100000);
    const depth = ensureFiniteNumber(args.depth, 'depth', 0.1, 100000);
    const orientationPlane = ensureCreoModelName(
      args.orientation_plane || 'TOP', 'orientation_plane');
    const directionSide = args.direction_side === undefined
      ? 2
      : Number(args.direction_side);
    if (directionSide !== 2) {
      throw new Error('Surface outset extrusion requires direction_side 2 (+Z).');
    }
    const current = runBridge('creo_bridge.exe', []);
    if (!current.model || current.model.name.toLowerCase() !== expectedSkeleton.toLowerCase()) {
      throw new Error(`Current Creo model must be ${expectedSkeleton}.`);
    }
    const length = surfaceLength + 2 * outset;
    const width = surfaceWidth + 2 * outset;
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_skeleton_box_bridge.exe', [
      expectedAssembly,
      expectedSkeleton,
      featureName,
      String(length),
      String(width),
      String(depth),
      `ID${surfaceId}`,
      orientationPlane,
      String(directionSide),
      String(surfaceId),
      String(outset),
      'OUTSET',
    ]);
    if (Math.abs(Number(result.sketch_surface_length) - surfaceLength) > 1e-6 ||
        Math.abs(Number(result.sketch_surface_width) - surfaceWidth) > 1e-6) {
      throw new Error('Surface size readback differs from the expected guarded values.');
    }
    return toolResult(withProjectModelCleanup(projectDirectory, result, [
      result.assembly_saved_file,
      result.skeleton_saved_file,
    ]));
  }

  if (name === 'creo_reverse_project_skeleton_box_direction') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const expectedSkeleton = ensureCreoModelName(
      args.expected_skeleton, 'expected_skeleton');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const expectedLength = ensureFiniteNumber(
      args.expected_length, 'expected_length', 0.1, 100000);
    const expectedWidth = ensureFiniteNumber(
      args.expected_width, 'expected_width', 0.1, 100000);
    const expectedHeight = ensureFiniteNumber(
      args.expected_height, 'expected_height', 0.1, 100000);
    const directionSide = Number(args.direction_side);
    if (!Number.isInteger(directionSide) || ![1, 2].includes(directionSide)) {
      throw new Error('direction_side must be 1 or 2.');
    }
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_skeleton_reverse_direction_bridge.exe', [
      expectedAssembly,
      expectedSkeleton,
      featureName,
      String(expectedLength),
      String(expectedWidth),
      String(expectedHeight),
      String(directionSide),
    ]);
    return toolResult(withProjectModelCleanup(projectDirectory, result, [
      result.assembly_saved_file,
      result.skeleton_saved_file,
    ]));
  }

  if (name === 'creo_resize_project_skeleton_box') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedAssembly = ensureCreoModelName(
      args.expected_assembly, 'expected_assembly');
    const expectedSkeleton = ensureCreoModelName(
      args.expected_skeleton, 'expected_skeleton');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const newFeatureName = ensureCreoModelName(
      args.new_feature_name || featureName, 'new_feature_name');
    const lengthSymbol = ensureDimensionSymbol(
      args.length_symbol, 'length_symbol', 'd3');
    const widthSymbol = ensureDimensionSymbol(
      args.width_symbol, 'width_symbol', 'd2');
    const heightSymbol = ensureDimensionSymbol(
      args.height_symbol, 'height_symbol', 'd1');
    if (new Set([
      lengthSymbol.toLowerCase(),
      widthSymbol.toLowerCase(),
      heightSymbol.toLowerCase(),
    ]).size !== 3) {
      throw new Error('length_symbol、width_symbol 和 height_symbol 必须互不相同。');
    }
    const expectedLength = ensureFiniteNumber(
      args.expected_length, 'expected_length', 0.1, 100000);
    const expectedWidth = ensureFiniteNumber(
      args.expected_width, 'expected_width', 0.1, 100000);
    const expectedHeight = ensureFiniteNumber(
      args.expected_height, 'expected_height', 0.1, 100000);
    const newLength = ensureFiniteNumber(
      args.new_length, 'new_length', 0.1, 100000);
    const newWidth = ensureFiniteNumber(
      args.new_width, 'new_width', 0.1, 100000);
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_skeleton_resize_box_bridge.exe', [
      expectedAssembly,
      expectedSkeleton,
      featureName,
      newFeatureName,
      lengthSymbol,
      widthSymbol,
      heightSymbol,
      String(expectedLength),
      String(expectedWidth),
      String(expectedHeight),
      String(newLength),
      String(newWidth),
    ]);
    return toolResult(withProjectModelCleanup(projectDirectory, result, [
      result.assembly_saved_file,
      result.skeleton_saved_file,
    ]));
  }

  if (name === 'creo_modify_project_feature_dimensions') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    if (!Array.isArray(args.modifications) ||
        args.modifications.length < 1 || args.modifications.length > 8) {
      throw new Error('modifications 必须包含 1-8 项尺寸修改。');
    }
    const seenSymbols = new Set();
    const flattened = [];
    for (const [index, item] of args.modifications.entries()) {
      if (!item || typeof item !== 'object' || Array.isArray(item)) {
        throw new Error(`modifications[${index}] 必须是尺寸修改对象。`);
      }
      const symbol = ensureDimensionSymbol(
        item.dimension_symbol, `modifications[${index}].dimension_symbol`);
      const normalizedSymbol = symbol.toLowerCase();
      if (seenSymbols.has(normalizedSymbol)) {
        throw new Error(`尺寸符号重复：${symbol}`);
      }
      seenSymbols.add(normalizedSymbol);
      const expectedValue = ensureFiniteNumber(
        item.expected_value,
        `modifications[${index}].expected_value`,
        0.000001,
        1000000);
      const newValue = ensureFiniteNumber(
        item.new_value,
        `modifications[${index}].new_value`,
        0.000001,
        1000000);
      flattened.push(symbol, String(expectedValue), String(newValue));
    }
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_dimension_modify_bridge.exe', [
      expectedModel,
      featureName,
      String(args.modifications.length),
      ...flattened,
    ]);
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_create_project_general_sketch') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const sketchPlane = ensureCreoModelName(args.sketch_plane, 'sketch_plane');
    const orientationPlane = ensureCreoModelName(
      args.orientation_plane, 'orientation_plane');
    if (sketchPlane.toLowerCase() === orientationPlane.toLowerCase()) {
      throw new Error('sketch_plane and orientation_plane must be different.');
    }
    const viewSide = args.view_side === undefined ? 1 : Number(args.view_side);
    if (!Number.isInteger(viewSide) || (viewSide !== 1 && viewSide !== 2)) {
      throw new Error('view_side must be 1 or 2.');
    }
    const orientationName = args.orientation_direction === undefined
      ? 'up' : args.orientation_direction;
    const orientationCodes = { up: 1, down: 2, left: 3, right: 4 };
    if (!Object.prototype.hasOwnProperty.call(orientationCodes, orientationName)) {
      throw new Error('orientation_direction must be up, down, left, or right.');
    }
    if (!Array.isArray(args.entities) ||
        args.entities.length < 1 || args.entities.length > 32) {
      throw new Error('entities must contain 1-32 sketch entities.');
    }
    const flattened = [];
    for (const [index, entity] of args.entities.entries()) {
      if (!entity || typeof entity !== 'object' || Array.isArray(entity)) {
        throw new Error(`entities[${index}] must be a sketch entity object.`);
      }
      if (entity.kind === 'line') {
        const allowed = new Set(['kind', 'x1', 'y1', 'x2', 'y2']);
        if (Object.keys(entity).some((key) => !allowed.has(key))) {
          throw new Error(`entities[${index}] line has fields for another geometry kind.`);
        }
        const x1 = ensureFiniteNumber(entity.x1, `entities[${index}].x1`, -1000000, 1000000);
        const y1 = ensureFiniteNumber(entity.y1, `entities[${index}].y1`, -1000000, 1000000);
        const x2 = ensureFiniteNumber(entity.x2, `entities[${index}].x2`, -1000000, 1000000);
        const y2 = ensureFiniteNumber(entity.y2, `entities[${index}].y2`, -1000000, 1000000);
        if (Math.hypot(x2 - x1, y2 - y1) < 0.000001) {
          throw new Error(`entities[${index}] line is degenerate.`);
        }
        flattened.push('line', String(x1), String(y1), String(x2), String(y2));
      } else if (entity.kind === 'circle') {
        const allowed = new Set(['kind', 'cx', 'cy', 'radius']);
        if (Object.keys(entity).some((key) => !allowed.has(key))) {
          throw new Error(`entities[${index}] circle has fields for another geometry kind.`);
        }
        const cx = ensureFiniteNumber(entity.cx, `entities[${index}].cx`, -1000000, 1000000);
        const cy = ensureFiniteNumber(entity.cy, `entities[${index}].cy`, -1000000, 1000000);
        const radius = ensureFiniteNumber(
          entity.radius, `entities[${index}].radius`, 0.000001, 1000000);
        flattened.push('circle', String(cx), String(cy), String(radius));
      } else if (entity.kind === 'arc') {
        const allowed = new Set([
          'kind', 'cx', 'cy', 'radius', 'start_angle_deg', 'end_angle_deg',
        ]);
        if (Object.keys(entity).some((key) => !allowed.has(key))) {
          throw new Error(`entities[${index}] arc has fields for another geometry kind.`);
        }
        const cx = ensureFiniteNumber(entity.cx, `entities[${index}].cx`, -1000000, 1000000);
        const cy = ensureFiniteNumber(entity.cy, `entities[${index}].cy`, -1000000, 1000000);
        const radius = ensureFiniteNumber(
          entity.radius, `entities[${index}].radius`, 0.000001, 1000000);
        const startAngle = ensureFiniteNumber(
          entity.start_angle_deg,
          `entities[${index}].start_angle_deg`, -360, 360);
        const endAngle = ensureFiniteNumber(
          entity.end_angle_deg,
          `entities[${index}].end_angle_deg`, -360, 360);
        const span = Math.abs(endAngle - startAngle);
        if (span < 0.001 || span >= 359.999) {
          throw new Error(`entities[${index}] arc span must be between 0.001 and 359.999 degrees.`);
        }
        flattened.push(
          'arc', String(cx), String(cy), String(radius),
          String(startAngle), String(endAngle));
      } else {
        throw new Error(`entities[${index}].kind must be line, circle, or arc.`);
      }
    }
    runBridge('creo_project_workdir_bridge.exe', [projectDirectory]);
    const result = runBridge('creo_general_sketch_bridge.exe', [
      expectedModel,
      featureName,
      sketchPlane,
      orientationPlane,
      String(viewSide),
      String(orientationCodes[orientationName]),
      String(args.entities.length),
      ...flattened,
    ]);
    result.orientation_direction_name = orientationName;
    return toolResult(withProjectModelCleanup(
      projectDirectory, result, [result.saved_file]));
  }

  if (name === 'creo_get_project_geometry') {
    const modelFile = resolveCurrentWorkingDirectoryModelFile(args.model_file);
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const result = runBridge('creo_geometry_inspect_bridge.exe', [modelFile]);
    if (!result || typeof result.model !== 'string' ||
        result.model.toLowerCase() !== expectedModel.toLowerCase()) {
      const error = new Error('项目几何读取的模型名称与 expected_model 不一致。');
      error.detail = {
        ok: false,
        stage: 'model_name_guard',
        expected_model: expectedModel,
        actual_model: result && result.model,
      };
      throw error;
    }
    return toolResult(result);
  }

  if (name === 'creo_display_project_model') {
    const session = runPersistentBasicModelRead();
    const modelFile = resolveWorkingDirectoryPartOrAssemblyFile(
      session.working_directory, args.model_file);
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    return toolResult({
      ...runPersistentDisplayModel(modelFile, expectedModel),
      working_directory: session.working_directory,
      directory_source: 'current_creo_session',
    });
  }

  if (name === 'creo_cleanup_project_versions') {
    const projectDirectory = resolveCurrentCreoWorkingDirectory();
    const keepFile = resolveCurrentWorkingDirectoryModelFile(args.keep_model_file);
    return toolResult(runProjectCleanup(projectDirectory, keepFile));
  }

  if (name === 'creo_get_dimensions') {
    if (args.model_file) {
      const modelFile = resolveSafeModelFile(args.model_file);
      return toolResult(runBridge('creo_dimensions_bridge.exe', [modelFile]));
    }
    return toolResult(runBridge('creo_dimensions_bridge.exe', []));
  }

  if (name === 'creo_get_project_dimensions') {
    const modelFile = resolveCurrentWorkingDirectoryModelFile(args.model_file);
    return toolResult(runBridge('creo_dimensions_bridge.exe', [modelFile]));
  }

  if (name === 'creo_get_features') {
    if (args.model_file) {
      const modelFile = resolveSafeModelFile(args.model_file);
      return toolResult(runBridge('creo_features_bridge.exe', [modelFile]));
    }
    return toolResult(runBridge('creo_features_bridge.exe', []));
  }

  if (name === 'creo_get_mass_properties') {
    if (args.model_file) {
      const modelFile = resolveSafeModelFile(args.model_file);
      return toolResult(runBridge('creo_mass_properties_bridge.exe', [modelFile]));
    }
    return toolResult(runBridge('creo_mass_properties_bridge.exe', []));
  }

  if (name === 'creo_verify_saved_copy') {
    const modelFile = resolveSafeModelFile(args.model_file);
    const parameter = args.parameter || 'CNAME';
    if (!allowedParameters.includes(parameter)) {
      throw new Error(`参数不在业务白名单中：${parameter}`);
    }
    const expectedValue = ensureString(args.expected_value, 'expected_value');
    return toolResult(runBridge('creo_verify_copy.exe', [modelFile, parameter, expectedValue]));
  }

  if (name === 'creo_create_safe_parameter_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    if (!allowedParameters.includes(args.parameter)) {
      throw new Error(`参数不在业务写入白名单中：${args.parameter}`);
    }
    const newValue = ensureString(args.new_value, 'new_value');
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`安全停止：输出目录中已存在 ${copyName} 的文件，拒绝覆盖。`);
    }
    return toolResult(runBridge('creo_write_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      args.parameter,
      newValue,
    ]));
  }

  if (name === 'creo_export_step') {
    const modelFile = resolveSafeModelFile(args.model_file);
    const outputName = ensureSimpleName(args.output_name, 'output_name');
    const outputBase = path.join(safeOutputRoot, outputName);
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${outputName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`安全停止：输出目录中已存在 ${outputName} 的文件，拒绝覆盖。`);
    }
    return toolResult(runBridge('creo_export_bridge.exe', [modelFile, outputBase]));
  }

  if (name === 'creo_create_safe_multi_parameter_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    if (!Array.isArray(args.updates) || args.updates.length < 1 || args.updates.length > 10) {
      throw new Error('updates 必须包含 1-10 个参数修改。');
    }
    const seen = new Set();
    const flatUpdates = [];
    for (const update of args.updates) {
      if (!update || !allowedParameters.includes(update.parameter)) {
        throw new Error(`参数不在业务写入白名单中：${update && update.parameter}`);
      }
      if (seen.has(update.parameter)) {
        throw new Error(`同一次操作中不能重复参数：${update.parameter}`);
      }
      seen.add(update.parameter);
      flatUpdates.push(update.parameter, ensureString(update.new_value, `${update.parameter}.new_value`));
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`安全停止：输出目录中已存在 ${copyName} 的文件，拒绝覆盖。`);
    }
    return toolResult(runBridge('creo_multi_write_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      ...flatUpdates,
    ]));
  }

  if (name === 'creo_create_safe_dimension_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const dimensionSymbol = ensureSimpleName(args.dimension_symbol, 'dimension_symbol');
    const expectedValue = Number(args.expected_value);
    const newValue = Number(args.new_value);
    if (!Number.isFinite(expectedValue) || !Number.isFinite(newValue) ||
        expectedValue <= 0 || newValue <= 0) {
      throw new Error('尺寸旧值和新值必须是正数。');
    }
    const ratio = newValue / expectedValue;
    if (ratio < 0.5 || ratio > 2.0) {
      throw new Error('安全停止：新尺寸必须在旧尺寸的 0.5-2 倍范围内。');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`安全停止：输出目录中已存在 ${copyName} 的文件，拒绝覆盖。`);
    }
    return toolResult(runBridge('creo_dimension_write_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      dimensionSymbol,
      String(expectedValue),
      String(newValue),
    ]));
  }

  if (name === 'creo_create_safe_feature_suppression_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const featureId = Number(args.feature_id);
    const expectedTypeCode = Number(args.expected_type_code);
    if (!Number.isInteger(featureId) || featureId < 1) {
      throw new Error('feature_id 必须是正整数。');
    }
    if (![913, 914, 911, 916].includes(expectedTypeCode)) {
      throw new Error('该特征类型不在安全抑制白名单中。');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`安全停止：输出目录中已存在 ${copyName} 的文件，拒绝覆盖。`);
    }
    return toolResult(runBridge('creo_feature_suppress_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      String(featureId),
      String(expectedTypeCode),
    ]));
  }

  if (name === 'creo_create_safe_feature_resume_copy') {
    const modelFile = resolveSafeModelFile(args.model_file);
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const featureId = Number(args.feature_id);
    const expectedTypeCode = Number(args.expected_type_code);
    if (!Number.isInteger(featureId) || featureId < 1) {
      throw new Error('feature_id 必须是正整数。');
    }
    if (![913, 914, 911, 916].includes(expectedTypeCode)) {
      throw new Error('该特征类型不在安全恢复白名单中。');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`安全停止：输出目录中已存在 ${copyName} 的文件，拒绝覆盖。`);
    }
    return toolResult(runBridge('creo_feature_resume_bridge.exe', [
      modelFile,
      expectedModel,
      copyName,
      safeOutputRoot,
      String(featureId),
      String(expectedTypeCode),
    ]));
  }

  if (name === 'creo_create_safe_datum_point_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const referenceCsys = ensureCreoModelName(
      args.reference_csys || 'PRT_CSYS_DEF',
      'reference_csys');
    const pointName = ensureCreoModelName(args.point_name, 'point_name');
    const coordinates = [Number(args.x), Number(args.y), Number(args.z)];
    if (coordinates.some((value) => !Number.isFinite(value) || Math.abs(value) > 1000)) {
      throw new Error('x、y、z 必须是 -1000 到 1000 之间的有限数值（使用当前模型单位）。');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`安全停止：输出目录中已存在 ${copyName} 的文件，拒绝覆盖。`);
    }
    return toolResult(runBridge('creo_datum_point_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      referenceCsys,
      pointName,
      ...coordinates.map(String),
    ]));
  }

  if (name === 'creo_create_safe_thru_hole_copy' ||
      name === 'creo_create_safe_advanced_hole_copy') {
    const isAdvanced = name === 'creo_create_safe_advanced_hole_copy';
    const style = isAdvanced ? (args.style || 'blind') : 'thru_all';
    if (isAdvanced && !['blind', 'counterbore', 'countersink', 'threaded'].includes(style)) {
      throw new Error('style must be blind, counterbore, countersink, or threaded.');
    }
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const holeName = ensureCreoModelName(args.hole_name, 'hole_name');
    const primaryPlane = ensureCreoModelName(args.primary_plane || 'FRONT', 'primary_plane');
    const referencePlane1 = ensureCreoModelName(
      args.reference_plane1 || 'RIGHT', 'reference_plane1');
    const referencePlane2 = ensureCreoModelName(
      args.reference_plane2 || 'TOP', 'reference_plane2');
    if (new Set([
      primaryPlane.toLowerCase(),
      referencePlane1.toLowerCase(),
      referencePlane2.toLowerCase(),
    ]).size !== 3) {
      throw new Error('primary_plane, reference_plane1, and reference_plane2 must be distinct.');
    }
    const threadSeries = style === 'threaded'
      ? ensureHoleTableToken(args.thread_series || 'ISO', 'thread_series')
      : '';
    const threadSize = style === 'threaded'
      ? ensureHoleTableToken(args.thread_size, 'thread_size')
      : '';
    const threadDepth = style === 'threaded' ? Number(args.thread_depth) : 0;
    const diameter = style === 'threaded'
      ? tapDrillDiameterFromHoleTable(threadSeries, threadSize)
      : Number(args.diameter);
    const offset1 = Number(args.offset1);
    const offset2 = Number(args.offset2);
    const directionSideArg = isAdvanced ? args.direction_side : args.thru_side;
    const directionSide = directionSideArg === undefined ? 2 : Number(directionSideArg);
    const depth = isAdvanced ? Number(args.depth) : 0;
    const counterboreDiameter = style === 'counterbore'
      ? Number(args.counterbore_diameter)
      : 0;
    const counterboreDepth = style === 'counterbore'
      ? Number(args.counterbore_depth)
      : 0;
    const countersinkDiameter = style === 'countersink'
      ? Number(args.countersink_diameter)
      : 0;
    const countersinkAngle = style === 'countersink'
      ? Number(args.countersink_angle)
      : 0;
    if (!Number.isFinite(diameter) || diameter < 0.1 || diameter > 100) {
      throw new Error('diameter must be a finite number from 0.1 to 100 model units.');
    }
    if (![offset1, offset2].every((value) =>
      Number.isFinite(value) && Math.abs(value) <= 1000)) {
      throw new Error('offset1 and offset2 must be finite numbers from -1000 to 1000 model units.');
    }
    if (isAdvanced && (!Number.isFinite(depth) || depth < 0.1 || depth > 500)) {
      throw new Error('depth must be a finite number from 0.1 to 500 model units.');
    }
    if (style === 'counterbore' &&
        (!Number.isFinite(counterboreDiameter) || counterboreDiameter < 0.2 ||
         counterboreDiameter > 200 || counterboreDiameter <= diameter)) {
      throw new Error('counterbore_diameter must be finite, from 0.2 to 200, and greater than diameter.');
    }
    if (style === 'counterbore' &&
        (!Number.isFinite(counterboreDepth) || counterboreDepth < 0.1 ||
         counterboreDepth >= depth)) {
      throw new Error('counterbore_depth must be finite, at least 0.1, and less than depth.');
    }
    if (style === 'countersink' &&
        (!Number.isFinite(countersinkDiameter) || countersinkDiameter < 0.2 ||
         countersinkDiameter > 200 || countersinkDiameter <= diameter)) {
      throw new Error('countersink_diameter must be finite, from 0.2 to 200, and greater than diameter.');
    }
    if (style === 'countersink' &&
        (!Number.isFinite(countersinkAngle) ||
         countersinkAngle < 30 || countersinkAngle > 150)) {
      throw new Error('countersink_angle must be a finite number from 30 to 150 degrees.');
    }
    if (style === 'countersink') {
      const countersinkDepth = (countersinkDiameter - diameter) * 0.5 /
        Math.tan(countersinkAngle * Math.PI / 360);
      if (!Number.isFinite(countersinkDepth) || countersinkDepth >= depth) {
        throw new Error('The calculated countersink depth must be less than depth.');
      }
    }
    if (style === 'threaded' &&
        (!Number.isFinite(threadDepth) || threadDepth < 0.1 || threadDepth >= depth)) {
      throw new Error('thread_depth must be finite, at least 0.1, and less than depth.');
    }
    if (!Number.isInteger(directionSide) || ![1, 2].includes(directionSide)) {
      throw new Error(`${isAdvanced ? 'direction_side' : 'thru_side'} must be 1 or 2.`);
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    return toolResult(runBridge('creo_hole_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      primaryPlane,
      referencePlane1,
      referencePlane2,
      holeName,
      String(diameter),
      String(offset1),
      String(offset2),
      String(directionSide),
      ...(style === 'blind' ? ['BLIND', String(depth)] : []),
      ...(style === 'counterbore'
        ? ['COUNTERBORE', String(depth), String(counterboreDiameter), String(counterboreDepth)]
        : []),
      ...(style === 'countersink'
        ? ['COUNTERSINK', String(depth), String(countersinkDiameter), String(countersinkAngle)]
        : []),
      ...(style === 'threaded'
        ? ['THREAD', String(depth), String(threadDepth), threadSeries, threadSize]
        : []),
    ]));
  }

  if (name === 'creo_create_safe_offset_datum_plane_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const referencePlane = ensureCreoModelName(
      args.reference_plane || 'FRONT', 'reference_plane');
    const planeName = ensureCreoModelName(args.plane_name, 'plane_name');
    const offset = Number(args.offset);
    if (referencePlane.toLowerCase() === planeName.toLowerCase()) {
      throw new Error('reference_plane and plane_name must be different.');
    }
    if (!Number.isFinite(offset) || Math.abs(offset) < 0.001 || Math.abs(offset) > 1000) {
      throw new Error('offset must be finite, nonzero, and from -1000 to 1000 model units.');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    return toolResult(runBridge('creo_datum_plane_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      referencePlane,
      planeName,
      String(offset),
    ]));
  }

  if (name === 'creo_display_safe_model') {
    const modelFile = resolveSafeModelFile(args.model_file);
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    return toolResult(runBridge('creo_display_model_bridge.exe', [
      modelFile,
      expectedModel,
    ]));
  }

  if (name === 'creo_create_safe_rectangle_extrusion_copy' ||
      name === 'creo_create_safe_rectangle_cut_copy') {
    const isCut = name === 'creo_create_safe_rectangle_cut_copy';
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const sketchPlane = ensureCreoModelName(args.sketch_plane || 'FRONT', 'sketch_plane');
    const orientationPlane = ensureCreoModelName(
      args.orientation_plane || 'TOP', 'orientation_plane');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    if (sketchPlane.toLowerCase() === orientationPlane.toLowerCase()) {
      throw new Error('sketch_plane and orientation_plane must be different.');
    }
    const width = Number(args.width);
    const height = Number(args.height);
    const depth = Number(args.depth);
    const directionSide = args.direction_side === undefined
      ? 1
      : Number(args.direction_side);
    if (![width, height, depth].every((value) =>
      Number.isFinite(value) && value >= 0.1 && value <= 500)) {
      throw new Error('width, height, and depth must be finite numbers from 0.1 to 500 model units.');
    }
    if (!Number.isInteger(directionSide) || ![1, 2].includes(directionSide)) {
      throw new Error('direction_side must be 1 or 2.');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    return toolResult(runBridge('creo_extrude_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      sketchPlane,
      orientationPlane,
      featureName,
      String(width),
      String(height),
      String(depth),
      String(directionSide),
      isCut ? 'RECTANGLE_CUT' : 'RECTANGLE',
    ]));
  }

  if (name === 'creo_create_safe_revolve_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const sketchPlane = ensureCreoModelName(args.sketch_plane || 'RIGHT', 'sketch_plane');
    const orientationPlane = ensureCreoModelName(
      args.orientation_plane || 'TOP', 'orientation_plane');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const operationMode = args.operation_mode;
    if (!['add', 'cut'].includes(operationMode)) {
      throw new Error('operation_mode must be add or cut.');
    }
    if (sketchPlane.toLowerCase() === orientationPlane.toLowerCase()) {
      throw new Error('sketch_plane and orientation_plane must be different.');
    }
    const axialWidth = Number(args.axial_width);
    const innerRadius = Number(args.inner_radius);
    const radialThickness = Number(args.radial_thickness);
    const directionSide = args.direction_side === undefined
      ? 1
      : Number(args.direction_side);
    if (![axialWidth, innerRadius, radialThickness].every((value) =>
      Number.isFinite(value) && value >= 0.1 && value <= 500)) {
      throw new Error('axial_width, inner_radius, and radial_thickness must be finite numbers from 0.1 to 500 model units.');
    }
    if (innerRadius + radialThickness > 500) {
      throw new Error('inner_radius plus radial_thickness must not exceed 500 model units.');
    }
    if (!Number.isInteger(directionSide) || ![1, 2].includes(directionSide)) {
      throw new Error('direction_side must be 1 or 2.');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    return toolResult(runBridge('creo_extrude_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      sketchPlane,
      orientationPlane,
      featureName,
      String(axialWidth),
      String(innerRadius),
      String(radialThickness),
      String(directionSide),
      operationMode === 'cut'
        ? 'REVOLVE_RECTANGLE_CUT'
        : 'REVOLVE_RECTANGLE',
    ]));
  }

  if (name === 'creo_create_safe_round_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const radius = Number(args.radius);
    const edgeId = args.edge_id === undefined ? null : Number(args.edge_id);
    if (!Number.isFinite(radius) || radius < 0.1 || radius > 100) {
      throw new Error('radius must be a finite number from 0.1 to 100 model units.');
    }
    if (edgeId !== null && (!Number.isInteger(edgeId) || edgeId < 1)) {
      throw new Error('edge_id must be a positive integer when provided.');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    return toolResult(runBridge('creo_extrude_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      'RIGHT',
      'TOP',
      featureName,
      edgeId === null ? '0.1' : String(edgeId),
      String(radius),
      '0.1',
      '1',
      'ROUND',
    ]));
  }

  if (name === 'creo_create_safe_chamfer_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const distance = Number(args.distance);
    const edgeId = args.edge_id === undefined ? null : Number(args.edge_id);
    if (!Number.isFinite(distance) || distance < 0.1 || distance > 100) {
      throw new Error('distance must be a finite number from 0.1 to 100 model units.');
    }
    if (edgeId !== null && (!Number.isInteger(edgeId) || edgeId < 1)) {
      throw new Error('edge_id must be a positive integer when provided.');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    return toolResult(runBridge('creo_extrude_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      'RIGHT',
      'TOP',
      featureName,
      edgeId === null ? '0.1' : String(edgeId),
      String(distance),
      '0.1',
      '1',
      'CHAMFER',
    ]));
  }

  if (name === 'creo_create_safe_shell_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const thickness = Number(args.thickness);
    const surfaceId = args.removed_surface_id === undefined
      ? null
      : Number(args.removed_surface_id);
    if (!Number.isFinite(thickness) || thickness < 0.1 || thickness > 100) {
      throw new Error('thickness must be a finite number from 0.1 to 100 model units.');
    }
    if (surfaceId !== null && (!Number.isInteger(surfaceId) || surfaceId < 1)) {
      throw new Error('removed_surface_id must be a positive integer when provided.');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    return toolResult(runBridge('creo_extrude_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      'RIGHT',
      'TOP',
      featureName,
      surfaceId === null ? '0.1' : String(surfaceId),
      String(thickness),
      '0.1',
      '1',
      'SHELL',
    ]));
  }

  if (name === 'creo_create_safe_draft_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const hingePlane = ensureCreoModelName(args.hinge_plane || 'FRONT', 'hinge_plane');
    const angleDegrees = Number(args.angle_degrees);
    const surfaceId = args.drafted_surface_id === undefined
      ? null
      : Number(args.drafted_surface_id);
    const directionSide = args.direction_side === undefined
      ? 1
      : Number(args.direction_side);
    if (!Number.isFinite(angleDegrees) || angleDegrees < 0.1 || angleDegrees > 30) {
      throw new Error('angle_degrees must be a finite number from 0.1 to 30.');
    }
    if (surfaceId !== null && (!Number.isInteger(surfaceId) || surfaceId < 1)) {
      throw new Error('drafted_surface_id must be a positive integer when provided.');
    }
    if (!Number.isInteger(directionSide) || ![1, 2].includes(directionSide)) {
      throw new Error('direction_side must be 1 or 2.');
    }
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    const orientationPlane = hingePlane.toLowerCase() === 'right' ? 'TOP' : 'RIGHT';
    return toolResult(runBridge('creo_extrude_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      hingePlane,
      orientationPlane,
      featureName,
      surfaceId === null ? '0.1' : String(surfaceId),
      String(angleDegrees),
      '0.1',
      String(directionSide),
      'DRAFT',
    ]));
  }

  if (name === 'creo_create_safe_part_mirror_copy') {
    const expectedModel = ensureCreoModelName(args.expected_model, 'expected_model');
    const copyName = ensureCreoModelName(args.copy_name, 'copy_name');
    const featureName = ensureCreoModelName(args.feature_name, 'feature_name');
    const mirrorPlane = ensureCreoModelName(args.mirror_plane || 'RIGHT', 'mirror_plane');
    const existing = fs.readdirSync(safeOutputRoot)
      .some((fileName) => fileName.toLowerCase().startsWith(`${copyName.toLowerCase()}.`));
    if (existing) {
      throw new Error(`Safe stop: files already exist for ${copyName}; overwrite refused.`);
    }
    return toolResult(runBridge('creo_extrude_bridge.exe', [
      expectedModel,
      copyName,
      safeOutputRoot,
      mirrorPlane,
      'PART',
      featureName,
      '1',
      '1',
      '1',
      '1',
      'MIRROR',
    ]));
  }

  throw new Error(`未知工具：${name}`);
}

function handleRequest(request) {
  if (!request || request.jsonrpc !== '2.0') {
    return;
  }

  if (request.id === undefined || request.id === null) {
    return;
  }

  try {
    if (request.method === 'initialize') {
      send({
        jsonrpc: '2.0',
        id: request.id,
        result: {
          protocolVersion: request.params && request.params.protocolVersion
            ? request.params.protocolVersion
            : '2025-06-18',
          capabilities: { tools: { listChanged: false } },
          serverInfo: { name: 'creo-safe-bridge', version: '0.9.0' },
          instructions: 'Creo 必须已在同一 Windows 会话中运行。每次新开 Creo 并选好工作目录后的首次命令，先调用 creo_start_resident_and_get_basic_model，快速建立会话绑定常驻桥接并读取基础模型信息；关闭该次 Creo 后桥接自动退出。读取工具不修改模型；写入工具拒绝覆盖或核对预期旧值，并在写入后验证。所有创建、读取、修改、装配、导出和清理工具都必须先读取当前 Creo 会话已经选择的工作目录，并且只能操作该目录的直接子文件；禁止接收 project_name、固定项目根目录或调用方提供的目录路径。以后新增的 Creo 工具也必须遵守相同规则。装配体、骨架、骨架实体、通用草绘及重合/对齐装配工具通过 Pro/TOOLKIT 正式接口执行。通用尺寸修改工具会核对当前零件、所属特征、尺寸符号、预期旧值和关系驱动状态；通用草绘工具会核对当前零件、承载平面、定向平面、特征名以及每个直线、圆和圆弧实体；重合/对齐装配工具会核对当前装配体、组件模型、全部平面引用、约束类型、基准侧与欠约束状态。任一失败都会在保存前回滚。成功保存后只把本次目标 .prt/.asm 模型族的旧版本移入 Windows 回收站，组件和无关文件保持不变。',
        },
      });
      return;
    }

    if (request.method === 'ping') {
      send({ jsonrpc: '2.0', id: request.id, result: {} });
      return;
    }

    if (request.method === 'tools/list') {
      send({ jsonrpc: '2.0', id: request.id, result: { tools: toolDefinitions } });
      return;
    }

    if (request.method === 'tools/call') {
      const params = request.params || {};
      const result = handleToolCall(params.name, params.arguments || {});
      send({ jsonrpc: '2.0', id: request.id, result });
      return;
    }

    send({
      jsonrpc: '2.0',
      id: request.id,
      error: { code: -32601, message: `Method not found: ${request.method}` },
    });
  } catch (error) {
    const data = error.detail || { message: error.message };
    if (request.method === 'tools/call') {
      send({ jsonrpc: '2.0', id: request.id, result: toolResult(data, true) });
    } else {
      send({
        jsonrpc: '2.0',
        id: request.id,
        error: { code: -32603, message: error.message, data },
      });
    }
  }
}

const input = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
input.on('line', (line) => {
  const trimmed = line.trim();
  if (!trimmed) return;
  try {
    const parsed = JSON.parse(trimmed);
    if (Array.isArray(parsed)) {
      parsed.forEach(handleRequest);
    } else {
      handleRequest(parsed);
    }
  } catch (error) {
    send({
      jsonrpc: '2.0',
      id: null,
      error: { code: -32700, message: 'Parse error' },
    });
  }
});
