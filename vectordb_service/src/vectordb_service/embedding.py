DEFAULT_MODEL = "Alibaba-NLP/gte-multilingual-base"


class EmbeddingLayer:

    def __init__(self, model_name=DEFAULT_MODEL):
        try:
            from sentence_transformers import SentenceTransformer
        except ModuleNotFoundError as error:
            if error.name != "sentence_transformers":
                raise
            raise RuntimeError(
                "Embedding dependencies are missing. Run: uv sync --extra embedding"
            ) from error

        self.model = SentenceTransformer(model_name, trust_remote_code=True)

    def encode(self, text: str):
        return self.model.encode(text, normalize_embeddings=True)
