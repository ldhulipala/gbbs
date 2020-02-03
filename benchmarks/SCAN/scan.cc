#define NOTMAIN

#include "benchmarks/SCAN/scan.h"

#include <cmath>
#include <algorithm>
#include <atomic>
#include <unordered_set>
#include <utility>
#include <vector>

#include "benchmarks/Connectivity/WorkEfficientSDB14/Connectivity.h"
#include "ligra/bridge.h"
#include "ligra/macros.h"
#include "pbbslib/binary_search.h"
#include "pbbslib/parallel.h"
#include "pbbslib/sample_sort.h"
#include "utils/assert.h"

namespace scan {

namespace {

using Weight = pbbslib::empty;
using Vertex = symmetric_vertex<Weight>;

using DirectedEdge = std::pair<uintE, uintE>;
using VertexSet =
  sparse_table<uintE, pbbslib::empty, decltype(&pbbslib::hash64_2)>;

// Holds a vertex and its degree.
struct VertexDegree {
  uintE vertex_id;
  uintE degree;
};

// Creates a `VertexSet` for holding up to `capacity` elements.
VertexSet MakeVertexSet(const size_t capacity) {
  return
    make_sparse_table<uintE, pbbslib::empty, decltype(&pbbslib::hash64_2)>(
      // Adding 1 avoids having small tables completely full
      capacity + 1, {UINT_E_MAX, pbbslib::empty{}}, pbbslib::hash64_2);
}

// Finds the least `i` such that `predicate(sequence[i])` is false. If
// `predicate(sequence[i])` is true for all `i`, then this returns
// `sequence.size()`.
//
// `sequence` and `predicate` must be partitioned such that there is some `i`
// (which will be the return value) for which `predicate(sequence[i])` is true
// for all j < i and for which `predicate(sequence[i])` is false for all j >= i.
//
// Running time is O(log [return value]).
template <typename SeqElement>
size_t BinarySearch(
    const pbbs::sequence<SeqElement>& sequence,
    std::function<bool(const SeqElement&)> predicate) {
  // Start off the binary search with an exponential search so that running time
  // while be O(log [return value]) rather than O(log sequence.size()).
  // In practice this probably doesn't matter much....
  size_t hi{0};
  while (hi < sequence.size() && predicate(sequence[hi])) {
    hi = std::min(sequence.size(), 2 * hi + 1);
  }
  // Invariant: predicate(sequence[lo]) && !predicate(sequence[hi])
  const size_t lo{hi / 2};

  return pbbs::binary_search(sequence.slice(lo, hi), predicate);
}

}  // namespace

namespace internal {

bool operator==(const NeighborSimilarity& a, const NeighborSimilarity& b) {
  return
    std::tie(a.neighbor, a.similarity) == std::tie(b.neighbor, b.similarity);
}

std::ostream&
operator<<(std::ostream& os, const NeighborSimilarity& neighbor_similarity) {
  os << "{neighbor=" << neighbor_similarity.neighbor
     << ", similarity=" << neighbor_similarity.similarity << '}';
  return os;
}

bool operator==(const CoreThreshold& a, const CoreThreshold& b) {
  return
    std::tie(a.vertex_id, a.threshold) == std::tie(b.vertex_id, b.threshold);
}

std::ostream&
operator<<(std::ostream& os, const CoreThreshold& core_threshold) {
  os << "{vertex=" << core_threshold.vertex_id
     << ", threshold=" << core_threshold.threshold << '}';
  return os;
}

// Compute structural similarities (as defined by SCAN) between each pair of
// adjacent vertices.
//
// The structural similarity between two vertices u and v is
//   (size of intersection of closed neighborhoods of u and v) /
//   (geometric mean of size of closed neighborhoods of u and of v)
// where the closed neighborhood of a vertex x consists of all neighbors of x
// along with x itself.
StructuralSimilarities ComputeStructuralSimilarities(
    symmetric_graph<symmetric_vertex, pbbslib::empty>* graph) {
  StructuralSimilarities similarities{
    graph->m,
    std::make_pair(UndirectedEdge{UINT_E_MAX, UINT_E_MAX}, 0.0),
    std::hash<UndirectedEdge>{}};

  std::vector<VertexSet> adjacency_list{graph->n};
  parallel_for(0, graph->n, [&graph, &adjacency_list](const size_t vertex_id) {
    Vertex vertex{graph->get_vertex(vertex_id)};
    auto* neighbors{&adjacency_list[vertex_id]};
    *neighbors = MakeVertexSet(vertex.getOutDegree());

    const auto update_adjacency_list{[&neighbors](
        const uintE source_vertex,
        const uintE neighbor_vertex,
        const Weight weight) {
      neighbors->insert(std::make_pair(neighbor_vertex, pbbslib::empty{}));
    }};
    vertex.mapOutNgh(vertex_id, update_adjacency_list);
  });

  // TODO(tom.tseng): This all might be overkill --- look at
  // ligra/vertex.h intersection::intersect
  graph->map_edges([&graph, &adjacency_list, &similarities](
        const uintE u_id,
        const uintE v_id,
        const Weight) {
      // Only perform this computation once for each undirected edge
      if (u_id < v_id) {
        Vertex u{graph->get_vertex(u_id)};
        Vertex v{graph->get_vertex(v_id)};
        const auto& u_neighbors{adjacency_list[u_id]};
        const auto& v_neighbors{adjacency_list[v_id]};

        const bool u_degree_is_smaller{u.getOutDegree() < v.getOutDegree()};
        const uintE smaller_degree_vertex_id{u_degree_is_smaller ? u_id : v_id};
        Vertex* smaller_degree_vertex{u_degree_is_smaller ? &u : &v};
        const auto& larger_degree_vertex_neighbors{
          u_degree_is_smaller ? v_neighbors : u_neighbors
        };

        std::atomic<uintE> num_shared_neighbors{0};
        const auto count_shared_neighbors{
          [&](const uintE, const uintE neighbor, const Weight) {
            if (larger_degree_vertex_neighbors.contains(neighbor)) {
                num_shared_neighbors++;
            }
          }};
        smaller_degree_vertex->mapOutNgh(
            smaller_degree_vertex_id,
            count_shared_neighbors);

        // The neighborhoods we've computed are open neighborhoods -- since
        // structural similarity uses closed neighborhoods, we need to adjust
        // the number and denominator a little.
        similarities.insert({UndirectedEdge{u_id, v_id},
            (num_shared_neighbors + 2) /
                (sqrt(graph->get_vertex(u_id).getOutDegree() + 1) *
                 sqrt(graph->get_vertex(v_id).getOutDegree() + 1))});
      }
  });

  return similarities;
}

// Computes an adjacency list for the graph in which each neighbor list is
// sorted by descending structural similarity with the source vertex.
//
// The output adjacency list `NO` is such that `NO[v][i]` is a pair `{u, sigma}`
// where `sigma` is the structural similarity between `v` and `u` and where `u`
// is the neighbor of `v` with the (zero-indexed) `i`-th  highest structural
// similarity with `v`.
//
// Unlike the presentation in "Efficient Structural Graph Clustering:  An
// Index-Based Approach", the neighbor list for a vertex `v` will not contain
// `v` itself, unless `(v, v)` is explicitly given as an edge in `graph`.
NeighborOrder ComputeNeighborOrder(
    symmetric_graph<symmetric_vertex, pbbslib::empty>* graph,
    const StructuralSimilarities& similarities) {
  NeighborOrder neighbor_order{
    graph->n,
    [&graph](size_t i) {
      return pbbs::sequence<NeighborSimilarity>{
        graph->get_vertex(i).getOutDegree()};
    }
  };

  par_for(0, graph->n, [&](const uintE v) {
    Vertex vertex{graph->get_vertex(v)};
    auto& v_order{neighbor_order[v]};

    par_for(0, vertex.getOutDegree(), [&](const size_t i) {
      const uintE neighbor{vertex.getOutNeighbor(i)};
      const float kNotFound{-1.0};
      const float similarity{
        similarities.find(UndirectedEdge{v, neighbor}, kNotFound)};
      v_order[i] = NeighborSimilarity{
          .neighbor = neighbor, .similarity = similarity};
    });

    // Sort by descending structural similarity
    const auto compare_similarities_descending{[](
        const NeighborSimilarity& a, const NeighborSimilarity& b) {
      return a.similarity > b.similarity;
    }};
    pbbs::sample_sort_inplace(v_order.slice(), compare_similarities_descending);
  });

  return neighbor_order;
}

// Returns a sequence CO where CO[i] for i >= 2 is a list of vertices that
// can be a core when the SCAN parameter mu is set to i. The vertices in
// CO[i], are sorted by their core threshold values, the maximum value of SCAN
// parameter epsilon such that the vertex is a core when mu == i.
//
// CO[0] and CO[1] are left empty --- when mu is less than 2, all vertices are
// always cores and have a core threshold of 1.
CoreOrder ComputeCoreOrder(const NeighborOrder& neighbor_order) {
  if (neighbor_order.empty()) {
    return CoreOrder{};
  }

  pbbs::sequence<VertexDegree> vertex_degrees{
    pbbs::map_with_index<VertexDegree>(
        neighbor_order,
        [](size_t i, const pbbs::sequence<NeighborSimilarity>& similarity) {
          return VertexDegree{
            .vertex_id = static_cast<uintE>(i),
            .degree = static_cast<uintE>(similarity.size())};
        })
  };
  // Sort `vertex_degrees` by ascending degree.
  integer_sort_inplace(
      vertex_degrees.slice(),
      [](const VertexDegree& vertex_degree) { return vertex_degree.degree; });
  const size_t max_degree{vertex_degrees[vertex_degrees.size() - 1].degree};

  // `bucket_offsets[i]` is the first index `j` at which
  // `vertex_degrees[j].degree >= i`.
  pbbs::sequence<uintE> degree_offsets{
    pbbs::sequence<uintE>::no_init(max_degree + 1)};
  const size_t min_degree{vertex_degrees[0].degree};
  par_for(0, min_degree + 1, [&](const size_t j) {
    degree_offsets[j] = 0;
  });
  par_for(1, vertex_degrees.size(), [&](const size_t i) {
    const size_t degree{vertex_degrees[i].degree};
    const size_t prev_degree{vertex_degrees[i-1].degree};
    if (degree != prev_degree) {
      par_for(prev_degree + 1, degree + 1, [&](const size_t j) {
        degree_offsets[j] = i;
      });
    }
  });

  const auto get_core_order{[&](const size_t mu) {
    if (mu <= 1) {
      return pbbs::sequence<CoreThreshold>{};
    }
    // Only vertices with high enough degree can be cores.
    const pbbs::sequence<VertexDegree>& core_vertices{
      vertex_degrees.slice(degree_offsets[mu - 1], vertex_degrees.size())};

    pbbs::sequence<CoreThreshold> core_thresholds{
      pbbs::map<CoreThreshold>(
        core_vertices,
        [&](const VertexDegree& vertex_degree) {
          return CoreThreshold{
            .vertex_id = vertex_degree.vertex_id,
            .threshold =
                neighbor_order[vertex_degree.vertex_id][mu - 2].similarity};
        })};
    // Sort by descending threshold
    const auto compare_threshold_descending{[](
        const CoreThreshold& a, const CoreThreshold& b) {
      return a.threshold > b.threshold;
    }};
    pbbs::sample_sort_inplace(
        core_thresholds.slice(), compare_threshold_descending);
    return core_thresholds;
  }};

  return CoreOrder{max_degree + 2, get_core_order};
}

}  // namespace internal

ScanIndex::ScanIndex(symmetric_graph<symmetric_vertex, pbbslib::empty>* graph)
  : num_vertices{graph->n}
  , neighbor_order{
      internal::ComputeNeighborOrder(
          graph,
          internal::ComputeStructuralSimilarities(graph))}
  , core_order{internal::ComputeCoreOrder(neighbor_order)} {}

Clustering ScanIndex::Cluster(const float epsilon, const uint64_t mu) const {
  if (mu <= 1) {
    // Every vertex is a core. Return connected components on edges with
    // similarity > epsilon.
    ABORT("SCAN for `mu <= 1` not yet implemented");
  }
  if (mu >= core_order.size()) {
    // Nothing is a core. There are no clusters, and every vertex is an outlier.
    return Clustering {
        .clusters = pbbs::sequence<pbbs::sequence<uintE>>{},
        .hubs = pbbs::sequence<uintE>{},
        .outliers =
          pbbs::sequence<uintE>{
            num_vertices, [](const size_t i) { return i; }}
    };
  }

  const size_t cores_end{
    BinarySearch<internal::CoreThreshold>(
        core_order[mu],
        [epsilon](const internal::CoreThreshold& core_threshold) {
          return core_threshold.threshold >= epsilon;
        })};
  const pbbs::range cores{core_order[mu].slice(0, cores_end)};

  pbbs::sequence<size_t> epsilon_neighborhood_offsets{
      pbbs::map<size_t>(
          cores,
          [&](const internal::CoreThreshold& core_threshold) {
            // Get the number of neighbors of core vertex
            // `core_threshold.vertex_id` that have at least `epsilon`
            // structural similarity with the core.
            return BinarySearch<internal::NeighborSimilarity>(
                neighbor_order[core_threshold.vertex_id],
                [epsilon](const internal::NeighborSimilarity& ns) {
                  return ns.similarity >= epsilon;
                });
          })};
  const size_t num_core_incident_edges{
    pbbslib::scan_add_inplace(epsilon_neighborhood_offsets)};
  // List of edges with structural similarity at least `epsilon` that are
  // incident on a core.
  // Note: edges of the form (core vertex, non-core) will appear only once in
  // this list. On the other hand, edges of the form (core vertex 1, core
  // vertex 2) will appear twice in this list, once in each direction.
  const pbbs::sequence<DirectedEdge> core_incident_edges{
    pbbs::sequence<DirectedEdge>::no_init(num_core_incident_edges)};
  par_for(0, cores.size(), [&](const size_t i) {
    const size_t offset{epsilon_neighborhood_offsets[i]};
    const size_t size{
      (i == cores.size()
       ? num_core_incident_edges
       : epsilon_neighborhood_offsets[i + 1]) - offset};
    const uintE core_id{cores[i].vertex_id};
    const auto& core_neighbors{neighbor_order[core_id]};
    par_for(0, size, [&](const size_t j) {
      core_incident_edges[offset + j] =
        std::make_pair(core_id, core_neighbors[j].neighbor);
    });
  });

  VertexSet cores_set{MakeVertexSet(cores.size())};
  par_for(0, cores.size(), [&](const size_t i) {
    cores_set.insert(std::make_pair(cores[i].vertex_id, pbbslib::empty{}));
  });

  // `partitioned_core_edges` is `core_incident_edges` partitioned into edges
  // whose endpoints are both cores and edges that have a non-core endpoint.
  pbbs::sequence<DirectedEdge> partitioned_core_edges{};
  size_t num_core_to_core_edges{0};
  std::tie(partitioned_core_edges, num_core_to_core_edges) =
    pbbs::split_two_with_predicate(
        core_incident_edges,
        [&cores_set](const DirectedEdge& edge) {
          // Only need to check second endpoint. First endpoint is a core.
          return !cores_set.contains(edge.second);
        });
  auto core_to_core_edges{
    partitioned_core_edges.slice(0, num_core_to_core_edges)};

  // Create graph consisting of edges of sufficient similarity between cores.
  // The vertex ids of the cores are kept the same for simplicity, so actually
  // all the non-core vertices are also in the graph as singletons.
  symmetric_graph<symmetric_vertex, pbbslib::empty> core_graph{
    sym_graph_from_edges<pbbslib::empty>(
        core_to_core_edges,
        num_vertices,
        [](const DirectedEdge& edge) { return edge.first; },
        [](const DirectedEdge& edge) { return edge.second; },
        [](const DirectedEdge& edge) { return pbbslib::empty{}; })};

  const pbbs::sequence<parent> core_connected_components{
    workefficient_cc::CC(core_graph)};
  // TODO how do I use the result?
  // these are labels
  // remember, you have a bunch of non-core singletons, and possibly also core
  // singletons (though ultimately no cluster will be a singleton -- any core
  // will have its epsilon-neighborhood in its cluster)
  //
  // you don't care about the labels, you're just returning a list of clusters
  // -- don't necessarily need to do any renumbering here
  //
  // each non-core can be in several clusters, and it can have several edges that
  // attach to the same cluster (though not too many, or else it would become a
  // core itself)
  //
  // ok, maybe just map the edges to relabel endpoint with core's cluster ID,
  // then sort?
  //
  // should also determine --- are the cluster ID's contiguous? it might not
  // actually matter for us though. Just read through the code and try to
  // understand it... Adding a comment to the file would be helpful

  // Process remaining core, non-core edges to assign non-core verts to cores...
  // TODO how to do this properly?

  // TODO implement
  return {};
}

}  // namespace scan
