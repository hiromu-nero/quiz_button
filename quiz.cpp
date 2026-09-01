#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <windows.h>
#include <conio.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

// ★ポート設定 (ご自身の環境に合わせてCOM番号を変更してください)
// ※ COMポート番号が10以上の場合は必ず "\\\\.\\COM10" のように指定する必要があります。
const char* PORT_NAME_1 = "\\\\.\\COM3"; 
const char* PORT_NAME_2 = "\\\\.\\COM4"; 

// 音声データポインタ
Mix_Chunk *sound_flag = NULL;
Mix_Chunk *sound_true = NULL;
Mix_Chunk *sound_false = NULL;

// --- シリアルポート設定 (Windows API) ---
HANDLE setup_serial(const char* portname) {
    HANDLE hSerial = CreateFileA(portname,
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                NULL);
    if (hSerial == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    DCB dcbSerialParams = {0};
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hSerial, &dcbSerialParams)) {
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    // ★通信速度: 115200bps
    dcbSerialParams.BaudRate = CBR_115200;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity   = NOPARITY;

    if (!SetCommState(hSerial, &dcbSerialParams)) {
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    // ノンブロッキング(即時タイムアウト)設定
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial, &timeouts);

    return hSerial;
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

// Arduinoへのコマンド送信
void send_cmd(HANDLE hSerial, const char* cmd) {
    if (hSerial != INVALID_HANDLE_VALUE) {
        DWORD bytesWritten;
        WriteFile(hSerial, cmd, strlen(cmd), &bytesWritten, NULL);
    }
}

// バッファに残ったゴミデータ（再生中の連打など）を消去
void flush_buffers(HANDLE hSerial1, HANDLE hSerial2) {
    if (hSerial1 != INVALID_HANDLE_VALUE) PurgeComm(hSerial1, PURGE_RXCLEAR | PURGE_TXCLEAR);
    if (hSerial2 != INVALID_HANDLE_VALUE) PurgeComm(hSerial2, PURGE_RXCLEAR | PURGE_TXCLEAR);
    
    // キーボードバッファのクリア (_kbhitがtrueの間、_getchで空読みする)
    while (_kbhit()) {
        _getch();
    }
}

// シリアルからデータ読み込み
int read_serial(HANDLE hSerial, char* buf, int max_size) {
    if (hSerial == INVALID_HANDLE_VALUE) return 0;
    DWORD bytesRead = 0;
    if (ReadFile(hSerial, buf, max_size - 1, &bytesRead, NULL) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        return (int)bytesRead;
    }
    return 0;
}

int main(int argc, char* args[]) {
    // 1. 音声初期化
    if (!init_audio()) return 1;
    
    // Windowsではターミナルのrawモード設定はconio.hの _kbhit() / _getch() で代用するため、Linuxのような複雑な設定は不要です。

    // 2. シリアルポート接続
    HANDLE hSerial1 = setup_serial(PORT_NAME_1);
    HANDLE hSerial2 = setup_serial(PORT_NAME_2);

    if (hSerial1 == INVALID_HANDLE_VALUE && hSerial2 == INVALID_HANDLE_VALUE) {
        printf("Error: Arduinoが見つかりません。(COMポートの番号を確認してください)\n");
        return 1;
    }

    printf("--- 高速早押し判定システム (115200bps / SDL2 / Windows版) ---\n");
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
            if (read_serial(hSerial1, buf, sizeof(buf)) > 0) {
                winner_id = atoi(buf);
                if (winner_id > 0) winner_port = 1;
            }
            
            // B. シリアルポート監視 (Arduino 2)
            if (winner_port == 0 && read_serial(hSerial2, buf, sizeof(buf)) > 0) {
                winner_id = atoi(buf);
                if (winner_id > 0) winner_port = 2;
            }

            // C. キーボード監視 (待機中でも音を鳴らす)
            if (_kbhit()) {
                key_buf = _getch();
                if (key_buf == '1') {
                    play_sound_blocking(sound_true);
                    flush_buffers(hSerial1, hSerial2);
                } 
                else if (key_buf == '3') {
                    play_sound_blocking(sound_false);
                    flush_buffers(hSerial1, hSerial2);
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
        if (winner_port == 1) send_cmd(hSerial1, cmd);
        else send_cmd(hSerial2, cmd);

        printf("\n>> [判定] ポート%d : ボタン%d\n", winner_port, winner_id);

        // ★その直後に音を鳴らし、終わるまで他の入力をブロックする
        play_sound_blocking(sound_flag);
        
        // 再生中に連打されたデータを捨てる
        flush_buffers(hSerial1, hSerial2);

        // ====================================================
        // 3. 判定入力待ち
        // ====================================================
        printf("判定 >> [1]正解 [3]誤答 [Space]スルー\n");
        bool valid_input = false;
        
        while (!valid_input) {
            if (_kbhit()) {
                key_buf = _getch();
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
        send_cmd(hSerial1, "r");
        send_cmd(hSerial2, "r");
        flush_buffers(hSerial1, hSerial2);
        
        // 誤作動防止の短いウェイト
        SDL_Delay(100);
    }
    
EXIT_PROGRAM:
    if (hSerial1 != INVALID_HANDLE_VALUE) CloseHandle(hSerial1);
    if (hSerial2 != INVALID_HANDLE_VALUE) CloseHandle(hSerial2);
    cleanup_audio();
    return 0;
}