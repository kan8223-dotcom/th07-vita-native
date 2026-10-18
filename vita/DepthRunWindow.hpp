#pragma once
#include <algorithm>

namespace th07_runwindow {
// RLE rows are ordered, disjoint, half-open intervals. Visit precisely the
// intervals that can survive the caller's original integer clipping, in the
// original order. Short rows retain a linear walk; long rows skip the prefix.
template<class Row, class Visitor>
void visit(const Row& row, int lo, int hi, Visitor&& visitor)
{
    if (lo >= hi) return;
    auto it = row.begin();
    if (row.size() >= 16)
        it = std::lower_bound(it, row.end(), lo,
            [](const auto& run, int boundary) { return run.second <= boundary; });
    for (; it != row.end(); ++it) {
        if (it->second <= lo) continue;
        if (it->first >= hi) break;
        visitor(*it);
    }
}
}
