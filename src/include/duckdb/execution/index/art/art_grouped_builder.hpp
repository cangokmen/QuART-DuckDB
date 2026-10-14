//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/art_grouped_builder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_key.hpp"

namespace duckdb {

//! ARTGroupedBuilder builds an ART from sorted keys one group at a time (QuART grouped bulk loading).
//! Each key diverges from the previous key at some byte. A group is the run of keys sharing all bytes up to
//! and including that byte, so it hangs as one new child under the node keyed by the divergence byte.
//! For dense fixed-size keys, a group holds up to 256 keys differing only in their last byte, and fills a
//! single node allocated at its final size. Otherwise, the group's subtree is built top-down with exact
//! prefixes and node sizes.
//! A cursor on the rightmost path remembers the nodes the previous group was attached under: a group
//! diverging at a node held by the cursor hangs under it directly (FP_INSERT), otherwise the cursor
//! moves to the new parent, splitting a prefix if the keys diverge inside one (BRIDGE).
//! Keys must be sorted across all Append calls. Gaps are fine. Repeated keys (non-unique indexes)
//! become nested leaves, and a key repeating the previous Append's last key takes the per-key insert.
class ARTGroupedBuilder {
public:
	ARTGroupedBuilder() = delete;
	explicit ARTGroupedBuilder(ART &art) : art(art) {
	}

public:
	//! Append a sorted chunk of keys and their row IDs to the ART.
	ARTConflictType Append(ArenaAllocator &arena, const unsafe_vector<ARTKey> &keys,
	                       const unsafe_vector<ARTKey> &row_ids, const idx_t count);

private:
	//! An internal node on the rightmost path. Its children are keyed by the key byte at depth.
	struct CursorEntry {
		CursorEntry(NodePtr &slot, const idx_t depth) : slot(slot), depth(depth) {
		}
		reference<NodePtr> slot;
		idx_t depth;
	};

	//! The children of one node, before they are written into the node.
	struct NodeChildren {
		idx_t count = 0;
		uint8_t bytes[256];
		NodePtr children[256];
	};

private:
	//! Returns the index one past the last key sharing the first shared_count bytes with keys[start].
	idx_t GetGroupEnd(const unsafe_vector<ARTKey> &keys, const idx_t start, const idx_t end,
	                  const idx_t shared_count) const;
	//! Returns the index one past the last key equal to keys[start]. All keys share the bytes before depth.
	idx_t GetRunEnd(const unsafe_vector<ARTKey> &keys, const idx_t start, const idx_t end, const idx_t depth) const;
	//! Returns the index one past the last key sharing the byte at depth with keys[start].
	idx_t GetByteRunEnd(const unsafe_vector<ARTKey> &keys, const idx_t start, const idx_t end, const idx_t depth) const;
	//! Returns the first byte position at which key differs from the previous key.
	idx_t GetMismatchDepth(const ARTKey &key) const;
	void SetPreviousKey(const ARTKey &key);

	//! Moves the cursor to the internal node at depth on the path of key, creating it if necessary,
	//! and returns the slot holding that node (getOrCreatePath).
	NodePtr &GetOrCreatePath(const ARTKey &key, const idx_t depth);
	//! Builds the subtree for keys[start, end), which share the bytes before depth, into node_ref.
	ARTConflictType NewSubtree(ArenaAllocator &arena, reference<NodePtr> &node_ref, const unsafe_vector<ARTKey> &keys,
	                           const unsafe_vector<ARTKey> &row_ids, const idx_t start, const idx_t end,
	                           const idx_t depth);
	//! Builds the node for keys[start, end) keyed by their byte at depth into node (newNode).
	//! The keys share the bytes before depth and differ at depth.
	ARTConflictType NewGroupNode(ArenaAllocator &arena, NodePtr &node, const unsafe_vector<ARTKey> &keys,
	                             const unsafe_vector<ARTKey> &row_ids, const idx_t start, const idx_t end,
	                             const idx_t depth);
	//! Builds the leaf for the equal keys keys[start, end), prefixed by the key bytes from depth.
	ARTConflictType NewLeaf(ArenaAllocator &arena, reference<NodePtr> &node_ref, const ARTKey &key, const idx_t depth,
	                        const unsafe_vector<ARTKey> &row_ids, const idx_t start, const idx_t end);

private:
	//! The ART holding the node memory.
	ART &art;
	//! The rightmost path, from the shallowest internal node to the previous group's parent.
	vector<CursorEntry> cursor;
	//! The previous (greatest) key. Copied, because key memory only lives for one Append.
	unsafe_vector<data_t> prev_key;
	bool has_prev_key = false;
};

} // namespace duckdb
