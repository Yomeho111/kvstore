import argparse
import asyncio
import configparser
import grpc
import json
import signal
from pathlib import Path

from .server import Rag
from .embedding import DEFAULT_MODEL
from . import vectordb_pb2_grpc


def load_config(config_dir=None, server_config_path=None):
    service_dir = Path(__file__).resolve().parent
    project_dir = service_dir.parent.parent
    source_checkout = (project_dir / "pyproject.toml").is_file()

    if config_dir is not None:
        config_dir = Path(config_dir)
    elif source_checkout and (project_dir.parent / "config").is_dir():
        config_dir = project_dir.parent / "config"

    if server_config_path is not None:
        server_config_path = Path(server_config_path)
    elif source_checkout and (project_dir / "server_config.json").is_file():
        server_config_path = project_dir / "server_config.json"
    else:
        server_config_path = service_dir / "default_config.json"

    with server_config_path.open(encoding="utf-8") as config_file:
        pool_config = json.load(config_file)
    if not isinstance(pool_config, dict):
        raise ValueError(f"{server_config_path}: configuration must be a JSON object")

    parameters = {}
    for backend in ("vectordb", "kvstore"):
        pool_settings = pool_config.get(backend)
        if not isinstance(pool_settings, dict):
            raise ValueError(
                f"{server_config_path}: missing or invalid '{backend}' object"
            )
        for setting in ("max_connection", "timeout"):
            value = pool_settings.get(setting)
            if type(value) is not int or value <= 0:
                raise ValueError(
                    f"{server_config_path}: {backend}.{setting} must be a positive integer"
                )

        host = pool_settings.get("host", "127.0.0.1")
        port = pool_settings.get("port", 8060 if backend == "vectordb" else 8050)
        location = f"{server_config_path}: {backend}"
        if config_dir is not None:
            ini_path = config_dir / f"{backend}.ini"
            parser = configparser.ConfigParser(interpolation=None)
            with ini_path.open(encoding="utf-8") as config_file:
                parser.read_file(config_file, source=str(ini_path))
            if not parser.has_section("server"):
                raise ValueError(f"{ini_path}: missing [server] section")
            host = parser.get("server", "ip", fallback="127.0.0.1")
            try:
                port = parser.getint("server", "port", fallback=None)
            except ValueError as error:
                raise ValueError(
                    f"{ini_path}: [server] port must be an integer"
                ) from error
            location = f"{ini_path}: [server]"

        if not isinstance(host, str) or not host.strip():
            raise ValueError(f"{location} host must not be empty")
        host = host.strip()
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError(f"{location} port must be between 1 and 65535")

        parameters.update(
            {
                f"{backend}_host": host,
                f"{backend}_port": port,
                f"{backend}_max_connections": pool_settings["max_connection"],
                f"{backend}_timeout": pool_settings["timeout"],
            }
        )

    return parameters


async def serve(
    config_dir=None,
    server_config_path=None,
    listen="127.0.0.1:50051",
    model_name=DEFAULT_MODEL,
):
    config = load_config(config_dir, server_config_path)
    rag = Rag(**config, model_name=model_name)
    server = grpc.aio.server()

    try:
        vectordb_pb2_grpc.add_RagServicer_to_server(rag, server)
        if server.add_insecure_port(listen) == 0:
            raise RuntimeError(f"Cannot listen on {listen}")
        await server.start()
        print(f"Async gRPC server started on {listen}", flush=True)
        await server.wait_for_termination()
    finally:
        try:
            await server.stop(grace=5)
        finally:
            await rag.aclose()


async def _run_with_signals(**settings):
    loop = asyncio.get_running_loop()
    task = asyncio.current_task()
    registered = False
    if task is not None:
        try:
            loop.add_signal_handler(signal.SIGTERM, task.cancel)
            registered = True
        except NotImplementedError:
            pass
    try:
        await serve(**settings)
    except asyncio.CancelledError:
        pass
    finally:
        if registered:
            loop.remove_signal_handler(signal.SIGTERM)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run the kvstore vector gRPC service")
    parser.add_argument(
        "--config-dir", type=Path, help="Directory containing the backend INI files"
    )
    parser.add_argument(
        "--server-config",
        type=Path,
        dest="server_config_path",
        help="Connection settings JSON",
    )
    parser.add_argument("--listen", default="127.0.0.1:50051", help="gRPC host:port")
    parser.add_argument(
        "--model",
        default=DEFAULT_MODEL,
        dest="model_name",
        help="768-dimensional model name or local path",
    )
    arguments = parser.parse_args(argv)
    try:
        asyncio.run(_run_with_signals(**vars(arguments)))
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, configparser.Error, RuntimeError) as error:
        parser.exit(2, f"vectordb-server: {error}\n")


if __name__ == "__main__":
    main()
