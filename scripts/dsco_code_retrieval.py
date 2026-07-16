#!/usr/bin/env python3
"""Semantic C symbol retrieval using a sovereign Matrix retrieval cascade.

Indexes named C functions from src/ and include/, retrieves with Qwen3 Embedding,
then cascades a fast BGE reranker into a deeper Qwen3 reranker. All inference is
local to Matrix. The index is content-addressed, compact, and resumable.
"""
import argparse, array, base64, hashlib, json, math, os, pathlib, re, subprocess, sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
CACHE=ROOT/'.cache/dsco-code-retrieval.json'
VECTOR_CACHE=ROOT/'.cache/dsco-code-embeddings.jsonl'
CLASSIFIER_CACHE=ROOT/'.cache/dsco-code-classifier.json'
BRIDGE=pathlib.Path.home()/'bridge/connect.sh'
MODEL='qwen3-embedding-4b-q4_k_m'
RETRIEVAL_SCHEME='qwen3-code-instruct-last-v1'
CLASSIFIER_MODEL=MODEL
FAST_RERANKER_MODEL='bge-reranker-v2-m3-q8_0'
RERANKER_MODEL='qwen3-reranker-4b-q4_k_m'
RETRIEVAL_TASK='Given a natural language request, retrieve relevant C source code that implements the requested behavior'
RERANK_TASK=(RETRIEVAL_TASK+'; prefer the most direct callable implementation or public tool entry point, '
             'and demote tests, incidental callers, and generic helpers')
CLASSIFICATION_TASK='Classify the software request by matching it to the most relevant implementation domain'
BATCH=8
RERANK_LIMIT=20
DEEP_RERANK_LIMIT=6
RRF_PREFILTER_WEIGHT=.30
RRF_FAST_WEIGHT=.25
RRF_DEEP_WEIGHT=.25
RRF_LEXICAL_WEIGHT=.20
DOMAINS={
    'agent_orchestration':'agent loops, swarm coordination, supervisors, workers and autonomous execution',
    'model_routing':'LLM providers, model routing, inference requests, completions and provider selection',
    'tool_execution':'tool registry, tool calls, command execution, filesystem and network tools',
    'governance_safety':'capability grants, governance gates, permissions, security and policy enforcement',
    'mcp':'Model Context Protocol clients, servers, sessions and transport',
    'other':'general source code implementation details',
}
DOMAIN_PROTOTYPES={
    'agent_orchestration':[
        'swarm child agents, worker groups, spawning and multi-agent orchestration',
        'supervisor restart, hot-swap, child process lifecycle and autonomous agent loops',
        'agent coordination, worker dispatch, polling and completion management',
    ],
    'model_routing':[
        'LLM provider selection, fallback models, model routing and inference requests',
        'chat completion streaming, response APIs, provider transport and model invocation',
        'router task complexity, model scoring, policy decisions and provider credentials',
    ],
    'tool_execution':[
        'bash commands, filesystem tools, tool registry and command execution',
        'tool calls, shell dispatch, read write edit operations and tool implementations',
        'tool discovery, invocation, output handling and execution metadata',
    ],
    'governance_safety':[
        'permissions, capability grants, security policy, exfiltration and control-plane denial',
        'governance gates, killswitches, circuit breakers, approvals and safety enforcement',
        'secret access, untrusted input, network egress and lethal-trifecta protection',
    ],
    'mcp':[
        'Model Context Protocol MCP client lifecycle, connection, filtering and shutdown',
        'MCP server JSON-RPC tools/call, resources, prompts and protocol transport',
        'MCP remote tool discovery, server registration, sessions and notifications',
    ],
    'other':[
        'miscellaneous utility helpers, data formatting, parsing and diagnostics',
        'general source code implementation details and low-level helper functions',
        'uncategorized code outside agents, models, tools, governance and MCP',
    ],
}
FILE_DOMAIN={
    'src/agent.c':'agent_orchestration','src/swarm.c':'agent_orchestration','src/supervisor.c':'agent_orchestration',
    'src/provider.c':'model_routing','src/llm.c':'model_routing','src/router.c':'model_routing',
    'src/tools.c':'tool_execution','include/tools.h':'tool_execution',
    'src/capability.c':'governance_safety','include/capability.h':'governance_safety','src/governance.c':'governance_safety',
    'src/mcp.c':'mcp','src/mcp_server.c':'mcp',
}
FUNC=re.compile(r'(?m)^(?P<sig>(?:static[ \t]+)?(?:inline[ \t]+)?[\w \t\*]+?[ \t]+(?P<name>[A-Za-z_]\w*)[ \t]*\([^;{}]*?\))[ \t]*\{')
TYPE=re.compile(r'(?m)^\s*(?:typedef\s+)?(?P<kind>struct|enum|union)\s+(?P<name>[A-Za-z_]\w*)[^;]*\{')
DEFINE=re.compile(r'(?m)^\s*#define\s+(?P<name>[A-Za-z_]\w*)(?P<body>[^\n]*)')
C_KEYWORDS={'if','for','while','switch'}

