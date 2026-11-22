// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (c) 2026 Slipstream Viewer contributors
// Data/API stubs for the unmodified production initializer and feature-mask methods.
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <ostream>
#include <cstdio>
#include <string>
#include <variant>
#include <utility>
#include "slgraphicsfeaturedefaults.h"
using U32=uint32_t; using S32=int32_t; using F32=float; using F64=double;
struct LLSD {
 using Integer=S32; using Map=std::map<std::string,LLSD>;
 std::variant<std::monostate,bool,S32,double,std::shared_ptr<Map>> v;
 LLSD()=default; LLSD(bool x):v(x){} LLSD(S32 x):v(x){} LLSD(double x):v(x){}
 static LLSD emptyMap(){LLSD x;x.v=std::make_shared<Map>();return x;}
 bool isMap()const{return std::holds_alternative<std::shared_ptr<Map>>(v);}
 bool isBoolean()const{return std::holds_alternative<bool>(v);}
 bool has(const std::string& k)const{return isMap()&&std::get<std::shared_ptr<Map>>(v)->count(k);}
 S32 size()const{return isMap()?static_cast<S32>(std::get<std::shared_ptr<Map>>(v)->size()):0;}
 bool asBoolean()const{return isBoolean()&&std::get<bool>(v);}
 double number()const {if(auto p=std::get_if<S32>(&v))return *p;if(auto p=std::get_if<double>(&v))return *p;return asBoolean()?1.:0.;}
 LLSD& operator[](const std::string& k){if(!isMap())*this=emptyMap();auto& p=std::get<std::shared_ptr<Map>>(v);if(!p.unique())p=std::make_shared<Map>(*p);return (*p)[k];}
 const LLSD& operator[](const std::string& k)const{static const LLSD empty;if(!has(k))return empty;return std::get<std::shared_ptr<Map>>(v)->at(k);}
 auto beginMap()const{return std::get<std::shared_ptr<Map>>(v)->begin();}
 auto endMap()const{return std::get<std::shared_ptr<Map>>(v)->end();}
};
bool llsd_equals(const LLSD&a,const LLSD&b){
 if(a.v.index()!=b.v.index())return false;
 if(a.isMap()){if(a.size()!=b.size())return false;for(auto&[k,x]:*std::get<std::shared_ptr<LLSD::Map>>(a.v)){if(!b.has(k)||!llsd_equals(x,std::get<std::shared_ptr<LLSD::Map>>(b.v)->at(k)))return false;}return true;}
 return a.v==b.v;
}
bool llsd_compare(const LLSD&a,const LLSD&b){return llsd_equals(a,b);}
enum {TYPE_BOOLEAN,TYPE_F32,TYPE_U32,TYPE_S32};
struct LLControlVariable {
 enum ePersist{PERSIST_NO,PERSIST_NONDFT,PERSIST_ALWAYS};
 int kind; LLSD current, factory; ePersist mPersist=PERSIST_NONDFT;
 bool transient=false, accept=true, fail=false;
 explicit LLControlVariable(int k,LLSD value):kind(k),current(value),factory(value){}
 bool isType(int k)const{return kind==k;}
 bool isDefault()const{return llsd_equals(current,factory)&&!transient;}
 bool hasUnsavedValue()const{return transient;}
 bool isPersisted()const{return mPersist!=PERSIST_NO;}
 void setPersist(ePersist p){mPersist=p;}
 LLSD getSaveValue()const{return current;}
 LLSD getDefault()const{return factory;}
 bool shouldSave(bool);
 void setValue(const LLSD& value){if(fail)throw std::bad_alloc();if(accept)current=value;}
};
struct ControlPtr {LLControlVariable*p; LLControlVariable*get()const{return p;} operator LLControlVariable*()const{return p;}};
struct LLControlGroup {
 std::map<std::string,std::unique_ptr<LLControlVariable>> controls;
 U32 quality=0;
 ControlPtr getControl(const std::string& k){auto i=controls.find(k);return {i==controls.end()?nullptr:i->second.get()};}
 U32 getU32(const std::string&)const{return quality;}
 void add(const std::string& k,int type,LLSD v){controls[k]=std::make_unique<LLControlVariable>(type,v);}
};
struct Sink {template<class T>Sink&operator<<(const T&){return *this;} Sink&operator<<(std::ostream&(*)(std::ostream&)){return *this;}};
#define LL_WARNS(...) Sink()
#define LL_WARNS_ONCE(...) Sink()
#define LL_INFOS(...) Sink()
#define LL_DEBUGS_ONCE(...) Sink()
#define LL_DEBUGS(...) Sink()
#define LL_DEBUGS_ON(...) Sink()
#define LL_CONT Sink()
#define LL_ENDL ""
template<class T>T llmin(T a,T b){return std::min(a,b);}
struct LLFeatureInfo{std::string mName;bool mAvailable=true;F32 mRecommendedLevel=0;LLFeatureInfo()=default;LLFeatureInfo(const std::string&n,bool a,F32 x):mName(n),mAvailable(a),mRecommendedLevel(x){}};
class LLFeatureList {
public:
 using feature_map_t=std::map<std::string,LLFeatureInfo>;
 std::string mName;feature_map_t mFeatures;
 explicit LLFeatureList(const std::string&n):mName(n){}
 auto&getFeatures(){return mFeatures;} void dump(){}
 void addFeature(const std::string&,bool,F32);bool isFeatureAvailable(const std::string&);F32 getRecommendedValue(const std::string&);bool maskList(LLFeatureList&);
};
class LLFeatureManager:public LLFeatureList {
public:
 std::array<LLFeatureList,7> tiers={LLFeatureList("Low"),LLFeatureList("LowMid"),LLFeatureList("Mid"),LLFeatureList("MidHigh"),LLFeatureList("High"),LLFeatureList("HighUltra"),LLFeatureList("Ultra")};
 LLFeatureManager():LLFeatureList("base"){}
 bool isValidGraphicsLevel(U32 n)const{return n<tiers.size();}U32 getMaxGraphicsLevel()const{return 6;}S32 getGPUClass()const{return 2;}
 std::string getNameForGraphicsLevel(U32 n)const{return tiers[n].mName;}
 LLFeatureList*findMask(const std::string&n){for(auto&t:tiers)if(t.mName==n)return &t;return nullptr;}
};
// Implementations below are extracted from production source by the source check.
