#include "dsco_crypto.h"
#include "crypto.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static void expect_hex(const uint8_t *data,size_t n,const char *expected) {
    char hex[8193];assert(n<=4096);hex_encode(data,n,hex);assert(!strcmp(hex,expected));
}
#ifdef HAVE_LIBSODIUM
static void *initialize(void *unused) { (void)unused;assert(dsco_crypto_init()==0);return NULL; }
#endif
int main(void) {
    uint8_t digest[32],key[32]={0},random[32]={0};
    dsco_hash((const uint8_t *)"abc",3,digest);
    expect_hex(digest,32,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    dsco_keyed_mac(key,(const uint8_t *)"abc",3,digest);
    expect_hex(digest,32,"fd7adb152c05ef80dccf50a1fa4c05d5a3ec6da95575fc312ae7c5d091836351");
    dsco_random_bytes(random,sizeof(random));assert(dsco_memcmp_ct(random,key,32)!=0);
    assert(dsco_memcmp_ct(NULL,NULL,0)==0);assert(dsco_memcmp_ct(random,random,32)==0);
    dsco_memzero(random,32);assert(dsco_memcmp_ct(random,key,32)==0);
    dsco_memzero(NULL,0);dsco_random_bytes(NULL,0);
    uint8_t locked[4096];memset(locked,1,sizeof(locked));
    if(dsco_mlock(locked,sizeof(locked))==0) { assert(dsco_munlock(locked,sizeof(locked))==0);assert(locked[0]==0&&locked[4095]==0); }
    assert(dsco_mlock(NULL,0)==-1);assert(dsco_munlock(NULL,0)==-1);
#ifdef HAVE_LIBSODIUM
    pthread_t threads[8];for(int i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,initialize,NULL));
    for(int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));assert(dsco_crypto_init()==0);
    assert(strstr(dsco_crypto_suite_id(),"HKDF-SHA512"));
    uint8_t ikm[22],salt[16],info[10],okm[42];memset(ikm,11,sizeof(ikm));
    for(int i=0;i<16;i++) salt[i]=(uint8_t)i;for(int i=0;i<10;i++) info[i]=(uint8_t)(240+i);
    assert(!dsco_hkdf_sha512(ikm,22,salt,13,info,10,okm,42));
    expect_hex(okm,42,"832390086cda71fb47625bb5ceb168e4c8e26a1a16ed34d9fc7fe92c1481579338da362cb8d9f925d7cb");
    assert(dsco_hkdf_sha512(NULL,1,NULL,0,NULL,0,okm,1)==-1);
    assert(dsco_hkdf_sha512(ikm,22,NULL,0,NULL,0,okm,16321)==-1);
    assert(!dsco_hkdf_sha512(NULL,0,NULL,0,NULL,0,NULL,0));
    uint8_t k1[32],k2[32];assert(!dsco_kdf_subkey(key,"one",k1));assert(!dsco_kdf_subkey(key,"two",k2));assert(dsco_memcmp_ct(k1,k2,32));
    assert(dsco_kdf_subkey(key,NULL,k1)==-1);
    assert(!dsco_pwhash_argon2id((const uint8_t *)"password",8,salt,k1,32));
    expect_hex(k1,32,"c5ffcfe6136b9ae0f8143a9fa6328976fdce39484b4a2b2dcb583d14ebc09714");
    assert(dsco_pwhash_argon2id(NULL,1,salt,k1,32)==-1);
    uint8_t nonce[32]={0},ct[100],pt[100];dsco_random_bytes(key,32);dsco_random_bytes(nonce,24);
    const uint8_t message[]={0,'m',0,'z'};const uint8_t aad[]={1,2,3};
    long len=dsco_aead_seal(key,nonce,aad,3,message,sizeof(message),ct,sizeof(ct));
    assert(len==(long)sizeof(message)+(dsco_crypto_aead_is_aegis()?32:16));
    assert(dsco_aead_open(key,nonce,aad,3,ct,(size_t)len,pt,sizeof(pt))==4);assert(!memcmp(pt,message,4));
    assert(dsco_aead_seal(key,nonce,aad,3,message,4,ct,3)==-1);
    assert(dsco_aead_seal(key,nonce,NULL,0,message,SIZE_MAX,ct,SIZE_MAX)==-1);
    assert(dsco_aead_open(key,nonce,aad,3,ct,(size_t)len,pt,3)==-1);
    ct[len-1]^=1;memset(pt,1,4);assert(dsco_aead_open(key,nonce,aad,3,ct,(size_t)len,pt,100)==-1);assert(pt[0]==0&&pt[3]==0);ct[len-1]^=1;
    nonce[0]^=1;assert(dsco_aead_open(key,nonce,aad,3,ct,(size_t)len,pt,100)==-1);nonce[0]^=1;
    assert(dsco_aead_open(key,nonce,(const uint8_t *)"bad",3,ct,(size_t)len,pt,100)==-1);
    if(!dsco_crypto_aead_is_aegis()) { nonce[31]=1;assert(dsco_aead_seal(key,nonce,NULL,0,NULL,0,ct,100)==-1);nonce[31]=0; }
    len=dsco_aead_seal(key,nonce,NULL,0,NULL,0,ct,100);assert(len>0);assert(dsco_aead_open(key,nonce,NULL,0,ct,(size_t)len,pt,100)==0);
    uint8_t pk[DSCO_HYBRID_PK_BYTES],sk[64],pqsk[DSCO_MLDSA_SK_BYTES],sig[DSCO_HYBRID_SIG_BYTES];
    assert(!dsco_identity_keygen(pk,sk,pqsk));len=dsco_identity_sign(sk,pqsk,message,4,sig);assert(len>0);
    assert(!dsco_identity_verify(pk,message,4,sig,(size_t)len));sig[0]^=1;assert(dsco_identity_verify(pk,message,4,sig,(size_t)len)==-1);sig[0]^=1;
    assert(dsco_identity_verify(pk,message,4,sig,(size_t)len-1)==-1);
    if(dsco_crypto_has_pq()) { sig[64]^=1;assert(dsco_identity_verify(pk,message,4,sig,(size_t)len)==-1);sig[64]^=1;assert(dsco_identity_verify(pk,message,4,sig,64)==-1); }
    /* RFC 8032 section 7.1 empty-message Ed25519 known-answer vector. */
    assert(hex_decode("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",64,sk,32)==32);
    assert(hex_decode("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",64,pk,32)==32);memcpy(sk+32,pk,32);
    len=dsco_identity_sign(sk,pqsk,NULL,0,sig);assert(len>0);
    expect_hex(sig,64,"e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    assert(!dsco_identity_verify(pk,NULL,0,sig,(size_t)len));
    uint8_t kem_pk[DSCO_HYBRID_KEM_PK_BYTES],kem_sk[32],kem_pq[DSCO_MLKEM_SK_BYTES],kem_ct[DSCO_HYBRID_KEM_CT_BYTES],ss1[64],ss2[64];
    assert(!dsco_kem_keygen(kem_pk,kem_sk,kem_pq));assert(!dsco_kem_encap(kem_pk,kem_ct,ss1));assert(!dsco_kem_decap(kem_sk,kem_pq,kem_ct,ss2));assert(!dsco_memcmp_ct(ss1,ss2,64));
    if(dsco_crypto_has_pq()) { kem_ct[32]^=1;assert(!dsco_kem_decap(kem_sk,kem_pq,kem_ct,ss2));assert(dsco_memcmp_ct(ss1,ss2,64));kem_ct[32]^=1; }
    else { kem_ct[32]=1;assert(dsco_kem_decap(kem_sk,kem_pq,kem_ct,ss2)==-1);kem_ct[32]=0; }
    memset(kem_pk,0,32);assert(dsco_kem_encap(kem_pk,kem_ct,ss2)==-1);
    dsco_memzero(sk,sizeof(sk));dsco_memzero(pqsk,sizeof(pqsk));dsco_memzero(kem_sk,sizeof(kem_sk));dsco_memzero(kem_pq,sizeof(kem_pq));dsco_memzero(ss1,sizeof(ss1));dsco_memzero(ss2,sizeof(ss2));
    printf("PASS crypto primitives: %s\n",dsco_crypto_suite_id());
#else
    assert(dsco_crypto_init()==-1);assert(!dsco_crypto_has_pq());assert(!dsco_crypto_aead_is_aegis());
    assert(strstr(dsco_crypto_suite_id(),"unavailable"));
    uint8_t pk[DSCO_HYBRID_PK_BYTES],sk[64],pq[DSCO_MLDSA_SK_BYTES],sig[DSCO_HYBRID_SIG_BYTES];
    uint8_t kp[DSCO_HYBRID_KEM_PK_BYTES],ks[32],kq[DSCO_MLKEM_SK_BYTES],ct[DSCO_HYBRID_KEM_CT_BYTES],ss[64];
    assert(dsco_hkdf_sha512(key,32,NULL,0,NULL,0,digest,32)==-1);
    assert(dsco_kdf_subkey(key,"x",digest)==-1);assert(dsco_pwhash_argon2id(key,32,key,digest,32)==-1);
    assert(dsco_aead_seal(key,key,NULL,0,key,32,ct,sizeof(ct))==-1);
    assert(dsco_aead_open(key,key,NULL,0,ct,32,digest,32)==-1);
    assert(dsco_identity_keygen(pk,sk,pq)==-1);assert(dsco_identity_sign(sk,pq,key,32,sig)==-1);assert(dsco_identity_verify(pk,key,32,sig,64)==-1);
    assert(dsco_kem_keygen(kp,ks,kq)==-1);assert(dsco_kem_encap(kp,ct,ss)==-1);assert(dsco_kem_decap(ks,kq,ct,ss)==-1);
    puts("PASS crypto portable hash/MAC/random/memory; unavailable backend fails closed");
#endif
    return 0;
}
