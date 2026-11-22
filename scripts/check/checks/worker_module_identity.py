"""Run production module selection/respin and recipe gates without a Vulkan device."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

NAME = "worker-module-identity"
STAGE = "source"
DESCRIPTION = "wire/plain recipe identities survive respin; ambiguous v4 recipes regenerate without deleting caches"
GUARDS = "AMD first warm: wire twins respun to plain, six worker misses and 33.872 seconds (2026-11-19)."


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++")
    if not cxx:
        result.fail("C++ compiler required for worker identity regression")
        return
    def read(name):
        return Path(ctx.path("indra/newview/" + name)).read_text()
    wire = read("vkgfx/gmdxrezfxvk.cpp").split('#include <vector>')[1].split('void LLCleanRendererVK::rezFxRecord')[0]
    renderer = read("vkgfx/llcleanrenderervk.cpp")
    spec = read("vkgfx/llcleanspecvk.cpp")
    worker = read("vkgfx/vk_pipeworker.cpp")
    header = read("gmdxrezfx.h")
    declarations = re.findall(r'    (?:const char\*|void|bool) wireModule(?:Spv)?\([^;]+;', header)
    selected = wire + function(renderer, 'VkShaderModule moduleFromSpv(')
    selected += '\n' + function(spec, 'VkShaderModule specModule(')
    selected += '\n' + function(spec, 'static bool worldRespin(')
    selected += '\n' + function(read('vkgfx/vk_pipeline.cpp'), 'std::string moduleLabel(')
    # Also execute the actual module lambda used when the rez effect changes at runtime.
    swap = renderer[renderer.index('auto module = [&](const uint32_t* code, size_t bytes, const char* name)'):]
    selected += '\nVkShaderModule swapModule(const uint32_t* code, size_t bytes, const char* name, bool want) {\n'
    selected += 'VkDevice dev = 1; std::vector<VkShaderModule> modules;\n'
    selected += function(swap, 'auto module = ') + '; return module(code, bytes, name); }\n'
    constants = worker[worker.index('constexpr char kRecipeMagic'):worker.index('struct Writer')]
    io = worker[worker.index('struct Writer'):worker.index('// Every GraphicsPipelineDesc field')]
    early = function(worker[worker.index('// #1411: a recipe of another format'):], '{')
    reader_gate = worker[worker.index('    Reader r(recipe);'):worker.index('    // #1411: THE VIEWER\'S DEVICE.')]
    parts = [constants, io, function(worker, 'bool writeHeader('),
             'bool earlyAccept(const std::string& recipe) { bool accepted = false; auto gate = [&]() {\n' + early + '\naccepted=true; }; gate(); return accepted; }',
             'int readAccept(const std::string& recipe) { const std::string recipePath="test";\n' + reader_gate + '\n(void)vendor; (void)device; (void)driver; return 0; }',
             function(worker, 'bool vkPipelineWorkerSaveRecipe(')]
    arrays = sorted(set(re.findall(r'\bkWorld\w+Spv\b', selected)))
    stubs = '\n'.join('static const uint32_t %s[] = {%du, %du}; static const size_t %s_bytes=sizeof(%s);' %
                      (name, i+100, i+200, name, name) for i, name in enumerate(arrays))
    source = '#include "' + ctx.path('scripts/check/test_worker_module_identity.cpp') + '"\n'
    source += 'namespace GMDXRezFx { bool wireframe() { return true; }\n' + '\n'.join(declarations) + '\n}\n'
    source += stubs + '\n' + selected + '\n' + '\n'.join(parts)
    source += '\n#include "' + ctx.path('scripts/check/test_worker_module_identity.cpp') + '"\n'
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp, 'test.cpp'); path.write_text(source)
        exe = str(Path(tmp, 'test'))
        proc = subprocess.run([cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', str(path), '-o', exe], capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
        proc = subprocess.run([exe], cwd=tmp, capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
    result.summary = "production viewer/spec/swap -> worker bytes match; v4 rejected, v5 regenerated; main/worker cache bytes unchanged"
