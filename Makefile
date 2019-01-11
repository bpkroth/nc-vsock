VERSION = 1.0.1
TARFILE = nc-vsock-${VERSION}.tar.gz

DEBUG =

CFLAGS = -Wall -O -g $(DEBUG)

all: nc-vsock vsock-latency-benchmark

debug: DEBUG = -DDEBUG

debug: all

clean:
	rm -f nc-vsock vsock-latency-benchmark

rpm:
	wget -O ~/rpmbuild/SOURCES/${TARFILE} https://github.com/stefanha/nc-vsock/archive/v${VERSION}.tar.gz
	rpmbuild -ba nc-vsock.spec

rpm-local:
	git archive --format tar.gz --prefix nc-vsock-${VERSION}/ --output ~/rpmbuild/SOURCES/${TARFILE} HEAD
	rpmbuild -ba nc-vsock.spec
