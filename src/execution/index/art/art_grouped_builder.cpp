#include "duckdb/execution/index/art/art_grouped_builder.hpp"

#include "duckdb/execution/index/art/art_operator.hpp"
#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/node256.hpp"
#include "duckdb/execution/index/art/node48.hpp"
#include "duckdb/execution/index/art/prefix.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//

ARTConflictType ARTGroupedBuilder::Append(ArenaAllocator &arena, const unsafe_vector<ARTKey> &keys,
                                          const unsafe_vector<ARTKey> &row_ids, const idx_t count) {
	if (count == 0) {
		return ARTConflictType::NO_CONFLICT;
	}

	if (!art.tree.HasMetadata()) {
		// Empty tree: the chunk is the first group, and its path starts at the root.
		D_ASSERT(!has_prev_key && cursor.empty());
		reference<NodePtr> root_ref(art.tree);
		auto result = NewSubtree(arena, root_ref, keys, row_ids, 0, count, 0);
		if (result != ARTConflictType::NO_CONFLICT) {
			return result;
		}
		SetPreviousKey(keys[count - 1]);
		return ARTConflictType::NO_CONFLICT;
	}

	idx_t i = 0;
	while (i < count) {
		auto &key = keys[i];
		D_ASSERT(key.len != 0);

		// Classify the key against the previous key.
		const auto depth = GetMismatchDepth(key);
		if (depth == key.len) {
			// The key repeats the previous key: take the per-key insert, which restructures the path.
			const auto run_end = GetRunEnd(keys, i, count, 0);
			for (idx_t j = i; j < run_end; j++) {
				auto result = ARTOperator::Insert(arena, art, art.tree, key, 0, row_ids[j], GateStatus::GATE_NOT_SET,
				                                  DeleteIndexInfo(), IndexAppendMode::DEFAULT);
				if (result != ARTConflictType::NO_CONFLICT) {
					return result;
				}
			}
			cursor.clear();
			i = run_end;
			continue;
		}

		// FP_INSERT, if the cursor already holds the node at depth, else BRIDGE.
		auto &parent = GetOrCreatePath(key, depth);

		// The group is the run of keys sharing the bytes up to and including depth.
		// It hangs under parent at the divergence byte, which no key so far has.
		const auto end = GetGroupEnd(keys, i, count, depth + 1);
		NodePtr child;
		reference<NodePtr> child_ref(child);
		auto result = NewSubtree(arena, child_ref, keys, row_ids, i, end, depth + 1);
		if (result != ARTConflictType::NO_CONFLICT) {
			return result;
		}
		NodePtr::InsertChild(art, parent, key[depth], child);

		SetPreviousKey(keys[end - 1]);
		i = end;
	}
	return ARTConflictType::NO_CONFLICT;
}

//===--------------------------------------------------------------------===//
// Groups and keys
//===--------------------------------------------------------------------===//

idx_t ARTGroupedBuilder::GetGroupEnd(const unsafe_vector<ARTKey> &keys, const idx_t start, const idx_t end,
                                     const idx_t shared_count) const {
	auto &first = keys[start];
	D_ASSERT(shared_count <= first.len);
	auto shares_prefix = [&](const ARTKey &key) {
		return key.len >= shared_count && memcmp(key.data, first.data, shared_count) == 0;
	};

	// Keys are sorted, so all keys between two keys sharing the prefix share it too.
	// This detects a full group of 256 keys in one comparison.
	auto group_end = start + 1;
	if (start + 256 <= end && shares_prefix(keys[start + 255])) {
		group_end = start + 256;
	}
	while (group_end < end && shares_prefix(keys[group_end])) {
		group_end++;
	}
	return group_end;
}

idx_t ARTGroupedBuilder::GetRunEnd(const unsafe_vector<ARTKey> &keys, const idx_t start, const idx_t end,
                                   const idx_t depth) const {
	auto &first = keys[start];
	auto run_end = start + 1;
	while (run_end < end && keys[run_end].len == first.len &&
	       memcmp(keys[run_end].data + depth, first.data + depth, first.len - depth) == 0) {
		run_end++;
	}
	return run_end;
}

