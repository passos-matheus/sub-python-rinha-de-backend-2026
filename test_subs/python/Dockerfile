FROM python:3.12-slim
WORKDIR /app
RUN pip install --no-cache-dir granian
COPY api.py .
CMD ["granian", "--interface", "asgi", "--host", "0.0.0.0", "--port", "3000", "--workers", "2", "--runtime-mode", "st", "--log-level", "error", "api:app"]
