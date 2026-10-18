#pragma once
#include <cstddef>
namespace th07_spritequad {
// AnmManager::PushSprite's canonical native triangle order, not the other
// winding sometimes used for a generic QUADS primitive.
inline constexpr unsigned order[6]={0,1,2,1,2,3};
template<class Vertex>
void finishExpanded(Vertex* six)
{
    six[3]=six[1];six[4]=six[2];
}
}
