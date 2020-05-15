FROM alpine:latest AS build_stage
RUN apk update
RUN apk add \
     		         autoconf \
             		 automake \
             		 build-base \
             		 ca-certificates \
             		 cargo \
             		 file-dev \
             		 geoip-dev \
             		 hiredis-dev \
             		 jansson-dev \
             		 libcap-ng-dev \
             		 libmaxminddb-dev \
             		 libnet-dev \
             		 libnetfilter_log-dev \
             		 libnetfilter_queue-dev \
             		 libnfnetlink-dev \
             		 libpcap-dev \
             		 libtool \
             		 luajit-dev \
             		 lz4-dev \
             		 nspr-dev \
             		 nss-dev \
             		 pcre-dev \
             		 py3-yaml \
             		 python3 \
             		 rust \
             		 yaml-dev \
             		 zlib-dev

RUN cargo install --force cbindgen
ENV PATH="/root/.cargo/bin:${PATH}"
COPY . .
RUN ./autogen.sh \
  && ./configure --disable-gccmarch-native \
  --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
  && make -j${nproc} \
  && make install DESTDIR=/suricata-docker \
  && make install-conf DESTDIR=/suricata-docker


FROM alpine:latest
RUN apk add file-dev \
    jansson-dev \
    libcap-ng-dev \
    libnet-dev \
    libpcap-dev \
    lz4-dev \
    nss-dev \
    pcre-dev \
    py-yaml \
    python3 \
    rsync \
    yaml-dev
RUN pip3 install awscli
WORKDIR /
COPY --from=build_stage suricata-docker/ /suricata-docker/
RUN rsync -rv --copy-links suricata-docker/* /
RUN rm -r suricata-docker/
COPY docker-entrypoint.sh /
RUN chmod +x docker-entrypoint.sh
ENTRYPOINT ["./docker-entrypoint.sh"]
