import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import model_lab_watch as watch


def feed(base, status, models=()):
    base.mkdir(parents=True)
    (base / 'inventory.json').write_text(json.dumps([
        {'provider': 'openai', 'status': status, 'catalog_auth': 'credential',
         'catalog_url': 'https://api.openai.com/v1/models'}]))
    if status == 'catalog_ok':
        (base / 'openai-models.json').write_text(json.dumps({'data': [{'id': m} for m in models]}))


def test_successful_deltas_failures_and_proposals(tmp_path):
    output = tmp_path / 'watch'
    a, b, c, d = [tmp_path / x for x in 'abcd']
    feed(a, 'catalog_ok', ['gpt-base'])
    feed(b, 'catalog_ok', ['gpt-base', 'gpt-new', 'bad`markdown'])
    feed(c, 'HTTP_401')
    feed(d, 'catalog_ok', ['gpt-new'])
    watch.run(a, output, now='2026-09-01T00-00-00Z')
    _, second = watch.run(b, output, now='2026-09-02T00-00-00Z')
    assert second['changes']['openai']['added'] == ['gpt-new']
    assert (output / 'runs/2026-09-02T00-00-00Z/proposals/openai.md').is_file()
    assert 'bad`markdown' not in (output / 'runs/2026-09-02T00-00-00Z/UPDATES.md').read_text()
    _, failure = watch.run(c, output, now='2026-09-03T00-00-00Z')
    assert failure['changes']['openai']['state'] == 'unverified'
    assert not failure['changes']['openai']['removed']
    assert watch.read(output / 'latest.json')['observations']['openai']['ids'] == ['gpt-base', 'gpt-new']
    _, last = watch.run(d, output, now='2026-09-04T00-00-00Z')
    assert last['changes']['openai']['removed'] == ['gpt-base']
    assert 'confirm' in (output / 'runs/2026-09-04T00-00-00Z/proposals/openai.md').read_text().lower()


def test_historical_gateway_catalog_can_be_compared(tmp_path):
    base = tmp_path / 'census/provider-inventory'
    feed(base, 'catalog_ok', ['gpt-base'])
    gateway = tmp_path / 'census/gateways'
    gateway.mkdir()
    (gateway / 'vercel-status.json').write_text(json.dumps({'http_status': 200, 'catalog_url': 'https://example.org/models'}))
    (gateway / 'vercel-models.json').write_text(json.dumps({'data': [{'id': 'openai/gpt-archive'}]}))
    rows, _, _ = watch.extract(base)
    assert rows['vercel']['ids'] == ['openai/gpt-archive']
    assert rows['vercel']['scope'] == 'historical_gateway_catalog'


def test_first_observation_is_not_claimed_as_new_release(tmp_path):
    feed(tmp_path / 'sample', 'catalog_ok', ['gpt-new'])
    _, run = watch.run(tmp_path / 'sample', tmp_path / 'out', now='2026-09-01T00-00-00Z')
    assert run['changes']['openai']['state'] == 'new_baseline'
    assert not run['changes']['openai']['added']
    overlay = watch.read(tmp_path / 'capability-sources/latest.json')
    assert overlay['summary']['lanes'] == sum(1 for _ in (watch.ROOT / 'reports/model-labs/current/routes.jsonl').open())
    assert overlay['routes'][0]['verified_context_tokens'] is None
