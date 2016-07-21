PROGRAM = rmate

SRCS = $(wildcard *.cpp)
OBJS = $(SRCS:.cpp=.o)

INCLUDES = 
CPPFLAGS += -Wall -Wextra -Wno-missing-field-initializers $(INCLUDES)
#LDFLAGS = -L.
#LDLIBS +=

$(PROGRAM): $(OBJS)

$(PROGRAM).o: version.h

version.h:
	sh version.sh $(MSG_DEF) > $@

clean:
	$(RM) $(PROGRAM) $(OBJS) version.h

.PHONY: release clean
