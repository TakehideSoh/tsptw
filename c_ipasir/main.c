#include "ipasir.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

// Minimal C implementation of SAT-based CEGAR for Hamiltonian cycle on .col/.gb graphs.
// It intentionally keeps dependencies small: only IPASIR (CaDiCaL backend).

// Tiny growable containers.
typedef struct {
  int *data;
  int size;
  int cap;
} IntVec;

// StrVec: growable vector of heap-allocated strings.
typedef struct {
  char **data;
  int size;
  int cap;
} StrVec;

// ClauseVec: growable vector of clauses (each clause is IntVec).
typedef struct {
  IntVec *data;
  int size;
  int cap;
} ClauseVec;

// Pair: directed arc (u -> v).
typedef struct {
  int u;
  int v;
} Pair;

// PairVec: growable vector of Pair.
typedef struct {
  Pair *data;
  int size;
  int cap;
} PairVec;

// CycleVec: growable vector of cycles (each cycle is IntVec).
typedef struct {
  IntVec *data;
  int size;
  int cap;
} CycleVec;

// Graph: 1-indexed graph with adjacency lists and directed arcs list.
typedef struct {
  int n;
  IntVec *adj;
  PairVec arcs;
  char **labels;
  bool has_labels;
} Graph;

// PairMap: open-address hash map from (u,v) to SAT literal.
typedef struct {
  uint64_t *keys;
  int *vals;
  int cap;
  int size;
} PairMap;

// IntCountMap: tiny map used for histogram output (length -> count).
typedef struct {
  int *keys;
  int *vals;
  int size;
  int cap;
} IntCountMap;

// Encoder: CNF encoding state and arc-literal mapping.
typedef struct {
  PairMap lit_map;
  IntVec arc_lits;
  int next_var;
} Encoder;

// TwoOptResult: clauses produced by merge step and remaining cycles.
typedef struct {
  ClauseVec block_clauses;
  CycleVec active_cycles;
} TwoOptResult;

// SolveResult: final CEGAR counters for reporting.
typedef struct {
  int increments;
  int added_block_clauses;
} SolveResult;

// GbArc: GraphBase arc record (tip vertex index, next arc index).
typedef struct {
  int dst;
  int next;
} GbArc;

// die: print an error message and abort.
static void die(const char *msg) {
  fprintf(stderr, "error: %s\n", msg);
  exit(1);
}

// max_int: return larger of two integers.
static int max_int(const int a, const int b) { return a > b ? a : b; }

