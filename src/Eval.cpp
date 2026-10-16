// The only ExprTk user. ExprTk takes minutes to compile, so it lives alone here: edits to Math.h/.cpp never
// rebuild it. Math.h is not included on purpose; keep compileCurve's signature in sync with it.
#include <QString>
#include <functional>
#include <memory>

// Everything beyond plain arithmetic is switched off (compile time).
#define exprtk_disable_string_capabilities
#define exprtk_disable_rtl_io
#define exprtk_disable_rtl_io_file
#define exprtk_disable_rtl_vecops
#define exprtk_disable_return_statement
#define exprtk_disable_break_continue
#define exprtk_disable_sc_andor
#define exprtk_disable_enhanced_features   // the specialised-node optimiser: most of the compile time
#define exprtk_disable_superscalar_unroll
#define exprtk_disable_caseinsensitivity
#include "../third_party/exprtk.hpp"

std::function<double(double)> compileCurve(const QString &expr) {
    struct Curve { double x = 0; exprtk::symbol_table<double> sym; exprtk::expression<double> e; };
    auto c = std::make_shared<Curve>();
    c->sym.add_variable("x", c->x);
    c->sym.add_constants();
    c->e.register_symbol_table(c->sym);
    static exprtk::parser<double> parser;   // ponytail: one shared parser, so curves compile on the GUI thread only
    if (!parser.compile(expr.toStdString(), c->e)) return {};
    return [c](double x) { c->x = x; return c->e.value(); };
}
