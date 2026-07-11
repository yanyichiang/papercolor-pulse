FROM python:3.12-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends fonts-noto-cjk \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY pyproject.toml README.md ./
COPY src ./src
RUN pip install --no-cache-dir .

RUN useradd --system --uid 10001 --home /nonexistent --shell /usr/sbin/nologin papercolor \
    && mkdir -p /data \
    && chown papercolor:papercolor /data

USER papercolor
EXPOSE 8767
CMD ["papercolor-gateway"]
