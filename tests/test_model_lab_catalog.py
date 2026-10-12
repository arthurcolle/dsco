import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import model_lab_catalog as catalog


def attribution():
    return catalog.Attribution(catalog.read(catalog.REGISTRY))


def test_host_is_not_developer_and_ids_remain_distinct():
    a = attribution()
    assert a.identify("deepseek-v4.1-flash", "ramp-router")[0] == "deepseek"
    assert a.identify("inkling", "ramp-router")[0] == "thinkingmachines"
    assert a.identify("spacexai/grok-4.6", "vercel")[0] == a.identify("x-ai/grok-4.6", "openrouter")[0]
    assert a.identify("unattributed-model", "nvidia")[0] is None
    assert a.identify("unattributed-model", "groq")[0] is None


def test_unknown_publisher_is_not_relabelled_as_base_lab():
    a = attribution()
    assert a.identify("dev/glm46", "novita")[0] is None
    assert a.identify("unknown/llama-finetune", "together")[0] is None
    assert a.identify("alpindale/WizardLM-2-8x22B", "huggingface")[0] == "alpindale"
    assert a.identify("o100-unrelated", "openrouter")[0] is None


def test_packaging_and_dynamic_alias_attribution():
    a = attribution()
    assert a.identify("nim/meta/llama-3.3-70b-instruct", "together")[0] == "meta"
    assert a.identify("nim/nv-mistralai/mistral-nemo-12b-instruct", "together")[0] == "nv-mistralai"
    assert a.identify("~openai/gpt-astra-latest", "openrouter")[0] == "openai"
    assert a.labs["nv-mistralai"]["members"] == ["nvidia", "mistral"]
    assert a.labs["aion-labs"]["kind"] == "composite_service"


def test_duplicate_alias_is_an_error():
    import pytest
    with pytest.raises(ValueError, match="Duplicate lab alias"):
        catalog.Attribution({"labs": [
            {"id": "a", "aliases": ["shared"]}, {"id": "b", "aliases": ["SHARED"]}
        ]})


def test_complete_inventory_and_specialist_contracts(tmp_path, monkeypatch):
    def write(name, value):
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(value))
    write("inventory.json", {"lanes": [
        {"lane_id": "or1", "provider": "openrouter", "model": "deepseek/deepseek-v4.1-flash", "auth_ready": True},
        {"lane_id": "unknown", "provider": "novita", "model": "dev/glm46", "auth_ready": True},
    ]})
    write("gateways/inventory.json", {"lanes": [
        {"lane_id": "ramp1", "provider": "ramp-router", "model": "deepseek-v4.1-flash", "hosting_provider": "fireworks", "credential_present": True},
        {"lane_id": "jev", "provider": "vercel", "model": "typesafe-ai/jev", "credential_present": True, "type": "evaluation"},
    ]})
    write("gateways/ramp-router-models.json", {"data": [{"id": "deepseek-v4.1-flash", "owned_by": "fireworks"}]})
    write("gateways/vercel-models.json", {"data": [{"id": "typesafe-ai/jev", "type": "evaluation", "supported_parameters": ["tools"]}]})
    write("summary.json", {"openrouter_account_key_status": 401})
    write("provider-inventory/inventory.json", [])
    write("auth-lanes.json", {"lanes": []})
    monkeypatch.setattr(catalog, "profile_transports", lambda: {name: {"transport": "PROVIDER_TRANSPORT_OPENAI_CHAT"}
                          for name in ("openrouter", "novita", "ramp-router", "vercel")})
    result = catalog.build(tmp_path)
    assert {r["lane_id"] for r in result["routes"]} == {"or1", "unknown", "ramp1", "jev"}
    by_id = {r["lane_id"]: r for r in result["routes"]}
    assert by_id["ramp1"]["lab_id"] == "deepseek"
    assert by_id["ramp1"]["hosting_provider"] == "fireworks"
    assert not by_id["or1"]["qualification_candidate"]
    assert not by_id["unknown"]["qualification_candidate"]
    assert not by_id["jev"]["general_agent_worker_candidate"]
    assert result["summary"]["source_lane_records"] == 4
    assert result["summary"]["unresolved_lane_records"] == 1
    assert result["summary"]["concurrent_worker_capacity"] is None


def test_atomic_catalog_write_preserves_valid_json(tmp_path):
    path = tmp_path / "catalog.json"
    catalog.atomic_json(path, {"generation": 1})
    catalog.atomic_json(path, {"generation": 2})
    assert json.loads(path.read_text()) == {"generation": 2}
    assert list(tmp_path.iterdir()) == [path]
