#include "dsco_crypto.h"
#include "crypto.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef HAVE_LIBSODIUM
#include <sodium.h>
#endif
#ifdef HAVE_LIBOQS
#include <oqs/oqs.h>
#if defined(OQS_ENABLE_SIG_ml_dsa_65) && defined(OQS_ENABLE_KEM_ml_kem_768)
#define DSCO_PQ_ENABLED 1
_Static_assert(OQS_SIG_ml_dsa_65_length_public_key == DSCO_MLDSA_PK_BYTES, "ML-DSA pk");
_Static_assert(OQS_SIG_ml_dsa_65_length_secret_key == DSCO_MLDSA_SK_BYTES, "ML-DSA sk");
_Static_assert(OQS_SIG_ml_dsa_65_length_signature == DSCO_MLDSA_SIG_BYTES, "ML-DSA sig");
_Static_assert(OQS_KEM_ml_kem_768_length_secret_key == DSCO_MLKEM_SK_BYTES, "ML-KEM sk");
_Static_assert(OQS_KEM_ml_kem_768_length_public_key == DSCO_MLKEM_PK_BYTES, "ML-KEM pk");
_Static_assert(OQS_KEM_ml_kem_768_length_ciphertext == DSCO_MLKEM_CT_BYTES, "ML-KEM ct");
#endif
#endif
#ifdef HAVE_BLAKE3
#include <blake3.h>
#endif

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static int init_status = -1;
static bool aegis_selected;
static void primitives_init(void) {
#ifdef HAVE_LIBSODIUM
    if (sodium_init() < 0) return;
#if defined(HAVE_LIBOQS) && !defined(DSCO_PQ_ENABLED)
    return; /* A requested PQ build must never silently become classical. */
#endif
#ifdef DSCO_PQ_ENABLED
    OQS_init();
#endif
#if defined(crypto_aead_aegis256_ABYTES) && !defined(DSCO_CRYPTO_FORCE_XCHACHA)
    aegis_selected = (sodium_runtime_has_armcrypto() || sodium_runtime_has_aesni()) != 0;
#endif
    init_status = 0;
#endif
}
int dsco_crypto_init(void) { return pthread_once(&init_once, primitives_init) == 0 ? init_status : -1; }
bool dsco_crypto_has_pq(void) {
#ifdef DSCO_PQ_ENABLED
    return dsco_crypto_init() == 0;
#else
    return false;
#endif
}
bool dsco_crypto_aead_is_aegis(void) { return dsco_crypto_init() == 0 && aegis_selected; }
const char *dsco_crypto_suite_id(void) {
    if (dsco_crypto_init() != 0) return "unavailable: configured crypto backend not usable";
#ifdef HAVE_BLAKE3
#define DSCO_HASH_SUITE "BLAKE3"
#else
#define DSCO_HASH_SUITE "SHA256/HMAC-SHA256"
#endif
#ifdef DSCO_PQ_ENABLED
#define DSCO_ID_SUITE "Ed25519+ML-DSA-65 + X25519+ML-KEM-768"
#else
#define DSCO_ID_SUITE "Ed25519 + X25519 (classical)"
#endif
    return aegis_selected ? "AEGIS-256 + HKDF-SHA512 + " DSCO_HASH_SUITE " + " DSCO_ID_SUITE
                          : "XChaCha20-Poly1305 + HKDF-SHA512 + " DSCO_HASH_SUITE " + " DSCO_ID_SUITE;
}
void dsco_memzero(void *p, size_t n) {
    if (!p || !n) return;
#ifdef HAVE_LIBSODIUM
    sodium_memzero(p,n);
#else
    volatile unsigned char *q=p; while(n--) *q++=0;
#endif
}
int dsco_memcmp_ct(const void *a, const void *b, size_t n) {
    if (!n) return 0;
    if (!a || !b) return -1;
#ifdef HAVE_LIBSODIUM
    return sodium_memcmp(a,b,n);
#else
    const volatile unsigned char *x=a,*y=b; unsigned char d=0;
    for(size_t i=0;i<n;i++) d|=x[i]^y[i];
    return d ? -1 : 0;
#endif
}
int dsco_mlock(void *p,size_t n) {
    if (!p || !n) return -1;
#ifdef HAVE_LIBSODIUM
    return sodium_mlock(p,n);
#else
    return mlock(p,n);
#endif
}
int dsco_munlock(void *p,size_t n) {
    if (!p || !n) return -1;
#ifdef HAVE_LIBSODIUM
    return sodium_munlock(p,n);
#else
    dsco_memzero(p,n); return munlock(p,n);
#endif
}
void dsco_random_bytes(void *buf,size_t len) {
    if (!len) return;
    if (!buf) abort();
#ifdef HAVE_LIBSODIUM
    if (dsco_crypto_init()!=0) abort();
    randombytes_buf(buf,len);
#else
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC); if(fd<0) abort();
    unsigned char *p=buf;
    while(len) { ssize_t n=read(fd,p,len>1048576?1048576:len); if(n<0&&errno==EINTR) continue;
        if(n<=0) { close(fd); abort(); } p+=n;len-=(size_t)n; }
    close(fd);
