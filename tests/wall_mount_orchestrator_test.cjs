'use strict';

const assert = require('assert');
const {
  parseCreoFile,
  chooseTopAssembly,
  chooseSkeleton,
  buildConversionStages,
} = require('../mcp/wall-mount-orchestrator.cjs');

assert.deepStrictEqual(parseCreoFile('M26Z0100000-V01.asm.12'), {
  file_name: 'M26Z0100000-V01.asm.12',
  model_name: 'M26Z0100000-V01',
  model_type: 'asm',
  version: 12,
});
assert.strictEqual(parseCreoFile('readme.txt'), null);

const models = [
  parseCreoFile('YQL600-255-22-10-210N.asm.4'),
  parseCreoFile('M26Z0100000-V01.asm.7'),
  parseCreoFile('M26Z0100000-V01_SKEL.prt.9'),
];
const top = chooseTopAssembly(models);
assert.strictEqual(top.model_name, 'M26Z0100000-V01');
assert.strictEqual(top.selection_rule, 'five_zero_assembly');
assert.strictEqual(chooseSkeleton(models, top.model_name).version, 9);

const fallback = chooseTopAssembly([
  parseCreoFile('YQL500-205-22-10-150N.asm.2'),
  parseCreoFile('M26Z0163001-V01.asm.3'),
]);
assert.strictEqual(fallback.model_name, 'M26Z0163001-V01');
assert.strictEqual(fallback.selection_rule, 'other_assembly_fallback');

const stages = buildConversionStages('65', { outer: [1660, 1113] });
assert.strictEqual(stages.length, 14);
assert.deepStrictEqual(stages[1].rule.outer, [1660, 1113]);
assert.strictEqual(stages[0].status, 'mapping_verified');

const mappedStages = buildConversionStages('65', { outer: [1660, 1113] }, {
  mapping_status: { steps_verified: [1, 2, 5, 6, 7], steps_partially_verified: [8] },
});
assert.strictEqual(mappedStages[4].status, 'mapping_verified');
assert.strictEqual(mappedStages[7].status, 'mapping_partially_verified');

console.log(JSON.stringify({ ok: true, stage_count: stages.length }));
