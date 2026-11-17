#pragma once
// Host implementations of guest functions whose lifted bodies stop at an
// instruction the cohort lifter left unsupported (__remill_error). Each one
// replaces the whole guest function through recomp::AddGuestOverride and
// mirrors the original AArch64 operation order exactly (binary32, no fused
// multiply-add, default FPCR). Platform-neutral: no Windows APIs.
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../runtime/runtime.h"

namespace bunker::windows::lift_gaps {

namespace detail {
struct Vec3 {float x,y,z;};
inline float LoadF32(addr_t address){float v;std::memcpy(&v,reinterpret_cast<const void*>(address),4);return v;}
inline Vec3 LoadVec3(addr_t address){return {LoadF32(address),LoadF32(address+4),LoadF32(address+8)};}
inline void SetS0(State &s,float value){std::memset(&s.simd.v[0],0,sizeof(s.simd.v[0]));std::memcpy(&s.simd.v[0],&value,4);}

// libunity 0x70160380: distance from point p to segment [a,b].
__attribute__((optimize("fp-contract=off")))
inline float SegmentDistance(Vec3 p,Vec3 a,Vec3 b){
  const float dx=b.x-a.x,dy=b.y-a.y,dz=b.z-a.z;
  float len2=dx*dx;len2=len2+dy*dy;len2=len2+dz*dz;
  const float wx=p.x-a.x,wy=p.y-a.y,wz=p.z-a.z;
  float sum;
  bool to_a=len2==0.0f;float t=0;
  if(!to_a){float dot=dx*wx;dot=dot+dy*wy;dot=dot+dz*wz;t=dot/len2;to_a=t<0.0f;}
  if(to_a){sum=wx*wx;sum=sum+wy*wy;sum=wz*wz+sum;}
  else if(!(t<=1.0f)){const float ex=p.x-b.x,ey=p.y-b.y,ez=p.z-b.z;sum=ex*ex;sum=sum+ey*ey;sum=sum+ez*ez;}
  else{
    const float qx=a.x+dx*t,qy=a.y+dy*t,qz=a.z+dz*t;
    const float ex=p.x-qx,ey=p.y-qy,ez=p.z-qz;
    float xy=ex*ex;xy=xy+ey*ey;sum=ez*ez+xy;
  }
  return std::sqrt(sum);
}

// libunity 0x7016047c: 0 when the 2D point is inside the quad (crossing
// test), otherwise the minimum distance to its four edges. x0 = four
// 12-byte vertices, x1 = point (x, y). The lifted body stops at the final
// FCSEL (0x70160590).
__attribute__((optimize("fp-contract=off")))
inline float QuadDistance(addr_t quad,addr_t point){
  const float px=LoadF32(point),py=LoadF32(point+4);
  auto vx=[&](unsigned i){return LoadF32(quad+12*(i&3));};
  auto vy=[&](unsigned i){return LoadF32(quad+12*(i&3)+4);};
  bool inside=false;unsigned previous=4;
  for(unsigned k=0;k<5;++k){
    const unsigned j=previous&3,i=k&3;previous=k;
    const float yi=vy(i),yj=vy(j);
    if((yi>py)==(yj>py))continue;
    const float xj=vx(j),xi=vx(i);
    const float crossing=xi+((py-yi)*(xj-xi))/(yj-yi);
    if(px<crossing)inside=!inside;
  }
  if(inside)return 0.0f;
  float best=-1.0f;
  for(unsigned i=1;i<5;++i){
    const Vec3 a{vx(i-1),vy(i-1),0.0f},b{vx(i),vy(i),0.0f};
    const float d=SegmentDistance({px,py,0.0f},a,b);
    if(d<best||(!(d<best)&&best<0.0f))best=d;
  }
  return best;
}
}  // namespace detail

// Register before recomp::PublishGuestOverrides().
inline void AddOverrides(){
  static const bool trace=[]{const char *v=std::getenv("BUNKER_LIFT_GAP_TRACE");return v&&std::strcmp(v,"0");}();
  recomp::AddGuestOverride(0x7016047c,[](State &s){
    const addr_t quad=s.gpr.x0.qword,point=s.gpr.x1.qword;const float result=detail::QuadDistance(quad,point);
    if(trace){static std::atomic<uint64_t> last{0};static std::atomic<int> n{0};uint64_t key;std::memcpy(&key,reinterpret_cast<const void*>(point),8);
      if(last.exchange(key)!=key)n=0;if(n.fetch_add(1)<20){using detail::LoadF32;
      std::fprintf(stderr,"Lift gaps: quad (%g,%g) (%g,%g) (%g,%g) (%g,%g) point (%g,%g) -> %g LR=%#llx\n",LoadF32(quad),LoadF32(quad+4),LoadF32(quad+12),LoadF32(quad+16),
        LoadF32(quad+24),LoadF32(quad+28),LoadF32(quad+36),LoadF32(quad+40),LoadF32(point),LoadF32(point+4),result,(unsigned long long)s.gpr.x30.qword);}}
    detail::SetS0(s,result);});
  std::fprintf(stderr,"Lift gaps: override 0x7016047c (quad distance, unsupported FCSEL at 0x70160590)\n");
}

}  // namespace bunker::windows::lift_gaps
