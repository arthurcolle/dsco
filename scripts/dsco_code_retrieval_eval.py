#!/usr/bin/env python3
"""Grounded regression suite for the sovereign code-retrieval cascade."""
import argparse, json, pathlib, sys, time

import dsco_code_retrieval as retrieval

CASES=[
    ('enforce capability permissions and block lethal trifecta before a tool call',['dsco_capability_gate'],[]),
    ('derive the capability bitmask required by a tool invocation',['dsco_caps_for_tool'],[]),
    ('execute a tool for a trust tier through governance approval',['tools_execute_for_tier'],[]),
    ('spawn a child agent process in a swarm',['swarm_spawn'],['tool_spawn_agent']),
    ('wait until any child agent in the swarm completes',['swarm_wait_any'],[]),
    ('main supervisor loop that restarts or hot-swaps child processes',['supervisor_run'],[]),
    ('build the ordered default fallback model list for a provider',['provider_build_default_fallback_models'],[]),
    ('classify task complexity for model routing',['router_classify_task'],[]),
    ('make the final routing decision for a request',['router_decide'],[]),
    ('perform a streaming LLM request',['llm_stream'],[]),
    ('stream an OpenAI-compatible provider response',['openai_stream'],[]),
    ('connect to every configured MCP server',['mcp_connect_all'],[]),
    ('discover tools exposed by remote MCP servers',['discover_tools'],[]),
    ('handle an MCP JSON-RPC tools/call request',['handle_tools_call'],[]),
    ('run the MCP server request loop over stdio',['mcp_server_run'],[]),
    ('implementation of the bash shell command tool',['tool_bash','tool_bash_compat'],[]),
    ('implementation that reads a file for the model',['tool_read_file'],[]),
    ('trigger the control-plane killswitch',['tool_killswitch_trigger'],[]),
    ('authorize an action under governance and GSU budget',['governance_authorize'],[]),
    ('check whether governance circuit breakers have tripped',['governance_check_breakers'],[]),
    ('export provider credentials into a child swarm process',['swarm_export_child_credential_for_provider'],[]),
]

def rank_of(names,expected):
    return next((rank for rank,name in enumerate(names,1) if name in expected),None)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--depth',type=int,default=retrieval.DEEP_RERANK_LIMIT)
    ap.add_argument('-k',type=int,default=20)
    ap.add_argument('--top',type=int,default=5)
    ap.add_argument('--json',type=pathlib.Path)
    args=ap.parse_args()
    retrieval.DEEP_RERANK_LIMIT=args.depth
    db=retrieval.build();rows=[];started=time.monotonic()
    for number,(query,literal,aliases) in enumerate(CASES,1):
        case_started=time.monotonic()
        results,classification=retrieval.search(db,query,args.k,args.top)
        seconds=time.monotonic()-case_started;names=[x['name'] for x in results]
        literal_rank=rank_of(names,literal);operational_rank=rank_of(names,literal+aliases)
        rows.append({'query':query,'literal':literal,'aliases':aliases,'literal_rank':literal_rank,
                     'operational_rank':operational_rank,'top':names,'classification':classification[0]['label'],
                     'seconds':seconds})
        print(f'{number:02d}/{len(CASES)} literal={literal_rank} operational={operational_rank} '
              f'{seconds:.2f}s {names[0]}',file=sys.stderr,flush=True)
    count=len(rows);within=lambda rank,k:rank is not None and rank<=k
    result={'depth':args.depth,'cases':count,'seconds':time.monotonic()-started,
            'literal_recall@1':sum(x['literal_rank']==1 for x in rows)/count,
            'operational_recall@1':sum(x['operational_rank']==1 for x in rows)/count,
            'literal_recall@5':sum(within(x['literal_rank'],5) for x in rows)/count,
            'literal_mrr':sum(1/x['literal_rank'] if x['literal_rank'] else 0 for x in rows)/count,
            'rows':rows}
    if args.json:
        args.json.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({key:value for key,value in result.items() if key!='rows'},indent=2))
    raise SystemExit(0 if result['operational_recall@1']==1.0 else 1)

if __name__=='__main__':main()
