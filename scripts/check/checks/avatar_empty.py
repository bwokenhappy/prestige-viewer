"""Exercise actual incremental renderer classification against the GLSL face rule."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
NAME='avatar-empty'
STAGE='source'
DESCRIPTION='empty-avatar option retains every shader-recognized face through dirty updates, release and resize'
GUARDS='False empty classifications must not remove avatar depth; upload uncertainty must retain the original path.'
def cut(s, start):
 i=s.index(start);j=s.index('{',i)+1;depth=1
 while depth:
  depth+=(s[j]=='{')-(s[j]=='}');j+=1
 return s[i:j]
def run(ctx,result):
 cpp=Path(ctx.path('indra/newview/vkgfx/llcleanrenderervk.cpp')).read_text()
 glsl=Path(ctx.path('indra/vkclean/shaders/clean_draw_view.glsl')).read_text()
 rule=cut(glsl,'bool avatarDepthFace(')
 update=cut(cpp,'void LLCleanRendererVK::updateInstClassCounts(').replace('LLCleanRendererVK::','Fixture::')
 # Actual route guard, exercised with stale publication and pending upload combinations below.
 guard=re.search(r'const bool empty = ([\s\S]*?);',cpp[cpp.index('RenderVKPerfAvatarDepthEmpty'):])
 if not guard:result.fail('empty-set route absent');return
 pre=r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
using uint=uint32_t;
struct Fixture {
 struct Instance {uint32_t lod_group_id=0,flags=0,skin_id=0;};
 static constexpr uint32_t NUM_BUCKETS=10,kClassRing=4;
 std::vector<uint8_t> mCpuInstances,mInstClass;
 std::vector<std::pair<uint32_t,uint32_t>> mInstRuns;
 uint32_t mInstLiveCount=0,mAvDepthFaceCount=0,mClassRingPos=0;
 uint32_t mClassCount[NUM_BUCKETS]{},mClassDynCount[NUM_BUCKETS]{};
 uint32_t mClassRing[kClassRing][2][NUM_BUCKETS]{};
 bool mAvDepthPublishedEmpty=false,mInstFullUpload=false;
 void updateInstClassCounts(size_t);
 bool skip(bool s_empty) const { return GUARD; }
 void set(const std::vector<Instance>& rows,uint32_t first=0) {
  mCpuInstances.resize(rows.size()*sizeof(Instance));
  if(!rows.empty())std::memcpy(mCpuInstances.data(),rows.data(),mCpuInstances.size());
  mInstRuns={{first,(uint32_t)rows.size()}};updateInstClassCounts(0);
 }
};
'''.replace('GUARD',guard.group(1))
 fixture=r'''
int main(){
 Fixture f; std::vector<Fixture::Instance> rows;
 for(uint flex=0;flex<2;++flex)for(uint flags=0;flags<65536;++flags){
  const uint v=flags|(flex?524288u:0u);
  rows.push_back({0,v,(flags&16u)?0xFFFFFFFFu:0u});
 }
 f.set(rows);uint expected=0;
 for(const auto&r:rows)expected+=avatarDepthFace(r.flags);
 if(f.mAvDepthFaceCount!=expected)return 1;
 // Duplicate/overlapping dirty spans must not double-count unchanged rows.
 f.mInstRuns={{0,100},{50,1000},{0,(uint)rows.size()}};f.updateInstClassCounts(0);
 if(f.mAvDepthFaceCount!=expected)return 2;
 for(uint i=0;i<rows.size();++i)if(i%3==0)rows[i].lod_group_id=0xFFFFFFFFu;
 f.set(rows);expected=0;
 for(const auto&r:rows)expected+=r.lod_group_id!=0xFFFFFFFFu&&avatarDepthFace(r.flags);
 if(f.mAvDepthFaceCount!=expected)return 3;
 // Shrink removes counts for the discarded tail; then partial flag changes.
 rows.resize(100);f.set(rows);expected=0;
 for(const auto&r:rows)expected+=r.lod_group_id!=0xFFFFFFFFu&&avatarDepthFace(r.flags);
 if(f.mAvDepthFaceCount!=expected)return 4;
 for(uint i=50;i<100;++i)rows[i]={0,0,0};f.set(rows,50);expected=0;
 for(const auto&r:rows)expected+=r.lod_group_id!=0xFFFFFFFFu&&avatarDepthFace(r.flags);
 if(f.mAvDepthFaceCount!=expected)return 5;
 rows.assign(100,{0,0,0});f.set(rows);
 if(f.mAvDepthFaceCount || f.skip(true))return 6; // old GPU publication uncertain
 f.mAvDepthPublishedEmpty=true;
 if(!f.skip(true)||f.skip(false))return 7;
 f.mInstFullUpload=true;if(f.skip(true))return 8;
 f.mInstFullUpload=false;f.mInstLiveCount=0xFFFFFFFFu;if(f.skip(true))return 9;
 f.mInstLiveCount=0;rows[0]={0,512u,0};f.set(rows);if(f.skip(true))return 10;
 std::puts("131072 shader classifications, incremental overlap/release/shrink/partial changes and conservative route guards passed");
}
'''
 compiler=os.environ.get('CXX') or shutil.which('c++')
 if not compiler:result.fail('C++ compiler required');return
 with tempfile.TemporaryDirectory() as tmp:
  src=Path(tmp)/'avatar.cpp';exe=Path(tmp)/'avatar';src.write_text(pre+rule+update+fixture)
  b=subprocess.run([compiler,'-std=c++17','-O2','-Wall',str(src),'-o',str(exe)],capture_output=True,text=True)
  if b.returncode:result.fail(b.stdout+b.stderr);return
  r=subprocess.run([str(exe)],capture_output=True,text=True)
  if r.returncode:result.fail(r.stdout+r.stderr+' exit='+str(r.returncode));return
 result.summary=r.stdout.strip()
