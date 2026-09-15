import json
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def test_oversized_and_partial_records():
    with tempfile.TemporaryDirectory() as d:
        c = pathlib.Path(d) / "probe.c"
        c.write_text(r'''
#include "swarm.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(void) { swarm_child_t x={0}; int fd=swarm_accounting_open(); if(fd<0) return 2; x.cost_fd=fd; x.cost_transport=true;
char big[25000]; int n=snprintf(big,sizeof(big),"{\"schema\":\"dsco.inference_cost.v1\",\"provider_reported_usd\":1.25,\"estimated_inference_usd\":2,\"budget_accounted_usd\":3,\"subscription_included\":true,\"provider\":\"p\",\"actual_model\":\"m\",\"x\":\""); memset(big+n,'a',20000-n); n=20000; n+=snprintf(big+n,sizeof(big)-n,"\"}\n"); write(fd,big,n); dprintf(fd,"{\"schema\":\"dsco.inference_cost.v1\",\"provider_reported_usd\":2,\"estimated_inference_usd\":3,\"budget_accounted_usd\":4}\n"); dprintf(fd,"{\"schema\":\"dsco.inference_cost.v1\",\"provider_reported_usd\":5"); swarm_accounting_read(&x); dprintf(fd,".5,\"estimated_inference_usd\":6,\"budget_accounted_usd\":7}\n"); swarm_accounting_read(&x); swarm_accounting_read(&x); printf("%d %.2f %.2f %.2f\n",x.cost_samples,x.reported_cost_usd,x.est_cost_usd,x.budget_accounted_usd); close(fd);  return 0; }
''')
        exe = pathlib.Path(d) / "probe"
        subprocess.run(["cc", "-std=c11", "-D_DARWIN_C_SOURCE", "-Iinclude", "-o", str(exe), str(c), "src/swarm_accounting.c", "src/json_util.c", "-lm"], cwd=ROOT, check=True)
        assert subprocess.check_output([str(exe)], cwd=ROOT, text=True).strip() == "3 8.75 11.00 14.00"

if __name__ == "__main__":
    test_oversized_and_partial_records()
    print("PASS: oversized and partial cost rows retain exact totals without duplicates")
