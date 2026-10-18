#pragma once
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

namespace th07_scratch {
// Active length is separate from the initialized high-water length. Every
// appended element must be overwritten before it is read or published.
template<class T>
T* append(std::vector<T>& storage,std::size_t& active,std::size_t count)
{
    static_assert(std::is_trivially_copyable<T>::value,"scratch is raw copied");
    const std::size_t first=active;
    if(storage.size()<first+count)storage.resize(first+count);
    active+=count;
    return count?storage.data()+first:nullptr;
}
template<class T>
void copy(std::vector<T>& storage,std::size_t& active,const T* source,std::size_t count)
{
    if(count)std::memcpy(append(storage,active,count),source,count*sizeof(T));
}
}
