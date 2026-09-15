#include "value_ledger.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const char *run;static bool append_ok;static int appends;
const char *chronicle_run_id(void){return run;}
bool chronicle_journal_append(const char *kind,const char *payload,bool durable){
 assert(!strcmp(kind,"value.receipt.v1"));assert(payload && durable);appends++;return append_ok;}
void sha256_hex(const uint8_t *p,size_t n,char *out){(void)p;(void)n;memset(out,'a',64);out[64]=0;}
int main(void){
 value_receipt_t r={.workload_id="test"};char hash[65]="stale";
 assert(!value_ledger_emit(&r,hash,sizeof(hash)) && !hash[0] && appends==0);
 run="";assert(!value_ledger_emit(&r,hash,sizeof(hash)) && appends==0);
 run="test";assert(!value_ledger_emit(&r,hash,sizeof(hash)) && !hash[0] && appends==1);
 append_ok=true;assert(value_ledger_emit(&r,hash,sizeof(hash)) && strlen(hash)==64 && appends==2);
 assert(!value_ledger_emit(NULL,hash,sizeof(hash)) && !hash[0]);
 assert(!value_ledger_emit(&r,hash,4) && !hash[0] && appends==2);
 puts("PASS: missing run, failed append, success-only hash, invalid input and short buffer");
}