def remote_py(code,timeout=180):
    # Send programs over stdin. Putting source cards in the SSH command itself
    # eventually exceeds the multiplexed SSH control message size.
    cmd=[str(BRIDGE),'exec-direct','matrix','python3 -']
    p=subprocess.run(cmd,input=code,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout)
    if p.returncode:
        raise subprocess.CalledProcessError(p.returncode,cmd,output=p.stdout,stderr=p.stderr)
    return p.stdout
def ensure_aux_services():
    code=r'''import json,os,signal,subprocess,time,urllib.request
BIN='/Users/agent/.lmstudio/extensions/backends/llama.cpp-mac-arm64-apple-metal-advsimd-2.25.0/llama-server'
BASE='/Volumes/Storage/LMStudio/models/sovereign-retrieval'
SERVICES={
8187:('Qwen3-Embedding-4B-Q4_K_M.gguf',[BIN,'-m',BASE+'/Qwen--Qwen3-Embedding-4B-GGUF/Qwen3-Embedding-4B-Q4_K_M.gguf','--embedding','--pooling','last','-c','8192','-b','8192','-ub','8192','-ngl','99','--host','127.0.0.1','--port','8187','--no-webui']),
8188:('bge-reranker-v2-m3-Q8_0.gguf',[BIN,'-m',BASE+'/gpustack--bge-reranker-v2-m3-GGUF/bge-reranker-v2-m3-Q8_0.gguf','--embedding','--pooling','rank','--reranking','-c','8192','-b','2048','-ub','2048','-ngl','99','--host','127.0.0.1','--port','8188','--no-webui']),
8189:('Qwen3-Reranker-4B.Q4_K_M.gguf',[BIN,'-m',BASE+'/QuantFactory--Qwen3-Reranker-4B-GGUF/Qwen3-Reranker-4B.Q4_K_M.gguf','-c','8192','-b','2048','-ub','512','-np','4','-ngl','99','--reasoning-format','none','--host','127.0.0.1','--port','8189','--no-webui']),
}
def props(port):
    try:return json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/props',timeout=1))
    except Exception:return None
def ready(port,needle):
    p=props(port);return bool(p and needle in p.get('model_path',''))
def busy(port):
    try:return any(x.get('is_processing') for x in json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/slots',timeout=1)))
    except Exception:return False
def start(args,log):
    f=open(log,'ab');subprocess.Popen(args,stdin=subprocess.DEVNULL,stdout=f,stderr=subprocess.STDOUT,start_new_session=True,close_fds=True);f.close()
for port,(needle,args) in SERVICES.items():
    if ready(port,needle):continue
    if busy(port):raise RuntimeError(f'Matrix port {port} is busy with an unexpected model')
    p=subprocess.run(['lsof','-tiTCP:'+str(port),'-sTCP:LISTEN'],text=True,stdout=subprocess.PIPE)
    for value in p.stdout.split():
        try:os.kill(int(value),signal.SIGTERM)
        except ProcessLookupError:pass
    time.sleep(.2);start(args,f'/tmp/sovereign-retrieval-{port}.log')
for _ in range(180):
    if all(ready(port,needle) for port,(needle,_) in SERVICES.items()):break
    time.sleep(.2)
else:raise RuntimeError('Matrix model services did not become ready')'''
    remote_py(code,30)
def matrix_call(code,port,timeout):
    last=None
    for attempt in range(2):
        ensure_aux_services()
        try:return remote_py(code,timeout)
        except (subprocess.CalledProcessError,subprocess.TimeoutExpired) as e:
            last=e
    detail=getattr(last,'stderr','') or str(last)
    raise RuntimeError(f'Matrix service on port {port} failed after health-checked retry: {detail[-2000:]}')
