/**
 * * 機能:
 * - Simple/Read/Battleモード
 * - 難易度選択 (Battle)
 * - ボタン別成績集計
 * - 読み上げ終了後3秒無操作でタイムアウト処理
 * * コンパイル:
 * make (Makefileを使用) または
 * g++ quiz_system_final_v7_fixed.cpp -o quiz_system -lSDL2 -lSDL2_mixer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/wait.h>
#include <signal.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

using namespace std;

// ==========================================
// ★設定
// ==========================================
const speed_t BAUDRATE = B115200; 
const char* PORT_NAME_1 = "/dev/ttyUSB0"; 
const char* PORT_NAME_2 = "/dev/ttyUSB1"; 
const Uint32 ANSWER_TIME_LIMIT_MS = 6000; // 早押し後の回答待ち時間
const Uint32 READ_FINISH_TIMEOUT_MS = 3000; // 読み上げ終了後の待機時間

Mix_Chunk *sound_flag = NULL;
Mix_Chunk *sound_true = NULL;
Mix_Chunk *sound_false = NULL;

struct termios orig_termios;

struct QuizData {
    int id;
    string question;
    string answer;
};

// 個別のボタン成績
struct ButtonStats {
    int correct = 0;
    int wrong = 0;
};

// 全体の成績管理用構造体
struct GameStats {
    ButtonStats p1_buttons[10];
    ButtonStats p2_buttons[10];
    ButtonStats pc_buttons[10];
    int p1_total_correct = 0;
    int p1_total_wrong = 0;
    int p2_total_correct = 0;
    int p2_total_wrong = 0;
    int slash_count = 0;
};

// ==========================================
// ★ユーティリティ
// ==========================================

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO); 
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

bool check_key_press(char *c) {
    if (read(STDIN_FILENO, c, 1) > 0) return true;
    return false;
}

int setup_serial(const char* portname) {
    int fd = open(portname, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }
    cfsetospeed(&tty, BAUDRATE); cfsetispeed(&tty, BAUDRATE);
    tty.c_cflag &= ~PARENB; tty.c_cflag &= ~CSTOPB; tty.c_cflag &= ~CSIZE; tty.c_cflag |= CS8;
    tty.c_cflag |= (CLOCAL | CREAD); tty.c_cflag &= ~CRTSCTS;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY); tty.c_oflag &= ~OPOST;
    tcsetattr(fd, TCSANOW, &tty); tcflush(fd, TCIFLUSH);
    return fd;
}

void send_cmd(int fd, const char* cmd) {
    if (fd >= 0) {
        // [修正] 戻り値を受け取って警告を抑制
        int ret = write(fd, cmd, strlen(cmd));
        (void)ret; // 未使用変数の警告消し
    }
}

void flush_buffers(int fd1, int fd2) {
    if (fd1 >= 0) tcflush(fd1, TCIFLUSH);
    if (fd2 >= 0) tcflush(fd2, TCIFLUSH);
    char k; while(read(STDIN_FILENO, &k, 1) > 0);
}

bool init_audio() {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) return false;
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) return false;
    sound_flag  = Mix_LoadWAV("flag.mp3");
    sound_true  = Mix_LoadWAV("true.mp3");
    sound_false = Mix_LoadWAV("false.mp3");
    if (!sound_flag || !sound_true || !sound_false) {
        printf("Error: 効果音ファイルが見つかりません。\n");
        return false;
    }
    return true;
}

void play_sound_blocking(Mix_Chunk* chunk) {
    if (!chunk) return;
    Mix_PlayChannel(-1, chunk, 0);
    while (Mix_Playing(-1) != 0) SDL_Delay(10); 
}

void play_mp3_blocking(const string& filename) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); dup2(devnull, STDIN_FILENO);
        execlp("mpg123", "mpg123", "-q", filename.c_str(), NULL);
        exit(1);
    } else {
        waitpid(pid, NULL, 0);
    }
}

void cleanup_audio() {
    Mix_FreeChunk(sound_flag); Mix_FreeChunk(sound_true); Mix_FreeChunk(sound_false);
    Mix_Quit(); SDL_Quit();
}

// ==========================================
// ★CSV読み込みロジックの修正
// ==========================================
vector<QuizData> loadCSV(const string& filename) {
    vector<QuizData> quizList;
    ifstream file(filename);
    if (!file.is_open()) return quizList;
    
    string line;
    bool first_line = true;
    
    while (getline(file, line)) {
        // 先頭のBOM(見えない文字)を除去
        if (first_line && line.size() >= 3 && 
            (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
            line = line.substr(3);
        }
        first_line = false;

        // すべての改行コード（\r, \n）を完全に削除
        line.erase(remove(line.begin(), line.end(), '\r'), line.end());
        line.erase(remove(line.begin(), line.end(), '\n'), line.end());

        if (line.empty()) continue;

        stringstream ss(line);
        string segment;
        QuizData data;

        // IDの取得
        if (!getline(ss, segment, ',')) continue;
        try { 
            data.id = stoi(segment); 
        } catch (...) { 
            continue; 
        }

        // ★第0問バグ対策: IDが0以下（不正な行）は登録しない
        if (data.id <= 0) continue;

        // 問題文の取得
        if (!getline(ss, data.question, ',')) continue;
        
        // ★749問バグ対策: 最後の回答要素はカンマがなくても行の最後まで取得する
        getline(ss, data.answer);

        quizList.push_back(data);
    }
    return quizList;
}

void generateAudio(const string& text, const string& filename) {
    remove(filename.c_str());
    string cmd = "gtts-cli \"" + text + "\" --lang ja --output " + filename;
    // [修正] systemの戻り値を受け取って警告抑制
    int ret = system(cmd.c_str());
    (void)ret;
}

string get_text_with_offset(const string& full_text, size_t slash_pos, int extra_chars) {
    if (slash_pos >= full_text.length()) return full_text;
    string part1 = full_text.substr(0, slash_pos);
    size_t start_idx = slash_pos + 1;
    size_t current_idx = start_idx;
    int count = 0;
    while (current_idx < full_text.length() && count < extra_chars) {
        current_idx++;
        while (current_idx < full_text.length() && (full_text[current_idx] & 0xC0) == 0x80) {
            current_idx++;
        }
        count++;
    }
    string part2 = full_text.substr(start_idx, current_idx - start_idx);
    return part1 + part2;
}

void update_stats(GameStats& stats, int port, int btn_id, bool is_correct) {
    if (port == 1) {
        if (is_correct) {
            stats.p1_total_correct++;
            if (btn_id >= 1 && btn_id <= 9) stats.p1_buttons[btn_id].correct++;
        } else {
            stats.p1_total_wrong++;
            if (btn_id >= 1 && btn_id <= 9) stats.p1_buttons[btn_id].wrong++;
        }
    } else if (port == 2) {
        if (is_correct) {
            stats.p2_total_correct++;
            if (btn_id >= 1 && btn_id <= 9) stats.p2_buttons[btn_id].correct++;
        } else {
            stats.p2_total_wrong++;
            if (btn_id >= 1 && btn_id <= 9) stats.p2_buttons[btn_id].wrong++;
        }
    } else if (port == 9) { 
        if (btn_id >= 1 && btn_id <= 9) {
            if (is_correct) stats.pc_buttons[btn_id].correct++;
            else stats.pc_buttons[btn_id].wrong++;
        }
    }
}

void print_stats(const GameStats& stats, const string& mode) {
    printf("\n=====================================\n");
    printf("         現在の成績レポート\n");
    printf("=====================================\n");
    
    printf("【Player 1】(Total 正解:%d / 誤答:%d)\n", stats.p1_total_correct, stats.p1_total_wrong);
    for (int i = 1; i <= 9; i++) {
        if (stats.p1_buttons[i].correct > 0 || stats.p1_buttons[i].wrong > 0) {
            printf("  - Button %d : 正解 %d / 誤答 %d\n", i, stats.p1_buttons[i].correct, stats.p1_buttons[i].wrong);
        }
    }

    printf("-------------------------------------\n");
    printf("【Player 2】(Total 正解:%d / 誤答:%d)\n", stats.p2_total_correct, stats.p2_total_wrong);
    for (int i = 1; i <= 9; i++) {
        if (stats.p2_buttons[i].correct > 0 || stats.p2_buttons[i].wrong > 0) {
            printf("  - Button %d : 正解 %d / 誤答 %d\n", i, stats.p2_buttons[i].correct, stats.p2_buttons[i].wrong);
        }
    }

    if (mode == "battle" || stats.slash_count > 0) {
        printf("-------------------------------------\n");
        printf("【Battleモード】\n");
        printf("  スラッシュ到達(自動正解): %d 回\n", stats.slash_count);
    }
    printf("=====================================\n");
}

// ==========================================
// モード: Simple
// ==========================================
void run_simple_mode(int fd1, int fd2, GameStats& stats) {
    printf("\n=== シンプル早押しモード ===\n");
    enable_raw_mode();
    char read_buf[64]; char key_buf;
    int winner_port = 0; int winner_id = 0;

    while (true) {
        winner_port = 0;
        while (winner_port == 0) {
            if (fd1 >= 0) {
                int n = read(fd1, read_buf, sizeof(read_buf));
                for(int i=0; i<n; i++) { if(read_buf[i]>='1' && read_buf[i]<='9') { winner_id=read_buf[i]-'0'; winner_port=1; break; } }
            }
            if(winner_port) break;
            if (fd2 >= 0) {
                int n = read(fd2, read_buf, sizeof(read_buf));
                for(int i=0; i<n; i++) { if(read_buf[i]>='1' && read_buf[i]<='9') { winner_id=read_buf[i]-'0'; winner_port=2; break; } }
            }
            if(winner_port) break;
            if (check_key_press(&key_buf)) {
                if (key_buf == 'q') return;
                if (key_buf == '1') { play_sound_blocking(sound_true); flush_buffers(fd1, fd2); }
                else if (key_buf == '3') { play_sound_blocking(sound_false); flush_buffers(fd1, fd2); }
            }
            usleep(100); 
        }

        if (winner_port != 0) {
            char cmd[16]; sprintf(cmd, "o%d", winner_id);
            if (winner_port == 1) send_cmd(fd1, cmd); else send_cmd(fd2, cmd);
            printf("\n>> [判定] Port:%d / Button:%d\n", winner_port, winner_id);
            play_sound_blocking(sound_flag); flush_buffers(fd1, fd2);

            printf("判定 >> [1]正解 [3]誤答 [Space]リセット\n");
            Uint32 start_wait = SDL_GetTicks();
            bool time_up_played = false; bool done = false;

            while (!done) {
                if (check_key_press(&key_buf)) {
                    if (key_buf=='1') { 
                        play_sound_blocking(sound_true); 
                        update_stats(stats, winner_port, winner_id, true);
                        done=true; 
                    }
                    else if (key_buf=='3') { 
                        play_sound_blocking(sound_false); 
                        update_stats(stats, winner_port, winner_id, false);
                        done=true; 
                    }
                    else if (key_buf==' ') done=true;
                }
                if (!done && !time_up_played && (SDL_GetTicks()-start_wait >= ANSWER_TIME_LIMIT_MS)) {
                     printf("\n>> 時間切れ\n"); play_sound_blocking(sound_false); time_up_played=true; 
                }
                SDL_Delay(10);
            }
            
            print_stats(stats, "simple");
            send_cmd(fd1, "r"); send_cmd(fd2, "r"); flush_buffers(fd1, fd2);
        }
    }
}

// ==========================================
// 共通ロジック: 問題ループ処理
// ==========================================
void run_quiz_loop(int fd1, int fd2, const string& csvFilename, bool isBattleMode, GameStats& stats) {
    vector<QuizData> quizList = loadCSV(csvFilename);
    if (quizList.empty()) { printf("Error: %s が読み込めません。\n", csvFilename.c_str()); return; }
    size_t total_questions = quizList.size();

    int extra_chars = 0;
    if (isBattleMode) {
        printf("\n=== Battleモード 難易度選択 ===\n");
        printf("[1] 難 (/で停止)\n");
        printf("[3] 普 (/から0.5秒後)\n");
        printf("[Space] 易 (/から1.0秒後)\n");
        enable_raw_mode();
        char k;
        bool selected = false;
        while (!selected) {
            if (check_key_press(&k)) {
                if (k == '1') { extra_chars = 0; printf(">> 選択: 難\n"); selected = true; } 
                else if (k == '3') { extra_chars = 3; printf(">> 選択: 普\n"); selected = true; } 
                else if (k == ' ') { extra_chars = 6; printf(">> 選択: 易\n"); selected = true; }
            }
            SDL_Delay(10);
        }
        disable_raw_mode();
    } else {
        printf("\n=== Readモード (全読み) ===\n");
    }

    printf("全 %zu 問。開始IDを入力: ", total_questions);
    // [修正] scanfの警告対応
    int startId = 1; 
    if(scanf("%d", &startId) != 1) startId = 1;
    
    int c; while ((c = getchar()) != '\n' && c != EOF);

    size_t startIndex = 0;
    for (size_t i = 0; i < total_questions; ++i) { if (quizList[i].id == startId) { startIndex = i; break; } }

    enable_raw_mode();
    char read_buf[64]; char key_buf;

    for (size_t count = 0; count < total_questions; ++count) {
        size_t current_idx = (startIndex + count) % total_questions;
        QuizData& q = quizList[current_idx];

        size_t slash_pos = string::npos;
        if (isBattleMode) {
            slash_pos = q.question.find('/');
        }
        bool has_slash = (slash_pos != string::npos);

        send_cmd(fd1, "r"); send_cmd(fd2, "r"); flush_buffers(fd1, fd2);

        printf("\n\n================================\n");
        printf("【第 %d 問】%s\n", q.id, has_slash ? " [Battle/Slash]" : "");
        printf("生成中...\n");

        disable_raw_mode();
        generateAudio("問題", "pre_question.mp3");
        string ans_text = "正解は、" + q.answer;
        generateAudio(ans_text, "answer.mp3");

        string question_text = q.question;
        if (has_slash) {
            question_text = get_text_with_offset(q.question, slash_pos, extra_chars);
        }
        generateAudio(question_text, "question.mp3");
        enable_raw_mode();

        printf("準備完了。[Space]開始 / [q]終了\n");
        while(true) {
            if(check_key_press(&key_buf)) {
                if(key_buf == ' ') break;
                if(key_buf == 'q') return;
            }
            SDL_Delay(10);
        }

        printf("問題\n");
        play_mp3_blocking("pre_question.mp3");
        SDL_Delay(300);

        printf("読み上げ中...\n");
        
        pid_t audio_pid = fork();
        bool audio_running = false;
        if (audio_pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); dup2(devnull, STDIN_FILENO);
            execlp("mpg123", "mpg123", "-q", "question.mp3", NULL);
            exit(1);
        } else {
            audio_running = true;
        }

        int winner_port = 0;
        int winner_id = 0;
        bool slash_triggered = false;
        bool time_out_triggered = false;
        Uint32 audio_finish_time = 0;

        while (winner_port == 0) {
            if (fd1 >= 0) {
                int n = read(fd1, read_buf, sizeof(read_buf));
                for(int j=0; j<n; j++) if(read_buf[j]>='1'&&read_buf[j]<='9'){ winner_id=read_buf[j]-'0'; winner_port=1; break; }
            }
            if(winner_port) break;

            if (fd2 >= 0) {
                int n = read(fd2, read_buf, sizeof(read_buf));
                for(int j=0; j<n; j++) if(read_buf[j]>='1'&&read_buf[j]<='9'){ winner_id=read_buf[j]-'0'; winner_port=2; break; }
            }
            if(winner_port) break;

            if(check_key_press(&key_buf)) {
                if(key_buf == 'q') { if(audio_running) kill(audio_pid, SIGTERM); return; }
                if(key_buf >= '1' && key_buf <= '4') { winner_id=key_buf-'0'; winner_port=9; break; }
            }

            if (audio_running) {
                if (waitpid(audio_pid, NULL, WNOHANG) > 0) {
                    audio_running = false;
                    audio_finish_time = SDL_GetTicks();
                    if (isBattleMode && has_slash) {
                        slash_triggered = true;
                        break; 
                    }
                }
            } else {
                if (audio_finish_time > 0 && (SDL_GetTicks() - audio_finish_time >= READ_FINISH_TIMEOUT_MS)) {
                    time_out_triggered = true;
                    break;
                }
            }
            usleep(100); 
        }

        // ==========================================
        // 分岐A: ユーザーがボタンを押した場合
        // ==========================================
        if (winner_port != 0) {
            if (audio_running) { kill(audio_pid, SIGTERM); waitpid(audio_pid, NULL, 0); audio_running = false; }
            
            char cmd[16]; sprintf(cmd, "o%d", winner_id);
            if(winner_port==1) send_cmd(fd1, cmd); else if(winner_port==2) send_cmd(fd2, cmd);

            printf("\n>> 早押し! Port:%d Button:%d\n", winner_port, winner_id);
            play_sound_blocking(sound_flag);
            flush_buffers(fd1, fd2);

            printf("判定待ち... [Space]で正解表示 (6秒経過で時間切れ音)\n");
            Uint32 start_wait = SDL_GetTicks();
            bool time_up_played = false; bool show_answer = false;
            while(!show_answer) {
                if(check_key_press(&key_buf)) {
                     if(key_buf == ' ') show_answer = true;
                     if(key_buf == 'q') return;
                }
                if (!show_answer && !time_up_played && (SDL_GetTicks()-start_wait >= ANSWER_TIME_LIMIT_MS)) {
                    printf("\n>> 時間切れ\n"); play_sound_blocking(sound_false); time_up_played = true; 
                }
                SDL_Delay(10);
            }
        }
        // ==========================================
        // 分岐B: スラッシュ到達 (自動正解)
        // ==========================================
        else if (slash_triggered) {
            printf("\n>> [Battle] 設定ポイント到達 (入力受付終了)\n");
            stats.slash_count++;
            
            play_sound_blocking(sound_flag);
            printf(">> 自動正解読み上げ\n");
            play_mp3_blocking("answer.mp3");
            play_sound_blocking(sound_true);
            
            print_stats(stats, isBattleMode ? "battle" : "read");
            flush_buffers(fd1, fd2);
        }
        // ==========================================
        // 分岐C: タイムアウト (3秒経過)
        // ==========================================
        else if (time_out_triggered) {
            printf("\n>> 時間切れ (読み上げ終了後3秒)\n");
            play_sound_blocking(sound_false); 
            printf(">> 正解読み上げ\n");
            play_mp3_blocking("answer.mp3");

            print_stats(stats, isBattleMode ? "battle" : "read");
            flush_buffers(fd1, fd2);
        }

        printf("--------------------\n");
        printf("Q: %s\n", q.question.c_str());
        printf("A: %s\n", q.answer.c_str());
        printf("--------------------\n");
        
        if (!slash_triggered && !time_out_triggered) {
            printf("[1]正解 [3]誤答 [Space]スルー\n");
            
            pid_t ans_pid = fork();
            bool ans_running = false;
            if (ans_pid == 0) {
                int devnull = open("/dev/null", O_WRONLY);
                dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); dup2(devnull, STDIN_FILENO);
                execlp("mpg123", "mpg123", "-q", "answer.mp3", NULL);
                exit(1);
            } else { ans_running = true; }

            bool judged = false;
            while(!judged) {
                if(check_key_press(&key_buf)) {
                    if(key_buf == '1') { 
                        play_sound_blocking(sound_true); 
                        update_stats(stats, winner_port, winner_id, true);
                        judged=true; 
                    }
                    else if(key_buf == '3') { 
                        play_sound_blocking(sound_false); 
                        update_stats(stats, winner_port, winner_id, false);
                        judged=true; 
                    }
                    else if(key_buf == ' ') { judged=true; }
                    else if(key_buf == 'q') { if (ans_running) kill(ans_pid, SIGTERM); return; }
                }
                if (ans_running && waitpid(ans_pid, NULL, WNOHANG) > 0) ans_running = false;
                SDL_Delay(10);
            }
            if (ans_running) { kill(ans_pid, SIGTERM); waitpid(ans_pid, NULL, 0); }
            
            print_stats(stats, isBattleMode ? "battle" : "read");
        } else {
            printf("[Space] 次の問題へ\n");
            while(true) {
                if(check_key_press(&key_buf) && key_buf == ' ') break;
                if(check_key_press(&key_buf) && key_buf == 'q') return;
                SDL_Delay(10);
            }
        }
        
        SDL_Delay(500);
    }
    printf("\n一巡しました。\n");
}

// ==========================================
// Main
// ==========================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s [simple|read|battle]\n", argv[0]);
        return 1;
    }

    if (!init_audio()) return 1;

    int fd1 = setup_serial(PORT_NAME_1);
    int fd2 = setup_serial(PORT_NAME_2);
    if (fd1 < 0 && fd2 < 0) printf("Warning: Arduinoなし\n");
    else printf("Arduino接続: %d, %d\n", fd1, fd2);

    GameStats stats;

    string mode = argv[1];
    if (mode == "simple") {
        run_simple_mode(fd1, fd2, stats);
    } else if (mode == "read") {
        run_quiz_loop(fd1, fd2, "quiz.csv", false, stats);
    } else if (mode == "battle") {
        run_quiz_loop(fd1, fd2, "slash.csv", true, stats);
    } else {
        printf("不明なモード: %s\n", mode.c_str());
    }

    // [修正] インデント修正
    if (fd1 >= 0) close(fd1); 
    if (fd2 >= 0) close(fd2);
    
    cleanup_audio();
    print_stats(stats, mode);

    return 0;
}