idx_t ARTGroupedBuilder::GetByteRunEnd(const unsafe_vector<ARTKey> &keys, const idx_t start, const idx_t end,
                                       const idx_t depth) const {
	const auto byte = keys[start][depth];
	auto run_end = start + 1;
	while (run_end < end && keys[run_end][depth] == byte) {
		run_end++;
	}
	return run_end;
}

idx_t ARTGroupedBuilder::GetMismatchDepth(const ARTKey &key) const {
	D_ASSERT(has_prev_key);
	const auto len = MinValue<idx_t>(prev_key.size(), key.len);
	for (idx_t i = 0; i < len; i++) {
		if (prev_key[i] != key.data[i]) {
			return i;
		}
	}
	// Keys are prefix-free, so matching up to the shorter length means equal keys.
	D_ASSERT(prev_key.size() == key.len);
	return len;
}

void ARTGroupedBuilder::SetPreviousKey(const ARTKey &key) {
	prev_key.resize(key.len);
	memcpy(prev_key.data(), key.data, key.len);
	has_prev_key = true;
}

//===--------------------------------------------------------------------===//
// Cursor
//===--------------------------------------------------------------------===//

NodePtr &ARTGroupedBuilder::GetOrCreatePath(const ARTKey &key, const idx_t depth) {
	D_ASSERT(art.tree.HasMetadata());

	// Drop the cursor entries below depth, they are no longer on the rightmost path.
	while (!cursor.empty() && cursor.back().depth > depth) {
		cursor.pop_back();
	}
	if (!cursor.empty() && cursor.back().depth == depth) {
		return cursor.back().slot.get();
	}

	// Walk down from the deepest cursor entry (or the root) along the key.
	// The tree contains the previous key, which matches key up to depth, so the path exists until there.
	reference<NodePtr> slot(art.tree);
	idx_t current = 0;
	if (!cursor.empty()) {
		auto &entry = cursor.back();
		slot = *entry.slot.get().GetChildMutable(art, key[entry.depth], true);
		current = entry.depth + 1;
	}

	while (true) {
		auto &node = slot.get();
		D_ASSERT(node.HasMetadata() && node.GetGateStatus() == GateStatus::GATE_NOT_SET);

		if (node.GetType() == NType::PREFIX) {
			Prefix prefix(art, node, true);
			const idx_t prefix_count = prefix.data[art.PrefixCount()];
			if (current + prefix_count <= depth) {
				current += prefix_count;
				slot = *prefix.child_slot;
				continue;
			}

			// The keys diverge inside this prefix: split it and replace the split byte with a Node4.
			const auto pos = UnsafeNumericCast<uint8_t>(depth - current);
			const auto byte = prefix.data[pos];
			NodePtr remainder;
			const auto status = Prefix::Split(art, slot, remainder, pos);

			Node4::New(art, slot);
			slot.get().SetGateStatus(status);
			Node4::InsertChild(art, slot, byte, remainder);

			cursor.emplace_back(slot.get(), depth);
			return slot.get();
		}

		D_ASSERT(node.IsInternalNode());
		cursor.emplace_back(node, current);
		if (current == depth) {
			return node;
		}
		auto child = node.GetChildMutable(art, key[current], true);
		D_ASSERT(child);
		slot = *child;
		current++;
	}
}

//===--------------------------------------------------------------------===//
// Nodes and leaves
//===--------------------------------------------------------------------===//

ARTConflictType ARTGroupedBuilder::NewSubtree(ArenaAllocator &arena, reference<NodePtr> &node_ref,
                                              const unsafe_vector<ARTKey> &keys, const unsafe_vector<ARTKey> &row_ids,
                                              const idx_t start, const idx_t end, const idx_t depth) {
	auto &first = keys[start];
	if (GetRunEnd(keys, start, end, depth) == end) {
		return NewLeaf(arena, node_ref, first, depth, row_ids, start, end);
	}

	// The keys are sorted, so they all share the common prefix of the first and the last key.
	// Keys are prefix-free, so distinct keys diverge before the shorter key ends.
	auto &last = keys[end - 1];
	auto split = depth;
	while (first[split] == last[split]) {
		split++;
		D_ASSERT(split < first.len && split < last.len);
	}
	if (split > depth) {
		Prefix::New(art, node_ref, first, depth, split - depth);
	}
	return NewGroupNode(arena, node_ref, keys, row_ids, start, end, split);
}

