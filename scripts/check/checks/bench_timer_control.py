"""Replay timestamp-overhead controls must actually disable timers and report that state."""
import os
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

NAME='bench-timer-control'
STAGE='source'
DESCRIPTION='benchmark GPU timer opt-out is honored, reported and restored'
GUARDS='A timers-OFF replay silently forcing timestamps ON and labeling the report true.'

PREAMBLE=r'''
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>
struct LLSD { bool value; explicit LLSD(bool v=false):value(v){} };
struct LLControlVariable { LLSD value; LLSD getValue(){return value;} void setValue(LLSD v,bool){value=v;} };
struct Store {
 std::map<std::string,LLControlVariable> controls;
 LLControlVariable* getControl(const std::string& k){auto i=controls.find(k);return i==controls.end()?nullptr:&i->second;}
 bool getBOOL(const std::string& k){return controls.at(k).value.value;}
} gSavedSettings;
struct Series {std::vector<float> v;};
int stats(const Series& s){return static_cast<int>(s.v.size());}
struct SegStats {Series gpu_ms;};
struct Run {std::vector<std::pair<std::string,LLSD>> saved;SegStats overall;};
'''


def run(ctx,result):
    src=open(ctx.path('indra/newview/slbench.cpp'),encoding='utf-8').read()
    helpers=src[src.index('    void saveAndSet('):src.index('    void applyCamera(')]
    enable=re.search(r'saveAndSet\(r, "RenderVKGpuTimers",[^\n]+;',src)
    metadata=re.search(r'settings\["gpu_timers"\] = [^\n]+;',src)
    if not enable or not metadata:result.fail('production timer enable/report expressions missing');return
    gpu_segment=re.search(r'[^\n]*o\["gpu_ms"\] = stats\(g.gpu_ms\);',src)
    gpu_overall=re.search(r'[^\n]*o\["gpu_ms"\] = stats\(r.overall.gpu_ms\);',src)
    if not gpu_segment or not gpu_overall:result.fail('production GPU report expressions missing');return
    gpu_code=gpu_segment.group()+'\n'+gpu_overall.group()
    fixture=PREAMBLE+helpers+r'''
int main(){
 for(bool initial:{false,true}) for(bool requested:{false,true}){
  gSavedSettings.controls.clear();
  gSavedSettings.controls["RenderVKGpuTimers"].value=LLSD(initial);
  gSavedSettings.controls["BenchGpuTimers"].value=LLSD(requested);
  Run r;
'''+enable.group()+r'''
  assert(gSavedSettings.getBOOL("RenderVKGpuTimers")==requested);
  std::map<std::string,bool> settings;
'''+metadata.group()+r'''
  assert(settings["gpu_timers"]==requested);
  restoreSettings(r);
  assert(r.saved.empty() && gSavedSettings.getBOOL("RenderVKGpuTimers")==initial);
 }
 Run r;SegStats g;std::map<std::string,int> o;
'''+gpu_code+r'''
 assert(!o.count("gpu_ms"));
 g.gpu_ms.v.push_back(0.f);r.overall.gpu_ms.v.push_back(0.f);
'''+gpu_code+r'''
 assert(o.count("gpu_ms") && o["gpu_ms"]==1);
 std::puts("4 production enable/report/restore combinations + unavailable GPU field omission PASS");
}
'''
    elems=list(ET.parse(ctx.path('indra/newview/app_settings/settings.xml')).getroot().find('map'))
    matches=[i for i,x in enumerate(elems) if x.tag=='key' and x.text=='BenchGpuTimers']
    if not matches:result.fail('BenchGpuTimers control missing');return
    values=list(elems[matches[0]+1]);pairs=dict(zip([x.text for x in values[::2]],[x.text for x in values[1::2]]))
    if pairs.get('Persist')!='0' or pairs.get('Type')!='Boolean' or pairs.get('Value')!='1':
        result.fail('BenchGpuTimers must be nonpersistent and default ON')
    with tempfile.TemporaryDirectory(prefix='bench-timer-',dir=os.environ.get('TMPDIR')) as tmp:
        source=os.path.join(tmp,'test.cpp');exe=os.path.join(tmp,'test')
        with open(source,'w',encoding='utf-8') as f:f.write(fixture)
        c=subprocess.run(['c++','-std=c++17','-O2',source,'-o',exe],capture_output=True,text=True)
        if c.returncode:result.fail(c.stdout+c.stderr);return
        r=subprocess.run([exe],capture_output=True,text=True)
        if r.returncode:result.fail(r.stdout+r.stderr);return
        result.summary=r.stdout.strip()
