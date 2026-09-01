# コンパイル設定
CXX = g++
# コンパイルオプション (-Wall:警告を表示, -O2:最適化)
CXXFLAGS = -Wall -O2
# リンクするライブラリ
LDFLAGS = -lSDL2 -lSDL2_mixer

# 生成する実行ファイル名とソース (quiz_system)
TARGET1 = quiz_system
SRC1 = quiz_system.cpp

# 生成する実行ファイル名とソース (quiz_buzzer)
TARGET2 = quiz_buzzer
SRC2 = quiz_buzzer.cpp

# make と打ったときに実行されるデフォルトターゲット（両方ビルドする）
all: $(TARGET1) $(TARGET2)

# quiz_system のコンパイルルール
$(TARGET1): $(SRC1)
	$(CXX) $(CXXFLAGS) $(SRC1) -o $(TARGET1) $(LDFLAGS)

# quiz_buzzer のコンパイルルール
$(TARGET2): $(SRC2)
	$(CXX) $(CXXFLAGS) $(SRC2) -o $(TARGET2) $(LDFLAGS)

# 生成ファイルを削除するルール (make clean で実行)
clean:
	rm -f $(TARGET1) $(TARGET2)

.PHONY: all clean
