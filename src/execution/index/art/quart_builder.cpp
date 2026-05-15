#include "duckdb/execution/index/art/quart_builder.hpp"

#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"
#include "duckdb/execution/index/art/node256.hpp"

namespace duckdb {

bool QuARTBuilder::IsCompleteGroup(const idx_t group_start, const idx_t end, const idx_t key_len) const {
	// Need 256 consecutive keys within [group_start, end].
	if (group_start + Node256::CAPACITY - 1 > end) {
		return false;
	}

	const auto &first = keys[group_start];
	const auto &last = keys[group_start + Node256::CAPACITY - 1];

	// Both boundary keys must have the expected length.
	if (first.len != key_len || last.len != key_len) {
		return false;
	}

	// The last byte of the first key must be 0 and of the last key must be 255.
	// Combined with sorted order and no duplicates, this guarantees all 256 byte
	// values are present in the group.
	if (first.data[key_len - 1] != 0 || last.data[key_len - 1] != 255) {
		return false;
	}

	// All prefix bytes (depths 0 .. key_len-2) must be identical between first and last.
	// Sorted order then ensures every key in between shares the same prefix.
	for (idx_t d = 0; d < key_len - 1; d++) {
		if (first.data[d] != last.data[d]) {
			return false;
		}
	}

	return true;
}

Node *QuARTBuilder::InsertPathToNode256(reference<Node> ref, const idx_t from_depth, const idx_t key_len,
                                        const idx_t group_start, const uint8_t *pf_bytes) {
	D_ASSERT(key_len >= 2);
	D_ASSERT(from_depth < key_len - 1);

	// Create intermediate inner Node4 nodes for depths from_depth .. key_len-3.
	// The condition (d + 1 < key_len - 1) is equivalent to d < key_len - 2 but avoids
	// unsigned underflow for small key_len values.
	// After the loop, ref wraps the bl node at depth key_len-2.
	for (idx_t d = from_depth; d + 1 < key_len - 1; d++) {
		// Create an empty child slot; the current node may grow.
		Node::InsertChild(art, ref, pf_bytes[d]);
		// Always re-fetch the child slot after InsertChild (node may have been reallocated).
		auto child_slot = ref.get().GetChildMutable(art, pf_bytes[d], true);
		// Initialize the new child as a Node4.
		Node::New(art, *child_slot, NType::NODE_4);
		// Advance ref to the new child (depth d+1).
		ref = reference<Node>(*child_slot);
	}

	// ref is now the bl node (depth key_len-2).
	// Capture a pointer to its slot in the parent — this is what the caller stores as bl_ref.
	// The slot address is stable until the parent expands, which only happens during bridge
	// events that always re-establish bl_ref from scratch anyway.
	Node *bl_slot = &ref.get();

	// Insert the Node256 as a child of the bl node at byte pf_bytes[key_len-2].
	Node::InsertChild(art, ref, pf_bytes[key_len - 2]);
	// Re-fetch after the potential grow of the bl node.
	auto node256_slot = ref.get().GetChildMutable(art, pf_bytes[key_len - 2], true);
	// Initialize as Node256.
	Node::New(art, *node256_slot, NType::NODE_256);

	// Bulk-fill the Node256 with 256 inlined leaves.
	// bytes[i] = last byte of key i in the group; row_id_values[i] = its row ID.
	uint8_t bytes[Node256::CAPACITY];
	row_t row_id_values[Node256::CAPACITY];
	for (idx_t i = 0; i < Node256::CAPACITY; i++) {
		bytes[i] = keys[group_start + i].data[key_len - 1];
		row_id_values[i] = row_ids[group_start + i].GetRowId();
	}
	Node256::BulkInsertInlinedLeaves(art, *node256_slot, bytes, row_id_values);

	return bl_slot;
}

reference<Node> QuARTBuilder::NavigateToDepth(reference<Node> root, const uint8_t *path_bytes,
                                              const idx_t depth) {
	for (idx_t d = 0; d < depth; d++) {
		auto slot = root.get().GetChildMutable(art, path_bytes[d], false);
		root = reference<Node>(*slot);
	}
	return root;
}

ARTConflictType QuARTBuilder::Build() {
	// Init() pushes exactly one entry spanning the entire key range at depth 0.
	D_ASSERT(s.size() == 1);
	auto entry = s.top();
	s.pop();
	D_ASSERT(s.empty());
	D_ASSERT(entry.start <= entry.end);
	D_ASSERT(entry.depth == 0);

	const idx_t row_count = entry.end - entry.start + 1;
	const idx_t key_len = keys[entry.start].len;
	D_ASSERT(key_len != 0);

	// Fast path requires key_len >= 2 (InsertPathToNode256 needs at least one prefix
	// byte plus one leaf byte). Fewer than 256 keys also precludes a complete group.
	const idx_t num_complete_groups = (key_len >= 2) ? (row_count / Node256::CAPACITY) : 0;

	// root_ref wraps entry.node. Node::New / InsertChild update entry.node in place
	// (they take Node & and assign through it), so root_ref always reflects the current
	// root handle even if the root node grows during bulk-load.
	reference<Node> root_ref(entry.node);

	// ---- Fast path: bulk-load complete groups of 256 ----
	// Mirrors QuART's bulkLoad(). State:
	//   bl_slot : raw pointer to the "bl node" (at depth key_len-2) slot in its parent.
	//             Equivalent to QuART's bl_ptr_ref. Stable between groups because the
	//             parent only expands during true bridge events, which always re-derive
	//             bl_slot from scratch via InsertPathToNode256.
	//   bl_pf[] : prefix bytes data[0..key_len-2] of the last processed group, used
	//             for bridge detection and navigation in subsequent groups.
	Node *bl_slot = nullptr;
	vector<uint8_t> bl_pf(key_len > 0 ? key_len - 1 : 0, 0);

	idx_t g = 0;
	for (; g < num_complete_groups; g++) {
		const idx_t group_start = entry.start + g * Node256::CAPACITY;
		const uint8_t *pf_bytes = keys[group_start].data;

		// Skip groups that do not cover all 256 leaf-byte values (e.g. gap in the
		// key space). Those keys are handled by the slow path below.
		if (!IsCompleteGroup(group_start, entry.end, key_len)) {
			break;
		}

		if (g == 0) {
			// First group: initialise root as Node4, then build the full path down to
			// the first Node256. Mirrors QuART's "root == nullptr" special case, but
			// also fixes the off-by-one bug in QuART that skipped the first key (i=1
			// instead of i=0): InsertPathToNode256 inserts all 256 keys including i=0.
			Node::New(art, root_ref, NType::NODE_4);
			bl_slot = InsertPathToNode256(root_ref, 0, key_len, group_start, pf_bytes);
		} else {
			// Find the bridge depth: the first prefix-byte position (in [0, key_len-2))
			// where this group's byte differs from the previous group's byte.
			// For sorted distinct complete groups a bridge is always found here.
			idx_t bridge = 0;
			while (bridge < key_len - 1 && pf_bytes[bridge] == bl_pf[bridge]) {
				bridge++;
			}
			// If bridge == key_len-1 all prefix bytes are equal, implying duplicate keys.
			D_ASSERT(bridge < key_len - 1);

			if (bridge == key_len - 2) {
				// No bridge: the bl node (d2) is unchanged; only a new Node256 child is
				// required. Use bl_slot directly to avoid navigating from root, mirroring
				// QuART's "else" branch which accesses bl_ptr / bl_ptr_ref directly.
				D_ASSERT(bl_slot != nullptr);
				bl_slot =
				    InsertPathToNode256(reference<Node>(*bl_slot), key_len - 2, key_len, group_start, pf_bytes);
			} else {
				// True bridge: navigate from root to the existing node at depth `bridge`
				// by following the PREVIOUS group's prefix bytes (bl_pf[0..bridge-1]).
				// That node is the deepest shared ancestor; InsertPathToNode256 creates
				// all new descendants (new bl node + Node256) from that point.
				bl_slot = InsertPathToNode256(NavigateToDepth(root_ref, bl_pf.data(), bridge),
				                             bridge, key_len, group_start, pf_bytes);
			}
		}

		// Record prefix bytes of this group for bridge detection in the next iteration.
		for (idx_t d = 0; d + 1 < key_len; d++) {
			bl_pf[d] = pf_bytes[d];
		}
	}

	// ---- Slow path: insert remaining keys individually ----
	// Covers: (a) keys after the last complete group, (b) all keys when there are no
	// complete groups (key_len < 2, fewer than 256 keys, or key space has gaps).
	// ARTOperator::Insert handles arbitrary key lengths, prefixes, and tree states,
	// and enforces uniqueness constraints when art.IsUnique() is true.
	const idx_t remaining_start = entry.start + g * Node256::CAPACITY;
	for (idx_t i = remaining_start; i <= entry.end; i++) {
		auto conflict = ARTOperator::Insert(arena, art, entry.node, keys[i], 0, row_ids[i],
		                                    GateStatus::GATE_NOT_SET, DeleteIndexInfo(),
		                                    IndexAppendMode::DEFAULT);
		if (conflict != ARTConflictType::NO_CONFLICT) {
			return conflict;
		}
	}

	return ARTConflictType::NO_CONFLICT;
}

} // namespace duckdb
