const fs = require('node:fs');
const wasmPath = process.argv[2];
if (process.argv.length !== 3) {
  console.error('用法: node run_multi_resource_cpu_test.js <生成的 .wasm 路径>');
  process.exit(2);
}
let memory, heap, passed = false;
const live = new Set();
function malloc(n) {
  const p = heap;
  heap = (heap + n + 15) & ~15;
  if (heap >= memory.buffer.byteLength) throw new Error('测试堆耗尽');
  live.add(p);
  return p;
}
const env = {
  malloc,
  calloc(n, size) {
    const p = malloc(n * size);
    new Uint8Array(memory.buffer).fill(0, p, p + n * size);
    return p;
  },
  free(p) { if (p && !live.delete(p)) throw new Error(`重复释放 ${p}`); },
  memcpy(dst, src, n) { new Uint8Array(memory.buffer).copyWithin(dst, src, src + n); return dst; },
  memset(dst, v, n) { new Uint8Array(memory.buffer).fill(v & 255, dst, dst + n); return dst; },
  exit(n) { throw new Error(`测试退出 ${n}`); },
  test_fail(line, p) {
    const bytes = new Uint8Array(memory.buffer);
    let end = p; while (bytes[end]) ++end;
    throw new Error(`第 ${line} 行断言失败: ${new TextDecoder().decode(bytes.subarray(p, end))}`);
  },
  test_pass(n) {
    if (passed || n !== 263020) throw new Error(`成功回调次数或断言数异常: ${n}，预期 263020`);
    passed = true;
    console.log(`通过 ${n} 次断言；实际提取的资源方法，CPU 替身，不是 GPU 验收。`);
  }
};
(async () => {
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), { env });
  memory = instance.exports.memory;
  heap = Number(instance.exports.__heap_base.value);
  instance.exports.run_tests();
  if (!passed) throw new Error('测试未报告成功');
  if (live.size) throw new Error(`CPU 替身存在 ${live.size} 项未释放分配`);
  console.log('CPU 替身分配已全部回收。');
})().catch(e => { console.error(e); process.exitCode = 1; });
