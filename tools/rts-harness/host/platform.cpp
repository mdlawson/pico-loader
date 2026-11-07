// Headless Platform implementation for melonDS's core.
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <filesystem>
#include "Platform.h"

namespace melonDS::Platform
{
struct FileHandle { FILE* f; };

int gLogVerbose = 0;
int gFrame = 0;
std::map<std::string, unsigned> gLogCounts;

void SignalStop(StopReason reason, void*) { fprintf(stderr, "[melonDS] stop requested, reason %d\n", (int)reason); }
std::string GetLocalFilePath(const std::string& filename) { return filename; }

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    bool r = (mode & FileMode::Read) != FileMode::None, w = (mode & FileMode::Write) != FileMode::None;
    bool preserve = (mode & FileMode::Preserve) != FileMode::None, nocreate = (mode & FileMode::NoCreate) != FileMode::None;
    bool append = (mode & FileMode::Append) != FileMode::None;
    const char* m;
    if (append) m = r ? "a+b" : "ab";
    else if (r && w) {
        if (preserve || nocreate) { if (!std::filesystem::exists(path)) { if (nocreate) return nullptr; FILE* c = fopen(path.c_str(), "wb"); if (c) fclose(c); } m = "r+b"; }
        else m = "w+b";
    }
    else if (w) { if (preserve && std::filesystem::exists(path)) m = "r+b"; else { if (nocreate && !std::filesystem::exists(path)) return nullptr; m = "wb"; } }
    else m = "rb";
    FILE* f = fopen(path.c_str(), m);
    if (!f) return nullptr;
    return new FileHandle{f};
}
FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(path, mode); }
bool FileExists(const std::string& name) { return std::filesystem::exists(name); }
bool LocalFileExists(const std::string& name) { return FileExists(name); }
bool CheckFileWritable(const std::string&) { return true; }
bool CheckLocalFileWritable(const std::string&) { return true; }
bool CloseFile(FileHandle* file) { if (!file) return false; fclose(file->f); delete file; return true; }
bool IsEndOfFile(FileHandle* file) { return feof(file->f); }
bool FileReadLine(char* str, int count, FileHandle* file) { return fgets(str, count, file->f) != nullptr; }
u64 FilePosition(FileHandle* file) { return ftello(file->f); }
bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{ int o = origin == FileSeekOrigin::Start ? SEEK_SET : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END; return fseeko(file->f, offset, o) == 0; }
void FileRewind(FileHandle* file) { rewind(file->f); }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file) { return fread(data, size, count, file->f); }
bool FileFlush(FileHandle* file) { return fflush(file->f) == 0; }
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file) { return fwrite(data, size, count, file->f); }
u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{ va_list a; va_start(a, fmt); int n = vfprintf(file->f, fmt, a); va_end(a); return n < 0 ? 0 : n; }
u64 FileLength(FileHandle* file) { auto p = ftello(file->f); fseeko(file->f, 0, SEEK_END); auto l = ftello(file->f); fseeko(file->f, p, SEEK_SET); return l; }

void Log(LogLevel level, const char* fmt, ...)
{
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    // single characters are the nocash debug port: gather them into lines, always shown
    if (buf[0] && !buf[1])
    {
        static std::string line;
        if (buf[0] == '\n') { fprintf(stderr, "[nocash %5d] %s\n", gFrame, line.c_str()); line.clear(); }
        else line += buf[0];
        return;
    }
    if (level == LogLevel::Debug && !gLogVerbose) return;
    // rate-limit repeated messages by their first 24 characters
    std::string key(buf, strnlen(buf, 24));
    unsigned n = ++gLogCounts[key];
    if (n <= 8 || gLogVerbose > 1) fprintf(stderr, "[melonDS] %s", buf);
    else if (n == 9) fprintf(stderr, "[melonDS] (more \"%s...\" messages counted, not shown)\n", key.c_str());
}

struct Thread { std::thread t; };
Thread* Thread_Create(std::function<void()> func) { return new Thread{std::thread(func)}; }
void Thread_Free(Thread* t) { if (t->t.joinable()) t->t.detach(); delete t; }
void Thread_Wait(Thread* t) { if (t->t.joinable()) t->t.join(); }

struct Semaphore { std::mutex m; std::condition_variable cv; int count = 0; };
Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* s) { delete s; }
void Semaphore_Reset(Semaphore* s) { std::lock_guard l(s->m); s->count = 0; }
void Semaphore_Wait(Semaphore* s) { std::unique_lock l(s->m); s->cv.wait(l, [&]{ return s->count > 0; }); s->count--; }
bool Semaphore_TryWait(Semaphore* s, int timeout_ms)
{ std::unique_lock l(s->m); if (!s->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&]{ return s->count > 0; })) return false; s->count--; return true; }
void Semaphore_Post(Semaphore* s, int count) { { std::lock_guard l(s->m); s->count += count; } s->cv.notify_all(); }

struct Mutex { std::mutex m; };
Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* m) { delete m; }
void Mutex_Lock(Mutex* m) { m->m.lock(); }
void Mutex_Unlock(Mutex* m) { m->m.unlock(); }
bool Mutex_TryLock(Mutex* m) { return m->m.try_lock(); }

void Sleep(u64 usecs) { std::this_thread::sleep_for(std::chrono::microseconds(usecs)); }
u64 GetMSCount() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u64 GetUSCount() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string gSavePath;
void WriteNDSSave(const u8* savedata, u32 savelen, u32, u32, void*)
{ if (gSavePath.empty()) return; FILE* f = fopen(gSavePath.c_str(), "wb"); if (f) { fwrite(savedata, 1, savelen, f); fclose(f); } }
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int len, u64, void*) { return len; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int len, u64, void*) { return len; }
int MP_SendReply(u8*, int len, u64, u16, void*) { return len; }
int MP_SendAck(u8*, int len, u64, void*) { return len; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }
int Net_SendPacket(u8*, int len, void*) { return len; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32* frame, int width, int height, bool, void*) { memset(frame, 0, width * height * 4); }
void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16* data, int maxlength, void*) { memset(data, 0, maxlength * 2); return maxlength; }
AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }
bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0; }
DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }
}
