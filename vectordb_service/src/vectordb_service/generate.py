from pathlib import Path


def main():
    from grpc_tools import protoc

    project_dir = Path(__file__).resolve().parents[2]
    proto_dir = project_dir / "proto"
    output_dir = project_dir / "src"
    proto_file = proto_dir / "vectordb.proto"
    if not proto_file.is_file():
        raise SystemExit("Run this command from an editable checkout containing proto/vectordb.proto")

    result = protoc.main(
        [
            "grpc_tools.protoc",
            f"-Ivectordb_service={proto_dir}",
            f"--python_out={output_dir}",
            f"--grpc_python_out={output_dir}",
            str(proto_file),
        ]
    )
    if result != 0:
        raise SystemExit(result)