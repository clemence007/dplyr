#include "pch.h"
#include <dplyr/main.h>

#include <boost/scoped_ptr.hpp>

#include <tools/Quosure.h>

#include <dplyr/checks.h>

#include <dplyr/GroupedDataFrame.h>
#include <dplyr/NaturalDataFrame.h>

#include <dplyr/Result/LazyRowwiseSubsets.h>
#include <dplyr/Result/CallProxy.h>

#include <dplyr/Gatherer.h>
#include <dplyr/NamedListAccumulator.h>

#include <dplyr/bad.h>
#include <dplyr/tbl_cpp.h>

using namespace Rcpp;
using namespace dplyr;

void check_not_groups(const QuosureList&, const RowwiseDataFrame&) {}
void check_not_groups(const QuosureList&, const NaturalDataFrame&) {}

void check_not_groups(const QuosureList& quosures, const GroupedDataFrame& gdf) {
  int n = quosures.size();
  for (int i = 0; i < n; i++) {
    if (gdf.has_group(quosures[i].name()))
      bad_col(quosures[i].name(), "can't be modified because it's a grouping variable");
  }
}

static
SEXP validate_unquoted_value(SEXP value, int nrows, SymbolString& name, const char* detail) {
  if (is_vector(value))
    check_length(Rf_length(value), nrows, detail, name);
  else
    bad_col(name, "is of unsupported type {type}", _["type"] = Rf_type2char(TYPEOF(value)));
  return value;
}

template <typename Data, typename Subsets>
DataFrame mutate_grouped(const DataFrame& df, const QuosureList& dots) {
  LOG_VERBOSE << "initializing proxy";

  typedef GroupedCallProxy<Data, Subsets> Proxy;
  Data gdf(df);
  int nexpr = dots.size();
  check_not_groups(dots, gdf);

  Proxy proxy(gdf);

  LOG_VERBOSE << "copying data to accumulator";

  NamedListAccumulator<Data> accumulator;
  int ncolumns = df.size();
  CharacterVector column_names = df.names();
  for (int i = 0; i < ncolumns; i++) {
    accumulator.set(column_names[i], df[i]);
  }

  LOG_VERBOSE << "processing " << nexpr << " variables";

  for (int i = 0; i < nexpr; i++) {
    Rcpp::checkUserInterrupt();
    const NamedQuosure& quosure = dots[i];

    Environment env = quosure.env();
    Shield<SEXP> call_(quosure.expr());
    SEXP call = call_;
    SymbolString name = quosure.name();
    proxy.set_env(env);

    LOG_VERBOSE << "processing " << name.get_utf8_cstring();

    RObject variable;

    if (TYPEOF(call) == LANGSXP || TYPEOF(call) == SYMSXP) {
      proxy.set_call(call);
      boost::scoped_ptr<Gatherer> gather(gatherer<Data, Subsets>(proxy, gdf, name));
      variable = gather->collect();
      if (Rf_isNull(variable)) {
        accumulator.rm(name);
        continue;
      }
    } else if (Rf_length(call) == 1) {
      boost::scoped_ptr<Gatherer> gather(constant_gatherer(call, gdf.nrows(), name));
      variable = gather->collect();
    } else if (Rf_isNull(call)) {
      accumulator.rm(name);
      continue;
    } else {
      variable = validate_unquoted_value(call, gdf.nrows(), name, check_length_message<Data>());
    }

    if (!Rcpp::traits::same_type<Data, NaturalDataFrame>::value) {
      Rf_setAttrib(variable, R_NamesSymbol, R_NilValue);
    }

    proxy.input(name, variable);
    accumulator.set(name, variable);
  }

  // basic structure of the data frame
  List res = accumulator;
  set_class(res, get_class(df));
  set_rownames(res, df.nrows());

  // let the grouping class deal with the rest, e.g. the
  // groups attribute
  return Data(res, gdf).data();
}


// [[Rcpp::export]]
SEXP mutate_impl(DataFrame df, QuosureList dots) {
  if (dots.size() == 0) return df;
  check_valid_colnames(df);
  if (is<RowwiseDataFrame>(df)) {
    return mutate_grouped<RowwiseDataFrame, LazyRowwiseSubsets>(df, dots);
  } else if (is<GroupedDataFrame>(df)) {
    return mutate_grouped<GroupedDataFrame, LazyGroupedSubsets>(df, dots);
  } else {
    return mutate_grouped<NaturalDataFrame, LazySubsets>(df, dots);
  }
}
