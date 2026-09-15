#!/usr/bin/env python3
"""Read native provider profiles and probe first-party catalogs, never print keys."""
import argparse,concurrent.futures,datetime,json,os,re,shlex,urllib.request,urllib.error
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def profiles():
    text=(ROOT/'src/provider_profiles.c').read_text().split('static const provider_profile_t PROVIDER_PROFILES[] = {',1)[1]
    rows=[]
    for block in re.split(r'\n\s*\.name = ',text)[1:]:
        def string(key):
            m=re.search(r'\.'+key+r'\s*=\s*"([^"]*)"',block);return m.group(1) if m else None
        name=re.match(r'"([^"]+)"',block).group(1)
        env=re.search(r'\.env_vars\s*=\s*\{([^}]+)\}',block)
        rows.append({'provider':name,'base_url':string('transport_base_url') or string('base_url'),
            'credential_names':re.findall(r'"([^"]+)"',env.group(1)) if env else [],
            'default_model':string('default_aux_model') or string('default_model'),
            'transport':(re.search(r'\.transport\s*=\s*(\w+)',block).group(1) if re.search(r'\.transport\s*=\s*(\w+)',block) else 'unknown')})
    return rows

def credentials():
    values={}
    path=Path.home()/'.dsco/env'
    if path.exists():
        for line in path.read_text().splitlines():
            try: parts=shlex.split(line,comments=True)
            except ValueError:continue
            if parts and parts[0]=='export':parts=parts[1:]
            if parts and '=' in parts[0]:k,v=parts[0].split('=',1);values[k]=v
    values.update(os.environ);return values

def discover(out):
    values=credentials();rows=profiles();out.mkdir(parents=True,exist_ok=True)
    def probe(row):
        row=dict(row);name=row['provider'];base=row['base_url'];keys=[k for k in row['credential_names'] if values.get(k)]
        row['configured_credential_names']=keys
        row['observed_at']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        row['native_transport_supported']=row['transport']!='PROVIDER_TRANSPORT_NONE'
        if not base or not base.startswith(('https://','http://')):row['status']='requires_endpoint_or_external_executor';return row
        local=name in ('local','ollama','lmstudio','mlx')
        row['credential_present']=bool(keys)
        row['catalog_auth']='credential' if keys else 'public_probe'
        url=base.rstrip('/')+'/models'
        if name=='anthropic':url=base.rstrip('/')+'/v1/models'
        headers={'User-Agent':'dsco-provider-inventory/1'}
        if keys:headers['Authorization']='Bearer '+values[keys[0]]
        if name=='anthropic':
            headers={'anthropic-version':'2023-06-01'}
            if keys:headers['x-api-key']=values[keys[0]]
        if name=='google':
            headers={'x-goog-api-key':values[keys[0]]} if keys else {};url='https://generativelanguage.googleapis.com/v1beta/models'
        row['catalog_url']=url
        try:
            with urllib.request.urlopen(urllib.request.Request(url,headers=headers),timeout=12) as r:data=json.load(r)
            items=data if isinstance(data,list) else (data.get('data') or data.get('models') or [])
            ids=[x.get('id',x.get('name')) for x in items if isinstance(x,dict)]
            row.update(status='catalog_ok',models=[x for x in ids if x],model_count=len(ids))
            (out/(name+'-models.json')).write_text(json.dumps(data,indent=2))
        except urllib.error.HTTPError as e:row['status']='HTTP_'+str(e.code)
        except Exception as e:row['status']=type(e).__name__
        return row
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:result=list(pool.map(probe,rows))
    (out/'inventory.json').write_text(json.dumps(result,indent=2))
    for row in result:print(json.dumps({k:row[k] for k in ('provider','status','model_count','configured_credential_names') if k in row}))
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();discover(a.output)