def brace_block(text,start,limit=5000):
    depth=0; quote=None; esc=False
    for i,ch in enumerate(text[start:start+limit],start):
        if quote:
            if esc:esc=False
            elif ch=='\\':esc=True
            elif ch==quote:quote=None
            continue
        if ch in "'\"":quote=ch
        elif ch=='{':depth+=1
        elif ch=='}':
            depth-=1
            if depth==0:return text[start:i+1]
    return text[start:start+limit]
def mask_comments(text):
    def blank(m):return ''.join('\n' if c=='\n' else ' ' for c in m.group(0))
    return re.sub(r'/\*.*?\*/|//[^\n]*',blank,text,flags=re.S)
def cards():
    out=[]
    # Specific use case: retrieve DSCO's agent/provider/tool/governance execution spine.
    core={'src/agent.c','src/provider.c','src/llm.c','src/router.c','src/capability.c','src/governance.c','src/mcp.c','src/mcp_server.c','src/swarm.c','src/supervisor.c','include/tools.h','include/capability.h','include/router.h'}
    for p in sorted(ROOT/x for x in core if (ROOT/x).exists()):
        text=p.read_text(errors='replace'); scan=mask_comments(text); rel=str(p.relative_to(ROOT))
        for m in FUNC.finditer(scan):
            if m.group('name') in C_KEYWORDS:continue
            body=brace_block(text,m.end()-1); line=text.count('\n',0,m.start())+1
            out.append({'id':f'{rel}:{line}:{m.group("name")}', 'file':rel,'line':line,'name':m.group('name'),'kind':'function','card':f'{rel}:{line} function {m.group("sig").strip()}\n{body[:1800]}'})
    # tools.c needs a conservative line-oriented function pass to avoid macro/table false positives.
    p=ROOT/'src/tools.c'; text=p.read_text(errors='replace'); scan=mask_comments(text); rel='src/tools.c'
    for m in re.finditer(r'(?m)^(?:static\s+)?(?:bool|void|int|char\s*\*|const\s+char\s*\*)\s+(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*?\)\s*\{',scan):
        line=text.count('\n',0,m.start())+1; body=brace_block(text,m.end()-1,3000)
        out.append({'id':f'{rel}:{line}:{m.group("name")}', 'file':rel,'line':line,'name':m.group('name'),'kind':'function','card':f'{rel}:{line} function {m.group(0)[:-1].strip()}\n{body[:1800]}'})
    return out
def fingerprint(items):
    h=hashlib.sha256()
    for x in items:h.update(x['id'].encode());h.update(x['card'].encode())
    return h.hexdigest()
def vector_key(card):
    return hashlib.sha256((MODEL+'\0'+RETRIEVAL_SCHEME+'\0Document\0'+card).encode()).hexdigest()
def pack_vector(vector):
    values=array.array('f',vector)
    if sys.byteorder!='little':values.byteswap()
    return base64.b64encode(values.tobytes()).decode()
def unpack_vector(encoded):
    values=array.array('f');values.frombytes(base64.b64decode(encoded))
    if sys.byteorder!='little':values.byteswap()
    return values.tolist()
def load_vector_cache():
    out={}
    if not VECTOR_CACHE.exists():return out
    with VECTOR_CACHE.open() as f:
        for line in f:
            try:
                row=json.loads(line)
                if row.get('model')==MODEL:
                    if isinstance(row.get('vector_f32'),str):out[row['key']]=unpack_vector(row['vector_f32'])
                    elif isinstance(row.get('vector'),list):out[row['key']]=row['vector']
            except (KeyError,json.JSONDecodeError):
                # A killed writer can leave one incomplete trailing record.
                continue
    return out
def append_vector_cache(cards_,vectors):
    VECTOR_CACHE.parent.mkdir(exist_ok=True)
    rows=[]
    for card,vector in zip(cards_,vectors):
        rows.append(json.dumps({'scheme':4,'model':MODEL,'key':vector_key(card),'dims':len(vector),'vector_f32':pack_vector(vector)},separators=(',',':')))
    with VECTOR_CACHE.open('a') as f:
        f.write('\n'.join(rows)+'\n')
        f.flush()
        os.fsync(f.fileno())
def compact_vector_cache(items,vectors):
    rows=[json.dumps({'scheme':4,'model':MODEL,'key':vector_key(x['card']),'dims':len(v),'vector_f32':pack_vector(v)},separators=(',',':')) for x,v in zip(items,vectors)]
    tmp=VECTOR_CACHE.with_name(f'{VECTOR_CACHE.name}.tmp-{os.getpid()}')
    try:
        with tmp.open('w') as f:
            f.write('\n'.join(rows)+'\n');f.flush();os.fsync(f.fileno())
        os.replace(tmp,VECTOR_CACHE)
    finally:
        tmp.unlink(missing_ok=True)
