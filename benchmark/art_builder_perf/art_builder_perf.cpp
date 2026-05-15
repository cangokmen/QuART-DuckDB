// art_builder_perf.cpp
//
// Standalone timing benchmark that compares three ART index-build strategies
// inside DuckDB for sorted sequential INT64 keys:
//
//   repeated_inserts  -- individual row inserts via Appender into a PRIMARY KEY
//                        table; each flush calls ARTOperator::Insert per key
//                        through the regular insert pipeline (no bulk builder)
//   art_builder       -- bulk sorted build using ARTBuilder
//                        (default DuckDB behaviour; PRAGMA disable_quart_index_build)
//   quart_builder     -- bulk sorted build using QuARTBuilder
//                        (PRAGMA enable_quart_index_build)
//
// Test sizes: N in {100M, 250M, 500M, 750M, 1000M, 2000M} (matching Figure 11
// in the QuART paper).
//
// For repeated_inserts the timed region is the Appender loop + Close() call
// (i.e., the time to build the PRIMARY KEY index incrementally from N sorted
// rows).  For art_builder and quart_builder the timed region is only
// CREATE INDEX on a pre-loaded table (so loading time is excluded).
//
// Usage:
//   ./art_builder_perf [--repeat R] [--sizes N1,N2,...] [--methods m1,m2,...]
//
//   --repeat R          number of timed repetitions per (N, method) (default 1)
//   --sizes N1,N2,...   comma-separated N values (default: full paper set)
//   --methods m1,m2,... subset of methods to run (default: all three)
//
// Output:
//   Results CSV  -> benchmark/art_builder_perf/results/results_<TS>.csv
//   Progress log -> benchmark/art_builder_perf/results/progress_<TS>.log
//   Both are also mirrored to stdout / stderr respectively.
//
// CSV columns: N,method,rep,build_ns

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "duckdb.hpp"
#include "duckdb/main/appender.hpp"

using namespace duckdb;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void die(const char *msg) {
	fprintf(stderr, "FATAL: %s\n", msg);
	exit(1);
}

