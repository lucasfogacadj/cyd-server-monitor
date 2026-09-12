# Stdlib pura: le do Prometheus e serve um JSON pequeno via http.server, entao
# nao ha nada para instalar e a imagem fica no tamanho da alpine base.
FROM python:3.12-alpine

LABEL org.opencontainers.image.title="tft-dashboard-api" \
      org.opencontainers.image.description="JSON compacto de saude do host para a telinha ESP32 (CYD)" \
      org.opencontainers.image.source="https://github.com/lucasfogacadj/cyd-server-monitor" \
      org.opencontainers.image.licenses="MIT"

WORKDIR /app
COPY api/tft_dashboard_api.py /app/tft_dashboard_api.py

ENV PYTHONUNBUFFERED=1
EXPOSE 9881

# 127.0.0.1 e nao "localhost": o wget do busybox resolve localhost para ::1
# primeiro e o servidor so faz bind em IPv4, dando connection refused.
HEALTHCHECK --interval=60s --timeout=5s --retries=3 --start-period=10s \
    CMD wget -q -O - http://127.0.0.1:9881/healthz || exit 1

CMD ["python", "/app/tft_dashboard_api.py"]
