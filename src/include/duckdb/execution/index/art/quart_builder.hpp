//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/quart_builder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/common/stack.hpp"

namespace duckdb {

//! QuARTBuilder bulk-loads a sorted sequence of keys into an ART index.
//! It extends the ARTBuilder approach with an optimization for dense leaf groups:
//! when 256 consecutive sorted keys all share the same prefix and differ only in
//! their last byte (taking all values 0–255), the builder creates a Node256 and
//! populates all 256 inlined leaves in a single pass, avoiding 256 individual
//! stack operations and InsertChild calls.
class QuARTBuilder {
public:
	QuARTBuilder() = delete;
	QuARTBuilder(ArenaAllocator &arena, ART &art, const unsafe_vector<ARTKey> &keys,
	             const unsafe_vector<ARTKey> &row_ids)
	    : arena(arena), art(art), keys(keys), row_ids(row_ids) {
	}

public:
	//! Initialize the QuART builder by passing a reference to the root node.
	void Init(Node &node, const idx_t end) {
		s.emplace(node, 0, end, 0);
	}
	//! Build the ART starting at the first entry in the stack.
	ARTConflictType Build();

private:
	struct NodeEntry {
		NodeEntry() = delete;
		NodeEntry(Node &node, const idx_t start, const idx_t end, const idx_t depth)
		    : node(node), start(start), end(end), depth(depth) {};

		Node &node;
		idx_t start;
		idx_t end;
		idx_t depth;
	};

	//! The arena holds any temporary memory allocated during the Build phase.
	ArenaAllocator &arena;
	//! The ART holding the node memory.
	ART &art;
	//! The keys to build the ART from.
	const unsafe_vector<ARTKey> &keys;
	//! The row IDs matching the keys.
	const unsafe_vector<ARTKey> &row_ids;
	//! The stack. While building, NodeEntry elements are pushed onto the stack.
	stack<NodeEntry> s;

	//! Returns true if keys[group_start .. group_start+255] form a dense leaf group:
	//! all 256 keys have length key_len, share the same prefix bytes data[0..key_len-2],
	//! and their last byte data[key_len-1] covers every value 0–255.
	//! Runs in O(key_len) by checking only the first and last keys of the group.
	bool IsCompleteGroup(idx_t group_start, idx_t end, idx_t key_len) const;

	//! Starting from the already-initialized node wrapped by `ref` at depth from_depth,
	//! inserts Node4 inner nodes for each depth in [from_depth+1 .. key_len-2], then
	//! allocates a Node256 at depth key_len-1 (byte pf_bytes[key_len-2] in the bl node)
	//! and bulk-fills it with 256 inlined leaves from keys[group_start .. group_start+255].
	//!
	//! Returns a raw pointer to the bl node's (depth key_len-2) slot in its parent.
	//! The caller wraps this in reference<Node> to obtain bl_ref for subsequent groups.
	Node *InsertPathToNode256(reference<Node> ref, idx_t from_depth, idx_t key_len,
	                          idx_t group_start, const uint8_t *pf_bytes);

	//! Navigates from the node referenced by `root` downward `depth` levels by following
	//! path_bytes[0], path_bytes[1], …, path_bytes[depth-1]. Returns a reference<Node>
	//! wrapping the child node at the requested depth. Caller must ensure the path exists.
	reference<Node> NavigateToDepth(reference<Node> root, const uint8_t *path_bytes, idx_t depth);
};

} // namespace duckdb
