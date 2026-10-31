#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
namespace recomp::checkpoints {
struct ElementState {
    std::uint32_t type=1;float supply=0;bool available=false;
};
using ElementTail=std::array<std::uint32_t,4>;
inline bool validElement(const ElementState& s){return s.type>=1&&s.type<=4&&std::isfinite(s.supply)&&s.supply>=0&&s.supply<=1000000;}
inline ElementTail elementTail(const ElementState& s,std::uint32_t slotHash){
    const std::uint32_t bits=std::bit_cast<std::uint32_t>(s.supply);
    return {0x454c4d31,s.type,bits,slotHash^0x454c4d31^s.type^bits};
}
inline ElementState decodeElement(const ElementTail& t,std::uint32_t slotHash){
    ElementState s{t[1],std::bit_cast<float>(t[2]),true};
    if(t[0]!=0x454c4d31||t[3]!=(slotHash^t[0]^t[1]^t[2])||!validElement(s))return {};
    return s;
}
}
