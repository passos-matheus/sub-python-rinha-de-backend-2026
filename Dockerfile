FROM python:3.12-slim
WORKDIR /app
RUN pip install --no-cache-dir socketify
COPY api.py .
CMD ["python", "api.py"]
