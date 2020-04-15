#include "benchmarks/SCAN/Naive/scan_helpers.h"

#include <functional>
#include <limits>

#include "pbbslib/stlalgs.h"

namespace naive_scan {

namespace internal {

CoreBFSEdgeMapFunctions::CoreBFSEdgeMapFunctions(
    const StructuralSimilarities& similarities,
    const Clustering& current_clustering,
    const float epsilon)
  : similarities_{similarities}
  , current_clustering_{current_clustering}
  , epsilon_{epsilon} {}

bool
CoreBFSEdgeMapFunctions::update(const uintE u, const uintE v, NoWeight) const {
  constexpr float
    kDefaultSimilarity{std::numeric_limits<float>::signaling_NaN()};
  return current_clustering_[v].empty()
    && similarities_.find({u, v}, kDefaultSimilarity) >= epsilon_;
}

bool CoreBFSEdgeMapFunctions::updateAtomic(
    const uintE u, const uintE v, NoWeight) const {
  return update(u, v, NoWeight{});
}

bool CoreBFSEdgeMapFunctions::cond(const uintE v) const {
  return current_clustering_[v].empty();
}

void RemoveDuplicates(vertexSubset* vertex_subset) {
  if (vertex_subset->isDense) {
    return;
  }
  pbbs::sequence<uintE> vertices(
      vertex_subset->size(),
      [&](const size_t i) { return vertex_subset->vtx(i); });
  pbbs::sequence<uintE> deduped_vertices{
    pbbs::remove_duplicates_ordered(vertices, std::less<uintE>{})};
  vertexSubset deduped_vertex_subset{
    vertex_subset->n, std::move(deduped_vertices)};
  vertex_subset->del();
  *vertex_subset = deduped_vertex_subset;
}


}  // namespace internal

}  // namespace naive_scan