// intvec_init: initialize an IntVec to empty.
static void intvec_init(IntVec *v) {
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// intvec_reserve: ensure IntVec capacity.
static void intvec_reserve(IntVec *v, int cap) {
  if (cap <= v->cap) return;
  int ncap = v->cap ? v->cap : 8;
  while (ncap < cap) ncap *= 2;
  int *ndata = (int *)realloc(v->data, (size_t)ncap * sizeof(int));
  if (!ndata) die("realloc failed");
  v->data = ndata;
  v->cap = ncap;
}

// intvec_push: append one integer.
static void intvec_push(IntVec *v, int x) {
  if (v->size == v->cap) intvec_reserve(v, v->size + 1);
  v->data[v->size++] = x;
}

// intvec_contains: linear membership test.
static bool intvec_contains(const IntVec *v, int x) {
  for (int i = 0; i < v->size; ++i) {
    if (v->data[i] == x) return true;
  }
  return false;
}

// intvec_copy: deep-copy one IntVec.
static void intvec_copy(IntVec *dst, const IntVec *src) {
  intvec_init(dst);
  if (src->size) {
    intvec_reserve(dst, src->size);
    memcpy(dst->data, src->data, (size_t)src->size * sizeof(int));
    dst->size = src->size;
  }
}

// intvec_free: release IntVec storage.
static void intvec_free(IntVec *v) {
  free(v->data);
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// strvec_init: initialize a StrVec to empty.
static void strvec_init(StrVec *v) {
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// strvec_reserve: ensure StrVec capacity.
static void strvec_reserve(StrVec *v, int cap) {
  if (cap <= v->cap) return;
  int ncap = v->cap ? v->cap : 8;
  while (ncap < cap) ncap *= 2;
  char **ndata = (char **)realloc(v->data, (size_t)ncap * sizeof(char *));
  if (!ndata) die("realloc failed");
  v->data = ndata;
  v->cap = ncap;
}

// strvec_push_take: append one owned string pointer.
static void strvec_push_take(StrVec *v, char *s) {
  if (v->size == v->cap) strvec_reserve(v, v->size + 1);
  v->data[v->size++] = s;
}

// strvec_free: release StrVec and contained strings.
static void strvec_free(StrVec *v) {
  for (int i = 0; i < v->size; ++i) free(v->data[i]);
  free(v->data);
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// pairvec_init: initialize a PairVec to empty.
static void pairvec_init(PairVec *v) {
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// pairvec_reserve: ensure PairVec capacity.
static void pairvec_reserve(PairVec *v, int cap) {
  if (cap <= v->cap) return;
  int ncap = v->cap ? v->cap : 8;
  while (ncap < cap) ncap *= 2;
  Pair *ndata = (Pair *)realloc(v->data, (size_t)ncap * sizeof(Pair));
  if (!ndata) die("realloc failed");
  v->data = ndata;
  v->cap = ncap;
}

// pairvec_push: append one directed arc.
static void pairvec_push(PairVec *v, int u, int w) {
  if (v->size == v->cap) pairvec_reserve(v, v->size + 1);
  v->data[v->size].u = u;
  v->data[v->size].v = w;
  v->size++;
}

// pairvec_free: release PairVec storage.
static void pairvec_free(PairVec *v) {
  free(v->data);
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// clausevec_init: initialize a ClauseVec to empty.
static void clausevec_init(ClauseVec *v) {
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// clausevec_reserve: ensure ClauseVec capacity.
static void clausevec_reserve(ClauseVec *v, int cap) {
  if (cap <= v->cap) return;
  int ncap = v->cap ? v->cap : 8;
  while (ncap < cap) ncap *= 2;
  IntVec *ndata = (IntVec *)realloc(v->data, (size_t)ncap * sizeof(IntVec));
  if (!ndata) die("realloc failed");
  v->data = ndata;
  v->cap = ncap;
}

// clausevec_push_copy: append deep-copied clause.
static void clausevec_push_copy(ClauseVec *v, const IntVec *cl) {
  if (v->size == v->cap) clausevec_reserve(v, v->size + 1);
  intvec_copy(&v->data[v->size], cl);
  v->size++;
}

// clausevec_push_take: append clause by moving ownership.
static void clausevec_push_take(ClauseVec *v, IntVec *cl) {
  if (v->size == v->cap) clausevec_reserve(v, v->size + 1);
  v->data[v->size] = *cl;
  v->size++;
  cl->data = NULL;
  cl->size = 0;
  cl->cap = 0;
}

// clausevec_extend_move: move all clauses from src into dst.
static void clausevec_extend_move(ClauseVec *dst, ClauseVec *src) {
  for (int i = 0; i < src->size; ++i) {
    clausevec_push_take(dst, &src->data[i]);
  }
  free(src->data);
  src->data = NULL;
  src->size = 0;
  src->cap = 0;
}

// clausevec_free: release all clauses and container.
static void clausevec_free(ClauseVec *v) {
  for (int i = 0; i < v->size; ++i) intvec_free(&v->data[i]);
  free(v->data);
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// cyclevec_init: initialize a CycleVec to empty.
static void cyclevec_init(CycleVec *v) {
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// cyclevec_reserve: ensure CycleVec capacity.
static void cyclevec_reserve(CycleVec *v, int cap) {
  if (cap <= v->cap) return;
  int ncap = v->cap ? v->cap : 8;
  while (ncap < cap) ncap *= 2;
  IntVec *ndata = (IntVec *)realloc(v->data, (size_t)ncap * sizeof(IntVec));
  if (!ndata) die("realloc failed");
  v->data = ndata;
  v->cap = ncap;
}

// cyclevec_push_copy: append deep-copied cycle.
static void cyclevec_push_copy(CycleVec *v, const IntVec *cyc) {
  if (v->size == v->cap) cyclevec_reserve(v, v->size + 1);
  intvec_copy(&v->data[v->size], cyc);
  v->size++;
}

// cyclevec_push_take: append cycle by moving ownership.
static void cyclevec_push_take(CycleVec *v, IntVec *cyc) {
  if (v->size == v->cap) cyclevec_reserve(v, v->size + 1);
  v->data[v->size] = *cyc;
  v->size++;
  cyc->data = NULL;
  cyc->size = 0;
  cyc->cap = 0;
}

// cyclevec_extend_copy: append deep copies of all cycles.
static void cyclevec_extend_copy(CycleVec *dst, const CycleVec *src) {
  for (int i = 0; i < src->size; ++i) cyclevec_push_copy(dst, &src->data[i]);
}

// cyclevec_free: release all cycles and container.
static void cyclevec_free(CycleVec *v) {
  for (int i = 0; i < v->size; ++i) intvec_free(&v->data[i]);
  free(v->data);
  v->data = NULL;
  v->size = 0;
  v->cap = 0;
}

// splitmix64: 64-bit mixing function for hash-table key dispersion.
static uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31U);
}

// pairmap_init: initialize pair-to-literal hash map.
static void pairmap_init(PairMap *m, int cap_hint) {
  int cap = 1;
  while (cap < cap_hint * 2) cap <<= 1;
  if (cap < 1024) cap = 1024;
  m->cap = cap;
  m->size = 0;
  m->keys = (uint64_t *)calloc((size_t)cap, sizeof(uint64_t));
  m->vals = (int *)calloc((size_t)cap, sizeof(int));
  if (!m->keys || !m->vals) die("calloc failed");
}

// make_key: pack (u,v) into one 64-bit key.
static uint64_t make_key(const int u, const int v) {
  return (((uint64_t)(uint32_t)u) << 32U) | (uint32_t)v;
}

// pairmap_rehash: rebuild hash table with larger capacity.
static void pairmap_rehash(PairMap *m, int new_cap) {
  PairMap n;
  n.cap = new_cap;
  n.size = 0;
  n.keys = (uint64_t *)calloc((size_t)new_cap, sizeof(uint64_t));
  n.vals = (int *)calloc((size_t)new_cap, sizeof(int));
  if (!n.keys || !n.vals) die("calloc failed");

  for (int i = 0; i < m->cap; ++i) {
    if (!m->keys[i]) continue;
    uint64_t key = m->keys[i];
    int val = m->vals[i];
    uint64_t h = splitmix64(key);
    int idx = (int)(h & (uint64_t)(new_cap - 1));
    while (n.keys[idx]) idx = (idx + 1) & (new_cap - 1);
    n.keys[idx] = key;
    n.vals[idx] = val;
    n.size++;
  }

  free(m->keys);
  free(m->vals);
  *m = n;
}

// pairmap_put: insert or overwrite (u,v) -> literal.
static void pairmap_put(PairMap *m, const int u, const int v, const int val) {
  if ((m->size + 1) * 10 > m->cap * 7) pairmap_rehash(m, m->cap * 2);
  const uint64_t key = make_key(u, v);
  uint64_t h = splitmix64(key);
  int idx = (int)(h & (uint64_t)(m->cap - 1));
  while (m->keys[idx] && m->keys[idx] != key) idx = (idx + 1) & (m->cap - 1);
  if (!m->keys[idx]) m->size++;
  m->keys[idx] = key;
  m->vals[idx] = val;
}

// pairmap_get: fetch literal for (u,v), or 0 if absent.
static int pairmap_get(const PairMap *m, const int u, const int v) {
  const uint64_t key = make_key(u, v);
  uint64_t h = splitmix64(key);
  int idx = (int)(h & (uint64_t)(m->cap - 1));
  while (m->keys[idx]) {
    if (m->keys[idx] == key) return m->vals[idx];
    idx = (idx + 1) & (m->cap - 1);
  }
  return 0;
}

// pairmap_free: release PairMap storage.
static void pairmap_free(PairMap *m) {
  free(m->keys);
  free(m->vals);
  m->keys = NULL;
  m->vals = NULL;
  m->cap = 0;
  m->size = 0;
}

// icount_init: initialize histogram map.
static void icount_init(IntCountMap *m) {
  m->keys = NULL;
  m->vals = NULL;
  m->size = 0;
  m->cap = 0;
}

// icount_inc: increment count of one key.
static void icount_inc(IntCountMap *m, int key) {
  for (int i = 0; i < m->size; ++i) {
    if (m->keys[i] == key) {
      m->vals[i]++;
      return;
    }
  }
  if (m->size == m->cap) {
    int ncap = m->cap ? m->cap * 2 : 8;
    int *nkeys = (int *)realloc(m->keys, (size_t)ncap * sizeof(int));
    int *nvals = (int *)realloc(m->vals, (size_t)ncap * sizeof(int));
    if (!nkeys || !nvals) die("realloc failed");
    m->keys = nkeys;
    m->vals = nvals;
    m->cap = ncap;
  }
  m->keys[m->size] = key;
  m->vals[m->size] = 1;
  m->size++;
}

// icount_sort: sort histogram entries by key.
static void icount_sort(IntCountMap *m) {
  for (int i = 0; i < m->size; ++i) {
    for (int j = i + 1; j < m->size; ++j) {
      if (m->keys[i] > m->keys[j]) {
        int tk = m->keys[i], tv = m->vals[i];
        m->keys[i] = m->keys[j];
        m->vals[i] = m->vals[j];
        m->keys[j] = tk;
        m->vals[j] = tv;
      }
    }
  }
}

// icount_print: print histogram as {k: v, ...}.
static void icount_print(const IntCountMap *m) {
  printf("{");
  for (int i = 0; i < m->size; ++i) {
    if (i) printf(", ");
    printf("%d: %d", m->keys[i], m->vals[i]);
  }
  printf("}");
}

// icount_free: release histogram storage.
static void icount_free(IntCountMap *m) {
  free(m->keys);
  free(m->vals);
  m->keys = NULL;
  m->vals = NULL;
  m->size = 0;
  m->cap = 0;
}

// now_ns: monotonic timestamp in nanoseconds.
static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// format_duration: pretty-print nanoseconds with adaptive unit.
static void format_duration(char *buf, size_t n, int64_t ns) {
  double us = (double)ns / 1e3;
  double ms = (double)ns / 1e6;
  double s = (double)ns / 1e9;
  if (us < 1000.0) {
    snprintf(buf, n, "%.3fus", us);
  } else if (ms < 1000.0) {
    snprintf(buf, n, "%.6fms", ms);
  } else {
    snprintf(buf, n, "%.6fs", s);
  }
}

// graph_init: initialize empty graph.
static void graph_init(Graph *g) {
  g->n = 0;
  g->adj = NULL;
  pairvec_init(&g->arcs);
  g->labels = NULL;
  g->has_labels = false;
}

// graph_resize: expand graph vertex range to 1..n.
static void graph_resize(Graph *g, int n) {
  if (n <= g->n) return;
  const int old_n = g->n;
  IntVec *nadj = (IntVec *)realloc(g->adj, (size_t)(n + 1) * sizeof(IntVec));
  if (!nadj) die("realloc failed");
  char **nlabels = (char **)realloc(g->labels, (size_t)(n + 1) * sizeof(char *));
  if (!nlabels) die("realloc failed");
  g->adj = nadj;
  g->labels = nlabels;
  const int init_from = (old_n == 0) ? 0 : (old_n + 1);
  for (int i = init_from; i <= n; ++i) intvec_init(&g->adj[i]);
  for (int i = init_from; i <= n; ++i) g->labels[i] = NULL;
  g->n = n;
}

// graph_add_edge: add undirected edge and both directed arcs.
static void graph_add_edge(Graph *g, int u, int v) {
  int mx = max_int(u, v);
  if (mx > g->n) graph_resize(g, mx);
  intvec_push(&g->adj[u], v);
  intvec_push(&g->adj[v], u);
  pairvec_push(&g->arcs, u, v);
  pairvec_push(&g->arcs, v, u);
}

// graph_free: release graph memory.
static void graph_free(Graph *g) {
  for (int i = 0; i <= g->n; ++i) intvec_free(&g->adj[i]);
  free(g->adj);
  g->adj = NULL;
  if (g->labels) {
    for (int i = 0; i <= g->n; ++i) free(g->labels[i]);
  }
  free(g->labels);
  g->labels = NULL;
  g->has_labels = false;
  g->n = 0;
  pairvec_free(&g->arcs);
}

// xstrdup_local: heap-copy a C string.
static char *xstrdup_local(const char *s) {
  size_t n = strlen(s);
  char *out = (char *)malloc(n + 1);
  if (!out) die("malloc failed");
  memcpy(out, s, n + 1);
  return out;
}

// trim_ascii: trim leading/trailing ASCII whitespace in-place.
static char *trim_ascii(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) {
    end--;
    *end = '\0';
  }
  return s;
}

// parse_nonneg_int: parse non-negative integer token.
static bool parse_nonneg_int(const char *s, int *out) {
  if (!s || !*s) return false;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0') return false;
  if (v < 0 || v > INT_MAX) return false;
  *out = (int)v;
  return true;
}

// parse_ref_token: parse GraphBase ref token like "A123"/"V12"/"0".
static bool parse_ref_token(const char *tok, char prefix, bool allow_zero, int *out) {
  if (!tok) return false;
  if (allow_zero && tok[0] == '0' && tok[1] == '\0') {
    *out = -1;
    return true;
  }
  if (tok[0] != prefix) return false;
  return parse_nonneg_int(tok + 1, out);
}

// csv_split_line: split one CSV line into fields (supports quoted commas and "").
static void csv_split_line(const char *line, StrVec *fields) {
  strvec_init(fields);
  size_t n = strlen(line);
  char *buf = (char *)malloc(n + 1);
  if (!buf) die("malloc failed");
  int bi = 0;
  bool in_quotes = false;

  for (size_t i = 0; i < n; ++i) {
    char c = line[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < n && line[i + 1] == '"') {
          buf[bi++] = '"';
          i++;
        } else {
          in_quotes = false;
        }
      } else {
        buf[bi++] = c;
      }
    } else {
      if (c == '"') {
        in_quotes = true;
      } else if (c == ',') {
        buf[bi] = '\0';
        strvec_push_take(fields, xstrdup_local(trim_ascii(buf)));
        bi = 0;
      } else {
        buf[bi++] = c;
      }
    }
  }
  buf[bi] = '\0';
  strvec_push_take(fields, xstrdup_local(trim_ascii(buf)));
  free(buf);
}

// ends_with_backslash: true if the string ends with '\' character.
static bool ends_with_backslash(const char *s) {
  size_t n = strlen(s);
  return n > 0 && s[n - 1] == '\\';
}

// read_gb_normalized_lines: read .gb file, join continuation lines, strip blanks.
static void read_gb_normalized_lines(FILE *fp, StrVec *lines) {
  strvec_init(lines);
  char *raw = NULL;
  size_t cap = 0;
  ssize_t nr;
  char *acc = NULL;
  size_t acc_len = 0;

  while ((nr = getline(&raw, &cap, fp)) != -1) {
    while (nr > 0 && (raw[nr - 1] == '\n' || raw[nr - 1] == '\r')) raw[--nr] = '\0';
    char *piece = trim_ascii(raw);
    size_t plen = strlen(piece);

    if (!acc) {
      acc = xstrdup_local(piece);
      acc_len = plen;
    } else {
      char *nacc = (char *)realloc(acc, acc_len + plen + 1);
      if (!nacc) die("realloc failed");
      acc = nacc;
      memcpy(acc + acc_len, piece, plen + 1);
      acc_len += plen;
    }

    if (ends_with_backslash(acc)) {
      acc[--acc_len] = '\0';
      continue;
    }

    char *trimmed = trim_ascii(acc);
    if (*trimmed) strvec_push_take(lines, xstrdup_local(trimmed));
    free(acc);
    acc = NULL;
    acc_len = 0;
  }
  if (acc) {
    char *trimmed = trim_ascii(acc);
    if (*trimmed) strvec_push_take(lines, xstrdup_local(trimmed));
    free(acc);
  }
  free(raw);
}

// pair_cmp_undirected: comparator for undirected edges represented by (u,v), u<=v.
static int pair_cmp_undirected(const void *a, const void *b) {
  const Pair *x = (const Pair *)a;
  const Pair *y = (const Pair *)b;
  if (x->u != y->u) return (x->u < y->u) ? -1 : 1;
  if (x->v != y->v) return (x->v < y->v) ? -1 : 1;
  return 0;
}

// input_to_graph_col: parse DIMACS-like .col text and build graph.
static Graph input_to_graph_col(const char *filename) {
  // DIMACS-like .col subset:
  //   c ...        comment
  //   p <n> <m>    graph size hint
  //   e <u> <v>    undirected edge
  FILE *fp = fopen(filename, "r");
  if (!fp) die("could not open input file");
  Graph g;
  graph_init(&g);
  char line[4096];
  while (fgets(line, sizeof(line), fp)) {
    if (!line[0]) continue;
    if (line[0] == 'c') continue;
    if (line[0] == 'p') {
      int n = 0, m = 0;
      if (sscanf(line, "p %d %d", &n, &m) >= 1 && n > 0) {
        graph_resize(&g, n);
      }
      continue;
    }
    if (line[0] == 'e') {
      int u = 0, v = 0;
      if (sscanf(line, "e %d %d", &u, &v) == 2) graph_add_edge(&g, u, v);
    }
  }
  fclose(fp);
  if (g.n == 0) die("graph has zero vertices");
  return g;
}

// input_to_graph_gb: parse Stanford GraphBase .gb and build undirected graph.
static Graph input_to_graph_gb(const char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) die("could not open input file");

  StrVec lines;
  read_gb_normalized_lines(fp, &lines);
  fclose(fp);
  if (lines.size < 3) die("gb file is too short");

  int vertices_idx = -1;
  int arcs_idx = -1;
  for (int i = 0; i < lines.size; ++i) {
    if (!strcmp(lines.data[i], "* Vertices")) vertices_idx = i;
    if (!strcmp(lines.data[i], "* Arcs")) arcs_idx = i;
  }
  if (vertices_idx < 0 || arcs_idx < 0 || vertices_idx >= arcs_idx) {
    die("gb file missing '* Vertices'/'* Arcs' sections");
  }

  int n = -1;
  int m = -1;
  {
    size_t header_len = 0;
    for (int i = 1; i < vertices_idx; ++i) header_len += strlen(lines.data[i]);
    char *header = (char *)malloc(header_len + 1);
    if (!header) die("malloc failed");
    size_t off = 0;
    for (int i = 1; i < vertices_idx; ++i) {
      size_t l = strlen(lines.data[i]);
      memcpy(header + off, lines.data[i], l);
      off += l;
    }
    header[off] = '\0';
    StrVec fields;
    csv_split_line(header, &fields);
    if (fields.size >= 3 && parse_nonneg_int(fields.data[1], &n) && parse_nonneg_int(fields.data[2], &m)) {
      // Parsed graph info from merged header blob.
    } else {
      n = -1;
      m = -1;
    }
    strvec_free(&fields);
    free(header);
  }
  if (n < 0 || m < 0) {
    for (int i = 1; i < vertices_idx; ++i) {
      if (lines.data[i][0] == '*') continue;
      StrVec fields;
      csv_split_line(lines.data[i], &fields);
      if (fields.size >= 3 && parse_nonneg_int(fields.data[1], &n) &&
          parse_nonneg_int(fields.data[2], &m)) {
        strvec_free(&fields);
        break;
      }
      strvec_free(&fields);
    }
  }
  if (n <= 0 || m <= 0) die("invalid gb graph info line");

  StrVec vlines;
  StrVec alines;
  strvec_init(&vlines);
  strvec_init(&alines);
  for (int i = vertices_idx + 1; i < arcs_idx; ++i) {
    if (lines.data[i][0] == '*') continue;
    strvec_push_take(&vlines, xstrdup_local(lines.data[i]));
  }
  for (int i = arcs_idx + 1; i < lines.size; ++i) {
    if (lines.data[i][0] == '*') continue;
    strvec_push_take(&alines, xstrdup_local(lines.data[i]));
  }
  if (vlines.size < n) die("gb vertex lines are shorter than declared");
  if (alines.size < m) die("gb arc lines are shorter than declared");

  int *first_arcs = (int *)malloc((size_t)n * sizeof(int));
  GbArc *arcs = (GbArc *)malloc((size_t)m * sizeof(GbArc));
  char **vertex_labels = (char **)calloc((size_t)n, sizeof(char *));
  if (!first_arcs || !arcs || !vertex_labels) die("malloc failed");
  bool any_vertex_label = false;

  for (int i = 0; i < n; ++i) {
    StrVec fields;
    csv_split_line(vlines.data[i], &fields);
    if (fields.size < 2) die("invalid gb vertex line");
    if (!parse_ref_token(fields.data[1], 'A', true, &first_arcs[i])) die("invalid gb vertex arc ref");
    if (fields.size >= 1 && fields.data[0][0] != '\0') {
      vertex_labels[i] = xstrdup_local(fields.data[0]);
      any_vertex_label = true;
    }
    strvec_free(&fields);
  }

  for (int i = 0; i < m; ++i) {
    StrVec fields;
    csv_split_line(alines.data[i], &fields);
    if (fields.size < 2) die("invalid gb arc line");
    if (!parse_ref_token(fields.data[0], 'V', false, &arcs[i].dst)) die("invalid gb arc tip ref");
    if (!parse_ref_token(fields.data[1], 'A', true, &arcs[i].next)) die("invalid gb arc next ref");
    strvec_free(&fields);
  }

  PairVec edges;
  pairvec_init(&edges);
  pairvec_reserve(&edges, m);

  int *seen = (int *)calloc((size_t)m, sizeof(int));
  if (!seen) die("calloc failed");
  int epoch = 1;
  for (int src = 0; src < n; ++src) {
    int arc = first_arcs[src];
    while (arc != -1) {
      if (arc < 0 || arc >= m) die("gb arc index out of range");
      if (seen[arc] == epoch) break;
      seen[arc] = epoch;

      int dst = arcs[arc].dst;
      if (dst < 0 || dst >= n) die("gb vertex index out of range");
      if (src != dst) {
        int u = src + 1;
        int v = dst + 1;
        if (u > v) {
          int t = u;
          u = v;
          v = t;
        }
        pairvec_push(&edges, u, v);
      }
      arc = arcs[arc].next;
    }
    if (epoch == INT_MAX) {
      memset(seen, 0, (size_t)m * sizeof(int));
      epoch = 1;
    } else {
      epoch++;
    }
  }

  qsort(edges.data, (size_t)edges.size, sizeof(Pair), pair_cmp_undirected);
  Graph g;
  graph_init(&g);
  graph_resize(&g, n);
  g.has_labels = any_vertex_label;
  if (any_vertex_label) {
    for (int i = 0; i < n; ++i) g.labels[i + 1] = vertex_labels[i];
  } else {
    for (int i = 0; i < n; ++i) free(vertex_labels[i]);
  }
  for (int i = 0; i < edges.size; ++i) {
    if (i > 0 && edges.data[i].u == edges.data[i - 1].u && edges.data[i].v == edges.data[i - 1].v) {
      continue;
    }
    graph_add_edge(&g, edges.data[i].u, edges.data[i].v);
  }

  free(seen);
  free(first_arcs);
  free(arcs);
  free(vertex_labels);
  pairvec_free(&edges);
  strvec_free(&vlines);
  strvec_free(&alines);
  strvec_free(&lines);
  return g;
}

// has_suffix: check exact case-sensitive suffix.
static bool has_suffix(const char *s, const char *suffix) {
  size_t ns = strlen(s);
  size_t nt = strlen(suffix);
  if (nt > ns) return false;
  return !strcmp(s + (ns - nt), suffix);
}

// input_to_graph: parse .col or .gb and build graph.
static Graph input_to_graph(const char *filename) {
  if (has_suffix(filename, ".gb")) return input_to_graph_gb(filename);
  return input_to_graph_col(filename);
}

// add_clause_solver: push one CNF clause to IPASIR solver.
static void add_clause_solver(void *solver, const IntVec *cl) {
  for (int i = 0; i < cl->size; ++i) ipasir_add(solver, cl->data[i]);
  ipasir_add(solver, 0);
}

// encoder_init: initialize SAT encoding context.
static void encoder_init(Encoder *e, int arc_count) {
  pairmap_init(&e->lit_map, arc_count * 2 + 1);
  intvec_init(&e->arc_lits);
  intvec_reserve(&e->arc_lits, arc_count);
  e->next_var = 1;
}

// encoder_free: release encoder resources.
static void encoder_free(Encoder *e) {
  pairmap_free(&e->lit_map);
  intvec_free(&e->arc_lits);
}

// encoder_new_lit: allocate next SAT variable id.
static int encoder_new_lit(Encoder *e) { return e->next_var++; }

// sinz_at_most_one: encode at-most-one constraint via Sinz encoding.
static void sinz_at_most_one(Encoder *e, const IntVec *lits, ClauseVec *cnf) {
  // Sinz sequential counter encoding for "at most one".
  if (lits->size <= 1) return;
  int n = lits->size;
  IntVec s;
  intvec_init(&s);
  intvec_reserve(&s, n - 1);
  for (int i = 0; i < n - 1; ++i) intvec_push(&s, encoder_new_lit(e));

  IntVec cl;
  intvec_init(&cl);

  intvec_push(&cl, -lits->data[0]);
  intvec_push(&cl, s.data[0]);
  clausevec_push_take(cnf, &cl);
  intvec_init(&cl);

  for (int i = 1; i < n - 1; ++i) {
    intvec_push(&cl, -lits->data[i]);
    intvec_push(&cl, s.data[i]);
    clausevec_push_take(cnf, &cl);
    intvec_init(&cl);

    intvec_push(&cl, -s.data[i - 1]);
    intvec_push(&cl, s.data[i]);
    clausevec_push_take(cnf, &cl);
    intvec_init(&cl);

    intvec_push(&cl, -lits->data[i]);
    intvec_push(&cl, -s.data[i - 1]);
    clausevec_push_take(cnf, &cl);
    intvec_init(&cl);
  }

  intvec_push(&cl, -lits->data[n - 1]);
  intvec_push(&cl, -s.data[n - 2]);
  clausevec_push_take(cnf, &cl);

  intvec_free(&s);
}

// two_loop_prohibition: forbid immediate 2-cycles u<->v.
static void two_loop_prohibition(const Graph *g, const Encoder *e, ClauseVec *cnf) {
  IntVec cl;
  for (int u = 1; u <= g->n; ++u) {
    for (int i = 0; i < g->adj[u].size; ++i) {
      int v = g->adj[u].data[i];
      if (u >= v) continue;
      int lit1 = pairmap_get(&e->lit_map, u, v);
      int lit2 = pairmap_get(&e->lit_map, v, u);
      intvec_init(&cl);
      intvec_push(&cl, -lit1);
      intvec_push(&cl, -lit2);
      clausevec_push_take(cnf, &cl);
    }
  }
}

// encode_graph: build base CNF for one-in/one-out Hamiltonian model.
static void encode_graph(Encoder *e, const Graph *g, ClauseVec *cnf) {
  // Directed arc variables x(u,v) for every undirected edge {u,v}.
  // Constraints:
  // 1) each vertex has exactly one outgoing arc
  // 2) each vertex has exactly one incoming arc
  // 3) forbid immediate 2-cycles (u->v and v->u simultaneously)
  clausevec_init(cnf);
  for (int i = 0; i < g->arcs.size; ++i) {
    int lit = encoder_new_lit(e);
    pairmap_put(&e->lit_map, g->arcs.data[i].u, g->arcs.data[i].v, lit);
    intvec_push(&e->arc_lits, lit);
  }

  for (int u = 1; u <= g->n; ++u) {
    IntVec out_lits, in_lits;
    intvec_init(&out_lits);
    intvec_init(&in_lits);
    intvec_reserve(&out_lits, g->adj[u].size);
    intvec_reserve(&in_lits, g->adj[u].size);
    for (int i = 0; i < g->adj[u].size; ++i) {
      int v = g->adj[u].data[i];
      intvec_push(&out_lits, pairmap_get(&e->lit_map, u, v));
      intvec_push(&in_lits, pairmap_get(&e->lit_map, v, u));
    }

    sinz_at_most_one(e, &out_lits, cnf);
    sinz_at_most_one(e, &in_lits, cnf);
    clausevec_push_copy(cnf, &out_lits);
    clausevec_push_copy(cnf, &in_lits);

    intvec_free(&out_lits);
    intvec_free(&in_lits);
  }
  two_loop_prohibition(g, e, cnf);
}

// get_solution_arcs: extract selected directed arcs from SAT model.
static PairVec get_solution_arcs(void *solver, const Graph *g, const Encoder *e) {
  PairVec out;
  pairvec_init(&out);
  pairvec_reserve(&out, g->arcs.size);
  for (int i = 0; i < g->arcs.size; ++i) {
    int lit = e->arc_lits.data[i];
    if (ipasir_val(solver, lit) > 0) pairvec_push(&out, g->arcs.data[i].u, g->arcs.data[i].v);
  }
  return out;
}

// get_solution_cycles: decompose selected arcs into disjoint cycles.
static CycleVec get_solution_cycles(const PairVec *sol_arcs, int n) {
  // SAT model gives one successor per vertex; follow successors to split into cycles.
  int *next_of = (int *)calloc((size_t)(n + 1), sizeof(int));
  bool *visited = (bool *)calloc((size_t)(n + 1), sizeof(bool));
  if (!next_of || !visited) die("calloc failed");

  for (int i = 0; i < sol_arcs->size; ++i) {
    next_of[sol_arcs->data[i].u] = sol_arcs->data[i].v;
  }

  CycleVec cycles;
  cyclevec_init(&cycles);
  for (int node = 1; node <= n; ++node) {
    if (!next_of[node] || visited[node]) continue;
    IntVec cyc;
    intvec_init(&cyc);
    int cur = node;
    for (;;) {
      visited[cur] = true;
      intvec_push(&cyc, cur);
      int nx = next_of[cur];
      if (!nx) break;
      cur = nx;
      if (visited[cur]) break;
    }
    cyclevec_push_take(&cycles, &cyc);
  }

  free(next_of);
  free(visited);
  return cycles;
}

// print_cycle_lengths_map: print cycle length histogram.
static void print_cycle_lengths_map(const CycleVec *cycles) {
  IntCountMap m;
  icount_init(&m);
  for (int i = 0; i < cycles->size; ++i) icount_inc(&m, cycles->data[i].size);
  icount_sort(&m);
  icount_print(&m);
  icount_free(&m);
}

// cegar_blocking_clauses: create clauses blocking one directed cycle.
static void cegar_blocking_clauses(const IntVec *cycle, const Encoder *e, ClauseVec *out) {
  // Block this directed cycle, and also its reverse direction (except length-2 case).
  IntVec cl;
  int len = cycle->size;
  intvec_init(&cl);
  for (int i = 0; i < len; ++i) {
    int u = cycle->data[i];
    int v = cycle->data[(i + 1) % len];
    intvec_push(&cl, -pairmap_get(&e->lit_map, u, v));
  }
  clausevec_push_take(out, &cl);

  if (len != 2) {
    intvec_init(&cl);
    for (int i = len - 1; i >= 0; --i) {
      int u = cycle->data[i];
      int v = cycle->data[(i + len - 1) % len];
      intvec_push(&cl, -pairmap_get(&e->lit_map, u, v));
    }
    clausevec_push_take(out, &cl);
  }
}

// asp_blocking_clauses_method2: create outgoing-cut clauses for a cycle.
static void asp_blocking_clauses_method2(const IntVec *cycle, const Graph *g, const Encoder *e,
                                         ClauseVec *out) {
  // For each subtour, require at least one arc leaving the cycle (both directions).
  // This is the method used by cegar-fix -b 3.
  bool *in_cycle = (bool *)calloc((size_t)(g->n + 1), sizeof(bool));
  if (!in_cycle) die("calloc failed");
  for (int i = 0; i < cycle->size; ++i) in_cycle[cycle->data[i]] = true;

  IntVec clause1, clause2;
  intvec_init(&clause1);
  intvec_init(&clause2);

  for (int i = 0; i < cycle->size; ++i) {
    int u = cycle->data[i];
    for (int j = 0; j < g->adj[u].size; ++j) {
      int v = g->adj[u].data[j];
      if (in_cycle[v]) continue;
      int lit1 = pairmap_get(&e->lit_map, u, v);
      int lit2 = pairmap_get(&e->lit_map, v, u);
      intvec_push(&clause1, lit1);
      intvec_push(&clause2, lit2);
    }
  }

  clausevec_push_take(out, &clause1);
  if (clause2.size) clausevec_push_take(out, &clause2);
  else intvec_free(&clause2);

  free(in_cycle);
}

// get_blocking_clauses: create and summarize cut clauses for cycles.
static ClauseVec get_blocking_clauses(const CycleVec *sol_cycles, const Graph *g, const Encoder *e,
                                      int block_method) {
  // block_method:
  //   0: direct subtour elimination (directed cycle blocking)
  //   3: ASP-style cut constraints used in original rust run
  ClauseVec clauses;
  clausevec_init(&clauses);
  IntCountMap cut_map;
  icount_init(&cut_map);

  for (int i = 0; i < sol_cycles->size; ++i) {
    ClauseVec sub;
    clausevec_init(&sub);
    if (block_method == 0) {
      cegar_blocking_clauses(&sol_cycles->data[i], e, &sub);
    } else if (block_method == 3) {
      asp_blocking_clauses_method2(&sol_cycles->data[i], g, e, &sub);
      if (sub.size > 0) icount_inc(&cut_map, sub.data[0].size);
    } else {
      die("unsupported block method");
    }
    clausevec_extend_move(&clauses, &sub);
  }

  if (block_method == 3) {
    icount_sort(&cut_map);
    printf("cut arcs by clause length (length:number) = ");
    icount_print(&cut_map);
    printf("\n");
  }
  icount_free(&cut_map);
  return clauses;
}

// cycle_join: stitch two cycles into one candidate merged cycle.
static IntVec cycle_join(const IntVec *cycle1, const IntVec *cycle2, int i, int j, bool reverse) {
  IntVec out;
  intvec_init(&out);
  if (reverse) {
    for (int k = 0; k <= i; ++k) intvec_push(&out, cycle1->data[k]);
    for (int k = j; k >= 0; --k) intvec_push(&out, cycle2->data[k]);
    if (j != cycle2->size - 1) {
      for (int k = cycle2->size - 1; k >= j + 1; --k) intvec_push(&out, cycle2->data[k]);
    }
    if (i != cycle1->size - 1) {
      for (int k = i + 1; k < cycle1->size; ++k) intvec_push(&out, cycle1->data[k]);
    }
  } else {
    for (int k = 0; k <= i; ++k) intvec_push(&out, cycle1->data[k]);
    for (int k = j; k < cycle2->size; ++k) intvec_push(&out, cycle2->data[k]);
    if (j != 0) {
      for (int k = 0; k < j; ++k) intvec_push(&out, cycle2->data[k]);
    }
    if (i != cycle1->size - 1) {
      for (int k = i + 1; k < cycle1->size; ++k) intvec_push(&out, cycle1->data[k]);
    }
  }
  return out;
}

// swap_node: find a feasible 2-opt style merge between two cycles.
static bool swap_node(const IntVec *cycle1, const IntVec *cycle2, const Graph *g, IntVec *new_cycle) {
  for (int i = 0; i < cycle1->size; ++i) {
    int left_head = cycle1->data[i];
    int left_tail = cycle1->data[(i + 1) % cycle1->size];
    IntVec *adjs_head = &g->adj[left_head];
    IntVec *adjs_tail = &g->adj[left_tail];

    for (int j = 0; j < cycle2->size; ++j) {
      int c2j = cycle2->data[j];
      if (!intvec_contains(adjs_head, c2j)) continue;
      int c2next = cycle2->data[(j + 1) % cycle2->size];
      if (intvec_contains(adjs_tail, c2next)) {
        *new_cycle = cycle_join(cycle1, cycle2, i, j, true);
        return true;
      }
      int c2prev = cycle2->data[(j + cycle2->size - 1) % cycle2->size];
      if (intvec_contains(adjs_tail, c2prev)) {
        *new_cycle = cycle_join(cycle1, cycle2, i, j, false);
        return true;
      }
    }
  }
  return false;
}

// get_active_cycles: materialize cycles referenced by active indexes.
static CycleVec get_active_cycles(const CycleVec *cycles, const IntVec *active_idx) {
  CycleVec out;
  cyclevec_init(&out);
  for (int i = 0; i < active_idx->size; ++i) {
    cyclevec_push_copy(&out, &cycles->data[active_idx->data[i]]);
  }
  return out;
}

// intvec_swap_remove: remove one index by swapping with last element.
static void intvec_swap_remove(IntVec *v, int idx) {
  v->data[idx] = v->data[v->size - 1];
  v->size--;
}

// two_opt: repeatedly merge cycles and generate additional block clauses.
static TwoOptResult two_opt(const CycleVec *sol_cycles, const Graph *g, const Encoder *e,
                            int block_method, int opt) {
  // Repeatedly try to merge subtours by 2-opt style edge exchange.
  // Returns:
  //   block_clauses: constraints to add this increment
  //   active_cycles: cycles remaining after merge attempts
  ClauseVec block_clauses;
  clausevec_init(&block_clauses);

  CycleVec cycles;
  cyclevec_init(&cycles);
  cyclevec_extend_copy(&cycles, sol_cycles);

  IntVec active;
  intvec_init(&active);
  for (int i = 0; i < cycles.size; ++i) intvec_push(&active, i);

  bool merged = true;
  bool *cache_vertex = (bool *)calloc((size_t)(cycles.size + 1024), sizeof(bool));
  if (!cache_vertex) die("calloc failed");

  ClauseVec maximum_block;
  clausevec_init(&maximum_block);

  if (opt != 3) {
    ClauseVec init = get_blocking_clauses(sol_cycles, g, e, block_method);
    clausevec_extend_move(&block_clauses, &init);
  }

  while (merged) {
    merged = false;
    ClauseVec new_block;
    clausevec_init(&new_block);
    int merged_i = 0, merged_j = 0;
    IntVec new_cycle;
    intvec_init(&new_cycle);

    for (int i = 0; i < active.size && !merged; ++i) {
      int left = active.data[i];
      if (!cache_vertex[left]) {
        for (int j = i + 1; j < active.size; ++j) {
          int right = active.data[j];
          if (swap_node(&cycles.data[left], &cycles.data[right], g, &new_cycle)) {
            CycleVec one;
            cyclevec_init(&one);
            cyclevec_push_copy(&one, &new_cycle);
            new_block = get_blocking_clauses(&one, g, e, block_method);
            cyclevec_free(&one);
            merged = true;
            merged_i = i;
            merged_j = j;
            break;
          }
        }
        cache_vertex[left] = true;
      }
      if (opt == 4 || opt == 5) {
        merged = false;
        break;
      }
    }

    if (merged) {
      cyclevec_push_take(&cycles, &new_cycle);
      intvec_swap_remove(&active, merged_j);
      intvec_swap_remove(&active, merged_i);
      intvec_push(&active, cycles.size - 1);
      if (cycles.size + 8 > sol_cycles->size + 1024) {
        bool *ncache = (bool *)realloc(cache_vertex, (size_t)(cycles.size + 1024) * sizeof(bool));
        if (!ncache) die("realloc failed");
        int old = sol_cycles->size + 1024;
        memset(ncache + old, 0, (size_t)(cycles.size + 1024 - old) * sizeof(bool));
        cache_vertex = ncache;
      }
    } else {
      intvec_free(&new_cycle);
    }

    if (active.size == 1 || !merged) {
      clausevec_free(&new_block);
      break;
    }

    if (opt == 1 || opt == 4) {
      clausevec_extend_move(&block_clauses, &new_block);
    } else {
      clausevec_free(&maximum_block);
      maximum_block = new_block;
      new_block.data = NULL;
      new_block.size = 0;
      new_block.cap = 0;
    }
    clausevec_free(&new_block);
  }

  if (opt == 2 && maximum_block.size) clausevec_extend_move(&block_clauses, &maximum_block);
  clausevec_free(&maximum_block);

  CycleVec active_cycles = get_active_cycles(&cycles, &active);
  if (opt == 3) {
    ClauseVec extra = get_blocking_clauses(&active_cycles, g, e, block_method);
    clausevec_extend_move(&block_clauses, &extra);
  }

  printf("number of merge operations = %d\n", cycles.size - sol_cycles->size);
  printf("number of resulting cycles = %d\n", active.size);
  printf("cycle lengths after merge (length:number) = ");
  print_cycle_lengths_map(&active_cycles);
  printf("\n");

  free(cache_vertex);
  cyclevec_free(&cycles);
  intvec_free(&active);

  TwoOptResult ret;
  ret.block_clauses = block_clauses;
  ret.active_cycles = active_cycles;
  return ret;
}

// print_solution_cycle: print one cycle as vertex sequence (gb label if available).
static void print_solution_cycle(const IntVec *cycle, const Graph *g) {
  for (int i = 0; i < cycle->size; ++i) {
    if (i) printf(" ");
    int v = cycle->data[i];
    if (g->has_labels && v >= 1 && v <= g->n && g->labels && g->labels[v] && g->labels[v][0] != '\0') {
      printf("%s", g->labels[v]);
    } else {
      printf("%d", v);
    }
  }
  printf("\n");
}

// cegar: iterative solve-cut loop until SAT Hamiltonian cycle or UNSAT.
static SolveResult cegar(Encoder *encoder, void *solver, const Graph *g, int64_t previous_ns) {
  // Fixed configuration compatible with:
  //   cegar-fix -e 1 -b 3 -t 3 -l 1
  const int block_method = 3;
  const int opt = 3;

  int count = 0;
  int clause_count = 0;
  int64_t prev = previous_ns;

  while (1) {
    // 1) Solve SAT
    // 2) If one cycle => SAT
    // 3) Otherwise derive cuts from subtours / merged subtours and iterate
    int res = ipasir_solve(solver);
    int64_t now = now_ns();
    int64_t sat_solving_time = now - prev;
    char buf_sat[64], buf_add[64], buf_inc[64];
    format_duration(buf_sat, sizeof(buf_sat), sat_solving_time);

    printf("\n");
    printf("CEGAR iteration...\n");
    printf("cegar iteration = %d\n", count);
    printf("sat solving time = %s\n", buf_sat);

    if (res == 10) {
      PairVec sol_arcs = get_solution_arcs(solver, g, encoder);
      CycleVec sol_cycles = get_solution_cycles(&sol_arcs, g->n);
      pairvec_free(&sol_arcs);

      if (sol_cycles.size == 1) {
        printf("\nsolution: \n");
        print_solution_cycle(&sol_cycles.data[0], g);
        printf("\n");
        printf("s SATISFIABLE\n");
        cyclevec_free(&sol_cycles);
        SolveResult r = {count, clause_count};
        return r;
      }

      printf("number of subcycles found = %d\n", sol_cycles.size);
      printf("cycle lengths before merge (length:number) = ");
      print_cycle_lengths_map(&sol_cycles);
      printf("\n");

      TwoOptResult topt = two_opt(&sol_cycles, g, encoder, block_method, opt);
      cyclevec_free(&sol_cycles);

      if (topt.active_cycles.size == 1) {
        int64_t now2 = now_ns();
        int64_t inc_time = now2 - prev;
        int64_t add_time = inc_time - sat_solving_time;
        format_duration(buf_add, sizeof(buf_add), add_time);
        format_duration(buf_inc, sizeof(buf_inc), inc_time);
        printf("number of added block clauses (this increment) = 0\n");
        printf("number of added block clauses (accumulated) = %d\n", clause_count);
        printf("add block clauses time = %s\n", buf_add);
        printf("increment time = %s\n", buf_inc);
        printf("\n");
        printf("hamiltonian cycle found by 2-opt\n");
        printf("solution: \n");
        print_solution_cycle(&topt.active_cycles.data[0], g);
        printf("\n");
        printf("s SATISFIABLE\n");
        clausevec_free(&topt.block_clauses);
        cyclevec_free(&topt.active_cycles);
        SolveResult r = {count, clause_count};
        return r;
      }

      count++;
      const int added_clause_count = topt.block_clauses.size;
      clause_count += added_clause_count;
      for (int i = 0; i < topt.block_clauses.size; ++i) {
        add_clause_solver(solver, &topt.block_clauses.data[i]);
      }
      clausevec_free(&topt.block_clauses);
      cyclevec_free(&topt.active_cycles);

      int64_t now2 = now_ns();
      int64_t inc_time = now2 - prev;
      int64_t add_time = inc_time - sat_solving_time;
      format_duration(buf_add, sizeof(buf_add), add_time);
      format_duration(buf_inc, sizeof(buf_inc), inc_time);
      printf("number of added block clauses (this increment) = %d\n", added_clause_count);
      printf("number of added block clauses (accumulated) = %d\n", clause_count);
      printf("add block clauses time = %s\n", buf_add);
      printf("increment time = %s\n", buf_inc);
      prev = now2;
    } else {
      printf("s UNSATISFIABLE\n");
      SolveResult r = {count, clause_count};
      return r;
    }
  }
}

// solve_hamilton: run encode + IPASIR solve + CEGAR and print stats.
static SolveResult solve_hamilton(const Graph *g) {
  int64_t enc_begin = now_ns();
  Encoder encoder;
  encoder_init(&encoder, g->arcs.size);
  ClauseVec cnf;
  encode_graph(&encoder, g, &cnf);
  int64_t enc_end = now_ns();
  char buf_enc[64];
  format_duration(buf_enc, sizeof(buf_enc), enc_end - enc_begin);
  printf("encoding time = %s\n", buf_enc);
  printf("\n");
  printf("encoding clauses number = %d\n", cnf.size);

  void *solver = ipasir_init();
  for (int i = 0; i < cnf.size; ++i) add_clause_solver(solver, &cnf.data[i]);
  clausevec_free(&cnf);

  SolveResult r = cegar(&encoder, solver, g, now_ns());
  ipasir_release(solver);
  encoder_free(&encoder);
  printf("overall cegar iterations = %d\n", r.increments);
  printf("overall number of added block clauses = %d\n", r.added_block_clauses);
  return r;
}

// main: entrypoint that loads graph, solves, and prints timing.
int main(int argc, char **argv) {
  const char *input = NULL;
  for (int i = 1; i < argc; ++i) {
    if ((!strcmp(argv[i], "-i") || !strcmp(argv[i], "--input")) && i + 1 < argc) {
      input = argv[++i];
    } else if (argv[i][0] != '-') {
      input = argv[i];
    }
  }
  if (!input) {
    fprintf(stderr, "usage: %s <benchmark.col|benchmark.gb>\n", argv[0]);
    return 2;
  }

  int64_t start = now_ns();
  printf("solve %s\n", input);
  Graph g = input_to_graph(input);
  int64_t after_input = now_ns();
  char buf_input[64], buf_solv[64], buf_all[64];
  format_duration(buf_input, sizeof(buf_input), after_input - start);
  printf("file input time = %s\n", buf_input);

  (void)solve_hamilton(&g);
  int64_t end = now_ns();
  format_duration(buf_solv, sizeof(buf_solv), end - after_input);
  format_duration(buf_all, sizeof(buf_all), end - start);
  printf("solving time = %s\n", buf_solv);
  printf("overall time = %s\n", buf_all);
  graph_free(&g);
  return 0;
}
