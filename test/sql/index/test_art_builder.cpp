#include "catch.hpp"
#include "test_helpers.hpp"

#include <string>

using namespace duckdb;

//! Helper that runs the same correctness suite against a connection.
//! The caller is responsible for enabling/disabling the QuARTBuilder PRAGMA before calling this.
static void RunBulkBuildTests(Connection &con) {
	duckdb::unique_ptr<QueryResult> result;

	// Dense sequential integers: optimal case for the QuART dense-leaf-group optimisation.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_int (i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_int SELECT range FROM range(10000)"));

	result = con.Query("SELECT COUNT(*) FROM t_int");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(10000)}));

	result = con.Query("SELECT COUNT(*) FROM t_int WHERE i >= 0");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(10000)}));

	result = con.Query("SELECT COUNT(*) FROM t_int WHERE i BETWEEN 100 AND 200");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(101)}));

	result = con.Query("SELECT COUNT(*) FROM t_int WHERE i < 256");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(256)}));

	result = con.Query("SELECT COUNT(*) FROM t_int WHERE i = 42");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(1)}));

	REQUIRE_NO_FAIL(con.Query("DROP TABLE t_int"));

	// Sparse integers: the dense-leaf-group optimisation should not trigger,
	// and the builder should fall back to regular ARTBuilder logic.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_sparse (i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_sparse VALUES (1), (1000), (1000000), (1000000000)"));

	result = con.Query("SELECT COUNT(*) FROM t_sparse");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(4)}));

	result = con.Query("SELECT COUNT(*) FROM t_sparse WHERE i BETWEEN 0 AND 10000");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(2)}));

	REQUIRE_NO_FAIL(con.Query("DROP TABLE t_sparse"));

	// BIGINT keys — the dense-leaf-group optimisation applies at depth 7 (last byte).
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_bigint (i BIGINT PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_bigint SELECT range FROM range(5000)"));

	result = con.Query("SELECT COUNT(*) FROM t_bigint");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(5000)}));

	result = con.Query("SELECT COUNT(*) FROM t_bigint WHERE i BETWEEN 0 AND 255");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(256)}));

	REQUIRE_NO_FAIL(con.Query("DROP TABLE t_bigint"));

	// VARCHAR keys: variable-length keys that span many byte depths.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_str (s VARCHAR PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_str VALUES ('apple'), ('banana'), ('cherry'), ('date'), ('elderberry')"));

	result = con.Query("SELECT COUNT(*) FROM t_str");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(5)}));

	result = con.Query("SELECT COUNT(*) FROM t_str WHERE s >= 'banana' AND s <= 'date'");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(3)}));

	REQUIRE_NO_FAIL(con.Query("DROP TABLE t_str"));

	// Non-unique index: multiple rows may share the same key.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_nonuniq (i INTEGER, j INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_nonuniq SELECT range % 100, range FROM range(1000)"));
	REQUIRE_NO_FAIL(con.Query("CREATE INDEX t_nonuniq_idx ON t_nonuniq (i)"));

	result = con.Query("SELECT COUNT(*) FROM t_nonuniq WHERE i = 0");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(10)}));

	result = con.Query("SELECT COUNT(*) FROM t_nonuniq WHERE i < 50");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(500)}));

	REQUIRE_NO_FAIL(con.Query("DROP TABLE t_nonuniq"));

	// Primary key on a table with exactly 256 rows sharing a common byte prefix:
	// exercises the boundary between one and two complete groups.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_256 (i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_256 SELECT range FROM range(256)"));

	result = con.Query("SELECT COUNT(*) FROM t_256");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(256)}));

	result = con.Query("SELECT i FROM t_256 WHERE i = 255");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(255)}));

	REQUIRE_NO_FAIL(con.Query("DROP TABLE t_256"));

	// Verify point lookups for values at group boundaries when using dense builds.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t_bounds (i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO t_bounds SELECT range FROM range(512)"));

	result = con.Query("SELECT i FROM t_bounds WHERE i = 0");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(0)}));

	result = con.Query("SELECT i FROM t_bounds WHERE i = 255");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(255)}));

	result = con.Query("SELECT i FROM t_bounds WHERE i = 256");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(256)}));

	result = con.Query("SELECT i FROM t_bounds WHERE i = 511");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(511)}));

	REQUIRE_NO_FAIL(con.Query("DROP TABLE t_bounds"));
}

TEST_CASE("Test ART bulk build with ARTBuilder", "[art-builder]") {
	DuckDB db(nullptr);
	Connection con(db);
	RunBulkBuildTests(con);
}

TEST_CASE("Test ART bulk build with QuARTBuilder", "[quart-builder]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_quart_index_build"));
	RunBulkBuildTests(con);
}

