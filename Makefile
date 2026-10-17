# Desktop build (SDL2 + OpenGL 4.6 core). Console: bash ps5/build.sh
#   make hands   headset + Sense controller test (simulated hands on a PC)
CXX      ?= g++
CXXFLAGS ?= -O2 -g

build:
	mkdir -p $@

build/hands: test/hands.cpp platform/platform_sdl.cpp platform/vr_desktop.cpp platform/vr_eyes.cpp platform/*.h | build
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Iplatform $(shell pkg-config --cflags sdl2) -o $@ test/hands.cpp platform/platform_sdl.cpp platform/vr_desktop.cpp platform/vr_eyes.cpp $(shell pkg-config --libs sdl2 gl) -lm

hands: build/hands
	./build/hands

build/pulse-map: tools/pulse-map/main.cpp analysis/*.cpp analysis/*.h | build
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Ianalysis -o $@ tools/pulse-map/main.cpp analysis/analysis.cpp analysis/mapper.cpp

build/pulse-view: tools/pulse-view/main.cpp analysis/*.cpp analysis/*.h core/*.cpp core/*.h render/*.cpp render/*.h platform/platform_sdl.cpp platform/*.h | build
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Ianalysis -Icore -Irender -Iplatform $(shell pkg-config --cflags sdl2) -o $@ tools/pulse-view/main.cpp core/game.cpp render/render.cpp analysis/analysis.cpp analysis/mapper.cpp platform/platform_sdl.cpp $(shell pkg-config --libs sdl2 gl) -lm -lpthread

view: build/pulse-view
	./build/pulse-view "$(SONG)"

build/game-test: tests/game_test.cpp core/*.cpp core/*.h analysis/mapper.cpp analysis/analysis.cpp | build
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Icore -Ianalysis -o $@ tests/game_test.cpp core/game.cpp analysis/mapper.cpp analysis/analysis.cpp

test: build/game-test
	./build/game-test

GAME_SRCS = game/main.cpp game/library.cpp game/scores.cpp game/tags.cpp core/game.cpp render/render.cpp analysis/analysis.cpp analysis/mapper.cpp audio/mp3.cpp platform/platform_sdl.cpp platform/vr_desktop.cpp platform/vr_eyes.cpp
build/game: $(GAME_SRCS) game/*.cpp core/*.h render/*.h audio/*.h analysis/*.h platform/*.h | build
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Icore -Irender -Ianalysis -Iaudio -Iplatform -Igame -Ithird_party $(shell pkg-config --cflags sdl2) -o $@ $(GAME_SRCS) $(shell pkg-config --libs sdl2 gl) -lm -lpthread

play: build/game
	./build/game $(ARGS)