def write_json_atomic(path,value):
    path.parent.mkdir(exist_ok=True)
    tmp=path.with_name(f'{path.name}.tmp-{os.getpid()}')
    try:
        tmp.write_text(json.dumps(value))
        os.replace(tmp,path)
    finally:
        tmp.unlink(missing_ok=True)
def load_index(path):
    db=json.loads(path.read_text())
    if 'vectors_f32' in db:db['vectors']=[unpack_vector(v) for v in db.pop('vectors_f32')]
    return db
def write_index(path,db):
    disk=dict(db);disk['vectors_f32']=[pack_vector(v) for v in disk.pop('vectors')]
    write_json_atomic(path,disk)
def embed(texts,role):
    if role=='Query':texts=[f'Instruct: {RETRIEVAL_TASK}\nQuery:{x}' for x in texts]
    elif role=='Classify':texts=[f'Instruct: {CLASSIFICATION_TASK}\nQuery:{x}' for x in texts]
    elif role!='Document':raise ValueError(f'unknown embedding role: {role}')
    code='''import json,urllib.request
x='''+json.dumps(texts)+'''
p=json.dumps({"model":'''+json.dumps(MODEL)+''',"input":x}).encode()
r=urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:8187/v1/embeddings",data=p,headers={"Content-Type":"application/json"}),timeout=120)
b=json.load(r);print(json.dumps([z["embedding"] for z in b["data"]]))'''
    return json.loads(matrix_call(code,8187,240))
def build(force=False):
    xs=cards(); fp=fingerprint(xs)
    if not force and CACHE.exists():
        db=load_index(CACHE)
        if db.get('version')==4 and db.get('model')==MODEL and db.get('embedding_scheme')==RETRIEVAL_SCHEME and db.get('fingerprint')==fp:return db
    cached=load_vector_cache()
    vec=[cached.get(vector_key(x['card'])) for x in xs]
    missing=[i for i,v in enumerate(vec) if v is None]
    if len(missing)!=len(xs):
        print(f'cached {len(xs)-len(missing)}/{len(xs)} embeddings',file=sys.stderr)
    for off in range(0,len(missing),BATCH):
        indices=missing[off:off+BATCH]
        batch_cards=[xs[i]['card'] for i in indices]
        batch_vec=embed(batch_cards,'Document')
        if len(batch_vec)!=len(indices):
            raise RuntimeError(f'embedding endpoint returned {len(batch_vec)} vectors for {len(indices)} inputs')
        append_vector_cache(batch_cards,batch_vec)
        for i,v in zip(indices,batch_vec):vec[i]=v
        done=len(xs)-len(missing)+min(off+BATCH,len(missing))
        print(f'embedded {done}/{len(xs)}',file=sys.stderr)
    if not vec:raise RuntimeError('no source cards found')
    dims=len(vec[0])
    if any(len(v)!=dims for v in vec):raise RuntimeError('embedding cache contains inconsistent dimensions')
    db={'version':4,'model':MODEL,'embedding_scheme':RETRIEVAL_SCHEME,'fingerprint':fp,'dims':dims,'items':xs,'vectors':vec}
    compact_vector_cache(xs,vec)
    write_index(CACHE,db)
    return db
def cos(a,b):return sum(x*y for x,y in zip(a,b))/(math.sqrt(sum(x*x for x in a))*math.sqrt(sum(x*x for x in b)))
def domain_lexical_bonus(query,label):
    q=query.lower()
    strong={
        'tool_execution':('read a file','reads a file','write a file','writes a file','edit a file','filesystem operation'),
        'governance_safety':('lethal trifecta','killswitch','kill switch'),
        'mcp':('json-rpc','tools/call'),
    }
    if any(p in q for p in strong.get(label,())):return .08
    phrases={
        'agent_orchestration':('swarm','supervisor','child agent','worker group'),
        'model_routing':('llm provider','fallback model','model routing','chat completion','inference request'),
        'tool_execution':('bash command','filesystem tool','tool registry','command execution'),
        'governance_safety':('capability grant','control-plane','exfiltration','security policy','permission'),
        'mcp':('mcp','model context protocol'),
        'other':('unrelated to','general source code implementation','miscellaneous utility','uncategorized'),
    }
    return .02 if any(p in q for p in phrases[label]) else 0.0