TEST_CASE("Test QuARTBuilder and ARTBuilder produce identical results", "[art-builder][quart-builder]") {
	// Build two databases with the same data, one using ARTBuilder and one using QuARTBuilder,
	// and compare the results of several queries.
	DuckDB db_art(nullptr);
	Connection con_art(db_art);

	DuckDB db_quart(nullptr);
	Connection con_quart(db_quart);
	REQUIRE_NO_FAIL(con_quart.Query("PRAGMA enable_quart_index_build"));

	// Create identical tables.
	REQUIRE_NO_FAIL(con_art.Query("CREATE TABLE t (i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con_quart.Query("CREATE TABLE t (i INTEGER PRIMARY KEY)"));

	REQUIRE_NO_FAIL(con_art.Query("INSERT INTO t SELECT range FROM range(10000)"));
	REQUIRE_NO_FAIL(con_quart.Query("INSERT INTO t SELECT range FROM range(10000)"));

	// Full scan.
	auto r_art = con_art.Query("SELECT COUNT(*) FROM t");
	auto r_quart = con_quart.Query("SELECT COUNT(*) FROM t");
	REQUIRE(CHECK_COLUMN(r_art, 0, {Value::BIGINT(10000)}));
	REQUIRE(CHECK_COLUMN(r_quart, 0, {Value::BIGINT(10000)}));

	// Range scan.
	r_art = con_art.Query("SELECT COUNT(*) FROM t WHERE i BETWEEN 512 AND 1023");
	r_quart = con_quart.Query("SELECT COUNT(*) FROM t WHERE i BETWEEN 512 AND 1023");
	REQUIRE(CHECK_COLUMN(r_art, 0, {Value::BIGINT(512)}));
	REQUIRE(CHECK_COLUMN(r_quart, 0, {Value::BIGINT(512)}));

	// Point lookup at group boundary.
	r_art = con_art.Query("SELECT i FROM t WHERE i = 255");
	r_quart = con_quart.Query("SELECT i FROM t WHERE i = 255");
	REQUIRE(CHECK_COLUMN(r_art, 0, {Value::INTEGER(255)}));
	REQUIRE(CHECK_COLUMN(r_quart, 0, {Value::INTEGER(255)}));

	r_art = con_art.Query("SELECT i FROM t WHERE i = 256");
	r_quart = con_quart.Query("SELECT i FROM t WHERE i = 256");
	REQUIRE(CHECK_COLUMN(r_art, 0, {Value::INTEGER(256)}));
	REQUIRE(CHECK_COLUMN(r_quart, 0, {Value::INTEGER(256)}));

	// Unique constraint violation should be detected by both builders.
	REQUIRE_FAIL(con_art.Query("INSERT INTO t VALUES (42)"));
	REQUIRE_FAIL(con_quart.Query("INSERT INTO t VALUES (42)"));
}

//! Verify a table containing the sequential integers [0, n) under column 'i' (INTEGER PRIMARY KEY).
//! Also checks that inserting an already-present key is rejected.
static void VerifySequentialIntTable(Connection &con, const string &table, int64_t n) {
	auto result = con.Query("SELECT COUNT(*) FROM " + table);
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(n)}));

	result = con.Query("SELECT COUNT(*) FROM " + table + " WHERE i >= 0");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(n)}));

	result = con.Query("SELECT i FROM " + table + " WHERE i = 0");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(0)}));

	result = con.Query("SELECT i FROM " + table + " WHERE i = " + std::to_string(n - 1));
	REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(static_cast<int32_t>(n - 1))}));

	if (n >= 256) {
		// Spans the first dense-leaf-group boundary (the QuART optimisation targets this).
		result = con.Query("SELECT COUNT(*) FROM " + table + " WHERE i < 256");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(256)}));

		result = con.Query("SELECT i FROM " + table + " WHERE i = 255");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(255)}));

		result = con.Query("SELECT i FROM " + table + " WHERE i = 256");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::INTEGER(256)}));
	}

	if (n > 200) {
		result = con.Query("SELECT COUNT(*) FROM " + table + " WHERE i BETWEEN 100 AND 200");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(101)}));
	}

	// Unique constraint: inserting an existing key must be rejected.
	REQUIRE_FAIL(con.Query("INSERT INTO " + table + " VALUES (0)"));
}

TEST_CASE("ART individual inserts correctness", "[art-individual]") {
	// Insert one row at a time — exercises the standard ART insert path, bypassing any builder.
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t (i INTEGER PRIMARY KEY)"));

	const int32_t N = 1000;
	for (int32_t i = 0; i < N; i++) {
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES ($1)", i));
	}

	VerifySequentialIntTable(con, "t", N);
}

