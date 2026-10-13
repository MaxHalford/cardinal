#pragma once

#include "duckdb/function/function_set.hpp"

namespace duckdb {

//! CALL cardinal_fit_eager(['table']): fit models now, rather than when queries need them,
//! and save them with the keys learned so far. No table: every table.
TableFunctionSet FitFunctions();

} // namespace duckdb