def classifier_prototypes():
    prototypes=[p for label in DOMAINS for p in DOMAIN_PROTOTYPES[label]]
    key=hashlib.sha256(json.dumps([MODEL,CLASSIFICATION_TASK,DOMAIN_PROTOTYPES],sort_keys=True).encode()).hexdigest()
    if CLASSIFIER_CACHE.exists():
        try:
            cached=json.loads(CLASSIFIER_CACHE.read_text())
            if cached.get('version')==1 and cached.get('fingerprint')==key:
                return [unpack_vector(v) for v in cached['vectors_f32']]
        except (KeyError,json.JSONDecodeError,ValueError):
            pass
    vectors=embed(prototypes,'Document')
    write_json_atomic(CLASSIFIER_CACHE,{'version':1,'model':MODEL,'fingerprint':key,'vectors_f32':[pack_vector(v) for v in vectors]})
    return vectors
def classify(query):
    labels=list(DOMAINS)
    vectors=classifier_prototypes();q=embed([query],'Classify')[0];offset=0;scored=[]
    for label in labels:
        count=len(DOMAIN_PROTOTYPES[label]); embedding_score=max(cos(q,v) for v in vectors[offset:offset+count]);offset+=count
        bonus=domain_lexical_bonus(query,label)
        scored.append({'label':label,'score':embedding_score+bonus,'embedding_score':embedding_score,'lexical_bonus':bonus})
    return sorted(scored,key=lambda x:x['score'],reverse=True)
def rerank(query,docs,names):
    code=r'''import concurrent.futures,json,re,time,urllib.request
q='''+json.dumps(query)+''';docs='''+json.dumps(docs)+''';names='''+json.dumps(names)+''';deep_limit='''+str(DEEP_RERANK_LIMIT)+r'''
task='''+json.dumps(RERANK_TASK)+r'''
def post(port,path,payload,timeout=180):
    req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(payload).encode(),headers={'Content-Type':'application/json'})
    return json.load(urllib.request.urlopen(req,timeout=timeout))
fast=post(8188,'/v1/rerank',{'model':'bge-reranker-v2-m3','query':q,'documents':docs,'top_n':len(docs)})
fast_results=fast['results'];deep_indices=[x['index'] for x in fast_results[:min(deep_limit,len(fast_results))]]
stop={'a','an','the','for','to','of','in','on','that','this','code','function','implementation','model','behavior'}
def terms(text):
    out=[]
    aliases={'cap':'capability','caps':'capability','router':'route','routing':'route','decision':'decide','every':'all'}
    for token in re.findall(r'[a-z0-9]+',text.lower()):
        if token.endswith('ies') and len(token)>4:token=token[:-3]+'y'
        elif token.endswith('s') and len(token)>3:token=token[:-1]
        token=aliases.get(token,token)
        if token not in stop:out.append(token)
    return set(out)
query_terms=terms(q)
lexical_scores=[]
for name in names:
    score=float(len(query_terms & terms(name)))
    if score and name.startswith('tool_') and ('for the model' in q.lower() or 'public tool' in q.lower()):score+=.25
    if '_test_' in name or name.startswith('test_') or name.endswith('_test'):score-=.5
    lexical_scores.append(score)
prefix='<|im_start|>system\nJudge whether the Document meets the requirements based on the Query and the Instruct provided. Note that the answer can only be "yes" or "no".<|im_end|>\n<|im_start|>user\n'
suffix='<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
def deep_score(index):
    prompt=prefix+f'<Instruct>: {task}\n<Query>: {q}\n<Document>: {docs[index]}'+suffix
    response=post(8189,'/completion',{'prompt':prompt,'n_predict':1,'temperature':0,'n_probs':128,'cache_prompt':True})
    choices=response.get('completion_probabilities',[{}])[0].get('top_logprobs',[])
    logprobs={x.get('token','').lower():x.get('logprob',-100.0) for x in choices}
    yes=logprobs.get('yes');no=logprobs.get('no')
    if yes is None or no is None:
        score=50.0 if response.get('content','').strip().lower().startswith('yes') else -50.0
    else:score=yes-no
    return index,score,response.get('tokens_evaluated',0)
started=time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:deep=list(executor.map(deep_score,deep_indices))
deep_by_index={index:(score,tokens) for index,score,tokens in deep}
fast_rank={item['index']:rank for rank,item in enumerate(fast_results,1)}
deep_rank={index:rank for rank,index in enumerate(sorted(deep_indices,key=lambda i:deep_by_index[i][0],reverse=True),1)}
all_indices=range(len(docs))
lexical_positive=sorted((i for i in all_indices if lexical_scores[i]>0),key=lambda i:(-lexical_scores[i],fast_rank[i]))
lexical_rank={index:rank for rank,index in enumerate(lexical_positive,1)}
fusion={index:'''+str(RRF_PREFILTER_WEIGHT)+r'''/(index+1)+'''+str(RRF_FAST_WEIGHT)+r'''/fast_rank[index]+('''+str(RRF_DEEP_WEIGHT)+r'''/deep_rank[index] if index in deep_rank else 0.0)+('''+str(RRF_LEXICAL_WEIGHT)+r'''/lexical_rank[index] if index in lexical_rank else 0.0) for index in all_indices}
results=[]
fast_by_index={item['index']:item['relevance_score'] for item in fast_results}
for index in sorted(all_indices,key=lambda i:fusion[i],reverse=True):
    deep_score_value,deep_tokens=deep_by_index.get(index,(None,0))
    results.append({'index':index,'relevance_score':fusion[index],'prefilter_rank':index+1,'fast_rank':fast_rank[index],'deep_rank':deep_rank.get(index),'lexical_rank':lexical_rank.get(index),'fast_rerank_score':fast_by_index[index],'deep_rerank_score':deep_score_value,'lexical_score':lexical_scores[index],'stage':'fused' if index in deep_by_index else 'fast','deep_tokens':deep_tokens})
meta={'model':'qwen3-reranker-4b-q4_k_m','fast_model':'bge-reranker-v2-m3-q8_0','candidate_count':len(docs),'deep_candidate_count':len(deep_indices),'prompt_tokens':fast.get('usage',{}).get('total_tokens',0),'deep_seconds':time.monotonic()-started}
for result in results:result.update(meta)
print(json.dumps({'results':results,**meta}))'''
    return json.loads(matrix_call(code,8189,240))['results']
