CXXFLAGS += -std=c++20 -Wall -Wextra -Werror
CXXFLAGS += $(shell pkg-config --cflags dvdread)

ifo2mkv : ifo2mkv.o
	$(CXX) -o $@ $^ $(shell pkg-config --libs dvdread)

.PHONY : clean

clean :
	$(RM) ifo2mkv ifo2mkv.o
