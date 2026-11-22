// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (c) 2026 Slipstream Viewer contributors
int main(){
 using namespace SLGraphicsFeatureDefaults;
 LLFeatureManager fm;
 fm.addFeature(settingNames[0],true,1);fm.addFeature(settingNames[1],true,256);fm.addFeature(settingNames[2],true,1);
 const std::array<F32,7> ca={0,0,0,0,1,1,1},lights={6,6,8,16,16,256,256},reuse={1,1,.5f,.35f,.25f,.1f,0};
 for(size_t i=0;i<7;++i){fm.tiers[i].addFeature(settingNames[0],true,ca[i]);fm.tiers[i].addFeature(settingNames[1],true,lights[i]);fm.tiers[i].addFeature(settingNames[2],true,reuse[i]);}
 auto fresh=[](){LLControlGroup s;s.add(markerName,99,LLSD::emptyMap());s.add(settingNames[0],TYPE_BOOLEAN,true);s.add(settingNames[1],TYPE_U32,S32(256));s.add(settingNames[2],TYPE_F32,.25);return s;};
 auto reset=[](){overrides.reset();unreadableOverride=false;savedInput=LLSD::emptyMap();readCalls=0;readOK=true;};
 // Every level, no broad table mutation, marker-fast path, and factory-equal persistence.
 for(U32 level=0;level<7;++level){reset();auto s=fresh();s.quality=level;initialize(s,fm,"profile");
  assert(s.getControl(settingNames[0]).p->current.asBoolean()==bool(ca[level]));
  assert(s.getControl(settingNames[1]).p->current.number()==lights[level]);
  assert(s.getControl(settingNames[2]).p->current.number()==double(reuse[level]));
  for(auto n:settingNames)assert(s.getControl(n).p->shouldSave(true));
  assert(validMarker(s.getControl(markerName).p->current));assert(fm.mFeatures[settingNames[0]].mRecommendedLevel==1);
  auto before=s.getControl(settingNames[0]).p->current;s.quality=level==0?6:0;int reads=readCalls;initialize(s,fm,"profile");assert(readCalls==reads);assert(llsd_equals(before,s.getControl(settingNames[0]).p->current));
 }
 // Explicit saved true equals factory true: must remain true at Low, then stay saved.
 reset();auto explicitDefault=fresh();savedInput[settingNames[0]]["Value"]=true;initialize(explicitDefault,fm,"profile");assert(explicitDefault.getControl(settingNames[0]).p->current.asBoolean());assert(explicitDefault.getControl(settingNames[0]).p->shouldSave(true));
 reset();auto explicitOff=fresh();explicitOff.quality=6;explicitOff.getControl(settingNames[0]).p->current=false;savedInput[settingNames[0]]["Value"]=false;initialize(explicitOff,fm,"profile");assert(!explicitOff.getControl(settingNames[0]).p->current.asBoolean());
 // Fresh preset results and an explicit live nondefault are not reset.
 reset();auto preset=fresh();preset.getControl(settingNames[0]).p->current=false;preset.getControl(settingNames[1]).p->current=S32(6);preset.getControl(settingNames[2]).p->current=1.;initialize(preset,fm,"profile");assert(!preset.getControl(settingNames[0]).p->current.asBoolean());assert(preset.getControl(settingNames[1]).p->current.number()==6);
 reset();auto live=fresh();live.quality=6;live.getControl(settingNames[1]).p->current=S32(13);initialize(live,fm,"profile");assert(live.getControl(settingNames[1]).p->current.number()==13);
 // A new pending registration doesn't disturb settings already completed.
 reset();auto partial=fresh();partial.getControl(markerName).p->current[settingNames[0]]=true;initialize(partial,fm,"profile");assert(partial.getControl(settingNames[0]).p->current.asBoolean());assert(partial.getControl(settingNames[1]).p->current.number()==6);
 // Temporary equal-default --set defers; it is not persisted as a permanent high-tier choice.
 reset();auto cli=fresh();noteOverride(settingNames[0]);initialize(cli,fm,"profile");assert(cli.getControl(settingNames[0]).p->current.asBoolean());assert(!cli.getControl(markerName).p->current.has(settingNames[0]));assert(!cli.getControl(settingNames[0]).p->shouldSave(true));overrides.reset();initialize(cli,fm,"profile");assert(!cli.getControl(settingNames[0]).p->current.asBoolean());
 reset();auto transient=fresh();transient.getControl(settingNames[0]).p->transient=true;initialize(transient,fm,"profile");assert(!transient.getControl(markerName).p->current.has(settingNames[0]));
 // File/validation/allocation failures leave work pending, never terminate startup.
 reset();auto broken=fresh();readOK=false;initialize(broken,fm,"profile");assert(broken.getControl(markerName).p->current.size()==0);
 reset();auto refused=fresh();refused.getControl(settingNames[0]).p->accept=false;initialize(refused,fm,"profile");assert(!refused.getControl(markerName).p->current.has(settingNames[0]));
 reset();auto oom=fresh();oom.getControl(settingNames[0]).p->fail=true;initialize(oom,fm,"profile");assert(oom.getControl(markerName).p->current.size()==0);
 reset();auto full=fresh();for(int i=0;i<64;++i)full.getControl(markerName).p->current["future"+std::to_string(i)]=true;initialize(full,fm,"profile");assert(full.getControl(markerName).p->current.size()==64);
 reset();auto invalid=fresh();invalid.getControl(markerName).p->current["bad"]=S32(1);initialize(invalid,fm,"profile");assert(readCalls==0);
 reset();auto fallback=fresh();fallback.quality=999;initialize(fallback,fm,"profile");assert(fallback.getControl(settingNames[1]).p->current.number()==8);
 // Hardware availability masking takes precedence, through the real mask methods.
 reset();auto unavailable=fresh();unavailable.quality=6;fm.mFeatures[settingNames[0]].mAvailable=false;initialize(unavailable,fm,"profile");assert(!unavailable.getControl(settingNames[0]).p->current.asBoolean());
 LLSD value;LLControlVariable integer(TYPE_U32,S32(0));assert(!numericValue(integer,INFINITY,value));assert(!numericValue(integer,2147483648.f,value));assert(!numericValue(integer,-1.f,value));
 std::puts("graphics feature defaults: production adapter, all tiers, saved equality, once-only, CLI, failure/bounds PASS");
}
