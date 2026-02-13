# syntax=docker/dockerfile:1

FROM python:3.12-alpine3.19

ENV PYCURL_SSL_LIBRARY=openssl

# Install dependencies
RUN apk add --no-cache ffmpeg libcurl

WORKDIR /app

COPY requirements.txt requirements.txt
RUN apk add --no-cache --virtual .build-deps gcc musl-dev curl-dev && \
    pip3 install pycurl && \
    apk del .build-deps
RUN pip3 install -r requirements.txt

COPY *.py ./
COPY streamonitor ./streamonitor

EXPOSE 5000
CMD [ "python3", "Downloader.py"]

