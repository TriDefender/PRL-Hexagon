/*
 * proof.c — PlainProof construction and wire encoding.
 *
 * A share opens the tile's 8 rows of A and 16 rows of Bᵀ with pearl_blake3
 * multileaf Merkle proofs keyed by the job key, and serializes the PlainProof
 * exactly like the official FFI:
 *
 *   bincode fixint little-endian of
 *     PlainProof { m: u64, n: u64, k: u64, noise_rank: u64,
 *                  a: MatrixMerkleProof, bt: MatrixMerkleProof, moe: Option }
 *     MatrixMerkleProof { proof: MerkleProof, row_indices: Vec<u64> }
 *     MerkleProof { leaf_data: Vec<Vec<u8>>,     // u64 count, per chunk u64(1024) + bytes
 *                   leaf_indices: Vec<u64>, total_leaves: u64,
 *                   root: [u8;32], siblings: Vec<[u8;32]> }
 *     moe = 0x00 (None)
 *   then standard base64 (with padding).
 *
 * The byte layout was confirmed against the official Rust serializer
 * (zk-pow PlainProof::to_base64 at the pinned revision).
 */
#include "pearl.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* caps sized for the supported shapes (m,n multiples of 64, 2048 <= k <= 4096):
 * the Bᵀ proof opens 16 rows of k bytes -> at most 16*(k/1024) <= 64 chunks;
 * siblings stay well below 128 at those sizes. Enforced in-loop, not after. */
#define MAX_PROOF_LEAVES 64
#define MAX_PROOF_SIBS 128

/* ---- growable byte buffer ---------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} buf;

static void buf_put(buf *b, const void *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n) nc *= 2;
        b->p = realloc(b->p, nc);
        if (!b->p) abort();
        b->cap = nc;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void buf_u64(buf *b, uint64_t v) { buf_put(b, &v, 8); }

/* ---- base64 (standard, padded) ------------------------------------------ */