#endif
}
static bool valid_bytes(const void *p,size_t n) { return p || !n; }
int dsco_hkdf_sha512(const uint8_t *ikm,size_t il,const uint8_t *salt,size_t sl,
                     const uint8_t *info,size_t nl,uint8_t *out,size_t ol) {
    if(!valid_bytes(ikm,il)||!valid_bytes(salt,sl)||!valid_bytes(info,nl)||!valid_bytes(out,ol)||ol>255u*64u) return -1;
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0) return -1;
    uint8_t prk[64],t[64],zeros[64]={0};crypto_auth_hmacsha512_state h;
    crypto_auth_hmacsha512_init(&h,salt?salt:zeros,salt?sl:sizeof(zeros));
    crypto_auth_hmacsha512_update(&h,ikm,il);crypto_auth_hmacsha512_final(&h,prk);
    size_t at=0,previous=0;unsigned char counter=1;
    while(at<ol) {
        crypto_auth_hmacsha512_init(&h,prk,sizeof(prk));
        if(previous) crypto_auth_hmacsha512_update(&h,t,previous);
        crypto_auth_hmacsha512_update(&h,info,nl);crypto_auth_hmacsha512_update(&h,&counter,1);
        crypto_auth_hmacsha512_final(&h,t);size_t n=ol-at<64?ol-at:64;memcpy(out+at,t,n);at+=n;previous=64;counter++;
    }
    dsco_memzero(prk,sizeof(prk));dsco_memzero(t,sizeof(t));dsco_memzero(&h,sizeof(h));return 0;
#else
    return -1;
#endif
}
int dsco_kdf_subkey(const uint8_t root[32],const char *label,uint8_t out[32]) {
    if(!root||!label||!out) return -1;
    return dsco_hkdf_sha512(root,32,(const uint8_t *)"dsco.v2",7,(const uint8_t *)label,strlen(label),out,32);
}
int dsco_pwhash_argon2id(const uint8_t *pw,size_t n,const uint8_t salt[16],uint8_t *out,size_t len) {
    if(!valid_bytes(pw,n)||!salt||!out) return -1;
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0||len<crypto_pwhash_BYTES_MIN||len>crypto_pwhash_BYTES_MAX||n>crypto_pwhash_PASSWD_MAX) return -1;
    int rc=crypto_pwhash(out,len,pw?(const char *)pw:"",n,salt,4,256u*1024u*1024u,crypto_pwhash_ALG_ARGON2ID13);
    if(rc) dsco_memzero(out,len);return rc;
#else
    (void)len;return -1;
