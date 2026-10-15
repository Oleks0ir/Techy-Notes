#undef NDEBUG   // asserts must run in Release builds too
#include "Sheet.h"
#include <cassert>
#include <cstdio>

int main() {
    Sheet s;
    assert(s.pages.contains(QPoint(0, 0)));
    assert(s.pageRect(QPoint(-1, 2)) == QRectF(-210, 594, 210, 297));
    std::puts("selftest ok");
}