TEST_CASE("Compare three insertion methods produce identical results",
          "[art-builder][quart-builder][art-individual]") {
	// Build three databases with the same data using each insertion method, then verify that
	// a common set of queries (full scan, range scans, point lookups, constraint checks)
	// returns identical results across all three.

	const int64_t N = 1000;

	// 1. Individual inserts — standard ART insert path, one row at a time.
	DuckDB db_individual(nullptr);
	Connection con_individual(db_individual);
	REQUIRE_NO_FAIL(con_individual.Query("CREATE TABLE t (i INTEGER PRIMARY KEY)"));
	for (int32_t i = 0; i < static_cast<int32_t>(N); i++) {
		REQUIRE_NO_FAIL(con_individual.Query("INSERT INTO t VALUES ($1)", i));
	}

	// 2. Single bulk ARTBuilder — one sorted INSERT over the full range.
	DuckDB db_art(nullptr);
	Connection con_art(db_art);
	REQUIRE_NO_FAIL(con_art.Query("CREATE TABLE t (i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con_art.Query("INSERT INTO t SELECT range FROM range(" + std::to_string(N) + ")"));

	// 3. Single bulk QuARTBuilder — same as above with the dense-leaf-group optimisation.
	DuckDB db_quart(nullptr);
	Connection con_quart(db_quart);
	REQUIRE_NO_FAIL(con_quart.Query("PRAGMA enable_quart_index_build"));
	REQUIRE_NO_FAIL(con_quart.Query("CREATE TABLE t (i INTEGER PRIMARY KEY)"));
	REQUIRE_NO_FAIL(con_quart.Query("INSERT INTO t SELECT range FROM range(" + std::to_string(N) + ")"));

	// Full count.
	{
		Value expected = Value::BIGINT(N);
		auto r1 = con_individual.Query("SELECT COUNT(*) FROM t");
		auto r2 = con_art.Query("SELECT COUNT(*) FROM t");
		auto r3 = con_quart.Query("SELECT COUNT(*) FROM t");
		REQUIRE(CHECK_COLUMN(r1, 0, {expected}));
		REQUIRE(CHECK_COLUMN(r2, 0, {expected}));
		REQUIRE(CHECK_COLUMN(r3, 0, {expected}));
	}

	// Range scan spanning the first dense-leaf-group boundary (0–255).
	{
		Value expected = Value::BIGINT(256);
		auto r1 = con_individual.Query("SELECT COUNT(*) FROM t WHERE i < 256");
		auto r2 = con_art.Query("SELECT COUNT(*) FROM t WHERE i < 256");
		auto r3 = con_quart.Query("SELECT COUNT(*) FROM t WHERE i < 256");
		REQUIRE(CHECK_COLUMN(r1, 0, {expected}));
		REQUIRE(CHECK_COLUMN(r2, 0, {expected}));
		REQUIRE(CHECK_COLUMN(r3, 0, {expected}));
	}

	// Interior range scan.
	{
		Value expected = Value::BIGINT(101);
		auto r1 = con_individual.Query("SELECT COUNT(*) FROM t WHERE i BETWEEN 100 AND 200");
		auto r2 = con_art.Query("SELECT COUNT(*) FROM t WHERE i BETWEEN 100 AND 200");
		auto r3 = con_quart.Query("SELECT COUNT(*) FROM t WHERE i BETWEEN 100 AND 200");
		REQUIRE(CHECK_COLUMN(r1, 0, {expected}));
		REQUIRE(CHECK_COLUMN(r2, 0, {expected}));
		REQUIRE(CHECK_COLUMN(r3, 0, {expected}));
	}

	// Point lookups at boundary and interior positions.
	for (int32_t key : {0, 255, 256, 999}) {
		Value expected = Value::INTEGER(key);
		string q = "SELECT i FROM t WHERE i = " + std::to_string(key);
		auto r1 = con_individual.Query(q);
		auto r2 = con_art.Query(q);
		auto r3 = con_quart.Query(q);
		REQUIRE(CHECK_COLUMN(r1, 0, {expected}));
		REQUIRE(CHECK_COLUMN(r2, 0, {expected}));
		REQUIRE(CHECK_COLUMN(r3, 0, {expected}));
	}

	// Unique constraint: every method must reject a duplicate key.
	REQUIRE_FAIL(con_individual.Query("INSERT INTO t VALUES (0)"));
	REQUIRE_FAIL(con_art.Query("INSERT INTO t VALUES (0)"));
	REQUIRE_FAIL(con_quart.Query("INSERT INTO t VALUES (0)"));
}