#endif
}
void dsco_hash(const uint8_t *msg,size_t n,uint8_t out[32]) {
    if(!out||!valid_bytes(msg,n)) abort();
#ifdef HAVE_BLAKE3
    blake3_hasher h;blake3_hasher_init(&h);blake3_hasher_update(&h,msg,n);blake3_hasher_finalize(&h,out,32);dsco_memzero(&h,sizeof(h));
#else
    sha256_ctx_t h;sha256_init(&h);sha256_update(&h,msg,n);sha256_final(&h,out);dsco_memzero(&h,sizeof(h));
#endif
}
void dsco_keyed_mac(const uint8_t key[32],const uint8_t *msg,size_t n,uint8_t out[32]) {
    if(!key||!out||!valid_bytes(msg,n)) abort();
#ifdef HAVE_BLAKE3
    blake3_hasher h;blake3_hasher_init_keyed(&h,key);blake3_hasher_update(&h,msg,n);blake3_hasher_finalize(&h,out,32);dsco_memzero(&h,sizeof(h));
#else
    uint8_t pad[64],inner[32];sha256_ctx_t h;
    memset(pad,0x36,sizeof(pad));for(size_t i=0;i<32;i++) pad[i]^=key[i];
    sha256_init(&h);sha256_update(&h,pad,sizeof(pad));sha256_update(&h,msg,n);sha256_final(&h,inner);
    memset(pad,0x5c,sizeof(pad));for(size_t i=0;i<32;i++) pad[i]^=key[i];
    sha256_init(&h);sha256_update(&h,pad,sizeof(pad));sha256_update(&h,inner,sizeof(inner));sha256_final(&h,out);
    dsco_memzero(pad,sizeof(pad));dsco_memzero(inner,sizeof(inner));dsco_memzero(&h,sizeof(h));
#endif
}
#ifdef HAVE_LIBSODIUM
static bool nonce_valid(const uint8_t nonce[32]) {
    const uint8_t zero[8]={0};return aegis_selected||dsco_memcmp_ct(nonce+24,zero,8)==0;
}
#endif
long dsco_aead_seal(const uint8_t key[32],const uint8_t nonce[32],const uint8_t *aad,size_t al,
                    const uint8_t *pt,size_t pl,uint8_t *ct,size_t cap) {
    if(!key||!nonce||!ct||!valid_bytes(aad,al)||!valid_bytes(pt,pl)) return -1;
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0||!nonce_valid(nonce)) return -1;
    size_t tag=aegis_selected?32u:16u;
    if(pl>(size_t)LONG_MAX-tag||cap<pl+tag) return -1;
    unsigned long long len=0;int rc;
#ifdef crypto_aead_aegis256_ABYTES
    if(aegis_selected) {
        if(pl>crypto_aead_aegis256_MESSAGEBYTES_MAX) return -1;
        rc=crypto_aead_aegis256_encrypt(ct,&len,pt,pl,aad,al,NULL,nonce,key);
    } else
#endif
    { if(pl>crypto_aead_xchacha20poly1305_ietf_MESSAGEBYTES_MAX) return -1;
      rc=crypto_aead_xchacha20poly1305_ietf_encrypt(ct,&len,pt,pl,aad,al,NULL,nonce,key); }
    if(rc) { dsco_memzero(ct,pl+tag);return -1; }return (long)len;
#else
    (void)cap;return -1;
#endif
}
long dsco_aead_open(const uint8_t key[32],const uint8_t nonce[32],const uint8_t *aad,size_t al,
                    const uint8_t *ct,size_t cl,uint8_t *pt,size_t cap) {
    if(!key||!nonce||!ct||!pt||!valid_bytes(aad,al)) return -1;
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0||!nonce_valid(nonce)) return -1;
    size_t tag=aegis_selected?32u:16u;
    if(cl<tag||cl-tag>(size_t)LONG_MAX||cap<cl-tag) return -1;
    unsigned long long len=0;int rc;
#ifdef crypto_aead_aegis256_ABYTES
    if(aegis_selected) rc=crypto_aead_aegis256_decrypt(pt,&len,NULL,ct,cl,aad,al,nonce,key);else
