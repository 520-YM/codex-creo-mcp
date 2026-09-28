'use strict';

const fs = require('fs');
const path = require('path');

function parseCreoFile(fileName) {
  const match = String(fileName).match(/^(.+)\.(asm|prt)(?:\.(\d+))?$/i);
  if (!match) return null;
  return {
    file_name: fileName,
    model_name: match[1],
    model_type: match[2].toLowerCase(),
    version: Number(match[3] || 0),
  };
}

function latestModelFamilies(directory) {
  const families = new Map();
  for (const fileName of fs.readdirSync(directory)) {
    const parsed = parseCreoFile(fileName);
    if (!parsed) continue;
    const key = `${parsed.model_name.toLowerCase()}.${parsed.model_type}`;
    const previous = families.get(key);
    if (!previous || parsed.version > previous.version) families.set(key, parsed);
  }
  return [...families.values()];
}

function chooseTopAssembly(models) {
  const assemblies = models.filter((item) => item.model_type === 'asm');
  if (assemblies.length === 0) {
    throw new Error('当前 Creo 工作目录中没有装配体文件。');
  }
  const fiveZero = assemblies.filter((item) => /00000/i.test(item.model_name));
  const candidates = fiveZero.length > 0 ? fiveZero : assemblies;
  candidates.sort((a, b) => {
    const aGas = /^YQL/i.test(a.model_name) ? 1 : 0;
    const bGas = /^YQL/i.test(b.model_name) ? 1 : 0;
    if (aGas !== bGas) return aGas - bGas;
    return b.version - a.version || a.model_name.localeCompare(b.model_name);
  });
  return {
    ...candidates[0],
    selection_rule: fiveZero.length > 0
      ? 'five_zero_assembly'
      : 'other_assembly_fallback',
    candidate_count: candidates.length,
  };
}

function chooseSkeleton(models, topAssemblyName) {
  const parts = models.filter((item) => item.model_type === 'prt');
  const exact = parts.filter((item) =>
    item.model_name.toLowerCase() === `${topAssemblyName}_skel`.toLowerCase());
  const candidates = exact.length > 0
    ? exact
    : parts.filter((item) => /(?:_skel|skeleton|骨架)$/i.test(item.model_name));
  candidates.sort((a, b) => b.version - a.version);
  return candidates[0] || null;
}

function readSizeRules(repositoryRoot) {
  const candidates = [
    path.join(repositoryRoot, 'standards', 'data', 'wall_mount_sizes.json'),
    path.join(__dirname, 'standards', 'data', 'wall_mount_sizes.json'),
    path.join(__dirname, '..', 'standards', 'data', 'wall_mount_sizes.json'),
  ];
  const file = candidates.find((candidate) => fs.existsSync(candidate));
  if (!file) throw new Error(`缺少壁挂尺寸规则：${candidates.join('；')}`);
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function readModelMap(repositoryRoot) {
  const candidates = [
    path.join(repositoryRoot, 'standards', 'data', 'wall_mount_model_map.json'),
    path.join(__dirname, 'standards', 'data', 'wall_mount_model_map.json'),
    path.join(__dirname, '..', 'standards', 'data', 'wall_mount_model_map.json'),
  ];
  const file = candidates.find((candidate) => fs.existsSync(candidate));
  if (!file) return null;
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function buildConversionStages(targetSize, productRule, modelMap = null) {
  const names = [
    '读取工作目录、顶层装配、骨架与关键特征',
    '修改骨架外尺寸',
    '切换目标屏并隐含其他屏',
    '修改壁挂圆角',
    '修改玻璃外尺寸、内尺寸和名称',
    '调整 VESA 孔并验证方向',
    '调整壁挂草绘',
    '适配风扇数量、间距和对称位置',
    '适配过滤棉',
    '重新计算并适配前框气弹簧',
    '重新计算并适配屏组件气弹簧',
    '适配铰链数量和间距',
    '返回顶层装配全面重新生成',
    '最终验证并保存',
  ];
  const verified = new Set(modelMap?.mapping_status?.steps_verified || []);
  const partial = new Set(modelMap?.mapping_status?.steps_partially_verified || []);
  return names.map((name, index) => ({
    stage: index + 1,
    name,
    target_size: String(targetSize),
    status: index === 0 || verified.has(index + 1)
      ? 'mapping_verified'
      : partial.has(index + 1)
        ? 'mapping_partially_verified'
        : 'requires_verified_model_mapping',
    rule: index === 1 ? { outer: productRule.outer } : undefined,
  }));
}

module.exports = {
  parseCreoFile,
  latestModelFamilies,
  chooseTopAssembly,
  chooseSkeleton,
  readSizeRules,
  readModelMap,
  buildConversionStages,
};