def search(db,query,k=40,top=8):
    q=embed([query],'Query')[0]; scores=[cos(q,v) for v in db['vectors']]
    semantic=sorted(range(len(db['items'])),key=lambda i:scores[i],reverse=True)[:k]
    classification=classify(query); cls={x['label']:x['score'] for x in classification}
    low=min(cls.values()); span=max(cls.values())-low or 1.0
    def prefilter(i):return scores[i]+.08*(cls.get(FILE_DOMAIN.get(db['items'][i]['file'],'other'),low)-low)/span
    ranked=sorted(semantic,key=prefilter,reverse=True)[:RERANK_LIMIT]
    rr=rerank(query,[db['items'][i]['card'] for i in ranked],[db['items'][i]['name'] for i in ranked]); out=[]
    for rank,r in enumerate(rr[:top],1):
        i=ranked[r['index']]; domain=FILE_DOMAIN.get(db['items'][i]['file'],'other');x=dict(db['items'][i]);x.update(rank=rank,domain=domain,classification_score=cls[domain],prefilter_score=prefilter(i),prefilter_rank=r['prefilter_rank'],fast_rank=r['fast_rank'],deep_rank=r['deep_rank'],lexical_rank=r['lexical_rank'],rerank_score=r['relevance_score'],fast_rerank_score=r['fast_rerank_score'],deep_rerank_score=r['deep_rerank_score'],lexical_score=r['lexical_score'],rerank_stage=r['stage'],rerank_candidates=r['candidate_count'],deep_rerank_candidates=r['deep_candidate_count'],rerank_prompt_tokens=r['prompt_tokens'],deep_rerank_seconds=r['deep_seconds'],cosine=scores[i]);x.pop('card',None);out.append(x)
    return out,classification
def main():
    ap=argparse.ArgumentParser();ap.add_argument('query',nargs='?');ap.add_argument('--build',action='store_true');ap.add_argument('--force',action='store_true');ap.add_argument('-k','--k',type=int,default=40);ap.add_argument('--top',type=int,default=8);a=ap.parse_args();db=build(a.force)
    if a.build and not a.query:print(json.dumps({'indexed':len(db['items']),'dims':db['dims'],'fingerprint':db['fingerprint'],'cache':str(CACHE)},indent=2));return
    if not a.query:ap.error('query required unless --build')
    results,classification=search(db,a.query,a.k,a.top)
    print(json.dumps({'query':a.query,'models':{'classification':CLASSIFIER_MODEL,'retrieval':MODEL,'fast_reranker':FAST_RERANKER_MODEL,'reranker':RERANKER_MODEL},'classification':classification,'results':results},indent=2))
if __name__=='__main__':main()