#endif
    rc=crypto_aead_xchacha20poly1305_ietf_decrypt(pt,&len,NULL,ct,cl,aad,al,nonce,key);
    if(rc) { dsco_memzero(pt,cl-tag);return -1; }return (long)len;
#else
    (void)cl;(void)cap;return -1;
#endif
}
int dsco_identity_keygen(uint8_t pk[DSCO_HYBRID_PK_BYTES],uint8_t sk[64],uint8_t pq[DSCO_MLDSA_SK_BYTES]) {
    if(!pk||!sk||!pq) return -1;
    dsco_memzero(pk,DSCO_HYBRID_PK_BYTES);dsco_memzero(sk,64);dsco_memzero(pq,DSCO_MLDSA_SK_BYTES);
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0||crypto_sign_keypair(pk,sk)!=0) return -1;
#ifdef DSCO_PQ_ENABLED
    if(OQS_SIG_ml_dsa_65_keypair(pk+32,pq)!=OQS_SUCCESS) {
        dsco_memzero(pk,DSCO_HYBRID_PK_BYTES);dsco_memzero(sk,64);dsco_memzero(pq,DSCO_MLDSA_SK_BYTES);return -1;
    }
#endif
    return 0;
#else
    return -1;
#endif
}
long dsco_identity_sign(const uint8_t sk[64],const uint8_t pq[DSCO_MLDSA_SK_BYTES],const uint8_t *msg,size_t n,uint8_t sig[DSCO_HYBRID_SIG_BYTES]) {
    if(!sk||!sig||!valid_bytes(msg,n)) return -1;
    dsco_memzero(sig,DSCO_HYBRID_SIG_BYTES);
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0) return -1;
    unsigned long long len=0;size_t plen=0;
    if(crypto_sign_detached(sig,&len,msg,n,sk)!=0) return -1;
#ifdef DSCO_PQ_ENABLED
    if(!pq||OQS_SIG_ml_dsa_65_sign(sig+64,&plen,msg?msg:(const uint8_t *)"",n,pq)!=OQS_SUCCESS||plen!=DSCO_MLDSA_SIG_BYTES) {
        dsco_memzero(sig,DSCO_HYBRID_SIG_BYTES);return -1;
    }
#else
    (void)pq;
#endif
    return (long)(len+plen);
#else
    (void)pq;return -1;
#endif
}
int dsco_identity_verify(const uint8_t pk[DSCO_HYBRID_PK_BYTES],const uint8_t *msg,size_t n,const uint8_t *sig,size_t len) {
    if(!pk||!sig||!valid_bytes(msg,n)) return -1;
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0) return -1;
#ifdef DSCO_PQ_ENABLED
    if(len!=DSCO_HYBRID_SIG_BYTES) return -1;
#else
    const uint8_t zero[DSCO_MLDSA_PK_BYTES]={0};
    if(len!=64||dsco_memcmp_ct(pk+32,zero,sizeof(zero))) return -1;
#endif
    if(crypto_sign_verify_detached(sig,msg,n,pk)!=0) return -1;
#ifdef DSCO_PQ_ENABLED
    if(OQS_SIG_ml_dsa_65_verify(msg?msg:(const uint8_t *)"",n,sig+64,len-64,pk+32)!=OQS_SUCCESS) return -1;
#endif
    return 0;
#else
    (void)len;return -1;
#endif
}
int dsco_kem_keygen(uint8_t pk[DSCO_HYBRID_KEM_PK_BYTES],uint8_t sk[32],uint8_t pq[DSCO_MLKEM_SK_BYTES]) {
    if(!pk||!sk||!pq) return -1;
    dsco_memzero(pk,DSCO_HYBRID_KEM_PK_BYTES);dsco_memzero(pq,DSCO_MLKEM_SK_BYTES);dsco_memzero(sk,32);
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0) return -1;
    randombytes_buf(sk,32);if(crypto_scalarmult_base(pk,sk)!=0) goto fail;