ARTConflictType ARTGroupedBuilder::NewGroupNode(ArenaAllocator &arena, NodePtr &node, const unsafe_vector<ARTKey> &keys,
                                                const unsafe_vector<ARTKey> &row_ids, const idx_t start,
                                                const idx_t end, const idx_t depth) {
	// Build the children first, as building a child allocates nodes.
	// The children live in the arena, as deep subtrees recurse.
	auto &children = *new (arena.AllocateAligned(sizeof(NodeChildren))) NodeChildren();
	for (idx_t i = start; i < end;) {
		const auto run_end = GetByteRunEnd(keys, i, end, depth);

		reference<NodePtr> child_ref(children.children[children.count]);
		auto result = NewSubtree(arena, child_ref, keys, row_ids, i, run_end, depth + 1);
		if (result != ARTConflictType::NO_CONFLICT) {
			return result;
		}
		children.bytes[children.count] = keys[i][depth];
		children.count++;
		i = run_end;
	}
	D_ASSERT(children.count > 1 && children.count <= 256);

	// Write the children into a node of the final size. The bytes are sorted and distinct.
	switch (NodePtr::GetInternalNodeType(children.count)) {
	case NType::NODE_4: {
		auto handle = Node4::New(art, node);
		auto &n = handle.Get<Node4>();
		for (idx_t i = 0; i < children.count; i++) {
			n.key[i] = children.bytes[i];
			n.children[i] = children.children[i];
		}
		n.count = UnsafeNumericCast<uint8_t>(children.count);
		break;
	}
	case NType::NODE_16: {
		auto handle = Node16::New(art, node);
		auto &n = handle.Get<Node16>();
		for (idx_t i = 0; i < children.count; i++) {
			n.key[i] = children.bytes[i];
			n.children[i] = children.children[i];
		}
		n.count = UnsafeNumericCast<uint8_t>(children.count);
		break;
	}
	case NType::NODE_48: {
		auto handle = Node48::New(art, node);
		auto &n = handle.Get<Node48>();
		for (idx_t i = 0; i < children.count; i++) {
			n.child_index[children.bytes[i]] = UnsafeNumericCast<uint8_t>(i);
			n.children[i] = children.children[i];
		}
		n.count = UnsafeNumericCast<uint8_t>(children.count);
		break;
	}
	case NType::NODE_256: {
		auto handle = Node256::New(art, node);
		auto &n = handle.Get<Node256>();
		for (idx_t i = 0; i < children.count; i++) {
			n.children[children.bytes[i]] = children.children[i];
		}
		n.count = UnsafeNumericCast<uint16_t>(children.count);
		break;
	}
	default:
		throw InternalException("invalid node type in ARTGroupedBuilder::NewGroupNode");
	}
	return ARTConflictType::NO_CONFLICT;
}

ARTConflictType ARTGroupedBuilder::NewLeaf(ArenaAllocator &arena, reference<NodePtr> &node_ref, const ARTKey &key,
                                           const idx_t depth, const unsafe_vector<ARTKey> &row_ids, const idx_t start,
                                           const idx_t end) {
	D_ASSERT(depth <= key.len);
	if (depth < key.len) {
		Prefix::New(art, node_ref, key, depth, key.len - depth);
	}

	if (end - start == 1) {
		Leaf::New(node_ref, row_ids[start].GetRowId());
		return ARTConflictType::NO_CONFLICT;
	}
	if (art.IsUnique()) {
		return ARTConflictType::CONSTRAINT;
	}

	// Repeated key: nest the row IDs, which are not sorted, behind a gate.
	for (idx_t i = start; i < end; i++) {
		ARTOperator::Insert(arena, art, node_ref, row_ids[i], 0, row_ids[i], GateStatus::GATE_SET, DeleteIndexInfo(),
		                    IndexAppendMode::DEFAULT);
	}
	node_ref.get().SetGateStatus(GateStatus::GATE_SET);
	return ARTConflictType::NO_CONFLICT;
}

} // namespace duckdb
