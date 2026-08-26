//! Raw FFI surface for the C Context Fabric (include/context_fabric.h).
//!
//! This is the ONLY place ABI-matching magic sizes are allowed to appear — they
//! are named and pinned to the C header, and never leak above `lib.rs`. Keep the
//! layout in exact sync with context_fabric.h; a mismatch is UB.

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_long, c_void};

/// ABI: must equal `CTX_ID_LEN` in context_fabric.h.
pub const CTX_ID_LEN: usize = 128;
/// ABI: must equal `CTX_KEY_STR_MAX` in context_fabric.h.
pub const CTX_KEY_STR_MAX: usize = 256;
/// ABI: must equal `ctx_search_hit_t.source` / `.preview` array sizes.
pub const HIT_SOURCE_LEN: usize = 128;
pub const HIT_PREVIEW_LEN: usize = 160;

// C enums have `int` ABI. Values mirror the header ordering.
pub const CTX_SLICE_NONE: c_int = 0;
pub const CTX_SLICE_BYTES: c_int = 1;
pub const CTX_SLICE_LINES: c_int = 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ctxkey_t {
    pub kind: c_int,
    pub id: [c_char; CTX_ID_LEN],
    pub slice_kind: c_int,
    pub slice_start: c_long,
    pub slice_end: c_long,
}

#[repr(C)]
pub struct ctx_put_opts_t {
    pub kind: c_int,
    pub source: *const c_char,
    pub tags: *const c_char,
    pub importance: f64,
    pub embed: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ctx_search_hit_t {
    pub key: ctxkey_t,
    pub score: f32,
    pub size: usize, // size_t
    pub source: [c_char; HIT_SOURCE_LEN],
    pub preview: [c_char; HIT_PREVIEW_LEN],
}

#[repr(C)]
pub struct ctx_stat_t {
    pub blobs: c_int,
    pub bytes: i64,
    pub vectors: c_int,
    pub pinned: c_int,
    pub scopes: c_int,
    pub embeddings_available: bool,
}

/// Opaque broker handle.
#[repr(C)]
pub struct ctx_broker_t {
    _private: [u8; 0],
}

extern "C" {
    pub fn ctx_broker_default() -> *mut ctx_broker_t;
    pub fn ctx_broker_open(db_path: *const c_char) -> *mut ctx_broker_t;
    pub fn ctx_broker_close(b: *mut ctx_broker_t);

    pub fn ctx_put(
        b: *mut ctx_broker_t,
        bytes: *const c_void,
        n: usize,
        opts: *const ctx_put_opts_t,
        out_key: *mut ctxkey_t,
        out_deduped: *mut bool,
    ) -> bool;

    pub fn ctx_get(b: *mut ctx_broker_t, key: *const ctxkey_t, out_len: *mut usize) -> *mut c_char;

    pub fn ctx_search(
        b: *mut ctx_broker_t,
        query: *const c_char,
        kind_filter: c_int,
        max: c_int,
        out: *mut ctx_search_hit_t,
    ) -> c_int;

    pub fn ctx_pin(b: *mut ctx_broker_t, key: *const ctxkey_t) -> bool;
    pub fn ctx_unpin(b: *mut ctx_broker_t, key: *const ctxkey_t) -> bool;

    pub fn ctx_scope_create(
        b: *mut ctx_broker_t,
        keys: *const ctxkey_t,
        n: c_int,
        out_scope: *mut ctxkey_t,
    ) -> bool;
    pub fn ctx_scope_resolve(
        b: *mut ctx_broker_t,
        scope: *const ctxkey_t,
        out: *mut ctxkey_t,
        max: c_int,
    ) -> c_int;

    pub fn ctx_broker_stat(b: *mut ctx_broker_t) -> ctx_stat_t;

    pub fn ctxkey_format(key: *const ctxkey_t, out: *mut c_char, cap: usize) -> c_int;
    pub fn ctxkey_parse(s: *const c_char, out: *mut ctxkey_t) -> bool;

    /// libc free — the fabric returns malloc'd buffers from ctx_get.
    pub fn free(ptr: *mut c_void);
}
