import json
from pathlib import Path

import pytest

from vectordb_service import run


@pytest.fixture
def configuration(tmp_path):
    config_dir = tmp_path / "config"
    config_dir.mkdir()
    for backend, port in (("kvstore", 9100), ("vectordb", 9200)):
        (config_dir / f"{backend}.ini").write_text(
            f"[server]\nip = 10.0.0.1\nport = {port}\n", encoding="utf-8"
        )
    settings = {
        backend: {"max_connection": 12, "timeout": 3}
        for backend in ("kvstore", "vectordb")
    }
    json_path = tmp_path / "server_config.json"
    json_path.write_text(json.dumps(settings), encoding="utf-8")
    return config_dir, json_path, settings


def test_explicit_configuration(configuration, tmp_path, monkeypatch):
    config_dir, json_path, _ = configuration
    monkeypatch.chdir(tmp_path)
    parsed = run.load_config(config_dir, json_path)
    assert parsed == {
        "kvstore_host": "10.0.0.1",
        "kvstore_port": 9100,
        "kvstore_max_connections": 12,
        "kvstore_timeout": 3,
        "vectordb_host": "10.0.0.1",
        "vectordb_port": 9200,
        "vectordb_max_connections": 12,
        "vectordb_timeout": 3,
    }


def test_source_checkout_defaults(configuration, tmp_path, monkeypatch):
    config_dir, json_path, _ = configuration
    project_dir = tmp_path / "vectordb_service"
    package_dir = project_dir / "src" / "vectordb_service"
    package_dir.mkdir(parents=True)
    (project_dir / "pyproject.toml").touch()
    json_path.rename(project_dir / "server_config.json")
    monkeypatch.setattr(run, "__file__", str(package_dir / "run.py"))
    monkeypatch.chdir("/")
    parsed = run.load_config()
    assert parsed["kvstore_port"] == 9100
    assert parsed["vectordb_port"] == 9200
    assert parsed["vectordb_max_connections"] == 12


def test_installed_package_defaults(tmp_path, monkeypatch):
    real_defaults = Path(run.__file__).with_name("default_config.json")
    package_dir = tmp_path / "site-packages" / "vectordb_service"
    package_dir.mkdir(parents=True)
    (package_dir / "default_config.json").write_bytes(real_defaults.read_bytes())
    monkeypatch.setattr(run, "__file__", str(package_dir / "run.py"))
    parsed = run.load_config()
    assert parsed["kvstore_port"] == 8050
    assert parsed["vectordb_port"] == 8060
    assert parsed["kvstore_host"] == "127.0.0.1"


def test_json_only_configuration(configuration, tmp_path, monkeypatch):
    _, json_path, settings = configuration
    settings["kvstore"].update(host="kv.example", port=9300)
    settings["vectordb"].update(host="vectors.example", port=9400)
    json_path.write_text(json.dumps(settings), encoding="utf-8")
    monkeypatch.setattr(
        run, "__file__", str(tmp_path / "installed" / "package" / "run.py")
    )
    parsed = run.load_config(server_config_path=json_path)
    assert parsed["kvstore_host"] == "kv.example"
    assert parsed["vectordb_port"] == 9400


@pytest.mark.parametrize("port", ["0", "65536", "abc", ""])
def test_invalid_ini_port(configuration, port):
    config_dir, json_path, _ = configuration
    (config_dir / "kvstore.ini").write_text(
        f"[server]\nport={port}\n", encoding="utf-8"
    )
    with pytest.raises(ValueError, match="port"):
        run.load_config(config_dir, json_path)


@pytest.mark.parametrize(
    "setting,value",
    [
        ("max_connection", 0),
        ("max_connection", True),
        ("timeout", -1),
        ("timeout", "5"),
    ],
)
def test_invalid_pool_settings(configuration, setting, value):
    config_dir, json_path, settings = configuration
    settings["kvstore"][setting] = value
    json_path.write_text(json.dumps(settings), encoding="utf-8")
    with pytest.raises(ValueError, match="positive integer"):
        run.load_config(config_dir, json_path)


@pytest.mark.parametrize("contents", ["[]", "{}", "{"])
def test_invalid_json(configuration, contents):
    config_dir, json_path, _ = configuration
    json_path.write_text(contents, encoding="utf-8")
    with pytest.raises(ValueError):
        run.load_config(config_dir, json_path)


def test_missing_ini(configuration):
    config_dir, json_path, _ = configuration
    (config_dir / "kvstore.ini").unlink()
    with pytest.raises(FileNotFoundError):
        run.load_config(config_dir, json_path)


def test_missing_server_section(configuration):
    config_dir, json_path, _ = configuration
    (config_dir / "kvstore.ini").write_text(
        "[persistence]\nmode=none\n", encoding="utf-8"
    )
    with pytest.raises(ValueError, match="missing"):
        run.load_config(config_dir, json_path)
