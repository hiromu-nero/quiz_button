#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <iostream>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

// ★通信速度: 115200bps
const speed_t BAUDRATE = B115200; 

// ★ポート設定 (環境に合わせて変更してください)
const char* PORT_NAME_1 = "/dev/ttyUSB0"; 
const char* PORT_NAME_2 = "/dev/ttyUSB1"; 

// 音声データポインタ
Mix_Chunk *sound_flag = NULL;
Mix_Chunk *sound_true = NULL;
Mix_Chunk *sound_false = NULL;

struct termios orig_termios;

// --- 端末制御関数 (キー入力用) ---
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO); // Enter不要、文字非表示
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0; // ノンブロッキング
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

// --- シリアルポート設定 ---
int setup_serial(const char* portname) {
    int fd = open(portname, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return -1;
    
    cfsetospeed(&tty, BAUDRATE); 
    cfsetispeed(&tty, BAUDRATE);
    
    tty.c_cflag &= ~PARENB; tty.c_cflag &= ~CSTOPB; tty.c_cflag &= ~CSIZE; tty.c_cflag |= CS8;
    tty.c_lflag &= ~ICANON; tty.c_lflag &= ~ECHO; tty.c_lflag &= ~ECHOE; tty.c_lflag &= ~ISIG;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tcsetattr(fd, TCSANOW, &tty);
    return fd;
}

// --- 音声初期化 ---
bool init_audio() {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) return false;
    // 44.1kHz, ステレオ, 2048チャンクサイズ
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) return false;
    
    sound_flag  = Mix_LoadWAV("flag.mp3");
    sound_true  = Mix_LoadWAV("true.mp3");
    sound_false = Mix_LoadWAV("false.mp3");

    if (!sound_flag || !sound_true || !sound_false) {
        printf("Error: 音声ファイル(flag.mp3等)が見つかりません。\n");
        return false;
    }
    return true;
}

// --- 音声再生・ブロック処理 ---
void play_sound_blocking(Mix_Chunk* chunk) {
    if (!chunk) return;
    Mix_PlayChannel(-1, chunk, 0);

    // 再生中はループして待ち、処理をブロックする
    while (Mix_Playing(-1) != 0) {
        SDL_Delay(10); 
    }
}

void cleanup_audio() {
    Mix_FreeChunk(sound_flag);
    Mix_FreeChunk(sound_true);
    Mix_FreeChunk(sound_false);
    Mix_Quit();
    SDL_Quit();
}

void send_cmd(int fd, const char* cmd) {
    if (fd >= 0) write(fd, cmd, strlen(cmd));
}

// バッファに残ったゴミデータ（再生中の連打など）を消去
void flush_buffers(int fd1, int fd2) {
    if (fd1 >= 0) tcflush(fd1, TCIFLUSH);
    if (fd2 >= 0) tcflush(fd2, TCIFLUSH);
    char k; while(read(STDIN_FILENO, &k, 1) > 0);
}

int main() {
    // 1. 音声初期化
    if (!init_audio()) return 1;
    
    // 2. キー入力モード変更
    enable_raw_mode();

    // 3. シリアルポート接続
    int fd1 = setup_serial(PORT_NAME_1);
    int fd2 = setup_serial(PORT_NAME_2);

    if (fd1 < 0 && fd2 < 0) {
        printf("Error: Arduinoが見つかりません。\n");
        return 1;
    }

    printf("--- 高速早押し判定システム (115200bps / SDL2) ---\n");
    printf("操作: [1]正解 [3]誤答 [Space]無効 [q]終了\n");

    char buf[32];
    char key_buf;
    int winner_port = 0;
    int winner_id = 0;

    while (1) {
        printf("\r待機中...                     "); 
        fflush(stdout);
        
        winner_port = 0;
        
        // ====================================================
        // 1. 待機ループ (シリアル & キーボード監視)
        // ====================================================
        while (winner_port == 0) {
            
            // A. シリアルポート監視 (Arduino 1)
            if (fd1 >= 0) {
                int n = read(fd1, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    winner_id = atoi(buf);
                    if (winner_id > 0) winner_port = 1;
                }
            }
            // B. シリアルポート監視 (Arduino 2)
            if (winner_port == 0 && fd2 >= 0) {
                int n = read(fd2, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    winner_id = atoi(buf);
                    if (winner_id > 0) winner_port = 2;
                }
            }

            // C. キーボード監視 (待機中でも音を鳴らす)
            if (read(STDIN_FILENO, &key_buf, 1) > 0) {
                if (key_buf == '1') {
                    play_sound_blocking(sound_true);
                    flush_buffers(fd1, fd2);
                } 
                else if (key_buf == '3') {
                    play_sound_blocking(sound_false);
                    flush_buffers(fd1, fd2);
                } 
                else if (key_buf == 'q') {
                    goto EXIT_PROGRAM;
                }
            }
            
            SDL_Delay(1); // CPU負荷対策
        }

        // ====================================================
        // 2. 早押し発生時の処理
        // ====================================================
        
        char cmd[16];
        sprintf(cmd, "o%d", winner_id);
        
        // ★最重要修正: 音を鳴らす「前」にLED点灯指令を送る！
        // これにより、音声の読み込みラグに関係なく、即座に光ります。
        if (winner_port == 1) send_cmd(fd1, cmd);
        else send_cmd(fd2, cmd);

        printf("\n>> [判定] ポート%d : ボタン%d\n", winner_port, winner_id);

        // ★その直後に音を鳴らし、終わるまで他の入力をブロックする
        play_sound_blocking(sound_flag);
        
        // 再生中に連打されたデータを捨てる
        flush_buffers(fd1, fd2);

        // ====================================================
        // 3. 判定入力待ち
        // ====================================================
        printf("判定 >> [1]正解 [3]誤答 [Space]スルー\n");
        bool valid_input = false;
        
        while (!valid_input) {
            if (read(STDIN_FILENO, &key_buf, 1) > 0) {
                if (key_buf == '1') {
                    printf("【正解】\n");
                    play_sound_blocking(sound_true);
                    valid_input = true;
                } 
                else if (key_buf == '3') {
                    printf("【誤答】\n");
                    play_sound_blocking(sound_false);
                    valid_input = true;
                }
                else if (key_buf == ' ') {
                    printf("【スルー】\n");
                    valid_input = true;
                }
            }
            SDL_Delay(1);
        }
        
        // リセット送信
        send_cmd(fd1, "r");
        send_cmd(fd2, "r");
        flush_buffers(fd1, fd2);
        
        // 誤作動防止の短いウェイト
        SDL_Delay(100);
    }
    
EXIT_PROGRAM:
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    cleanup_audio();
    return 0;
}