static bool exec(Connection &con, const std::string &sql) {
	auto res = con.Query(sql);
	if (res->HasError()) {
		fprintf(stderr, "  SQL error [%s]: %s\n", sql.c_str(), res->GetError().c_str());
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Timing helpers for each strategy
// ---------------------------------------------------------------------------

// repeated_inserts: create an empty PK table and insert N sorted rows one by
// one via Appender.  Each internal flush (~204800 rows) triggers
// ARTOperator::Insert per key through the regular insert pipeline.
// Returns elapsed nanoseconds, or -1 on error.
static int64_t time_repeated_inserts(Connection &con, int64_t N) {
	if (!exec(con, "CREATE TABLE perf_t (id BIGINT PRIMARY KEY)")) {
		return -1;
	}

	Appender appender(con, "perf_t");

	auto t0 = steady_clock::now();
	for (int64_t i = 0; i < N; i++) {
		appender.BeginRow();
		appender.Append<int64_t>(i);
		appender.EndRow();
	}
	appender.Close();
	auto t1 = steady_clock::now();

	return static_cast<int64_t>(duration_cast<nanoseconds>(t1 - t0).count());
}

// art_builder / quart_builder: pre-load N rows into a table without an index,
// then time CREATE INDEX with the given pragmas.
// Returns elapsed nanoseconds, or -1 on error.
static int64_t time_bulk_build(Connection &con, int64_t N, const std::vector<std::string> &pragmas) {
	std::string load_sql =
	    "CREATE TABLE perf_t AS SELECT range::BIGINT AS id FROM range(" + std::to_string(N) + ")";
	if (!exec(con, load_sql)) {
		return -1;
	}

	for (auto &p : pragmas) {
		if (!exec(con, p)) {
			return -1;
		}
	}

	auto t0 = steady_clock::now();
	bool ok = exec(con, "CREATE INDEX perf_idx ON perf_t USING ART(id)");
	auto t1 = steady_clock::now();

	if (!ok) {
		return -1;
	}
	return static_cast<int64_t>(duration_cast<nanoseconds>(t1 - t0).count());
}

// ---------------------------------------------------------------------------
// Method descriptors
// ---------------------------------------------------------------------------

enum class MethodKind { REPEATED_INSERTS, BULK_BUILD };

struct Method {
	const char *name;
	MethodKind kind;
	std::vector<std::string> pragmas; // only used for BULK_BUILD
};

static std::vector<Method> all_methods() {
	return {
	    {"repeated_inserts", MethodKind::REPEATED_INSERTS, {}},
	    {"art_builder", MethodKind::BULK_BUILD, {"PRAGMA disable_quart_index_build"}},
	    {"quart_builder", MethodKind::BULK_BUILD, {"PRAGMA enable_quart_index_build"}},
	};
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

static std::vector<int64_t> default_sizes() {
	return {100000000LL, 250000000LL, 500000000LL, 750000000LL, 1000000000LL, 2000000000LL};
}

static std::vector<int64_t> parse_sizes(const char *arg) {
	std::vector<int64_t> result;
	std::string s(arg);
	size_t pos = 0;
	while (pos < s.size()) {
		size_t comma = s.find(',', pos);
		if (comma == std::string::npos) {
			comma = s.size();
		}
		result.push_back(std::stoll(s.substr(pos, comma - pos)));
		pos = comma + 1;
	}
	return result;
}

static std::vector<Method> parse_methods(const char *arg) {
	std::vector<Method> all = all_methods();
	std::vector<Method> result;
	std::string s(arg);
	size_t pos = 0;
	while (pos < s.size()) {
		size_t comma = s.find(',', pos);
		if (comma == std::string::npos) {
			comma = s.size();
		}
		std::string name = s.substr(pos, comma - pos);
		bool found = false;
		for (auto &m : all) {
			if (name == m.name) {
				result.push_back(m);
				found = true;
				break;
			}
		}
		if (!found) {
			fprintf(stderr, "Unknown method: %s\n", name.c_str());
			fprintf(stderr, "  Valid methods: repeated_inserts, art_builder, quart_builder\n");
			exit(1);
		}
		pos = comma + 1;
	}
	return result;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
	int repeat = 1;
	std::vector<int64_t> sizes = default_sizes();
	std::vector<Method> methods = all_methods();

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
			repeat = atoi(argv[++i]);
			if (repeat < 1) {
				die("--repeat must be >= 1");
			}
		} else if (strcmp(argv[i], "--sizes") == 0 && i + 1 < argc) {
			sizes = parse_sizes(argv[++i]);
		} else if (strcmp(argv[i], "--methods") == 0 && i + 1 < argc) {
			methods = parse_methods(argv[++i]);
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("Usage: %s [--repeat R] [--sizes N1,N2,...] [--methods m1,m2,...]\n", argv[0]);
			printf("  methods: repeated_inserts, art_builder, quart_builder\n");
			return 0;
		} else {
			fprintf(stderr, "Unknown argument: %s\n", argv[i]);
			return 1;
		}
	}

	// Build timestamped output paths.
	std::time_t now_t = std::time(nullptr);
	char ts[32];
	std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now_t));
	std::string results_dir = "benchmark/art_builder_perf/results";
	(void)system(("mkdir -p " + results_dir).c_str());

	std::string csv_path = results_dir + "/results_" + ts + ".csv";
	std::string log_path = results_dir + "/progress_" + ts + ".log";

	std::ofstream csv_file(csv_path);
	std::ofstream log_file(log_path);
	if (!csv_file.is_open()) {
		die(("Cannot open " + csv_path).c_str());
	}
	if (!log_file.is_open()) {
		die(("Cannot open " + log_path).c_str());
	}

	fprintf(stderr, "Results : %s\n", csv_path.c_str());
	fprintf(stderr, "Log     : %s\n\n", log_path.c_str());

	auto emit = [&](const std::string &line) {
		csv_file << line << "\n";
		csv_file.flush();
		printf("%s\n", line.c_str());
		fflush(stdout);
	};
	auto log = [&](const std::string &msg) {
		log_file << msg << "\n";
		log_file.flush();
		fprintf(stderr, "%s\n", msg.c_str());
		fflush(stderr);
	};

	emit("N,method,rep,build_ns");

	for (int64_t N : sizes) {
		log("=== N=" + std::to_string(N) + " ===");

		for (auto &method : methods) {
			for (int rep = 1; rep <= repeat; rep++) {
				std::string prefix = "  [" + std::string(method.name) + "] rep " +
				                     std::to_string(rep) + "/" + std::to_string(repeat);

				// Fresh in-memory database per (N, method, rep).
				DuckDB db(nullptr);
				Connection con(db);

				int64_t ns = -1;
				if (method.kind == MethodKind::REPEATED_INSERTS) {
					log(prefix + " -- inserting " + std::to_string(N) + " rows one by one...");
					ns = time_repeated_inserts(con, N);
				} else {
					log(prefix + " -- loading " + std::to_string(N) + " rows...");
					// time_bulk_build loads data then times CREATE INDEX.
					// The log line for "building index" is printed inside the function
					// after loading completes; print it here before the call instead.
					ns = time_bulk_build(con, N, method.pragmas);
				}

				if (ns < 0) {
					emit(std::to_string(N) + "," + method.name + "," + std::to_string(rep) + ",ERROR");
				} else {
					emit(std::to_string(N) + "," + method.name + "," + std::to_string(rep) + "," +
					     std::to_string(ns));
					char buf[64];
					snprintf(buf, sizeof(buf), "%.3f s", ns / 1e9);
					log(prefix + " -- done: " + buf);
				}
			}
		}
	}

	return 0;
}
