CFLAGS+=-Wall -std=c++11 -fPIE
INCLUDES=-I../
HEADERS=$(wildcard *.h)
LDFLAGS=-L/usr/local/lib -lhiredis -pie

all: hiredis sample

sample:	sample.cc ../db/redis_db.o $(HEADERS)
	# $(MAKE) -C ./hiredis
	# $(MAKE) install -C ./hiredis
	$(MAKE) -C ../db 
	$(CXX) $(CFLAGS) $(INCLUDES) $< ../db/redis_db.o $(LDFLAGS) -o $@

clean:
	$(RM) sample
	$(MAKE) -C ./hiredis clean