static const char B64T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const uint8_t *in, size_t len, size_t *out_len) {
    size_t n = ((len + 2) / 3) * 4;
    char *out = malloc(n + 1);
    if (!out) abort();
    size_t i, o = 0;
    for (i = 0; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t)(in[i] << 16 | in[i + 1] << 8 | in[i + 2]);
        out[o++] = B64T[(v >> 18) & 63];
        out[o++] = B64T[(v >> 12) & 63];
        out[o++] = B64T[(v >> 6) & 63];
        out[o++] = B64T[v & 63];
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64T[(v >> 18) & 63];
        out[o++] = B64T[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = (uint32_t)(in[i] << 16 | in[i + 1] << 8);
        out[o++] = B64T[(v >> 18) & 63];
        out[o++] = B64T[(v >> 12) & 63];
        out[o++] = B64T[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

/* ---- one matrix's multileaf proof --------------------------------------- */

/* collect the sorted unique chunk indices covering `rows` of a k-wide matrix.
 * Returns 0 when they exceed `cap` (the caller refuses the share). */
static size_t leaf_indices_for_rows(const uint32_t *rows, size_t nrows, uint32_t k,
                                    uint64_t *leaves, size_t cap) {
    size_t n = 0;
    for (size_t r = 0; r < nrows; r++) {
        uint64_t first = (uint64_t)rows[r] * k / 1024;
        uint64_t last = ((uint64_t)rows[r] * k + k - 1) / 1024;
        for (uint64_t c = first; c <= last; c++) {
            /* rows are ascending: dedupe against the last entry only */
            if (n && leaves[n - 1] == c) continue;
            if (n >= cap) return 0;
            leaves[n++] = c;
        }
    }
    return n;
}

static bool serialize_matrix_proof(buf *b, const uint8_t *matrix, uint32_t rows_total, uint32_t k,
                                   const uint8_t *key32, const uint32_t *rows, size_t nrows) {
    uint64_t leaves[MAX_PROOF_LEAVES];
    size_t nleaves = leaf_indices_for_rows(rows, nrows, k, leaves, MAX_PROOF_LEAVES);
    if (nleaves == 0) return false;

    uint64_t total_bytes = (uint64_t)rows_total * k;
    uint64_t total_leaves = (total_bytes + 1023) / 1024;

    /* build all tree layers (chunk CVs + parent layers) */
    uint8_t *cur = malloc(total_leaves * 32);
    uint8_t *next = malloc(((total_leaves + 1) / 2 + 1) * 32);
    if (!cur || !next) abort();

    for (uint64_t i = 0; i < total_leaves; i++)
        pr_chunk_cv(key32, matrix + i * 1024, i, cur + i * 32);

    enum { MAXL = 32 };
    uint8_t *layer_ptr[MAXL];
    uint8_t *alloc_blocks[MAXL];
    int nlayers = 0;
    layer_ptr[0] = cur;
    alloc_blocks[0] = cur;
    nlayers = 1;

    uint64_t count = total_leaves;
    while (count > 2 && nlayers < MAXL) {
        uint64_t outn = (count + 1) / 2;
        for (uint64_t i = 0; i < outn; i++) {
            if (2 * i + 1 < count)
                pr_parent_cv(key32, cur + 2 * i * 32, cur + (2 * i + 1) * 32, next + i * 32);
            else
                memcpy(next + i * 32, cur + 2 * i * 32, 32);
        }
        cur = next;
        count = outn;
        layer_ptr[nlayers] = cur;
        alloc_blocks[nlayers] = cur;
        nlayers++;
        next = malloc(((count + 1) / 2 + 1) * 32);
        if (!next) abort();
    }

    /* collect siblings level by level, exactly like get_multileaf_proof */
    uint64_t cur_set[2 * MAX_PROOF_LEAVES];
    size_t cur_n = nleaves;
    memcpy(cur_set, leaves, nleaves * 8);

    uint8_t sib_buf[MAX_PROOF_SIBS * 32];
    size_t nsib = 0;
    uint64_t level_len = total_leaves;
    int level = 0;
    bool failed = nlayers >= MAXL;
    while (!failed && level_len > 1 && cur_n > 0) {
        if (level >= nlayers) { failed = true; break; }
        for (size_t x = 0; x < cur_n && !failed; x++) {
            uint64_t i = cur_set[x];
            if (i & 1) {
                int has = 0;
                for (size_t y = 0; y < cur_n; y++)
                    if (cur_set[y] == i - 1) { has = 1; break; }
                if (!has) {
                    if (nsib >= MAX_PROOF_SIBS) { failed = true; break; }
                    memcpy(sib_buf + nsib * 32, layer_ptr[level] + (i - 1) * 32, 32);
                    nsib++;
                }
            } else {
                int has = 0;
                for (size_t y = 0; y < cur_n; y++)
                    if (cur_set[y] == i + 1) { has = 1; break; }
                if (!has && i + 1 < level_len) {
                    if (nsib >= MAX_PROOF_SIBS) { failed = true; break; }
                    memcpy(sib_buf + nsib * 32, layer_ptr[level] + (i + 1) * 32, 32);
                    nsib++;
                }
            }
        }
        for (size_t x = 0; x < cur_n; x++) cur_set[x] /= 2;
        size_t w = 0;
        for (size_t x = 0; x < cur_n; x++)
            if (w == 0 || cur_set[w - 1] != cur_set[x]) cur_set[w++] = cur_set[x];
        cur_n = w;
        level_len = (level_len + 1) / 2;
        level++;
    }

    uint8_t root[32];
    if (!failed) {
        if (total_leaves >= 2)
            pr_root_cv(key32, layer_ptr[nlayers - 1], layer_ptr[nlayers - 1] + 32, root);
        else
            memcpy(root, layer_ptr[0], 32); /* single-chunk tree (unreachable for legal shapes) */
    }

    if (failed) {
        for (int i = 0; i < nlayers; i++) free(alloc_blocks[i]);
        return false;
    }

    /* ---- serialize ---- */
    buf_u64(b, nleaves);
    for (size_t i = 0; i < nleaves; i++) {
        buf_u64(b, 1024);
        buf_put(b, matrix + leaves[i] * 1024, 1024);
    }
    buf_u64(b, nleaves);
    for (size_t i = 0; i < nleaves; i++) buf_u64(b, leaves[i]);
    buf_u64(b, total_leaves);
    buf_put(b, root, 32);
    buf_u64(b, nsib);
    buf_put(b, sib_buf, nsib * 32);
    /* row_indices */
    buf_u64(b, nrows);
    for (size_t i = 0; i < nrows; i++) buf_u64(b, rows[i]);

    for (int i = 0; i < nlayers; i++) free(alloc_blocks[i]);
    return true;
}

bool pr_build_share(const pr_attempt_mats *am, const pr_commitment *c, uint32_t t_rows, uint32_t t_cols,
                    pr_share *out) {
    if (!pr_tile_base_valid(t_rows, t_cols, am->m, am->n)) return false;

    uint32_t a_rows[PEARL_TILE_ROWS], bt_rows[PEARL_TILE_COLS];
    for (unsigned u = 0; u < PEARL_TILE_ROWS; u++) a_rows[u] = t_rows + pr_row_off(u);
    for (unsigned v = 0; v < PEARL_TILE_COLS; v++) bt_rows[v] = t_cols + pr_col_off(v);

    buf b = {0};
    buf_u64(&b, am->m);
    buf_u64(&b, am->n);
    buf_u64(&b, am->k);
    buf_u64(&b, PEARL_RANK);
    if (!serialize_matrix_proof(&b, (const uint8_t *)am->a, am->m, am->k, c->job_key.b, a_rows,
                                PEARL_TILE_ROWS) ||
        !serialize_matrix_proof(&b, (const uint8_t *)am->bt, am->n, am->k, c->job_key.b, bt_rows,
                                PEARL_TILE_COLS)) {
        free(b.p);
        return false;
    }
    uint8_t none = 0x00;
    buf_put(&b, &none, 1); /* moe: Option::None */

    out->data = b.p;
    out->len = b.len;
    out->b64 = b64_encode(b.p, b.len, &out->b64_len);
    return true;
}

void pr_share_free(pr_share *s) {
    if (!s) return;
    free(s->data);
    free(s->b64);
    memset(s, 0, sizeof(*s));
}
