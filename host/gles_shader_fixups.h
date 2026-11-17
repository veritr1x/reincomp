#pragma once
// Portable GLSL ES source fixups applied when guest shader source is handed to
// the host GLES implementation. No platform dependencies.
//
// Float literals at the top of the float range: HLSLcc (Unity's shader
// cross-compiler) spells FLT_MAX as 3.40282347e+38. That decimal lies above
// FLT_MAX (3.4028234664e+38) but within half a ULP, so IEEE round-to-nearest
// (what GLSL ES requires and Android drivers do) yields FLT_MAX. ANGLE parses
// it as +inf instead, which zeroes Unity's sign idiom
// clamp(x * FLT_MAX + 0.5, 0, 1) and makes e.g. the PPv2 LUT bake black.
// Such literals are rewritten to a spelling that is below FLT_MAX in decimal
// and rounds to exactly FLT_MAX.
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <string>

namespace bunker::gles_fixups {

// Returns true and rewrites `source` when a literal was changed.
inline bool ClampFloatMaxLiterals(std::string &source){
  bool changed=false;std::string out;out.reserve(source.size());
  // Values in (FLT_MAX, FLT_MAX + half ULP) round to FLT_MAX in float.
  const double limit=double(FLT_MAX)+std::ldexp(1.0,127-24);
  for(size_t i=0;i<source.size();){
    const char c=source[i];
    const bool starts=(std::isdigit(static_cast<unsigned char>(c))||(c=='.'&&i+1<source.size()&&std::isdigit(static_cast<unsigned char>(source[i+1]))))&&
        (i==0||!(std::isalnum(static_cast<unsigned char>(source[i-1]))||source[i-1]=='_'||source[i-1]=='.'));
    if(!starts){out+=c;++i;continue;}
    size_t j=i;while(j<source.size()&&(std::isdigit(static_cast<unsigned char>(source[j]))||source[j]=='.'))++j;
    bool exponent=false;
    if(j<source.size()&&(source[j]=='e'||source[j]=='E')){size_t k=j+1;if(k<source.size()&&(source[k]=='+'||source[k]=='-'))++k;
      if(k<source.size()&&std::isdigit(static_cast<unsigned char>(source[k]))){exponent=true;j=k;while(j<source.size()&&std::isdigit(static_cast<unsigned char>(source[j])))++j;}}
    size_t end=j;if(end<source.size()&&(source[end]=='f'||source[end]=='F'))++end;
    const std::string token=source.substr(i,j-i);
    if(exponent){const double value=std::strtod(token.c_str(),nullptr);
      if(value>double(FLT_MAX)&&value<limit){out+="3.40282346e+38";if(end>j)out+=source.substr(j,end-j);changed=true;i=end;continue;}}
    out+=source.substr(i,end-i);i=end;
  }
  if(changed)source.swap(out);
  return changed;
}

}  // namespace bunker::gles_fixups
