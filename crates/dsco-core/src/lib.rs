//! dsco-core — typed Rust core for DSCO.
//!
//! A safe, idiomatic wrapper over the C Context Fabric. Above this line there
//! are no raw pointers, no fixed-size buffers, and no `int`-typed enums: a key
//! is a `CtxKey`, a kind is a `CtxKind`, a slice is a `Slice`, content is a
//! `Vec<u8>`. The unsafe ABI surface is confined to [`ffi`].

mod ffi;

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_long, c_void};

/// The kind of thing a [`CtxKey`] addresses.
#[repr(i32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CtxKind {
    Blob = 0,
    File = 1,
    Msg = 2,
    Tool = 3,
    Mem = 4,
    Plan = 5,
    Run = 6,
    Scope = 7,
}

impl CtxKind {
    fn from_raw(v: c_int) -> CtxKind {
        match v {
            1 => CtxKind::File,
            2 => CtxKind::Msg,
            3 => CtxKind::Tool,
            4 => CtxKind::Mem,
            5 => CtxKind::Plan,
            6 => CtxKind::Run,
            7 => CtxKind::Scope,
            _ => CtxKind::Blob,
        }
    }
}

/// A span of a stored object — the type system, not a `#slice` string, decides
/// whether it's bytes or lines.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Slice {
    Whole,
    /// `[start, end)`; `end < 0` means "to end".
    Bytes { start: i64, end: i64 },
    /// 1-based inclusive `[start, end]`; `end < 0` means "to end".
    Lines { start: i64, end: i64 },
}

/// The universal handle: `ck:kind:id[#slice]`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CtxKey {
    pub kind: CtxKind,
    pub id: String,
    pub slice: Slice,
}

impl CtxKey {
    fn to_raw(&self) -> ffi::ctxkey_t {
        let (sk, ss, se) = match self.slice {
            Slice::Whole => (ffi::CTX_SLICE_NONE, 0i64, 0i64),
            Slice::Bytes { start, end } => (ffi::CTX_SLICE_BYTES, start, end),
            Slice::Lines { start, end } => (ffi::CTX_SLICE_LINES, start, end),
        };
        let mut id = [0 as c_char; ffi::CTX_ID_LEN];
        let bytes = self.id.as_bytes();
        let n = bytes.len().min(ffi::CTX_ID_LEN - 1);
        for i in 0..n {
            id[i] = bytes[i] as c_char;
        }
        ffi::ctxkey_t {
            kind: self.kind as c_int,
            id,
            slice_kind: sk,
            slice_start: ss as c_long,
            slice_end: se as c_long,
        }
    }

    fn from_raw(k: &ffi::ctxkey_t) -> CtxKey {
        let id = unsafe { CStr::from_ptr(k.id.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let slice = match k.slice_kind {
            x if x == ffi::CTX_SLICE_BYTES => Slice::Bytes {
                start: k.slice_start as i64,
                end: k.slice_end as i64,
            },
            x if x == ffi::CTX_SLICE_LINES => Slice::Lines {
                start: k.slice_start as i64,
                end: k.slice_end as i64,
            },
            _ => Slice::Whole,
        };
        CtxKey {
            kind: CtxKind::from_raw(k.kind),
            id,
            slice,
        }
    }

    /// Parse a canonical `ck:kind:id[#slice]` string via the C parser.
    pub fn parse(s: &str) -> Option<CtxKey> {
        let cs = CString::new(s).ok()?;
        let mut raw = zeroed_key();
        let ok = unsafe { ffi::ctxkey_parse(cs.as_ptr(), &mut raw) };
        ok.then(|| CtxKey::from_raw(&raw))
    }
}

impl std::fmt::Display for CtxKey {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let raw = self.to_raw();
        let mut buf = [0 as c_char; ffi::CTX_KEY_STR_MAX];
        let n = unsafe { ffi::ctxkey_format(&raw, buf.as_mut_ptr(), buf.len()) };
        if n < 0 {
            return Err(std::fmt::Error);
        }
        let s = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy();
        f.write_str(&s)
    }
}

/// A search result.
#[derive(Debug, Clone)]
pub struct Hit {
    pub key: CtxKey,
    pub score: f32,
    pub size: usize,
    pub source: String,
    pub preview: String,
}

/// A handle to the process-wide Context Fabric broker.
///
/// This wraps the C singleton returned by `ctx_broker_default()`, which the C
/// side owns; dropping a `Broker` does not close it.
pub struct Broker {
    raw: *mut ffi::ctx_broker_t,
    owned: bool,
}

impl Broker {
    /// Open (or attach to) the default broker at `$DSCO_CONTEXT_DB`.
    pub fn default() -> Option<Broker> {
        let raw = unsafe { ffi::ctx_broker_default() };
        (!raw.is_null()).then_some(Broker { raw, owned: false })
    }

    /// Open an isolated broker at `db_path` (owned; closed on drop).
    pub fn open(db_path: &str) -> Option<Broker> {
        let path = CString::new(db_path).ok()?;
        let raw = unsafe { ffi::ctx_broker_open(path.as_ptr()) };
        (!raw.is_null()).then_some(Broker { raw, owned: true })
    }

    /// Store bytes; returns the content-addressed key and whether it deduped.
    pub fn put(&self, bytes: &[u8], kind: CtxKind, source: Option<&str>) -> Option<(CtxKey, bool)> {
        let src = source.and_then(|s| CString::new(s).ok());
        let opts = ffi::ctx_put_opts_t {
            kind: kind as c_int,
            source: src.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            tags: std::ptr::null(),
            importance: 0.5,
            embed: -1,
        };
        let mut key = zeroed_key();
        let mut deduped = false;
        let ok = unsafe {
            ffi::ctx_put(
                self.raw,
                bytes.as_ptr() as *const c_void,
                bytes.len(),
                &opts,
                &mut key,
                &mut deduped,
            )
        };
        ok.then(|| (CtxKey::from_raw(&key), deduped))
    }

