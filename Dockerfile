FROM python:3.12-slim
WORKDIR /app
RUN apt-get update && apt-get install -y --no-install-recommends libuv1 && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir socketify
COPY api.py .
CMD ["python", "api.py"]