#ifdef DSCO_PQ_ENABLED
    if(OQS_KEM_ml_kem_768_keypair(pk+32,pq)!=OQS_SUCCESS) goto fail;
#endif
    return 0;
fail: dsco_memzero(pk,DSCO_HYBRID_KEM_PK_BYTES);dsco_memzero(sk,32);dsco_memzero(pq,DSCO_MLKEM_SK_BYTES);return -1;
#else
    return -1;
#endif
}
#ifdef HAVE_LIBSODIUM
static int kem_derive(const uint8_t raw[64],const uint8_t pk[32],const uint8_t ct[DSCO_HYBRID_KEM_CT_BYTES],uint8_t out[64]) {
    uint8_t info[32+DSCO_HYBRID_KEM_CT_BYTES];
    memcpy(info,pk,32);memcpy(info+32,ct,DSCO_HYBRID_KEM_CT_BYTES);
    const char *salt=dsco_crypto_has_pq()?"dsco.kem.v1.hybrid":"dsco.kem.v1.classical";
    return dsco_hkdf_sha512(raw,64,(const uint8_t *)salt,strlen(salt),info,sizeof(info),out,64);
}
#endif
int dsco_kem_encap(const uint8_t pk[DSCO_HYBRID_KEM_PK_BYTES],uint8_t ct[DSCO_HYBRID_KEM_CT_BYTES],uint8_t out[64]) {
    if(!pk||!ct||!out) return -1;
    dsco_memzero(ct,DSCO_HYBRID_KEM_CT_BYTES);dsco_memzero(out,64);
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0) return -1;
    uint8_t ephemeral[32],raw[64]={0};int rc=-1;
#ifndef DSCO_PQ_ENABLED
    const uint8_t zero[DSCO_MLKEM_PK_BYTES]={0};if(dsco_memcmp_ct(pk+32,zero,sizeof(zero))) return -1;
#endif
    randombytes_buf(ephemeral,32);
    if(crypto_scalarmult_base(ct,ephemeral)!=0||crypto_scalarmult(raw,ephemeral,pk)!=0) goto done;
#ifdef DSCO_PQ_ENABLED
    if(OQS_KEM_ml_kem_768_encaps(ct+32,raw+32,pk+32)!=OQS_SUCCESS) goto done;
#endif
    rc=kem_derive(raw,pk,ct,out);
done: dsco_memzero(ephemeral,sizeof(ephemeral));dsco_memzero(raw,sizeof(raw));
    if(rc) { dsco_memzero(ct,DSCO_HYBRID_KEM_CT_BYTES);dsco_memzero(out,64); }return rc;
#else
    return -1;
#endif
}
int dsco_kem_decap(const uint8_t sk[32],const uint8_t pq[DSCO_MLKEM_SK_BYTES],const uint8_t ct[DSCO_HYBRID_KEM_CT_BYTES],uint8_t out[64]) {
    if(!sk||!ct||!out) return -1;
    dsco_memzero(out,64);
#ifdef HAVE_LIBSODIUM
    if(dsco_crypto_init()!=0) return -1;
    uint8_t raw[64]={0},pk[DSCO_HYBRID_KEM_PK_BYTES]={0};int rc=-1;
    if(crypto_scalarmult(raw,sk,ct)!=0||crypto_scalarmult_base(pk,sk)!=0) goto done;
#ifdef DSCO_PQ_ENABLED
    if(!pq||OQS_KEM_ml_kem_768_decaps(raw+32,ct+32,pq)!=OQS_SUCCESS) goto done;
#else
    (void)pq;const uint8_t zero[DSCO_MLKEM_CT_BYTES]={0};if(dsco_memcmp_ct(ct+32,zero,sizeof(zero))) goto done;
#endif
    rc=kem_derive(raw,pk,ct,out);
done: dsco_memzero(raw,sizeof(raw));dsco_memzero(pk,sizeof(pk));if(rc) dsco_memzero(out,64);return rc;
#else
    (void)pq;return -1;
#endif
}