    /// Fetch (a slice of) content by key.
    pub fn get(&self, key: &CtxKey) -> Option<Vec<u8>> {
        let raw = key.to_raw();
        let mut len: usize = 0;
        let p = unsafe { ffi::ctx_get(self.raw, &raw, &mut len) };
        if p.is_null() {
            return None;
        }
        let out = unsafe { std::slice::from_raw_parts(p as *const u8, len) }.to_vec();
        unsafe { ffi::free(p as *mut c_void) };
        Some(out)
    }

    /// Semantic + lexical search; `kind` filters, `None` matches any.
    pub fn search(&self, query: &str, kind: Option<CtxKind>, max: usize) -> Vec<Hit> {
        let q = match CString::new(query) {
            Ok(q) => q,
            Err(_) => return Vec::new(),
        };
        let cap = max.min(32).max(1);
        let mut buf: Vec<ffi::ctx_search_hit_t> = Vec::with_capacity(cap);
        let n = unsafe {
            ffi::ctx_search(
                self.raw,
                q.as_ptr(),
                kind.map_or(-1, |k| k as c_int),
                cap as c_int,
                buf.as_mut_ptr(),
            )
        };
        let mut hits = Vec::new();
        if n > 0 {
            unsafe { buf.set_len(n as usize) };
            for h in &buf {
                hits.push(Hit {
                    key: CtxKey::from_raw(&h.key),
                    score: h.score,
                    size: h.size,
                    source: cstr_field(&h.source),
                    preview: cstr_field(&h.preview),
                });
            }
        }
        hits
    }

    /// Pin a key so it survives compaction and assembles into the window first.
    pub fn pin(&self, key: &CtxKey) -> bool {
        let raw = key.to_raw();
        unsafe { ffi::ctx_pin(self.raw, &raw) }
    }

    /// Remove a pin.
    pub fn unpin(&self, key: &CtxKey) -> bool {
        let raw = key.to_raw();
        unsafe { ffi::ctx_unpin(self.raw, &raw) }
    }

    /// Bundle keys into a delegation scope (`ck:scope:...`).
    pub fn scope_create(&self, keys: &[CtxKey]) -> Option<CtxKey> {
        if keys.is_empty() {
            return None;
        }
        let raws: Vec<ffi::ctxkey_t> = keys.iter().map(CtxKey::to_raw).collect();
        let mut out = zeroed_key();
        let ok = unsafe {
            ffi::ctx_scope_create(self.raw, raws.as_ptr(), raws.len() as c_int, &mut out)
        };
        ok.then(|| CtxKey::from_raw(&out))
    }

    /// Expand a scope into its member keys.
    pub fn scope_resolve(&self, scope: &CtxKey) -> Vec<CtxKey> {
        let raw = scope.to_raw();
        const MAX: usize = 64;
        let mut buf: Vec<ffi::ctxkey_t> = Vec::with_capacity(MAX);
        let n = unsafe { ffi::ctx_scope_resolve(self.raw, &raw, buf.as_mut_ptr(), MAX as c_int) };
        if n <= 0 {
            return Vec::new();
        }
        unsafe { buf.set_len(n as usize) };
        buf.iter().map(CtxKey::from_raw).collect()
    }

    /// Fabric statistics.
    pub fn stat(&self) -> Stat {
        let s = unsafe { ffi::ctx_broker_stat(self.raw) };
        Stat {
            blobs: s.blobs as usize,
            bytes: s.bytes,
            vectors: s.vectors as usize,
            pinned: s.pinned as usize,
            scopes: s.scopes as usize,
            embeddings_available: s.embeddings_available,
        }
    }
}

impl Drop for Broker {
    fn drop(&mut self) {
        // Only close brokers we opened; the default broker is a C-owned singleton.
        if self.owned && !self.raw.is_null() {
            unsafe { ffi::ctx_broker_close(self.raw) };
        }
    }
}

/// Snapshot of fabric contents.
#[derive(Debug, Clone)]
pub struct Stat {
    pub blobs: usize,
    pub bytes: i64,
    pub vectors: usize,
    pub pinned: usize,
    pub scopes: usize,
    pub embeddings_available: bool,
}

fn zeroed_key() -> ffi::ctxkey_t {
    ffi::ctxkey_t {
        kind: 0,
        id: [0 as c_char; ffi::CTX_ID_LEN],
        slice_kind: 0,
        slice_start: 0,
        slice_end: 0,
    }
}

fn cstr_field(field: &[c_char]) -> String {
    unsafe { CStr::from_ptr(field.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_roundtrips_through_display_and_parse() {
        let k = CtxKey {
            kind: CtxKind::Blob,
            id: "9f3ac1".into(),
            slice: Slice::Bytes { start: 0, end: 2048 },
        };
        let s = k.to_string();
        assert_eq!(s, "ck:blob:9f3ac1#b:0-2048");
        let back = CtxKey::parse(&s).expect("parse");
        assert_eq!(back, k);
    }

    #[test]
    fn line_slice_formats() {
        let k = CtxKey {
            kind: CtxKind::File,
            id: "abc".into(),
            slice: Slice::Lines { start: 120, end: 160 },
        };
        assert_eq!(k.to_string(), "ck:file:abc#l:120-160");
    }
}
