#!/usr/bin/env python3
"""Local Jina reranker-v1-tiny-en ONNX runner."""
import argparse,json,math,time
import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer
BASE='/Volumes/Storage/LMStudio/models/jinaai/jina-reranker-v1-tiny-en'
TOK=Tokenizer.from_file(BASE+'/tokenizer.json')
SESSION=ort.InferenceSession(BASE+'/onnx/model_int8.onnx',providers=['CPUExecutionProvider'])
def rerank(query,docs,max_length=1024):
 pairs=[(query,d) for d in docs]; enc=TOK.encode_batch(pairs,add_special_tokens=True)
 ids=[]; masks=[]; types=[]
 for e in enc:
  x=e.ids[:max_length]; m=[1]*len(x); t=(e.type_ids or [0]*len(x))[:max_length]; ids.append(x);masks.append(m);types.append(t)
 n=max(map(len,ids)); pad=TOK.token_to_id('[PAD]') or 0
 def arr(xs,val):return np.asarray([x+[val]*(n-len(x)) for x in xs],dtype=np.int64)
 feed={'input_ids':arr(ids,pad),'attention_mask':arr(masks,0)}
 names={x.name for x in SESSION.get_inputs()}
 if 'token_type_ids' in names:feed['token_type_ids']=arr(types,0)
 out=SESSION.run(None,feed)[0].reshape(-1); scores=(1/(1+np.exp(-out))).tolist()
 return sorted([{'index':i,'relevance_score':float(scores[i]),'document':d} for i,d in enumerate(docs)],key=lambda x:x['relevance_score'],reverse=True)
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--query',required=True);ap.add_argument('--documents-json',required=True);a=ap.parse_args();t=time.perf_counter();r=rerank(a.query,json.loads(a.documents_json));print(json.dumps({'seconds':time.perf_counter()-t,'results':r},ensure_ascii=False,indent=2))
if __name__=='__main__':main()
